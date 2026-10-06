// Implementation of the vmem related functions for the PS Vita, with the
// kubridge kernel plugin.
// The dynarec code lives in a memory block made readable, writable and
// executable once at startup, so any thread can write code without further
// kernel calls.
// Guest memory: a reserved address range (Sh4RCB + the 512 MB SH4 address
// space) where the guest memory block (RAM, VRAM, ARAM, ERAM) is committed as
// mirrors, once.
// Accesses to the uncommitted parts fault and the dynarec rewrites them to
// call the memory handlers (see main.cpp). Code pages in RAM and texture pages
// in VRAM are write protected like on other platforms.
#include "types.h"

#ifdef __vita__
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/vshbridge.h>
#include <kubridge.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "oslib/virtmem.h"
#include "hw/sh4/sh4_if.h"
#include "stdclass.h"

#include <mutex>
#include <vector>

namespace virtmem
{

constexpr size_t CODE_AREA_SIZE = 16_MB;
static SceUID codeBlock = -1;
static u8 *codeBase;
static size_t codeUsed;
static int codeError;

// kubridge is linked with weak imports: don't call it unless it's loaded
static bool kubridgeLoaded()
{
	int unused[2];
	return _vshKernelSearchModuleByName("kubridge", unused) >= 0;
}

// Called by static initializers: logging isn't available yet, failures are
// reported by prepare_jit_block.
u8 *vita_code_area(size_t size)
{
	if (codeBlock < 0)
	{
		if (codeError != 0)
			return nullptr;
		if (!kubridgeLoaded())
		{
			codeError = SCE_KERNEL_ERROR_MODULEMGR_NOENT;
			return nullptr;
		}
		SceKernelAllocMemBlockKernelOpt opt{};
		opt.size = sizeof(opt);
		SceUID block = kuKernelAllocMemBlock("flycast_code", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, CODE_AREA_SIZE, &opt);
		if (block < 0)
		{
			codeError = block;
			return nullptr;
		}
		void *base;
		// Protect the whole block exactly once: kubridge 0.3.x protects
		// partial ranges by splitting the block's pages
		if ((codeError = sceKernelGetMemBlockBase(block, &base)) < 0
				|| (codeError = kuKernelMemProtect(base, CODE_AREA_SIZE,
						KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC)) < 0)
		{
			sceKernelFreeMemBlock(block);
			return nullptr;
		}
		codeBlock = block;
		codeBase = (u8 *)base;
	}
	size = (size + PAGE_MASK) & ~PAGE_MASK;
	if (codeUsed + size > CODE_AREA_SIZE)
		return nullptr;
	u8 *area = codeBase + codeUsed;
	codeUsed += size;
	return area;
}

static u8 *guestBase;
static u8 *backingBase;	// TELEMETRY (temporary)
static void checkMirrorsAtInit();
static std::mutex checkMutex;	// TELEMETRY (temporary)
// TELEMETRY (temporary): bisect switch. Commit each guest memory once, with its
// own pages, instead of mirrors of a backing block
static bool noAlias;
struct Committed { u64 start, size; };
static std::vector<Committed> committed;
static SceUID reserveBlock = -1;
// All the guest memories (RAM, VRAM, ARAM, ERAM), mapped as mirrors
static SceUID backingBlock = -1;
static std::vector<Mapping> mappings;
static std::mutex protectMutex;

static const Mapping *writableMapping(u64 offset, u64 len)
{
	if (noAlias)
	{
		for (const Committed& c : committed)
			if (offset >= c.start && offset + len <= c.start + c.size)
				for (const Mapping& map : mappings)
					if (map.memsize != 0 && map.allow_writes && c.start >= map.start_address && c.start < map.end_address)
						return &map;
		return nullptr;
	}
	for (const Mapping& map : mappings)
		if (map.memsize != 0 && map.allow_writes
				&& offset >= map.start_address && offset + len <= map.end_address)
			return &map;
	return nullptr;
}

// Guest memory write protection (code pages in RAM, textures in VRAM)
static bool guestProtect(void *start, size_t len, bool lock)
{
	if (guestBase == nullptr || (u8 *)start < guestBase || len == 0 || vitaNoProtect)
		return false;
	const u64 inpage = ((u8 *)start - guestBase) & PAGE_MASK;
	const u64 offset = (u8 *)start - guestBase - inpage;
	len = (len + inpage + PAGE_MASK) & ~PAGE_MASK;
	if (writableMapping(offset, len) == nullptr)
		return false;
	const SceUInt32 prot = KU_KERNEL_PROT_READ | (lock ? 0 : KU_KERNEL_PROT_WRITE);
	std::lock_guard<std::mutex> _(protectMutex);
	vitaTrace[VT_PROT_IN]++;
	int rc = kuKernelMemProtect(&guestBase[offset], len, prot);
	vitaTrace[VT_PROT_OUT]++;
	if (rc < 0)
	{
		ERROR_LOG(VMEM, "kuKernelMemProtect(%x, %x, %x) failed: %08x", (u32)offset, (u32)len, prot, rc);
		die("kuKernelMemProtect failed");
	}
	return true;
}

bool region_lock(void *start, size_t len)
{
	return guestProtect(start, len, true);
}

bool region_unlock(void *start, size_t len)
{
	return guestProtect(start, len, false);
}

// kuKernelMemProtect unmaps pages before mapping them again with the new
// protection: other threads accessing them meanwhile get a translation fault.
// Retry them once the protection change is done.
bool vita_transient_fault(void *address)
{
	thread_local void *lastAddress;
	thread_local int retries;
	if (guestBase == nullptr || (u8 *)address < guestBase
			|| writableMapping(((u8 *)address - guestBase) & ~(uintptr_t)PAGE_MASK, PAGE_SIZE) == nullptr)
		return false;
	if (address != lastAddress)
	{
		lastAddress = address;
		retries = 0;
	}
	if (++retries > 100)
		return false;
	vitaTrace[VT_TRANSIENT]++;
	// wait for the protection change in progress
	std::lock_guard<std::mutex> _(protectMutex);
	return true;
}

bool region_set_exec(void *start, size_t len)
{
	return false;
}

// Sh4RCB, the 512 MB SH4 address space, and the writable ARAM mapping after it
// (see addrspace::initMappings)
constexpr size_t GUEST_SPACE_SIZE = 512_MB + ARAM_SIZE_MAX;
static_assert(sizeof(Sh4RCB) % PAGE_SIZE == 0, "Sh4RCB must be page aligned");

// Same kubridge usage as flycast-vita 1.1, which is known to be safe: one
// reserved block, one commit per mirror, protection changes on page ranges,
// and memory blocks freed without decommitting.
bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)
{
	// TELEMETRY (temporary): bisect switch
	SceIoStat st;
	if (sceIoGetstat("ux0:data/flycast/data/novmem", &st) >= 0)
	{
		WARN_LOG(VMEM, "novmem: no fast memory access");
		return false;
	}
	if (!kubridgeLoaded())
	{
		WARN_LOG(VMEM, "kubridge isn't loaded: no fast memory access");
		return false;
	}
	noAlias = sceIoGetstat("ux0:data/flycast/data/noalias", &st) >= 0;
	void *base = nullptr;
	SceUID block = kuKernelMemReserve(&base, sizeof(Sh4RCB) + GUEST_SPACE_SIZE, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
	if (block < 0)
	{
		WARN_LOG(VMEM, "kuKernelMemReserve failed: %08x", block);
		return false;
	}
	SceUID backing = sceKernelAllocMemBlock("flycast_guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (ramSize + PAGE_MASK) & ~PAGE_MASK, nullptr);
	if (backing < 0)
	{
		WARN_LOG(VMEM, "Can't allocate guest memory: %08x", backing);
		sceKernelFreeMemBlock(block);
		return false;
	}
	vitaTrace[VT_COMMIT_IN]++;
	int rc = kuKernelMemCommit(base, sizeof(Sh4RCB), KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, nullptr);
	vitaTrace[VT_COMMIT_OUT]++;
	if (rc < 0)
	{
		WARN_LOG(VMEM, "kuKernelMemCommit(Sh4RCB) failed: %08x", rc);
		sceKernelFreeMemBlock(block);
		sceKernelFreeMemBlock(backing);
		return false;
	}
	reserveBlock = block;
	backingBlock = backing;
	sceKernelGetMemBlockBase(backing, (void **)&backingBase);
	if (noAlias)
		NOTICE_LOG(VMEM, "noalias: no guest memory mirrors");
	guestBase = (u8 *)base + sizeof(Sh4RCB);
	*sh4rcb_addr = base;
	*vmem_base_addr = guestBase;
	INFO_LOG(VMEM, "Guest address space reserved at %p", base);

	return true;
}

// The reserved block's pages alias the backing block's physical pages without
// a reference to it: free it first.
void destroy()
{
	if (reserveBlock < 0)
		return;
	{ std::lock_guard<std::mutex> _(checkMutex); vitaTrace[VT_PHASE] = 10; }
	sceKernelDelayThread(300000);	// TELEMETRY (temporary): let the trace record each step
	// Freeing the reserved block while it still maps the guest memory hangs
	// the system. kubridge 0.3 decommits safely one page at a time only: when a
	// call unmaps several pages, it merges them into a page object it has
	// already freed.
	for (const Mapping& map : mappings)
		if (map.memsize != 0 && !noAlias)
			for (u64 offset = map.start_address; offset < map.end_address; offset += PAGE_SIZE)
			{
				int rc = kuKernelMemDecommit(&guestBase[offset], PAGE_SIZE);
				vitaTrace[VT_DECOMMIT_OUT]++;
				if (rc < 0)
				{
					ERROR_LOG(VMEM, "kuKernelMemDecommit(%x) failed: %08x", (u32)offset, rc);
					break;
				}
			}
	vitaTrace[VT_PHASE] = 13;
	sceKernelDelayThread(300000);
	sceKernelFreeMemBlock(reserveBlock);
	vitaTrace[VT_PHASE] = 11;
	sceKernelDelayThread(300000);
	reserveBlock = -1;
	guestBase = nullptr;
	mappings.clear();
	if (backingBlock >= 0)
		sceKernelFreeMemBlock(backingBlock);
	backingBlock = -1;
	committed.clear();
	vitaTrace[VT_PHASE] = 12;
	sceKernelDelayThread(300000);
}

// The FPCB is a two-level table (FPCB_L1_SIZE) allocated from the heap: there
// are no on-demand pages
void reset_mem(void *ptr, unsigned size_bytes)
{
	die("reset_mem: not supported");
}

void ondemand_page(void *address, unsigned size_bytes)
{
	die("ondemand_page: not supported");
}

static bool sameMappings(const Mapping *vmem_maps, unsigned nummaps)
{
	if (nummaps != mappings.size())
		return false;
	for (unsigned i = 0; i < nummaps; i++)
	{
		const Mapping& a = vmem_maps[i];
		const Mapping& b = mappings[i];
		if (a.start_address != b.start_address || a.end_address != b.end_address
				|| a.memoffset != b.memoffset || a.memsize != b.memsize || a.allow_writes != b.allow_writes)
			return false;
	}
	return true;
}

void create_mappings(const Mapping *vmem_maps, unsigned nummaps)
{
	// The mappings only depend on the platform. Committing over committed
	// pages or decommitting mirrors split by protection changes isn't safe
	// with kubridge 0.3: keep the first set of mappings.
	if (sameMappings(vmem_maps, nummaps))
		return;
	if (!mappings.empty())
		die("Guest memory mappings can't change: restart Flycast");

	for (unsigned i = 0; i < nummaps; i++)
	{
		const Mapping& map = vmem_maps[i];
		mappings.push_back(map);
		// Unmapped areas stay reserved: accesses fault
		if (map.memsize == 0)
			continue;
		verify(map.memsize % PAGE_SIZE == 0 && map.start_address % PAGE_SIZE == 0);
		const u64 rangeSize = map.end_address - map.start_address;
		verify(rangeSize % map.memsize == 0 && map.end_address <= GUEST_SPACE_SIZE);

		if (noAlias)
		{
			// first writable mirror of each memory only
			if (!map.allow_writes)
				continue;
			bool done = false;
			for (unsigned k = 0; k < i; k++)
				if (vmem_maps[k].memsize != 0 && vmem_maps[k].allow_writes && vmem_maps[k].memoffset == map.memoffset)
					done = true;
			if (done)
				continue;
			KuKernelMemCommitOpt opt{};
			opt.size = sizeof(opt);
			opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
			opt.baseBlock = backingBlock;
			opt.baseOffset = map.memoffset;
			int rc = kuKernelMemCommit(&guestBase[map.start_address], map.memsize, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, &opt);
			vitaTrace[VT_COMMIT_OUT]++;
			if (rc < 0)
			{
				ERROR_LOG(VMEM, "kuKernelMemCommit(%x, %x) failed: %08x", (u32)map.start_address, (u32)map.memsize, rc);
				die("Can't map guest memory");
			}
			committed.push_back({ map.start_address, map.memsize });
			NOTICE_LOG(VMEM, "noalias: committed %08x size %x", (u32)map.start_address, (u32)map.memsize);
			continue;
		}
		KuKernelMemCommitOpt opt{};
		opt.size = sizeof(opt);
		opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
		opt.baseBlock = backingBlock;
		opt.baseOffset = map.memoffset;
		const SceUInt32 prot = KU_KERNEL_PROT_READ | (map.allow_writes ? KU_KERNEL_PROT_WRITE : 0);
		// One commit per mirror
		for (u64 offset = map.start_address; offset < map.end_address; offset += map.memsize)
		{
			vitaTrace[VT_COMMIT_IN]++;
			int rc = kuKernelMemCommit(&guestBase[offset], map.memsize, prot, &opt);
			vitaTrace[VT_COMMIT_OUT]++;
			if (rc < 0)
			{
				ERROR_LOG(VMEM, "kuKernelMemCommit(%x, %x) failed: %08x", (u32)offset, (u32)map.memsize, rc);
				die("Can't map guest memory");
			}
		}
	}
	if (!noAlias)
		checkMirrorsAtInit();
}

// TELEMETRY (temporary): checks that every mirror page maps the guest memory
// block. Returns the number of pages whose first bytes differ from the backing
// block, and the first one's guest offset.
u32 vita_check_mirrors(u32 *firstOffset)
{
	std::lock_guard<std::mutex> _(checkMutex);
	if (guestBase == nullptr || backingBase == nullptr || noAlias || vitaTrace[VT_PHASE] >= 10)
		return 0;
	u32 bad = 0;
	for (const Mapping& map : mappings)
		if (map.memsize != 0)
			for (u64 offset = map.start_address; offset < map.end_address; offset += PAGE_SIZE)
			{
				const u8 *mirror = &guestBase[offset];
				const u8 *backing = &backingBase[map.memoffset + (offset - map.start_address) % map.memsize];
				if (memcmp(mirror, backing, 64) == 0)
					continue;
				// guest writes in progress: check again
				int retry = 0;
				for (; retry < 3; retry++)
				{
					sceKernelDelayThread(1000);
					if (memcmp(mirror, backing, 64) == 0)
						break;
				}
				if (retry < 3)
					continue;
				if (bad++ == 0)
					*firstOffset = (u32)offset;
			}
	return bad;
}

// Writes a pattern into the backing block and reads it through every mirror
static void checkMirrorsAtInit()
{
	for (const Mapping& map : mappings)
		if (map.memsize != 0)
			for (u64 p = 0; p < map.memsize; p += PAGE_SIZE)
				*(u32 *)&backingBase[map.memoffset + p] = (u32)(map.memoffset + p) | 0x80000001;
	u32 bad = 0;
	for (const Mapping& map : mappings)
		if (map.memsize != 0)
			for (u64 offset = map.start_address; offset < map.end_address; offset += PAGE_SIZE)
			{
				u32 expected = (u32)(map.memoffset + (offset - map.start_address) % map.memsize) | 0x80000001;
				u32 v = *(volatile u32 *)&guestBase[offset];
				if (v != expected && bad++ < 16)
					ERROR_LOG(VMEM, "mirror %08x reads %08x instead of %08x", (u32)offset, v, expected);
			}
	vitaTrace[VT_MISMAP] = bad;
	NOTICE_LOG(VMEM, "Mirror check: %u bad pages", bad);
	for (const Mapping& map : mappings)
		if (map.memsize != 0)
			memset(&backingBase[map.memoffset], 0, map.memsize);
}

bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)
{
	if (code_area == nullptr)
	{
		if (codeError != 0)
			ERROR_LOG(DYNAREC, "No code area: kubridge error %08x (is kubridge.skprx installed?)", codeError);
		else
			ERROR_LOG(DYNAREC, "No code area: %x of %x bytes used", (u32)codeUsed, (u32)CODE_AREA_SIZE);
		return false;
	}
	*code_area_rwx = code_area;
	return true;
}

bool prepare_jit_block(void *code_area, size_t size, void **code_area_rw, ptrdiff_t *rx_offset)
{
	return false;
}

void release_jit_block(void *code_area, size_t size)
{
}

void release_jit_block(void *code_area1, void *code_area2, size_t size)
{
}

void jit_set_exec(void *code, size_t size, bool enable)
{
}

void flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end)
{
	kuKernelFlushCaches(icache_start, (u8 *)icache_end - (u8 *)icache_start + 1);
	if (dcache_start != icache_start)
		kuKernelFlushCaches(dcache_start, (u8 *)dcache_end - (u8 *)dcache_start + 1);
}

} // namespace virtmem

#endif // __vita__

// TELEMETRY (temporary): flycast 1.1's exact kubridge sequence, in isolation
namespace virtmem {
void vita_vmem_selftest()
{
	constexpr u32 RAM = 16_MB, VRAM = 8_MB, ARAM = 2_MB;
	struct { u32 start, end, offset, size; bool write; } maps[] = {
		{ 0x00800000, 0x01000000, RAM + VRAM, ARAM, true },
		{ 0x02800000, 0x03000000, RAM + VRAM, ARAM, true },
		{ 0x04000000, 0x05000000, RAM, VRAM, true },
		{ 0x06000000, 0x07000000, RAM, VRAM, true },
		{ 0x0C000000, 0x10000000, 0, RAM, true },
	};
	for (u32 iter = 0; iter < 3; iter++)
	{
		auto phase = [iter](u32 step) {
			vitaTrace[VT_PHASE] = 100 + iter * 10 + step;
			sceKernelDelayThread(300000);
		};
		phase(0);
		void *base = nullptr;
		SceUID reserve = kuKernelMemReserve(&base, 512_MB + 0x10000 + 8_MB, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
		SceUID backing = sceKernelAllocMemBlock("selftest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, RAM + VRAM + ARAM, nullptr);
		NOTICE_LOG(VMEM, "selftest %u: reserve %x base %p backing %x", iter, reserve, base, backing);
		if (reserve < 0 || backing < 0)
			return;
		u8 *guest = (u8 *)base + 0x10000;
		phase(1);
		KuKernelMemCommitOpt opt{};
		opt.size = sizeof(opt);
		opt.attr = KU_KERNEL_MEM_COMMIT_ATTR_HAS_BASE;
		opt.baseBlock = backing;
		for (const auto& m : maps)
			for (u32 a = m.start; a < m.end; a += m.size)
			{
				opt.baseOffset = m.offset;
				int rc = kuKernelMemCommit(guest + a, m.size, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE, &opt);
				if (rc < 0)
					NOTICE_LOG(VMEM, "selftest commit %x failed %x", a, rc);
			}
		phase(2);
		guest[0x0C000000] = 0x5a;
		bool ok = guest[0x0D000000] == 0x5a;
		NOTICE_LOG(VMEM, "selftest %u: mirror %s", iter, ok ? "ok" : "BAD");
		phase(3);
		sceKernelFreeMemBlock(reserve);
		phase(4);
		sceKernelFreeMemBlock(backing);
		phase(5);
	}
	NOTICE_LOG(VMEM, "selftest done");
	vitaTrace[VT_PHASE] = 199;
	sceKernelDelayThread(500000);
	sceKernelExitProcess(0);
}
}
