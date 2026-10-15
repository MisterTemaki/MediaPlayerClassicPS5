// Media Player Classic PS5: the dashboard app built into MediaPlayerClassicPS5.elf (ProsperoInstall.h).
//
// The Makefile passes the five files' paths: eboot.bin (the launcher, which itself carries the emulator),
// sce_sys/param.json, sce_sys/icon0.png (the Media Player Classic PS5 icon, ps5/app/sce_sys/icon0.png),
// sce_sys/pic0.dds + pic1.dds (the home screen backgrounds) and sce_module/libc.prx (the C runtime module a native title carries, as PS5SX2's app folder). Only the final MediaPlayerClassicPS5.elf links this file.
//
// The eboot and the two backgrounds are stored as zlib streams (the Makefile's .z files; APP_*_SIZE = their
// inflated sizes): the payload stays under half the size.
//
// SPDX-License-Identifier: MIT

#include "ProsperoInstall.h"

#ifndef APP_EBOOT_SIZE
#define APP_EBOOT_SIZE 0
#endif
#ifndef APP_PIC0_SIZE
#define APP_PIC0_SIZE 0
#endif
#ifndef APP_PIC1_SIZE
#define APP_PIC1_SIZE 0
#endif

#define MPCPS5_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const unsigned char sym##_begin[];                                                              \
	extern "C" const unsigned char sym##_end[];

MPCPS5_INCBIN(mpcps5_app_eboot, APP_EBOOT)
MPCPS5_INCBIN(mpcps5_app_param, APP_PARAM)
MPCPS5_INCBIN(mpcps5_app_icon0, APP_ICON0)
MPCPS5_INCBIN(mpcps5_app_pic0, APP_PIC0)
MPCPS5_INCBIN(mpcps5_app_pic1, APP_PIC1)
MPCPS5_INCBIN(mpcps5_app_libc, APP_LIBC)

const EmbeddedAppFile* EmbeddedAppFiles()
{
	static const EmbeddedAppFile files[] = {
		{"sce_sys/param.json", mpcps5_app_param_begin, mpcps5_app_param_end, 0},
		{"sce_sys/icon0.png", mpcps5_app_icon0_begin, mpcps5_app_icon0_end, 0},
		{"sce_sys/pic0.dds", mpcps5_app_pic0_begin, mpcps5_app_pic0_end, APP_PIC0_SIZE},
		{"sce_sys/pic1.dds", mpcps5_app_pic1_begin, mpcps5_app_pic1_end, APP_PIC1_SIZE},
		{"sce_module/libc.prx", mpcps5_app_libc_begin, mpcps5_app_libc_end, 0},
		{"eboot.bin", mpcps5_app_eboot_begin, mpcps5_app_eboot_end, APP_EBOOT_SIZE},
		{nullptr, nullptr, nullptr, 0},
	};
	return files;
}
