// Media Player Classic PS5: the resources built into the app, and the DPI scale.
//
// From MPC-HC 1.7.13's own resources (src/mpc-hc/res, GPLv3): the toolbar (toolbar.svg, rasterized at the
// window's scale with NanoSVG as MPC-HC's SVGImage does), the logo shown while nothing plays (logo.3.png,
// DEF_LOGO), the status bar's audio pictures (stereo.bmp, mono.bmp, noaudio.bmp) and the program icon
// (icon.ico's pictures, as PNGs: assets/res/icon-*.png). The fonts: mpc_font.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_gdi.h"

#include <cstddef>
#include <cstdint>

#define MPC_INCBIN(sym, path)                                                                                      \
	__asm__(".section .rodata\n"                                                                                   \
			".balign 16\n"                                                                                         \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                            \
			".incbin \"" path "\"\n"                                                                               \
			".global " #sym "_end\n" #sym "_end:\n"                                                                \
			".previous\n");                                                                                        \
	extern "C" const unsigned char sym##_begin[];                                                                  \
	extern "C" const unsigned char sym##_end[];

namespace mpc
{
// DpiHelper: the window is drawn as Windows draws it at this many dots per inch (96 = 100 %, 144 = 150 %).
extern int g_dpi;
inline int ScaleX(int v) { return (v * g_dpi + (v >= 0 ? 48 : -48)) / 96; } // MulDiv rounding
inline int ScaleY(int v) { return ScaleX(v); }
inline int ScaleFloorX(int v) { return v * g_dpi / 96; }
inline int ScaleFloorY(int v) { return ScaleFloorX(v); }
inline double ScaleFactor() { return g_dpi / 96.0; }

struct Blob
{
	const unsigned char* data;
	size_t size;
};

namespace res
{
Blob FontSelawik();
Blob FontSelawikBold();
Blob FontRoboto();

// toolbar.svg at a button size of px (16 * scale): 15 pictures in a row, and their grayed copy
const Image& Toolbar(int px);
const Image& ToolbarDisabled(int px);
// logo.3.png
const Image& Logo();
// IDB_AUDIOTYPE_*: 0 none, 1 mono, 2 stereo
const Image& AudioType(int which);
// the program icon at about px pixels (16, 24, 32, 48, 256 are in icon.ico)
const Image& AppIcon(int px);
// a PNG/JPEG/BMP file's picture (stb_image), premultiplied
bool DecodeImage(const uint8_t* data, size_t size, Image& out);
} // namespace res
} // namespace mpc
