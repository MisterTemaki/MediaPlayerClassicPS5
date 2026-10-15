// Media Player Classic PS5: the built-in resources (mpc_res.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_res.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "nanosvg.h"
#include "nanosvgrast.h"

#ifndef MPC_ASSET_DIR
#error MPC_ASSET_DIR must name ps5/assets
#endif

MPC_INCBIN(mpc_font_selawik, MPC_ASSET_DIR "/fonts/Selawik-Regular.ttf")
MPC_INCBIN(mpc_font_selawik_bold, MPC_ASSET_DIR "/fonts/Selawik-Bold.ttf")
MPC_INCBIN(mpc_font_roboto, MPC_ASSET_DIR "/fonts/Roboto-Regular.ttf")
MPC_INCBIN(mpc_res_toolbar_svg, MPC_ASSET_DIR "/res/toolbar.svg")
MPC_INCBIN(mpc_res_logo, MPC_ASSET_DIR "/res/logo.3.png")
MPC_INCBIN(mpc_res_stereo, MPC_ASSET_DIR "/res/stereo.bmp")
MPC_INCBIN(mpc_res_mono, MPC_ASSET_DIR "/res/mono.bmp")
MPC_INCBIN(mpc_res_noaudio, MPC_ASSET_DIR "/res/noaudio.bmp")
MPC_INCBIN(mpc_res_icon16, MPC_ASSET_DIR "/res/icon-16.png")
MPC_INCBIN(mpc_res_icon24, MPC_ASSET_DIR "/res/icon-24.png")
MPC_INCBIN(mpc_res_icon32, MPC_ASSET_DIR "/res/icon-32.png")
MPC_INCBIN(mpc_res_icon48, MPC_ASSET_DIR "/res/icon-48.png")
MPC_INCBIN(mpc_res_icon256, MPC_ASSET_DIR "/res/icon-256.png")

namespace mpc
{
int g_dpi = 144;

namespace res
{
namespace
{
Blob B(const unsigned char* b, const unsigned char* e)
{
	return Blob{b, size_t(e - b)};
}

Image Load(const unsigned char* b, const unsigned char* e)
{
	Image img;
	DecodeImage(b, size_t(e - b), img);
	return img;
}

struct ToolbarCache
{
	int px = 0;
	Image normal, disabled;
};
ToolbarCache g_toolbar;

void MakeToolbar(int px)
{
	if (g_toolbar.px == px)
		return;
	g_toolbar.px = px;
	// SVGImage::Load: nsvgParse at 96 dpi, rasterized at scale
	std::string text(reinterpret_cast<const char*>(mpc_res_toolbar_svg_begin),
		size_t(mpc_res_toolbar_svg_end - mpc_res_toolbar_svg_begin));
	NSVGimage* svg = nsvgParse(&text[0], "px", 96.0f);
	Image img;
	if (svg)
	{
		const float scale = px / svg->height;
		const int w = int(std::lround(svg->width * scale)), h = int(std::lround(svg->height * scale));
		std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
		if (NSVGrasterizer* rast = nsvgCreateRasterizer())
		{
			nsvgRasterize(rast, svg, 0, 0, scale, rgba.data(), w, h, w * 4);
			nsvgDeleteRasterizer(rast);
		}
		nsvgDelete(svg);
		img.FromRGBA(rgba.data(), w, h);
	}
	g_toolbar.normal = img;
	g_toolbar.disabled = img.Grayed();
}
} // namespace

Blob FontSelawik()
{
	return B(mpc_font_selawik_begin, mpc_font_selawik_end);
}
Blob FontSelawikBold()
{
	return B(mpc_font_selawik_bold_begin, mpc_font_selawik_bold_end);
}
Blob FontRoboto()
{
	return B(mpc_font_roboto_begin, mpc_font_roboto_end);
}

const Image& Toolbar(int px)
{
	MakeToolbar(px);
	return g_toolbar.normal;
}

const Image& ToolbarDisabled(int px)
{
	MakeToolbar(px);
	return g_toolbar.disabled;
}

const Image& Logo()
{
	static Image img = Load(mpc_res_logo_begin, mpc_res_logo_end);
	return img;
}

const Image& AudioType(int which)
{
	static Image none = Load(mpc_res_noaudio_begin, mpc_res_noaudio_end);
	static Image mono = Load(mpc_res_mono_begin, mpc_res_mono_end);
	static Image stereo = Load(mpc_res_stereo_begin, mpc_res_stereo_end);
	return which == 2 ? stereo : which == 1 ? mono : none;
}

const Image& AppIcon(int px)
{
	static std::map<int, Image> scaled;
	static Image i16 = Load(mpc_res_icon16_begin, mpc_res_icon16_end);
	static Image i24 = Load(mpc_res_icon24_begin, mpc_res_icon24_end);
	static Image i32 = Load(mpc_res_icon32_begin, mpc_res_icon32_end);
	static Image i48 = Load(mpc_res_icon48_begin, mpc_res_icon48_end);
	static Image i256 = Load(mpc_res_icon256_begin, mpc_res_icon256_end);
	if (px == 16)
		return i16;
	if (px == 24)
		return i24;
	if (px == 32)
		return i32;
	if (px == 48)
		return i48;
	if (px == 256)
		return i256;
	Image& s = scaled[px];
	if (s.IsNull())
		s = (px < 48 ? i48 : i256).Scaled(px, px);
	return s;
}

bool DecodeImage(const uint8_t* data, size_t size, Image& out)
{
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load_from_memory(data, int(size), &w, &h, &n, 4);
	if (!px)
		return false;
	out.FromRGBA(px, w, h);
	stbi_image_free(px);
	return true;
}
} // namespace res
} // namespace mpc
