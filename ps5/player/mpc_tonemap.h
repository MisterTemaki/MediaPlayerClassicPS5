// Media Player Classic PS5: HDR10 (PQ) and HLG pictures on the SDR screen the player draws.
//
// The TV gets an SDR picture (the window, the menus, the video in it), so an HDR film is tone mapped on the way:
// libswscale scales the picture to the window and turns it into 16-bit R'G'B' (BT.2020, still PQ or HLG), then
// Convert() takes it to linear light (PQ's EOTF, or HLG's inverse OETF and its OOTF), from BT.2020 to BT.709
// primaries, compresses the highlights from the film's peak (its mastering metadata, 1000 nits when it has none)
// to the picture's white (Options > Output, 250 nits by default), and encodes it for the screen (gamma 2.4).
// The curves are BT.2390's EETF (the default), Hable's filmic one and Reinhard's; run on the scaled picture
// (1080p at most), in a few threads.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

extern "C" {
#include <libavutil/frame.h>
}

#include <cstdint>

namespace mpc
{
namespace tonemap
{
enum Mode
{
	Off = 0,
	Bt2390 = 1,
	Hable = 2,
	Reinhard = 3,
};

enum class Transfer
{
	Sdr,
	Pq,
	Hlg,
};

// What the picture is: PQ (HDR10, Dolby Vision's base layer), HLG, or SDR.
Transfer TransferOf(const AVFrame* f);
// The film's peak in nits: its content light level, else its mastering display's, else 1000.
double SourcePeak(const AVFrame* f);

struct Params
{
	Mode mode = Bt2390;
	Transfer transfer = Transfer::Pq;
	double src_peak = 1000; // nits
	double white = 250;     // nits shown as the screen's white
};

// src: R, G, B, A 16-bit (AV_PIX_FMT_RGBA64LE, full range), w x h, src_pitch in pixels.
// dst: RGBA 8-bit (AV_PIX_FMT_RGBA), dst_pitch in pixels.
void Convert(const uint16_t* src, int src_pitch, uint32_t* dst, int dst_pitch, int w, int h, const Params& p);
} // namespace tonemap
} // namespace mpc
