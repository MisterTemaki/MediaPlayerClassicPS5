// SPDX-License-Identifier: MIT
// Exercise the real Vulkan HDR target, UI shader, raw 10-bit copy and recovery on lavapipe.
#include "ps5gpu/gpu.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0, on_calls = 0, off_calls = 0;
static bool refuse_hdr = false;
static int output_range = 2;
static std::vector<uint8_t> pixels;
static void check(bool ok, const char* name) { printf("%s %s\n", ok ? "ok  " : "FAIL", name); failures += !ok; }
static double pq(double nits)
{
	const double p = pow(nits / 10000, 0.1593017578125);
	return pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375);
}
static bool near(int x, int y, double value)
{
	if (pixels.size() != 16 * 16 * 4) return false;
	for (int c = 0; c < 3; c++)
		if (std::abs(int(pixels[(y * 16 + x) * 4 + c]) - int(std::lround(value * 255))) > 2) return false;
	return true;
}
int main()
{
	ps5gpu::Config config;
	config.video = true;
	config.hdr_output = true;
	config.host_import = false;
	config.sync_build = true;
	config.log = [](const char* s) { fprintf(stderr, "%s\n", s); };
	config.host_present = [](const uint8_t* p, int w, int h, bool) { pixels.assign(p, p + w * h * 4); };
	config.set_hdr_output = [](bool on, int w, int h) {
		if (w != 16 || h != 16) return -1;
		if (on) { ++on_calls; return refuse_hdr ? -1 : 0; }
		++off_calls; return 0;
	};
	config.hdr_output_range = [] { return output_range; };
	ps5gpu::Configure(config);
	if (!ps5gpu::Init(16, 16, true, 16, 16)) return 2;
	check(ps5gpu::VideoAvailable(), "video pipeline self-test passes before HDR is enabled");
	std::vector<uint32_t> ui(256, 0);
	ui[0] = 0xffffffff; // opaque SDR white, the rest transparent
	std::vector<uint16_t> plane(256, uint16_t(std::lround(pq(1000) * 1023)));
	ps5gpu::VideoFrame frame;
	frame.width = frame.height = 16;
	frame.bits = 10;
	frame.layout = ps5gpu::VideoLayout::PlanarGBR;
	frame.full_range = true;
	frame.matrix = ps5gpu::ColorMatrix::BT2020;
	frame.transfer = ps5gpu::VideoTransfer::Pq;
	frame.peak_nits = 1000;
	for (auto& p : frame.plane) p = {plane.data(), 32};
	ps5gpu::VideoView view;
	view.dst = {0, 0, 16, 16};
	view.tone_map = ps5gpu::ToneMap::Bt2390;
	check(ps5gpu::SetVideo(frame, view) && ps5gpu::Present(ui.data(), {}, false) && ps5gpu::HdrOutputActive(),
		"PQ picture activates native HDR10 rendering after output accepts it");
	check(near(8, 8, pq(1000)), "1000-nit PQ picture is preserved without SDR tone mapping");
	check(near(0, 0, pq(203)), "SDR UI white is converted to 203-nit HDR graphics white");
	for (double nits : {100.0, 4000.0})
	{
		std::fill(plane.begin(), plane.end(), uint16_t(std::lround(pq(nits) * 1023)));
		check(ps5gpu::SetVideo(frame, view) && ps5gpu::Present(ui.data(), {}, false) && near(8, 8, pq(nits)),
			"PQ luminance below and above SDR white passes without clipping to SDR");
	}
	check(on_calls == 1, "unchanged output mode is requested only once");
	frame.transfer = ps5gpu::VideoTransfer::Hlg;
	std::fill(plane.begin(), plane.end(), 767);
	const double e = 767.0 / 1023;
	const double scene = (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12;
	check(ps5gpu::SetVideo(frame, view) && ps5gpu::Present(ui.data(), {}, false) && near(8, 8, pq(1000 * pow(scene, 1.2))),
		"HLG scene light is converted to absolute light and PQ output");
	ps5gpu::HideVideo();
	check(ps5gpu::Present(ui.data(), {}, false) && !ps5gpu::HdrOutputActive() && near(0, 0, 1),
		"closing HDR video restores SDR output and the original UI white");
	frame.transfer = ps5gpu::VideoTransfer::Pq;
	std::fill(plane.begin(), plane.end(), uint16_t(std::lround(pq(1000) * 1023)));
	refuse_hdr = true;
	check(ps5gpu::SetVideo(frame, view) && ps5gpu::Present(ui.data(), {}, false) && !ps5gpu::HdrOutputActive(),
		"display refusing HDR keeps SDR tone mapping");
	const int refused_calls = on_calls;
	for (int i = 0; i < 3; i++) ps5gpu::Present(ui.data(), {}, false);
	check(on_calls == refused_calls, "refused HDR is not requested again every frame");
	ps5gpu::HideVideo();
	ps5gpu::Present(ui.data(), {}, false);
	refuse_hdr = false;
	output_range = 1;
	ps5gpu::SetVideo(frame, view);
	for (int i = 0; i < 62; i++) ps5gpu::Present(ui.data(), {}, false);
	check(!ps5gpu::HdrOutputActive(), "output that accepts the request but stays SDR is restored to SDR rendering");
	ps5gpu::HideVideo();
	ps5gpu::Present(ui.data(), {}, false);
	output_range = 2;
	ps5gpu::SetVideo(frame, view);
	ps5gpu::Present(ui.data(), {}, false);
	check(ps5gpu::HdrOutputActive(), "a later compatible HDR session can enable output again");
	const int before_shutdown = off_calls;
	ps5gpu::Shutdown();
	check(off_calls == before_shutdown + 1, "shutdown restores SDR before releasing VideoOut resources");
	config.hdr_output = false;
	ps5gpu::Configure(config);
	const int before_disabled = on_calls;
	if (!ps5gpu::Init(16, 16, true, 16, 16)) return 2;
	check(ps5gpu::SetVideo(frame, view) && ps5gpu::Present(ui.data(), {}, false) &&
		!ps5gpu::HdrOutputActive() && on_calls == before_disabled, "HDR output disabled by preference retains SDR");
	ps5gpu::Shutdown();
	return failures ? 1 : 0;
}
