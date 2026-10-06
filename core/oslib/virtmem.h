#pragma once
#include "types.h"

#if defined(_WIN32) || defined(__APPLE__)
#define DECLARE_CODE_CACHE(Name, Size) static u8 *Name;
#elif defined(__ANDROID__)
#define DECLARE_CODE_CACHE(Name, Size) alignas(4096) static u8 Name[Size];
#elif defined(__OpenBSD__)
#define DECLARE_CODE_CACHE(Name, Size) alignas(4096) static u8 Name[Size] __attribute__((section(".openbsd.mutable")));
#elif defined(__vita__)
namespace virtmem {
u8 *vita_code_area(size_t size);
// True if a translation fault at this address is caused by a protection change
// in progress: the access must be retried
bool vita_transient_fault(void *address);
}
// TELEMETRY (temporary): hang trace counters, see core/vita/main.cpp
#include <atomic>
enum VitaTrace { VT_FAULTS, VT_TRAMP, VT_INPLACE, VT_TRANSIENT, VT_PROT_IN, VT_PROT_OUT, VT_DECOMMIT_IN,
	VT_DECOMMIT_OUT, VT_COMMIT_IN, VT_COMMIT_OUT, VT_FAULT_ADDR, VT_FAULT_PC, VT_FRAMES, VT_PHASE, VT_RESETS, VT_MISMAP, VT_COUNT };
namespace virtmem { u32 vita_check_mirrors(u32 *firstOffset); void vita_vmem_selftest(); }
extern std::atomic<u32> vitaTrace[VT_COUNT];
extern bool vitaNoProtect;
extern bool vitaNoFastmem;
// Code caches are carved out of a single executable VM domain memory block
#define DECLARE_CODE_CACHE(Name, Size) static u8 (&Name)[Size] = *reinterpret_cast<u8 (*)[Size]>(virtmem::vita_code_area(Size));
#elif defined(__unix__) || defined(__SWITCH__) || defined(__HAIKU__)
#define DECLARE_CODE_CACHE(Name, Size) alignas(4096) static u8 Name[Size] __attribute__((section(".text")));
#else
#error Unknown platform for dynarec code cache declaration
#endif

namespace virtmem
{

struct Mapping {
	u64 start_address, end_address;
	u64 memoffset, memsize;
	bool allow_writes;
};

// Platform specific vmemory API
// To initialize (maybe) the vmem subsystem
bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize);
// To reset the on-demand allocated pages.
void reset_mem(void *ptr, unsigned size_bytes);
// To handle a fault&allocate an ondemand page.
void ondemand_page(void *address, unsigned size_bytes);
// To create the mappings in the address space.
void create_mappings(const Mapping *vmem_maps, unsigned nummaps);
// Just tries to wipe as much as possible in the relevant area.
void destroy();
// Given a block of data in the .text section, prepares it for JIT action.
// both code_area and size are page aligned. Returns success.
bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx);
// Same as above but uses two address spaces one with RX and RW protections.
// Note: this function doesnt have to be implemented, it's a fallback for the above one.
bool prepare_jit_block(void *code_area, size_t size, void **code_area_rw, ptrdiff_t *rx_offset);
// This might not need an implementation (ie x86/64 cpus).
void flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end);
// Change a code buffer permissions from r-x to/from rw-
void jit_set_exec(void* code, size_t size, bool enable);
// Release a jit block previously allocated by prepare_jit_block
void release_jit_block(void *code_area, size_t size);
// Release a jit block previously allocated by prepare_jit_block (with dual RW and RX areas)
void release_jit_block(void *code_area1, void *code_area2, size_t size);

// The address part of a host pointer. On arm64, Android may put a tag in the top
// byte of heap pointers, and a signal handler receives fault addresses without it.
static inline uintptr_t untag(const void *p)
{
#if HOST_CPU == CPU_ARM64 && defined(__ANDROID__)
	return (uintptr_t)p & 0x00ffffffffffffffull;
#else
	return (uintptr_t)p;
#endif
}

bool region_lock(void *start, std::size_t len);
bool region_unlock(void *start, std::size_t len);
bool region_set_exec(void *start, std::size_t len);

} // namespace vmem
