// Implementation of the vmem related functions for the PS Vita, with the
// kubridge kernel plugin.
// The dynarec code lives in a memory block made readable, writable and
// executable once at startup, so any thread can write code without further
// kernel calls.
// The guest memory isn't mapped into a reserved address space: it's allocated
// on the heap and accessed through the memory handlers. kubridge's reserved
// memory blocks with mirrored pages hang the system when they are freed, even
// by the kernel when the process is terminated.
#include "types.h"

#ifdef __vita__
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/vshbridge.h>
#include <kubridge.h>

#include "oslib/virtmem.h"
#include "stdclass.h"

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

// No reserved guest address space: the guest memory is allocated on the heap
bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)
{
	return false;
}

void destroy()
{
}

void reset_mem(void *ptr, unsigned size_bytes)
{
	die("reset_mem: not supported");
}

void ondemand_page(void *address, unsigned size_bytes)
{
	die("ondemand_page: not supported");
}

void create_mappings(const Mapping *vmem_maps, unsigned nummaps)
{
	die("create_mappings: not supported");
}

bool region_lock(void *start, size_t len)
{
	return false;
}

bool region_unlock(void *start, size_t len)
{
	return false;
}

bool region_set_exec(void *start, size_t len)
{
	return false;
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
