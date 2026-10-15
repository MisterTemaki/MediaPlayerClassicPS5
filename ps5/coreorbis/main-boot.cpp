// Media Player Classic PS5: the app's entry point (eboot.bin).
//
// The player runs as the dashboard app itself, a native PS5 title built the way PS5SX2 builds its eboot
// (ps5/coreorbis/link-vk.sh): the console gives the controller only to the title in front.
// MediaPlayerClassicPS5.elf is the installer and the helper (installer/installer_main.cpp).
//
// It asks to leave the sandbox (ProsperoJailbreak.h: /data and USB drives), brings the PS5 layer up in order --
// log, folders, video, audio, pads -- and hands over to the player's window (player/mpc_app.h). It ends
// through the system (sceSystemServiceLoadExec("exit")), as PS5SX2 learned it must.
//
//   eboot.bin [file]     with a path or URL, it opens at once (the host build's tests use it)
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_vdec.h"
#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoInput.h"
#include "ProsperoJailbreak.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "mpc_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>

#ifndef MPCPS5_PS5_VERSION
#define MPCPS5_PS5_VERSION "dev"
#endif

namespace
{
void WritePid()
{
	const std::string path = OrbisRoot() + "/pid.txt";
	if (FILE* f = fopen(path.c_str(), "w"))
	{
		fprintf(f, "%d\n", int(getpid()));
		fclose(f);
	}
}

// The end of the app: through the system, then _exit() if it never comes (PS5SX2's OrbisExitApp: on the
// console a title's exit() or return from main ends in SIGSYS).
[[noreturn]] void ExitApp(int status)
{
	OrbisLog("[boot] exit %d", status);
	ProsperoNotifyFlush();
	OrbisLogClose();
	fflush(stdout);
	fflush(stderr);
	const int rc = sceSystemServiceLoadExec("exit", nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	_exit(status);
}

struct Args
{
	int argc;
	char** argv;
	int status;
};

int Run(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	crashlog::Install();
	MPCPS5_STAGE(Boot, "start");
	OrbisLog("[boot] Media Player Classic PS5 %s, %s (built %s %s), pid %d", MPCPS5_PS5_VERSION,
		mpc::EngineVersion().c_str(), __DATE__, __TIME__, int(getpid()));

	// the console's video decoder: its library and compute queue only come before the jailbreak (mpc_vdec.h)
	MPCPS5_STAGE(Boot, "video decoder");
	mpc::hwdec::Prepare();

	// out of the sandbox, like PS5SX2: before it the app sees no /data and no USB drives
	MPCPS5_STAGE(Boot, "jailbreak");
	std::string how;
	const bool freed = jailbreak::RequestForSelf(how);
	OrbisLog("[boot] jailbreak: %s", freed ? how.c_str() : "nobody answered");
	const bool have_data = OrbisPathsInit();
	OrbisLogOpen(); // also writes the lines above
	if (!have_data)
		OrbisLog("[boot] can't create %s: no access to /data", OrbisRoot().c_str());
	else
		WritePid();

	MPCPS5_STAGE(Boot, "video");
	if (!ps5video::Init())
	{
		OrbisLog("[boot] video init failed");
		ProsperoNotify("Media Player Classic PS5: video failed to start (see %s/logs/boot.log)", OrbisRoot().c_str());
		return 1;
	}
	OrbisLog("[boot] splash screen hidden: %x", unsigned(sceSystemServiceHideSplashScreen()));
	MPCPS5_STAGE(Boot, "audio");
	if (!ps5audio::Init())
		OrbisLog("[boot] audio init failed: running without sound");
	MPCPS5_STAGE(Boot, "pad");
	if (!ps5input::Init())
		OrbisLog("[boot] pad init failed");

	const std::string first = argc > 1 && argv[1] ? argv[1] : "";
	const int status = mpc::RunApp(first, have_data);

	OrbisLog("[boot] leaving");
	ps5input::Shutdown();
	ps5audio::Shutdown();
	ps5video::Shutdown();
	return status;
}

void* RunThread(void* p)
{
	Args* a = static_cast<Args*>(p);
	a->status = Run(a->argc, a->argv);
	return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
	// The player runs on a thread with a stack of its own (8 MiB): a title's main thread gets a small one.
	Args args = {argc, argv, 1};
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
	pthread_t t;
	if (pthread_create(&t, &attr, RunThread, &args) == 0)
		pthread_join(t, nullptr);
	else
		args.status = Run(argc, argv);
	pthread_attr_destroy(&attr);
	ExitApp(args.status);
}
