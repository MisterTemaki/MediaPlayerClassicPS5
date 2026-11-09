// Media Player Classic PS5 1.6: the CPU's work per picture, 1.5.4's path against 1.6's, on the host.
//
//   mpc-gpu-bench <clip> [pictures] [out WxH]
//
// The clip's first pictures are decoded once (FFmpeg) and kept; then, for each, what the UI thread did with it:
//   1.5.4: RenderVideo's conversion: libswscale to the window (1920x1080 into a 1700x956 picture, bicubic, accurate
//          rounding, 6 threads; 16-bit RGB for HDR then mpc_tonemap.cpp's tone mapping on 6 threads), the window's
//          copy of it into the surface;
//   1.6:   ps5gpu::SetVideo (one copy into the staging ring) or, the planes in imported memory as the console's decoder
//          leaves them, nothing; then ps5gpu::Present (the surface's copy, recording, submitting).
// Times: CPU time (all the process's threads for 1.5.4's path, which runs on 7; the calling thread's for 1.6's, since
// the rest is the GPU's -- here lavapipe, whose own threads are the "GPU") and wall time. lavapipe's numbers are only
// relative: on the PS5 the drawing is the GPU's.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_tonemap.h"
#include "ps5gpu/gpu.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

void OrbisLog(const char* fmt, ...)
{
	if (!getenv("BENCH_LOG"))
		return;
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
	fputc('\n', stderr);
}
namespace crashlog
{
void* ArmThread() { return nullptr; }
void DisarmThread(void*) {}
} // namespace crashlog

namespace
{
double Clock(clockid_t c)
{
	timespec t;
	clock_gettime(c, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

struct Times
{
	double cpu = 0, wall = 0;
	int n = 0;
};

void Present(const std::vector<uint32_t>& ui)
{
	ps5gpu::Present(ui.data(), {0, 0, 1920, 1080}, false);
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: mpc-gpu-bench <clip> [pictures] [out WxH]\n");
		return 2;
	}
	const int want = argc > 2 ? atoi(argv[2]) : 20;
	int ow = 1920, oh = 1080;
	if (argc > 3)
		sscanf(argv[3], "%dx%d", &ow, &oh);
	AVFormatContext* ic = nullptr;
	if (avformat_open_input(&ic, argv[1], nullptr, nullptr) < 0 || avformat_find_stream_info(ic, nullptr) < 0)
		return 1;
	const int vs = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	const AVCodec* dec = avcodec_find_decoder(ic->streams[vs]->codecpar->codec_id);
	AVCodecContext* c = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(c, ic->streams[vs]->codecpar);
	c->thread_count = 2;
	avcodec_open2(c, dec, nullptr);
	std::vector<AVFrame*> pics;
	AVPacket* pkt = av_packet_alloc();
	while (int(pics.size()) < want && av_read_frame(ic, pkt) >= 0)
	{
		if (pkt->stream_index == vs && avcodec_send_packet(c, pkt) >= 0)
		{
			AVFrame* f = av_frame_alloc();
			while (avcodec_receive_frame(c, f) == 0 && int(pics.size()) < want)
			{
				pics.push_back(f);
				f = av_frame_alloc();
			}
			av_frame_free(&f);
		}
		av_packet_unref(pkt);
	}
	if (pics.empty())
		return 1;
	const AVFrame* f0 = pics[0];
	const mpc::tonemap::Transfer tr = mpc::tonemap::TransferOf(f0);
	const bool hdr = tr != mpc::tonemap::Transfer::Sdr;
	printf("%s: %d pictures %dx%d %s%s\n", argv[1], int(pics.size()), f0->width, f0->height,
		av_get_pix_fmt_name(AVPixelFormat(f0->format)), hdr ? ", HDR (BT.2390, white 250 nits)" : "");

	// ---- 1.5.4: RenderVideo's work, into the window's picture (the 16:9 film in the default window: 1700x956)
	const int w = 1700, h = 956;
	SwsContext* sws = sws_alloc_context();
	av_opt_set_int(sws, "srcw", f0->width, 0);
	av_opt_set_int(sws, "srch", f0->height, 0);
	av_opt_set_int(sws, "src_format", f0->format, 0);
	av_opt_set_int(sws, "dstw", w, 0);
	av_opt_set_int(sws, "dsth", h, 0);
	av_opt_set_int(sws, "dst_format", hdr ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA, 0);
	av_opt_set_int(sws, "sws_flags", SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, 0);
	av_opt_set_int(sws, "threads", 6, 0);
	if (sws_init_context(sws, nullptr, nullptr) < 0)
		return 1;
	std::vector<uint16_t> rgb16(size_t(w) * h * 4);
	std::vector<uint32_t> layer(size_t(w) * h), surface(size_t(1920) * 1080);
	Times old;
	for (int pass = 0; pass < 2; pass++) // the first pass warms up (threads, tables)
		for (AVFrame* f : pics)
		{
			const double c0 = Clock(CLOCK_PROCESS_CPUTIME_ID), w0 = Clock(CLOCK_MONOTONIC);
			AVFrame* out = av_frame_alloc();
			out->width = w;
			out->height = h;
			out->format = hdr ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
			out->data[0] = hdr ? reinterpret_cast<uint8_t*>(rgb16.data()) : reinterpret_cast<uint8_t*>(layer.data());
			out->linesize[0] = w * (hdr ? 8 : 4);
			out->buf[0] = av_buffer_create(out->data[0], size_t(out->linesize[0]) * h, [](void*, uint8_t*) {}, nullptr, 0);
			sws_scale_frame(sws, out, f);
			av_frame_free(&out);
			if (hdr)
			{
				mpc::tonemap::Params p;
				p.transfer = tr;
				p.src_peak = mpc::tonemap::SourcePeak(f);
				mpc::tonemap::Convert(rgb16.data(), w, layer.data(), w, w, h, p);
			}
			for (int y = 0; y < h; y++) // the window's BitBlt into the surface
				memcpy(&surface[size_t(y + 62) * 1920 + 110], &layer[size_t(y) * w], size_t(w) * 4);
			if (pass)
			{
				old.cpu += Clock(CLOCK_PROCESS_CPUTIME_ID) - c0;
				old.wall += Clock(CLOCK_MONOTONIC) - w0;
				old.n++;
			}
		}
	mpc::tonemap::Shutdown();

	// ---- 1.6: SetVideo + Present (lavapipe off screen at ow x oh)
	ps5gpu::Config cfg;
	cfg.video = true;
	cfg.shipped_dir = "";
	if (getenv("BENCH_LOG"))
		cfg.log = [](const char* l) { fprintf(stderr, "%s\n", l); };
	ps5gpu::Configure(cfg);
	if (!ps5gpu::Init(1920, 1080, true, ow, oh) || !ps5gpu::VideoAvailable())
	{
		printf("no GPU video path on this host\n");
		return 1;
	}
	std::fill(surface.begin(), surface.end(), 0xff000000u);
	for (int y = 62; y < 62 + h; y++)
		std::fill(&surface[size_t(y) * 1920 + 110], &surface[size_t(y) * 1920 + 110 + w], 0u); // transparent over the video
	ps5gpu::VideoView view;
	view.dst = {110, 62, w, h};
	// the planes as an FFmpeg frame gives them (copied once), and as the console's decoder leaves them (in place: the
	// planes copied beforehand into an imported, aligned block, as the decoder's buffers are)
	auto describe = [&](const AVFrame* f, ps5gpu::VideoFrame& v) {
		const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
		v = ps5gpu::VideoFrame();
		v.width = f->width;
		v.height = f->height;
		v.bits = d->comp[0].depth;
		v.chroma_shift_x = d->log2_chroma_w;
		v.chroma_shift_y = d->log2_chroma_h;
		v.layout = d->comp[1].plane == d->comp[2].plane ? ps5gpu::VideoLayout::SemiPlanarUV : ps5gpu::VideoLayout::Planar;
		for (int i = 0; i < 3; i++)
			v.plane[i] = {f->data[i], f->linesize[i]};
		v.matrix = hdr ? ps5gpu::ColorMatrix::BT2020 : ps5gpu::ColorMatrix::BT709;
		v.transfer = tr == mpc::tonemap::Transfer::Pq ? ps5gpu::VideoTransfer::Pq
					 : tr == mpc::tonemap::Transfer::Hlg ? ps5gpu::VideoTransfer::Hlg
														: ps5gpu::VideoTransfer::Sdr;
		v.peak_nits = mpc::tonemap::SourcePeak(f);
	};
	Times copied, in_place;
	auto run = [&](Times& t, bool place) {
		// in place: a block laid out as the console decoder's (one picture: luma, then interleaved chroma, 16-bit
		// words for 10-bit), imported once
		const size_t align = ps5gpu::HostMemoryAlignment();
		std::vector<uint8_t*> blocks;
		ps5gpu::HostMemory* hm = nullptr;
		size_t pitch = 0, bytes = 0;
		const int bps = hdr ? 2 : 1;
		if (place)
		{
			if (!align)
				return false;
			pitch = (size_t(f0->width) * bps + 255) & ~size_t(255);
			const size_t one = pitch * size_t(f0->height) * 3 / 2;
			bytes = (one * pics.size() + align - 1) / align * align;
			uint8_t* block = static_cast<uint8_t*>(aligned_alloc(align, bytes));
			blocks.push_back(block);
			for (size_t k = 0; k < pics.size(); k++)
			{
				const AVFrame* f = pics[k];
				uint8_t* p = block + k * one;
				for (int y = 0; y < f->height; y++)
					memcpy(p + size_t(y) * pitch, f->data[0] + size_t(y) * f->linesize[0], size_t(f->width) * bps);
				uint8_t* uv = p + pitch * f->height;
				for (int y = 0; y < f->height / 2; y++)
					for (int x = 0; x < f->width / 2; x++)
						for (int k2 = 0; k2 < bps; k2++)
						{
							uv[size_t(y) * pitch + x * 2 * bps + k2] = f->data[1][size_t(y) * f->linesize[1] + x * bps + k2];
							uv[size_t(y) * pitch + x * 2 * bps + bps + k2] = f->data[2][size_t(y) * f->linesize[2] + x * bps + k2];
						}
			}
			hm = ps5gpu::ImportHostMemory(block, bytes);
			if (!hm)
				return false;
		}
		for (int pass = 0; pass < 2; pass++)
			for (size_t k = 0; k < pics.size(); k++)
			{
				ps5gpu::VideoFrame v;
				describe(pics[k], v);
				if (place)
				{
					const size_t one = pitch * size_t(f0->height) * 3 / 2;
					v.layout = ps5gpu::VideoLayout::SemiPlanarUV;
					v.plane[0] = {blocks[0] + k * one, int(pitch)};
					v.plane[1] = {blocks[0] + k * one + pitch * f0->height, int(pitch)};
					v.memory = hm;
				}
				ps5gpu::TakeVideoStats();
				const double c0 = Clock(CLOCK_THREAD_CPUTIME_ID), w0 = Clock(CLOCK_MONOTONIC);
				ps5gpu::SetVideo(v, view);
				Present(surface);
				const double c1 = Clock(CLOCK_THREAD_CPUTIME_ID), w1 = Clock(CLOCK_MONOTONIC);
				const ps5gpu::VideoStats s = ps5gpu::TakeVideoStats();
				if (pass)
				{
					// the calling thread's CPU time; the wall time without the waits for the GPU (lavapipe drawing)
					t.cpu += c1 - c0;
					t.wall += (w1 - w0) - s.wait_ms;
					t.n++;
				}
			}
		Present(surface); // the last picture read, released
		if (hm)
			ps5gpu::ReleaseHostMemory(hm);
		for (uint8_t* b : blocks)
			free(b);
		return true;
	};
	run(copied, false);
	const bool placed = run(in_place, true);
	const ps5gpu::VideoStats gs = ps5gpu::TakeVideoStats();
	(void)gs;
	printf("  1.5.4 (libswscale%s, the window's copy):  CPU %7.2f ms per picture (all threads), %7.2f ms wall\n",
		hdr ? " + tone mapping" : "", old.cpu / old.n, old.wall / old.n);
	printf("  1.6, one copy into the staging ring + Present:  CPU %7.2f ms per picture (the calling thread), %7.2f ms wall without GPU waits\n",
		copied.cpu / copied.n, copied.wall / copied.n);
	if (placed)
		printf("  1.6, in place (the decoder's buffers imported) + Present: CPU %7.2f ms per picture, %7.2f ms wall without GPU waits\n",
			in_place.cpu / in_place.n, in_place.wall / in_place.n);
	ps5gpu::Shutdown();
	for (AVFrame* f : pics)
		av_frame_free(&f);
	avcodec_free_context(&c);
	avformat_close_input(&ic);
	return 0;
}
