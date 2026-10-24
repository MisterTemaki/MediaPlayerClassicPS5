// libFuzzer harness for the fonts folder (player/mpc_sfnt.cpp + stb_truetype as mpc_font.cpp uses them): a file the
// check lets through must be readable by stb_truetype without reading outside the buffer -- each face initialised,
// a few characters looked up, measured, kerned, rasterized. Host only, not part of the app:
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -Iplayer -isystem third_party/stb \
//     tests/fuzz/fuzz_font.cpp player/mpc_sfnt.cpp -o fuzz_font
//   ./fuzz_font -max_total_time=300 -max_len=2000000 corpus/   (corpus: a few .ttf/.otf/.ttc files)
//
// SPDX-License-Identifier: MIT
#include "mpc_sfnt.h"

// stb_truetype's asserts abort on a damaged font (a CFF index, an unknown cmap format): the user's fonts are checked
// (mpc_sfnt.h) and read through bounded buffers, so they are off, as in a release build (tests/fuzz/fuzz_font.cpp
// runs this configuration under ASan)
#define STBTT_assert(x) ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	// as the player holds a font: the file, then sfnt::kPad zeros
	std::vector<uint8_t> buf(size + mpc::sfnt::kPad, 0);
	if (size)
		memcpy(buf.data(), data, size);
	const int faces = mpc::sfnt::CountFaces(buf.data(), size, nullptr);
	for (int i = 0; i < faces; i++)
	{
		uint32_t off = 0;
		if (!mpc::sfnt::CheckFace(buf.data(), size, i, &off, nullptr))
			continue;
		stbtt_fontinfo info;
		if (!stbtt_InitFont(&info, buf.data(), int(off)))
			continue;
		if (!mpc::sfnt::GposKerningFits(buf.data(), size, off))
			info.gpos = 0; // as mpc_font.cpp does
		const float scale = stbtt_ScaleForMappingEmToPixels(&info, 24);
		int prev = 0;
		for (int cp : {0x41, 0x67, 0x57, 0xe9, 0x416, 0x65e5, 0x2603, 0x1f600})
		{
			const int g = stbtt_FindGlyphIndex(&info, cp);
			int adv, lsb, x0, y0, x1, y1;
			stbtt_GetGlyphHMetrics(&info, g, &adv, &lsb);
			if (prev)
				stbtt_GetGlyphKernAdvance(&info, prev, g);
			prev = g;
			stbtt_GetGlyphBitmapBox(&info, g, scale, scale, &x0, &y0, &x1, &y1);
			const int w = x1 - x0, h = y1 - y0;
			if (w > 0 && h > 0 && w < 512 && h < 512)
			{
				std::vector<uint8_t> bits(size_t(w) * h);
				stbtt_MakeGlyphBitmap(&info, bits.data(), w, h, w, scale, scale, g);
			}
		}
	}
	return 0;
}
