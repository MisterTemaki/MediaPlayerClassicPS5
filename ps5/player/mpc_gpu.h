// Media Player Classic PS5 1.6: the video drawn by the GPU.
//
// Up to 1.5.4 each picture went through the CPU on its way to the screen: the decoder's output copied into an FFmpeg
// frame, libswscale's bicubic scaling and conversion to RGBA (or 16-bit RGB for HDR, then mpc_tonemap.cpp's tone
// mapping), a copy into the window, the tile swizzle into the scan-out buffer. A 4K HDR film dropped pictures.
//
// Now the GPU does it (ps5-slang-gpu 1.1's SetVideo, Vulkan through RADV): the picture's planes as they come from the
// decoder (NV12, P010, yuv420p, yuv420p10le, 4:2:2, 4:4:4, gray, RGB...), scaled, converted with the stream's matrix
// and range, and tone mapped with the same numbers and the same Options > Output settings (BT.2390, Hable, Reinhard,
// off; the white), in one pass straight into the display's resolution. The window, the menus, subtitles and the OSD
// are drawn on the CPU as before into the surface, which is transparent over the video's rectangle; the GPU lays the
// surface over the picture. What the GPU can't take (a format it doesn't read, a failed set-up) is drawn on the CPU as
// before, and the log says why.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_gdi.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstdint>
#include <string>

namespace mpc
{
namespace gpu
{
// The GPU draws the pictures (ps5video::GpuActive and the video pipeline there).
bool Active();
// At the app's start: the console decoder hands its pictures out in place when the GPU draws them and can read them
// where they are (ps5gpu::HostMemoryAlignment); logged.
void Start();
// At the app's end, before the GPU shuts down: the decoder blocks the closed films left, let go.
void Stop();
// How much later than the scan-out buffers' direct flip a picture is on screen (Player::SetDisplayDelay): on the console
// the swapchain (FIFO, three images: one shown, one waiting for the vblank, one drawn) holds a picture one refresh more;
// 0 when the CPU draws, and on the host (its frames are read back at once).
double DisplayDelay();

struct Options
{
	int tone_map = 1;   // 0 off, 1 BT.2390, 2 Hable, 3 Reinhard (Options > Output)
	int white_nits = 250;
	int resizer = 0;    // 0 bilinear, 1 bicubic (Options > Output > Resizer)
};

// The picture `f` (the player's current one, `id` changing with each new one) in `dst` (surface pixels, the video's
// whole rectangle: it may reach past the screen). False: the GPU didn't take it (the CPU draws it this time; why says
// why, once per format). A picture already taken (the same id) is only moved.
bool Show(const AVFrame* f, uint64_t id, const CRect& dst, const Options& o, std::string* why);
// Nothing of the video on the GPU this frame (no picture, a menu over the whole screen, the CPU drew it).
void Hide();

// The GPU path's times, averaged over half a second: "GPU: upload 0.4 ms (one copy), draw 1.2 ms, present 0.3 ms"
// (View > Statistics, the log); "" when the GPU isn't drawing.
std::string TakeStats();
// The same, short, for the statistics over the video (R3 + Square): "GPU 1.2 ms, copy 0.4 ms" / "GPU 1.2 ms, in place"
std::string ShortStats();
// How the GPU got the picture it shows now: "in place", "copied" or "none" (the host tests' state line)
const char* ShownKind();
} // namespace gpu
} // namespace mpc
