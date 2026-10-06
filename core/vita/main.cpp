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

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

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

static bool fileExists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
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
}

// Guest memory isn't accessed through a reserved address space (see
// vita_vmem.cpp): there are no faults to handle
void os_InstallFaultHandler() {
}

void os_UninstallFaultHandler() {
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

	if (flycast_init(2, launchArgv))
		die("Flycast initialization failed");

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	flycast_term();
	sceKernelExitProcess(0);

	return 0;
}
#endif // __vita__
