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
#include "types.h"

#ifdef __vita__
#include "log/LogManager.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "oslib/oslib.h"
#include "oslib/directory.h"
#include "oslib/i18n.h"
#include "stdclass.h"
#include "oslib/host_context.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "rend/TexCache.h"
#include "hw/mem/addrspace.h"
#include "hw/mem/mem_watch.h"
#include "oslib/virtmem.h"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>
#include <kubridge.h>
#include <malloc.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef USE_SDL
#include "sdl/sdl.h"
#endif

// Guest memory and the dynarec jump table (FPCB) are allocated from this heap
int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
// std::thread and std::async threads (game loader, emulator) otherwise get
// pthread-embedded's 32 KiB default stack
extern "C" unsigned int _pthread_stack_default_user = 512 * 1024;
// vitaGL memory pools
constexpr int VGL_RAM_POOL_SIZE = 32 * 1024 * 1024;
constexpr int VGL_CDRAM_POOL_SIZE = 48 * 1024 * 1024;

[[noreturn]] static void early_fatal_error(const char *msg)
{
	vglInit(0);
	SceMsgDialogUserMessageParam msg_param{};
	msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
	msg_param.msg = (const SceChar8 *)msg;
	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg_param;
	sceMsgDialogInit(&param);
	while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
		vglSwapBuffers(GL_TRUE);
	sceKernelExitProcess(0);
	for (;;)
		;
}

// TELEMETRY (temporary)
void vita_mem_report(const char *where)
{
	SceKernelFreeMemorySizeInfo info{};
	info.size = sizeof(info);
	sceKernelGetFreeMemorySize(&info);
	struct mallinfo mi = mallinfo();
	WARN_LOG(COMMON, "TM mem %s: free user %d cdram %d phycont %d, heap arena %d used %d free %d",
			where, info.size_user >> 10, info.size_cdram >> 10, info.size_phycont >> 10,
			mi.arena >> 10, mi.uordblks >> 10, mi.fordblks >> 10);
}

static bool fileExists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

// TELEMETRY (temporary): counters appended to a file and synced every 100 ms
// by their own thread, so that they survive a system freeze
std::atomic<u32> vitaTrace[VT_COUNT];
bool vitaNoProtect;
bool vitaNoFastmem;

static int traceThread(SceSize, void *)
{
	SceUID fd = sceIoOpen("ux0:data/flycast/data/trace.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd < 0)
		return 0;
	char line[256];
	int n = snprintf(line, sizeof(line), "start noprotect=%d nofastmem=%d novmem=%d\n", vitaNoProtect, vitaNoFastmem,
			fileExists("ux0:data/flycast/data/novmem"));
	sceIoWrite(fd, line, n);
	for (u32 seq = 0;; seq++)
	{
		n = snprintf(line, sizeof(line),
				"%u t=%llu fr=%u ph=%u flt=%u tr=%u inp=%u trans=%u prot=%u/%u dec=%u/%u com=%u/%u rst=%u last=%08x@%08x\n",
				seq, sceKernelGetProcessTimeWide() / 1000, (u32)vitaTrace[VT_FRAMES], (u32)vitaTrace[VT_PHASE],
				(u32)vitaTrace[VT_FAULTS], (u32)vitaTrace[VT_TRAMP], (u32)vitaTrace[VT_INPLACE],
				(u32)vitaTrace[VT_TRANSIENT], (u32)vitaTrace[VT_PROT_IN], (u32)vitaTrace[VT_PROT_OUT],
				(u32)vitaTrace[VT_DECOMMIT_IN], (u32)vitaTrace[VT_DECOMMIT_OUT],
				(u32)vitaTrace[VT_COMMIT_IN], (u32)vitaTrace[VT_COMMIT_OUT], (u32)vitaTrace[VT_RESETS],
				(u32)vitaTrace[VT_FAULT_ADDR], (u32)vitaTrace[VT_FAULT_PC]);
		sceIoWrite(fd, line, n);
		if (seq % 20 == 0)
		{
			u32 first = 0;
			u32 bad = virtmem::vita_check_mirrors(&first);
			n = snprintf(line, sizeof(line), "mirrors init=%u bad=%u first=%08x\n", (u32)vitaTrace[VT_MISMAP], bad, first);
			sceIoWrite(fd, line, n);
		}
		sceIoSyncByFd(fd, 0);
		sceKernelDelayThread(100000);
	}
}

static void startTrace()
{
	vitaNoProtect = fileExists("ux0:data/flycast/data/noprotect");
	vitaNoFastmem = fileExists("ux0:data/flycast/data/nofastmem");
	SceUID thid = sceKernelCreateThread("trace", traceThread, 0x40, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, nullptr);
	if (thid >= 0)
		sceKernelStartThread(thid, 0, nullptr);
}

static void checkRequirements()
{
	if (!fileExists("ur0:/data/libshacccg.suprx") && !fileExists("ur0:/data/external/libshacccg.suprx"))
		early_fatal_error("Error: Runtime shader compiler (libshacccg.suprx) is not installed.");
}

static std::string makeDir(const char *path)
{
	flycast::mkdir(path, 0777);
	return std::string(path) + "/";
}

namespace hostfs
{

std::string getScreenshotsPath()
{
	return makeDir("ux0:picture/SCREENSHOT/flycast");
}

}

void os_DoEvents() {
	// TELEMETRY (temporary)
	vitaTrace[VT_FRAMES]++;
	static u64 last;
	u64 now = sceKernelGetProcessTimeWide();
	if (now - last >= 500000) {
		last = now;
		vita_mem_report("tick");
		void bm_report();
		bm_report();
	}
}

// Data aborts: guest memory accesses of the dynarec outside the committed
// guest memory, writes to write-protected code pages (see vita_vmem.cpp). Same
// handling as the Linux SIGSEGV handler, done in the handler like flycast-vita
// 1.1. kubridge runs the handler in user mode on the faulting thread's stack.
// The deprecated registration function returns the system's default handler
// (coredump) as the previous one and also catches prefetch aborts.
static KuKernelAbortHandler nextAbortHandler;
static bool abortHandlerInstalled;

static bool handleFault(void *addr, u32 status, host_context_t& ctx)
{
	// Translation fault (section or page) during a protection change
	if ((status == 5 || status == 7) && virtmem::vita_transient_fault(addr))
		return true;
	// Ram watcher for net rollback
	if (memwatch::writeAccess(addr))
		return true;
	// code protection in RAM
	if (bm_RamWriteAccess(addr))
		return true;
	// texture protection in VRAM
	if (VramLockedWrite((u8 *)addr))
		return true;
	// FPCB jump table protection
	if (addrspace::bm_lockedWrite((u8 *)addr))
		return true;
	// fast mem access rewriting
	return sh4Dynarec != nullptr && sh4Dynarec->rewrite(ctx, addr);
}

static void abortHandler(KuKernelAbortContext *exc)
{
	vitaTrace[VT_FAULTS]++;
	vitaTrace[VT_FAULT_ADDR] = exc->FAR;
	vitaTrace[VT_FAULT_PC] = exc->pc;
	// A fault handled without effect would repeat forever, and the system
	// can't terminate a thread stuck in this loop: crash instead
	thread_local u32 lastPc, lastAddr, repeats;
	if (exc->pc == lastPc && exc->FAR == lastAddr)
		repeats++;
	else
	{
		lastPc = exc->pc;
		lastAddr = exc->FAR;
		repeats = 0;
	}
	if (exc->abortType == KU_KERNEL_ABORT_TYPE_DATA_ABORT && repeats < 1000)
	{
		void *addr = (void *)exc->FAR;
		const u32 status = (exc->FSR & 0xf) | ((exc->FSR >> 6) & 0x10);
		host_context_t ctx;
		ctx.pc = exc->pc;
		memcpy(ctx.reg, &exc->r0, sizeof(ctx.reg));	// r0-r14
		if (handleFault(addr, status, ctx))
		{
			memcpy(&exc->r0, ctx.reg, sizeof(ctx.reg));
			exc->pc = ctx.pc;
			return;
		}
		ERROR_LOG(COMMON, "Data abort @ %p invalid access to %p", (void *)exc->pc, addr);
	}
	else if (repeats >= 1000)
		ERROR_LOG(COMMON, "Data abort @ %p repeats on %p", (void *)exc->pc, (void *)exc->FAR);
	nextAbortHandler(exc);
}

void os_InstallFaultHandler()
{
	// Only needed for fast memory access, which requires kubridge
	if (!addrspace::virtmemEnabled() || abortHandlerInstalled)
		return;
	if (kuKernelRegisterAbortHandler(abortHandler, &nextAbortHandler, nullptr) < 0 || nextAbortHandler == nullptr)
		die("Can't install the data abort handler");
	abortHandlerInstalled = true;
}

void os_UninstallFaultHandler()
{
	if (!abortHandlerInstalled)
		return;
	kuKernelReleaseAbortHandler();
	abortHandlerInstalled = false;
}

void os_RunInstance(int argc, const char *argv[]) {
}

int main(int argc, char *argv[])
{
	checkRequirements();

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
	vglInitWithCustomSizes(0, 960, 544, VGL_RAM_POOL_SIZE, VGL_CDRAM_POOL_SIZE, 0, 0,
			SCE_GXM_MULTISAMPLE_NONE);

	LogManager::Init();
	startTrace();
	if (fileExists("ux0:data/flycast/data/vmemtest"))	// TELEMETRY (temporary)
		virtmem::vita_vmem_selftest();
	vita_mem_report("after vitaGL");
	i18n::init();

	std::string configDir = makeDir("ux0:data/flycast");
	std::string dataDir = makeDir("ux0:data/flycast/data");
	set_user_config_dir(configDir);
	set_user_data_dir(dataDir);
	add_system_config_dir(configDir);
	add_system_data_dir(dataDir);
	INFO_LOG(BOOT, "Config dir is: %s", get_writable_config_path("").c_str());
	INFO_LOG(BOOT, "Data dir is:   %s", get_writable_data_path("").c_str());

	// Custom bubbles pass the content path as a launch parameter.
	// Otherwise boot the BIOS.
	static char bootParams[1024];
	sceAppMgrGetAppParam(bootParams);
	const char *param = strstr(bootParams, "&param=");
	char *launchArgv[] = { argv[0], (char *)"dc_bios", nullptr };
	if (strstr(bootParams, "psgm:play") != nullptr && param != nullptr)
		launchArgv[1] = (char *)param + 7;

#ifdef USE_SDL
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		die("SDL: Initialization failed!");
#endif

	void common_linux_setup();
	common_linux_setup();

	vita_mem_report("before init");
	if (flycast_init(2, launchArgv))
		die("Flycast initialization failed");
	// now that the guest address space is reserved
	os_InstallFaultHandler();

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	vitaTrace[VT_PHASE] = 1;
	flycast_term();
	vitaTrace[VT_PHASE] = 2;
	os_UninstallFaultHandler();
	vitaTrace[VT_PHASE] = 3;
	sceKernelExitProcess(0);

	return 0;
}
#endif // __vita__
