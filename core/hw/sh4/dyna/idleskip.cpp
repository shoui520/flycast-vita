/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "idleskip.h"
#include "blockmanager.h"
#include "ngen.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

bool idleskip_enabled = true;
u32 idleskip_ioReads;
int idleskip_counterBias;

bool idleskip_blockPure(RuntimeBlockInfo *block)
{
	if (mmu_enabled() || block->BlockType == BET_StaticIntr || block->BlockType == BET_DynamicIntr)
		return false;
	for (const shil_opcode& op : block->oplist)
	{
		switch (op.op)
		{
		case shop_ifb:
		case shop_writem:
		case shop_pref:
		case shop_sync_sr:
		case shop_illegal:
			return false;
		case shop_readm:
			if (op.rs1.is_imm())
			{
				// constant address reads of I/O call the handler directly
				void *ptr;
				bool isRam;
				u32 paddr;
				if (!rdv_readMemImmediate(op.rs1._imm, op.size, ptr, isRam, paddr, block) || !isRam)
					return false;
			}
			break;
		default:
			break;
		}
	}
	return true;
}

// Blocks that are only impure because they write memory are interpreted while
// probing. These are the operations the interpreter and the dynarec do alike.
bool idleskip_blockInterpretable(RuntimeBlockInfo *block)
{
	if (block->has_fpu_op || block->BlockType == BET_StaticIntr || block->BlockType == BET_DynamicIntr)
		return false;
	for (const shil_opcode& op : block->oplist)
	{
		switch (op.op)
		{
		case shop_mov32:
		case shop_jdyn:
		case shop_jcond:
		case shop_readm:
		case shop_writem:
		case shop_and:
		case shop_or:
		case shop_xor:
		case shop_not:
		case shop_add:
		case shop_sub:
		case shop_neg:
		case shop_shl:
		case shop_shr:
		case shop_sar:
		case shop_adc:
		case shop_sbc:
		case shop_negc:
		case shop_ror:
		case shop_rocl:
		case shop_rocr:
		case shop_swaplb:
		case shop_shld:
		case shop_shad:
		case shop_ext_s8:
		case shop_ext_s16:
		case shop_mul_u16:
		case shop_mul_s16:
		case shop_mul_i32:
		case shop_test:
		case shop_seteq:
		case shop_setge:
		case shop_setgt:
		case shop_setae:
		case shop_setab:
		case shop_setpeq:
		case shop_xtrct:
			break;
		default:
			return false;
		}
	}
	return true;
}

namespace {

// Register file compared between two rounds: everything a block can read or
// write, up to (not including) the dynarec and scheduler state
constexpr size_t RegsStart = offsetof(Sh4Context, xf);
constexpr size_t RegsSize = offsetof(Sh4Context, CpuRunning) - RegsStart;

constexpr int MaxRoundBlocks = 16;
constexpr int MaxProbeBlocks = 32;
constexpr int MaxBackoff = 8;
// interpreted blocks per probe: idle loops have few impure blocks, other code
// would only make failed probes longer
constexpr int MaxInterpreted = 2;
// Blocks aren't interpreted after a few probes failed in a row, until a probe
// from the same pc is skipped. Indexed by pc, collisions only change the heuristic.
constexpr u8 InterpretTries = 2;
u8 failedProbes[16384];
u8& failures(u32 pc) {
	return failedProbes[(pc >> 1) % std::size(failedProbes)];
}

bool probing;
int realCounter;		// real cycle counter at the start of the last block
u32 roundPc;
int roundBlocks;
int roundCycles[MaxRoundBlocks];
int probeBlocks;
int interpretedBlocks;
// blocks interpreted by the current probe
u32 interpreted[MaxInterpreted];
u32 ioReadsAtStart;
// probe candidates to pass over after a failed probe
int backoff;
int backoffLength = 1;
u8 snapshot[RegsSize];
// a block of the current round wrote a new value to memory
bool roundWrites;

// Interpreted blocks only access RAM. Writes of the value already in memory
// change nothing. Other writes are logged so that they can be undone.
// Other accesses abort the block, which is then undone.
bool aborted;
u32 abortedAccess[2];
constexpr int MaxWrites = 16;
struct LoggedWrite
{
	u32 addr;
	u32 size;
	u32 old;
};
LoggedWrite writeLog[MaxWrites];
int writeCount;
u32 blockStart;
u32 blockEnd;
Sh4Context savedContext;

template<typename T>
T *probeRam(u32 addr)
{
	// area 3 outside of P4, aligned
	if (aborted || addr >= 0xE0000000 || ((addr >> 26) & 7) != 3 || addr % sizeof(T) != 0)
	{
		aborted = true;
		abortedAccess[0] = abortedAccess[1] = 0;
		return (T *)abortedAccess;
	}
	return (T *)&mem_b[addr & RAM_MASK];
}

template<typename T>
T DYNACALL probeRead(u32 addr) {
	return *probeRam<T>(addr);
}

template<typename T>
void writeRam(u32 addr, T data)
{
	if constexpr (sizeof(T) == 1)
		addrspace::write8(addr, data);
	else if constexpr (sizeof(T) == 2)
		addrspace::write16(addr, data);
	else
		addrspace::write32(addr, data);
}

template<typename T>
void DYNACALL probeWrite(u32 addr, T data)
{
	T *p = probeRam<T>(addr);
	if (aborted || *p == data)
		return;
	if (writeCount == MaxWrites
			// self-modifying code runs differently in the dynarec
			|| ((addr & RAM_MASK) + sizeof(T) > (blockStart & RAM_MASK) && (addr & RAM_MASK) < (blockEnd & RAM_MASK)))
	{
		aborted = true;
		return;
	}
	writeLog[writeCount++] = { addr, (u32)sizeof(T), (u32)*p };
	writeRam(addr, data);
}

u64 DYNACALL probeRead64(u32 addr) {
	aborted = true;
	return 0;
}

void DYNACALL probeWrite64(u32 addr, u64 data) {
	aborted = true;
}

// The dynarec skips bra/nop pairs at the targets of read-only blocks
// (SSAOptimizer::skipSingleBranchTarget)
u32 skipSingleBranchTarget(u32 addr, u32 vaddr, u32 guestOpcodes)
{
	const u32 startPage = vaddr >> 12;
	const u32 endPage = (vaddr + (guestOpcodes - 1) * 2) >> 12;
	for (int i = 0; i < 5; i++)
	{
		if ((addr >> 12) < startPage || ((addr + 2) >> 12) > endPage)
			break;
		const u32 op = IReadMem16(addr);
		if ((op & 0xF000) != 0xA000)
			break;
		const u16 delayOp = IReadMem16(addr + 2);
		if (delayOp != 0x0000 && delayOp != 0x0009)
			break;
		const int disp = GetSImm12(op) * 2 + 4;
		if (disp == 0)
			break;
		addr += disp;
	}
	return addr;
}

// Runs the block at ctx.pc like the dynarec would, without its cycles.
// Returns false, with nothing changed, if it can't be interpreted.
[[gnu::noinline]] bool interpretBlock(Sh4Context& ctx, RuntimeBlockInfo *block, int& cycles)
{
	if (ReadMem32 != &addrspace::read32 || WriteMem32 != &addrspace::write32 || block->vaddr != ctx.pc)
		return false;
	// set when the block is compiled
	if (block->idleskipInterpretable != 1)
		return false;
	blockStart = block->vaddr;
	blockEnd = blockStart + block->sh4_code_size;
	cycles = block->guest_cycles;
	const bool staticExit = BET_GET_CLS(block->BlockType) != BET_CLS_Dynamic;
	const bool readOnly = block->read_only;
	const u32 branchBlock = block->BranchBlock;
	const u32 nextBlock = block->NextBlock;
	const u32 guestOpcodes = block->guest_opcodes;

	memcpy(&savedContext, &ctx, sizeof(Sh4Context));
	writeCount = 0;
	aborted = false;
	const ReadMem8Func read8 = ReadMem8;
	const ReadMem16Func read16 = ReadMem16;
	const ReadMem32Func read32 = ReadMem32;
	const ReadMem64Func read64 = ReadMem64;
	const WriteMem8Func write8 = WriteMem8;
	const WriteMem16Func write16 = WriteMem16;
	const WriteMem32Func write32 = WriteMem32;
	const WriteMem64Func write64 = WriteMem64;
	ReadMem8 = &probeRead<u8>;
	ReadMem16 = &probeRead<u16>;
	ReadMem32 = &probeRead<u32>;
	ReadMem64 = &probeRead64;
	WriteMem8 = &probeWrite<u8>;
	WriteMem16 = &probeWrite<u16>;
	WriteMem32 = &probeWrite<u32>;
	WriteMem64 = &probeWrite64;
	// delay slots
	Sh4Interpreter::Instance = Sh4Recompiler::Instance;

	bool done = true;
	try {
		// The block ends with its last instruction or a taken branch.
		// Branches run their delay slot.
		u32 pc;
		do {
			pc = ctx.pc;
			const u16 op = IReadMem16(pc);
			ctx.pc = pc + 2;
			OpPtr[op](&ctx, op);
		} while (ctx.pc == pc + 2 && ctx.pc != blockEnd && !aborted);
		if (aborted)
			done = false;
		else if (staticExit)
		{
			// the dynarec jumps to one of the static exits
			if (readOnly)
				ctx.pc = skipSingleBranchTarget(ctx.pc, blockStart, guestOpcodes);
			done = ctx.pc == branchBlock || ctx.pc == nextBlock;
		}
	} catch (const SH4ThrownException&) {
		done = false;
	}

	Sh4Interpreter::Instance = nullptr;
	ReadMem8 = read8;
	ReadMem16 = read16;
	ReadMem32 = read32;
	ReadMem64 = read64;
	WriteMem8 = write8;
	WriteMem16 = write16;
	WriteMem32 = write32;
	WriteMem64 = write64;

	if (!done)
	{
		for (int i = writeCount - 1; i >= 0; i--)
		{
			const LoggedWrite& w = writeLog[i];
			if (w.size == 1)
				writeRam<u8>(w.addr, w.old);
			else if (w.size == 2)
				writeRam<u16>(w.addr, w.old);
			else
				writeRam<u32>(w.addr, w.old);
		}
		memcpy(&ctx, &savedContext, sizeof(Sh4Context));
		return false;
	}
	if (writeCount != 0)
		roundWrites = true;
	return true;
}

const u8 *regs(const Sh4Context& ctx) {
	return (const u8 *)&ctx + RegsStart;
}

// A timeslice boundary, as reached when the counter is negative at a block start
u32 sliceEnd(Sh4Context& ctx, int counter)
{
	ctx.cycle_counter = counter + SH4_TIMESLICE;
	if (!ctx.CpuRunning)
		return IdleSkipCleanup;
	if (UpdateSystem_INTC())
		return IdleSkipDoIter;
	return IdleSkipResume;
}

// Advance whole rounds from roundPc. Every block start of a skipped round sees
// either a non-negative counter or a timeslice boundary that does nothing
// (no scheduler event due, no pending interrupt).
// Returns the counter at the start of the next round.
[[gnu::noinline]] int skipRounds(Sh4Context& ctx, int counter)
{
	int period = 0;
	for (int i = 0; i < roundBlocks; i++)
		period += roundCycles[i];
	if (period <= 0)
		return counter;
	// lowest counter seen at a block start, relative to the round start
	const int lowest = period - roundCycles[roundBlocks - 1];
	const int sliceRounds = SH4_TIMESLICE / period;
	for (;;)
	{
		const int ahead = counter - lowest;
		if (ahead >= 0)
		{
			// rounds = ahead / period. No hardware divide on Cortex-A9: after a
			// boundary, ahead is less than SH4_TIMESLICE, so start from a close estimate.
			int rounds;
			if (ahead < SH4_TIMESLICE)
			{
				rounds = sliceRounds;
				int rest = ahead - rounds * period;
				while (rest < 0)
				{
					rest += period;
					rounds--;
				}
				while (rest >= period)
				{
					rest -= period;
					rounds++;
				}
			}
			else
			{
				rounds = ahead / period;
			}
			counter -= (rounds + 1) * period;
		}
		// the next round crosses a timeslice boundary
		int c = counter;
		int next = ctx.sh4_sched_next;
		for (int i = 0; i < roundBlocks; i++)
		{
			if (c < 0)
			{
				if (ctx.interrupt_pend != 0 || next < SH4_TIMESLICE || !ctx.CpuRunning)
					return counter;
				c += SH4_TIMESLICE;
				next -= SH4_TIMESLICE;
			}
			c -= roundCycles[i];
		}
		counter = c;
		ctx.sh4_sched_next = next;
	}
}

[[gnu::noinline]] void startProbe(Sh4Context& ctx, u32 pc)
{
	probing = true;
	realCounter = ctx.cycle_counter;
	roundPc = pc;
	roundBlocks = 0;
	probeBlocks = 0;
	interpretedBlocks = 0;
	ioReadsAtStart = idleskip_ioReads;
	memcpy(snapshot, regs(ctx), RegsSize);
	roundWrites = false;
	// make the next block start call back
	idleskip_counterBias = realCounter + 1;
	ctx.cycle_counter = -1;
}

// the blocks interpreted by a probe weren't part of a skipped round
void interpretedFailed()
{
	for (int i = 0; i < std::min(interpretedBlocks, MaxInterpreted); i++)
	{
		u8& f = failures(interpreted[i]);
		if (f < InterpretTries)
			f++;
	}
}

void probeFailed()
{
	interpretedFailed();
	backoff = backoffLength;
	backoffLength = std::min(backoffLength * 2, MaxBackoff);
}

}

namespace {

constexpr u32 ProbeEnded = ~0u;

// Impure blocks never run with the probe counter: interpret them
// The block at pc, if compiled. The last one found is kept while no block is discarded.
RuntimeBlockInfo *blockAt(u32 pc)
{
	static u32 lastPc = ~0u;
	static u32 lastGeneration;
	static RuntimeBlockInfo *lastBlock;
	if (pc != lastPc || bm_generation != lastGeneration)
	{
		RuntimeBlockInfoPtr block = bm_GetBlock(pc);
		if (block == nullptr)
			return nullptr;
		lastPc = pc;
		lastGeneration = bm_generation;
		lastBlock = block.get();
	}
	return lastBlock;
}

[[gnu::noinline]] u32 impureBlock(Sh4Context& ctx, u32 pc, int counter)
{
	int blockCycles;
	RuntimeBlockInfo *block;
	// checked first: refused blocks are common, and cold in the cache
	if (interpretedBlocks == MaxInterpreted || failures(pc) >= InterpretTries
			|| (block = blockAt(pc)) == nullptr
			|| (interpreted[interpretedBlocks++] = pc, !interpretBlock(ctx, block, blockCycles)))
	{
		probeFailed();
		ctx.cycle_counter = counter;
		return IdleSkipResume;
	}
	probing = true;
	realCounter = counter;
	idleskip_counterBias = counter + 1;
	// as if the block ran with the probe counter
	ctx.cycle_counter = -1 - blockCycles;
	return IdleSkipDoIter;
}

// A block start while probing. Returns ProbeEnded when the probe is over and
// this block start is a regular timeslice boundary.
[[gnu::noinline]] u32 probeHook(Sh4Context& ctx, u32 pcFlag, int& counter)
{
	const u32 pc = pcFlag & ~1u;
	const bool pure = pcFlag & 1;
	probing = false;
	idleskip_counterBias = 0;
	if (!ctx.CpuRunning)
		// Stop() from another thread reset the counter to 0
		return sliceEnd(ctx, counter);
	// the block started at the last hook ran with a counter of -1
	const int cycles = -1 - counter;
	counter = realCounter - cycles;
	if (counter >= 0)
	{
		if (idleskip_ioReads != ioReadsAtStart || probeBlocks++ == MaxProbeBlocks
				|| roundBlocks == MaxRoundBlocks)
		{
			probeFailed();
			ctx.cycle_counter = counter;
			return IdleSkipResume;
		}
		roundCycles[roundBlocks++] = cycles;
		if (pc == roundPc)
		{
			// a round that doesn't change registers or memory is repeated identically
			if (!roundWrites && memcmp(snapshot, regs(ctx), RegsSize) == 0)
			{
				for (int i = 0; i < std::min(interpretedBlocks, MaxInterpreted); i++)
					failures(interpreted[i]) = 0;
				counter = skipRounds(ctx, counter);
				backoffLength = 1;
				if (counter >= 0)
				{
					ctx.cycle_counter = counter;
					return IdleSkipResume;
				}
				// a boundary that does something is due right here
				return sliceEnd(ctx, counter);
			}
			// not a fixed point yet: start the round again from here
			memcpy(snapshot, regs(ctx), RegsSize);
			roundBlocks = 0;
			roundWrites = false;
		}
		if (!pure)
			return impureBlock(ctx, pc, counter);
		probing = true;
		realCounter = counter;
		idleskip_counterBias = counter + 1;
		ctx.cycle_counter = -1;
		return IdleSkipResume;
	}
	// the real counter ran out before a round was seen
	interpretedFailed();
	return ProbeEnded;
}

}

u32 DYNACALL idleskip_sched(u32 pcFlag, int counter)
{
	Sh4Context& ctx = Sh4cntx;
	ctx.pc = pcFlag & ~1u;
	if (unlikely(probing))
	{
		const u32 action = probeHook(ctx, pcFlag, counter);
		if (action != ProbeEnded)
			return action;
	}

	const u32 action = sliceEnd(ctx, counter);
	if (action == IdleSkipResume && (pcFlag & 1) && idleskip_enabled && !mmu_enabled())
	{
		if (backoff > 0)
			backoff--;
		else if (ctx.cycle_counter >= 0)
			startProbe(ctx, pcFlag & ~1u);
	}
	return action;
}

