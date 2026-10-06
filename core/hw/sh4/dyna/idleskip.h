/*
	Exact fast-forward of SH4 idle loops (dynarec, no MMU).

	A timeslice boundary that starts on a block without side effects arms a
	probe that follows the next blocks. If they come back to the same pc with
	an identical register file, without writing memory or reading I/O, every
	further round of the loop is identical until the next scheduler event or
	interrupt, so whole rounds are skipped by only advancing the cycle counter
	and the timeslice boundaries they would have crossed.
*/
#pragma once
#include "types.h"

struct RuntimeBlockInfo;

// Block can be part of a skippable loop: no memory writes, store queue
// flushes, interpreter fallbacks, SR updates or interrupt checks
bool idleskip_blockPure(RuntimeBlockInfo *block);

// Called by the dynarec when the cycle counter is negative at a block start.
// pcFlag: block pc | 1 if the block is pure. Returns IdleSkipAction.
// IdleSkipDoIter: continue at ctx.pc.
enum IdleSkipAction : u32 { IdleSkipResume, IdleSkipDoIter, IdleSkipCleanup };
u32 DYNACALL idleskip_sched(u32 pcFlag, int counter);

extern bool idleskip_enabled;
// Incremented by every memory handler (non direct-mapped) read
extern u32 idleskip_ioReads;
// Added to the context cycle counter to get the real one while probing
extern int idleskip_counterBias;
