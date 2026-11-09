// Media Player Classic PS5 1.6: the video drawn by the GPU (mpc_gpu.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_gpu.h"

#include "OrbisPaths.h"
#include "ProsperoVideo.h"
#include "mpc_tonemap.h"
#include "mpc_vdec.h"
#include "ps5gpu/gpu.h"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <unistd.h>

namespace mpc
{
namespace gpu
{
namespace
{
uint64_t g_shown_id = 0;
bool g_shown = false;
const char* g_shown_kind = "none";
// (review L2, M-B) a second start guard, for pictures read in place: written before the run's first import of the
// decoder's buffers (the step most likely to bring the app down), kept until a Present after the first in-place picture
// was taken has come back. Still there at the next start: that start copies them instead.
bool g_inplace_armed = false;   // the marker written
bool g_inplace_checked = false; // the marker's whole life is over for this run
uint64_t g_inplace_taken_at = UINT64_MAX; // ps5video::GpuPresents() when the first in-place picture was taken
std::string InPlaceMarker()
{
	return OrbisRoot() + "/logs/gpu-inplace-starting";
}
void InPlaceDone()
{
	unlink(InPlaceMarker().c_str());
	g_inplace_armed = false;
	g_inplace_checked = true;
}
// before the import
void InPlaceArm()
{
	if (g_inplace_checked || g_inplace_armed)
		return;
	if (FILE* f = fopen(InPlaceMarker().c_str(), "w"))
		fclose(f);
	g_inplace_armed = true;
#ifndef __PROSPERO__
	// host tests: MPCPS5_HOST_DIE_AT_IMPORT=1: the app ends here, as a crash in the import would
	if (getenv("MPCPS5_HOST_DIE_AT_IMPORT") && *getenv("MPCPS5_HOST_DIE_AT_IMPORT") == '1')
	{
		OrbisLog("[gpu] (test) the app ends at the import");
		_exit(3);
	}
#endif
}
// each frame: an in-place picture taken (`taken`), or the GPU done with the frame that read it. (final review 2) The
// Present after taking it (taken_at + 1) submits the frame that reads the decoder's memory; only the Present after that
// one (taken_at + 2) has waited for its fence (on the console a frame's fence is waited at the next Present).
void InPlaceProgress(bool taken)
{
	if (g_inplace_checked || !g_inplace_armed)
		return;
	if (taken && g_inplace_taken_at == UINT64_MAX)
		g_inplace_taken_at = ps5video::GpuPresents();
	else if (g_inplace_taken_at != UINT64_MAX && ps5video::GpuPresents() >= g_inplace_taken_at + 2)
	{
		OrbisLog("[gpu] in-place start guard cleared: present %llu, the first in-place picture taken after present %llu",
			(unsigned long long)ps5video::GpuPresents(), (unsigned long long)g_inplace_taken_at);
		InPlaceDone(); // the frame that read it has finished on the GPU
	}
}
std::set<int> g_refused_formats; // logged once each

double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The stream's matrix as RenderVideo has always picked it: BT.709 for HD, BT.601 for SD when it doesn't say
bool Matrix(const AVFrame* f, ps5gpu::ColorMatrix& m)
{
	switch (f->colorspace)
	{
		case AVCOL_SPC_BT709: m = ps5gpu::ColorMatrix::BT709; return true;
		case AVCOL_SPC_BT470BG:
		case AVCOL_SPC_SMPTE170M: m = ps5gpu::ColorMatrix::BT601; return true;
		case AVCOL_SPC_BT2020_NCL:
		case AVCOL_SPC_BT2020_CL: m = ps5gpu::ColorMatrix::BT2020; return true;
		case AVCOL_SPC_FCC: m = ps5gpu::ColorMatrix::FCC; return true;
		case AVCOL_SPC_SMPTE240M: m = ps5gpu::ColorMatrix::SMPTE240M; return true;
		case AVCOL_SPC_UNSPECIFIED:
		case AVCOL_SPC_RESERVED:
		case AVCOL_SPC_RGB: m = f->height >= 720 ? ps5gpu::ColorMatrix::BT709 : ps5gpu::ColorMatrix::BT601; return true;
		default: return false; // YCgCo, ICtCp...: libswscale's way (the CPU)
	}
}

// The stream's chroma siting (review L6), as libswscale is told it in RenderVideo: unspecified is centred
void ChromaPos(const AVFrame* f, ps5gpu::VideoFrame& v)
{
	// unspecified: centred over the luma pixels the chroma sample covers (libswscale's default for every subsampling:
	// 1.5 pixels in from the first for 4:1:0, not the 0.5 of 4:2:0)
	v.chroma_pos_x = v.chroma_pos_y = 0.5;
	int x = 128, y = 128; // 1/256 of a luma pixel from the first one's centre
	if (f->chroma_location == AVCHROMA_LOC_UNSPECIFIED || av_chroma_location_enum_to_pos(&x, &y, f->chroma_location) < 0)
		return;
	const int sx = (1 << v.chroma_shift_x) - 1, sy = (1 << v.chroma_shift_y) - 1;
	v.chroma_pos_x = sx ? std::min(1.0, x / 256.0 / sx) : 0.5;
	v.chroma_pos_y = sy ? std::min(1.0, y / 256.0 / sy) : 0.5;
}

// An FFmpeg frame's planes as ps5-slang-gpu takes them; false: the CPU draws it
bool Describe(const AVFrame* f, ps5gpu::VideoFrame& v, std::string& why)
{
	const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
	if (!d)
	{
		why = "an unknown pixel format";
		return false;
	}
	if (d->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT |
					   AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_BE))
	{
		why = std::string(d->name) + ": paletted, bit-packed, float, Bayer or big-endian";
		return false;
	}
	v = ps5gpu::VideoFrame();
	v.width = f->width;
	v.height = f->height;
	const int depth = d->comp[0].depth;
	const int bytes = depth > 8 ? 2 : 1;
	v.bits = depth;
	v.chroma_shift_x = d->log2_chroma_w;
	v.chroma_shift_y = d->log2_chroma_h;
	for (int i = 0; i < d->nb_components; i++)
	{
		if (d->comp[i].depth != depth || (d->comp[i].shift != 0 && d->comp[i].shift + depth != 16))
		{
			why = std::string(d->name) + ": samples of different sizes or shifts";
			return false;
		}
	}
	v.msb = d->comp[0].shift != 0;
	auto plane = [&](int k, int p) { v.plane[k] = {f->data[p], f->linesize[p]}; };
	if (d->flags & AV_PIX_FMT_FLAG_RGB)
	{
		if (d->flags & AV_PIX_FMT_FLAG_PLANAR)
		{
			// gbrp: R in comp 0, G in comp 1, B in comp 2, each a plane of its own
			if (d->nb_components < 3 || d->comp[0].step != bytes)
			{
				why = std::string(d->name) + ": planar RGB laid out otherwise";
				return false;
			}
			v.layout = ps5gpu::VideoLayout::PlanarGBR;
			plane(0, d->comp[1].plane);
			plane(1, d->comp[2].plane);
			plane(2, d->comp[0].plane);
			v.chroma_shift_x = v.chroma_shift_y = 0;
			v.full_range = true;
		}
		else
		{
			// rgba, bgra, argb, abgr, rgb0, bgr0, 0rgb, 0bgr: four bytes a pixel
			if (depth != 8 || d->comp[0].step != 4 || d->nb_components < 3)
			{
				why = std::string(d->name) + ": packed RGB other than four bytes a pixel";
				return false;
			}
			v.layout = ps5gpu::VideoLayout::Packed;
			for (int i = 0; i < 3; i++)
				v.rgb_index[i] = d->comp[i].offset;
			plane(0, 0);
			v.chroma_shift_x = v.chroma_shift_y = 0;
			v.full_range = true;
		}
	}
	else
	{
		if (d->nb_components == 1 || (d->nb_components == 2 && (d->flags & AV_PIX_FMT_FLAG_ALPHA)))
		{
			if (d->comp[0].step != bytes)
			{
				why = std::string(d->name) + ": gray with alpha";
				return false;
			}
			v.layout = ps5gpu::VideoLayout::Gray;
			plane(0, d->comp[0].plane);
		}
		else if (d->comp[1].plane == d->comp[2].plane)
		{
			if (d->comp[0].step != bytes || d->comp[1].step != 2 * bytes)
			{
				why = std::string(d->name) + ": semi-planar laid out otherwise";
				return false;
			}
			v.layout = d->comp[1].offset < d->comp[2].offset ? ps5gpu::VideoLayout::SemiPlanarUV : ps5gpu::VideoLayout::SemiPlanarVU;
			plane(0, d->comp[0].plane);
			plane(1, d->comp[1].plane);
		}
		else
		{
			if (d->comp[0].step != bytes || d->comp[1].step != bytes || d->comp[2].step != bytes ||
				d->comp[0].plane == d->comp[1].plane)
			{
				why = std::string(d->name) + ": packed Y'CbCr (yuyv...)";
				return false;
			}
			v.layout = ps5gpu::VideoLayout::Planar;
			plane(0, d->comp[0].plane);
			plane(1, d->comp[1].plane);
			plane(2, d->comp[2].plane);
		}
		if (!Matrix(f, v.matrix))
		{
			why = std::string("the colour matrix ") + av_color_space_name(f->colorspace);
			return false;
		}
		// yuvj formats are full range by definition
		v.full_range = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
					   f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P ||
					   f->format == AV_PIX_FMT_YUVJ440P || f->format == AV_PIX_FMT_YUVJ411P;
	}
	const tonemap::Transfer tr = tonemap::TransferOf(f);
	v.transfer = tr == tonemap::Transfer::Pq ? ps5gpu::VideoTransfer::Pq
				 : tr == tonemap::Transfer::Hlg ? ps5gpu::VideoTransfer::Hlg
												: ps5gpu::VideoTransfer::Sdr;
	v.peak_nits = tonemap::SourcePeak(f);
	ChromaPos(f, v);
	if (!ps5gpu::VideoFormatSupported(v))
	{
		why = std::string(d->name) + ": " + std::to_string(f->width) + "x" + std::to_string(f->height) +
			  " isn't a picture the GPU path takes";
		return false;
	}
	return true;
}

// a frame reference the GPU holds until it has read the picture (an in-place picture: the decoder's buffer)
void FreeFrame(void* ctx)
{
	AVFrame* f = static_cast<AVFrame*>(ctx);
	av_frame_free(&f);
}

void ReleaseImport(void* import)
{
	const bool up = ps5gpu::VideoAvailable();
	ps5gpu::ReleaseHostMemory(static_cast<ps5gpu::HostMemory*>(import));
	OrbisLog("[gpu] the decoder's picture buffers let go (%s)",
		up ? "their import released" : "after the GPU went: their import only forgotten");
}

// A picture the console's decoder handed out in place (mpc_vdec.h): its planes where the decoder wrote them, read by the
// GPU there (the decoder's block imported once), or copied from there once when the import isn't possible.
bool DescribeInPlace(const AVFrame* f, const hwdec::ZeroCopyInfo* z, ps5gpu::VideoFrame& v, std::string& why)
{
	v = ps5gpu::VideoFrame();
	v.width = f->width;
	v.height = f->height;
	v.layout = ps5gpu::VideoLayout::SemiPlanarUV;
	v.bits = z->lsb10 ? 10 : 8;
	v.msb = false;
	v.plane[0] = {f->data[0], f->linesize[0]};
	v.plane[1] = {f->data[1], f->linesize[1]};
	if (!Matrix(f, v.matrix))
	{
		why = std::string("the colour matrix ") + av_color_space_name(f->colorspace);
		return false;
	}
	v.full_range = f->color_range == AVCOL_RANGE_JPEG;
	const tonemap::Transfer tr = tonemap::TransferOf(f);
	v.transfer = tr == tonemap::Transfer::Pq ? ps5gpu::VideoTransfer::Pq
				 : tr == tonemap::Transfer::Hlg ? ps5gpu::VideoTransfer::Hlg
												: ps5gpu::VideoTransfer::Sdr;
	v.peak_nits = tonemap::SourcePeak(f);
	ChromaPos(f, v);
	bool failed = false;
	void* import = hwdec::PoolImport(z, &failed);
	if (!import && !failed)
	{
		InPlaceArm(); // (review M-B) before the import
		import = ps5gpu::ImportHostMemory(z->base, z->bytes);
		hwdec::SetPoolImport(z, import, import == nullptr);
		if (!import)
		{
			OrbisLog("[gpu] the decoder's picture buffers (%zu KiB) couldn't be imported: each picture is copied once",
				z->bytes >> 10);
			if (g_inplace_taken_at == UINT64_MAX)
				InPlaceDone(); // the import came back (nothing in place to wait for)
		}
	}
	if (import)
	{
		AVFrame* ref = av_frame_clone(f); // the decoder's buffer stays ours until the GPU has read it
		if (!ref)
		{
			why = "no memory for a frame reference";
			return false;
		}
		v.memory = static_cast<ps5gpu::HostMemory*>(import);
		v.release = FreeFrame;
		v.release_ctx = ref;
	}
	if (!ps5gpu::VideoFormatSupported(v))
	{
		if (v.release)
			v.release(v.release_ctx);
		why = "the decoder's picture isn't one the GPU path takes";
		return false;
	}
	return true;
}

ps5gpu::VideoView View(const CRect& dst, const Options& o)
{
	ps5gpu::VideoView v;
	v.dst = {dst.left, dst.top, dst.Width(), dst.Height()};
	v.tone_map = ps5gpu::ToneMap(std::max(0, std::min(o.tone_map, 3)));
	v.white_nits = o.white_nits;
	v.filter = o.resizer == 1 ? ps5gpu::VideoFilter::Bicubic : ps5gpu::VideoFilter::Bilinear;
	return v;
}

// the statistics, averaged over half a second
std::mutex g_stats_mu;
double g_stats_t0 = 0;
std::string g_stats_text, g_stats_short;
} // namespace

bool Active()
{
	return ps5video::GpuActive() && ps5gpu::VideoAvailable();
}

double DisplayDelay()
{
#ifdef __PROSPERO__
	return Active() ? 1.0 / 59.94 : 0.0;
#else
	return 0.0;
#endif
}

void Start()
{
	bool in_place = Active() && ps5gpu::HostMemoryAlignment() > 0;
#ifndef __PROSPERO__
	if (getenv("MPCPS5_HOST_INPLACE") && *getenv("MPCPS5_HOST_INPLACE") == '0')
		in_place = false; // host tests: the GPU path with copies
#endif
	if (access(InPlaceMarker().c_str(), F_OK) == 0)
	{
		unlink(InPlaceMarker().c_str());
		if (in_place)
			OrbisLog("[gpu] the last start didn't get past its first picture read in place: this one copies them (the "
					 "next tries again)");
		in_place = false;
		g_inplace_checked = true;
	}
	hwdec::SetZeroCopy(in_place);
	// (review L3) the GPU given up while running: pictures copied again, the decoder's imports let go while the device
	// is still there
	ps5video::SetGpuLostHook([] {
		// (final review 2) while the device is still there: the pictures it holds let go, then every import, of the
		// closed films' blocks and of the open decoder's (its pictures copied from then on)
		hwdec::SetZeroCopy(false);
		ps5gpu::ReleaseVideo();
		g_shown = false;
		g_shown_kind = "none";
		hwdec::CollectPools(ReleaseImport);
		hwdec::DropImports(ReleaseImport);
	});
	OrbisLog("[gpu] video: %s", !Active() ? "drawn on the CPU"
						: in_place ? "drawn by the GPU; the console decoder's pictures read where it wrote them (no copy)"
								   : "drawn by the GPU; each picture copied once");
}

void Stop()
{
	// (review L-e) the pictures the GPU still holds let go (an in-place one keeps its decoder block alive), then the
	// closed films' decoder blocks and their imports, before the GPU shuts down
	if (Active())
		ps5gpu::ReleaseVideo();
	g_shown = false;
	g_shown_kind = "none";
	hwdec::CollectPools(ReleaseImport);
	// (final review 2) an orderly exit after the GPU finished with every picture: the in-place start guard's marker
	// goes, whether or not a picture read in place was shown (the import didn't bring the app down)
	if (g_inplace_armed)
		InPlaceDone();
}

bool Show(const AVFrame* f, uint64_t id, const CRect& dst, const Options& o, std::string* why)
{
	hwdec::CollectPools(ReleaseImport); // the decoder blocks no frame holds any more
	InPlaceProgress(false);
	if (!Active() || !f)
		return false;
	const ps5gpu::VideoView view = View(dst, o);
	if (g_shown && id == g_shown_id)
	{
		ps5gpu::SetVideoView(view);
		return true;
	}
	ps5gpu::VideoFrame v;
	std::string w;
	const hwdec::ZeroCopyInfo* z = hwdec::ZeroCopyOf(f);
	if (!(z ? DescribeInPlace(f, z, v, w) : Describe(f, v, w)) || !ps5gpu::SetVideo(v, view))
	{
		if (w.empty())
			w = "the GPU didn't take it (see the [gpu] lines)";
		if (g_refused_formats.insert(f->format).second)
			OrbisLog("[gpu] a %dx%d %s picture is drawn on the CPU: %s", f->width, f->height,
				av_get_pix_fmt_name(AVPixelFormat(f->format)) ? av_get_pix_fmt_name(AVPixelFormat(f->format)) : "?", w.c_str());
		if (why)
			*why = w;
		Hide();
		return false;
	}
	g_shown = true;
	g_shown_id = id;
	g_shown_kind = z && v.memory ? "in place" : "copied";
	if (z && v.memory)
		InPlaceProgress(true);
	// as the CPU path logs it: what an HDR film is tone mapped from and to (once per change)
	static std::string logged;
	if (v.transfer != ps5gpu::VideoTransfer::Sdr)
	{
		static const char* const kModes[4] = {"off", "BT.2390", "Hable", "Reinhard"};
		char b[192];
		if (ps5gpu::HdrOutputActive())
			snprintf(b, sizeof(b), "%s picture on the GPU: native HDR10/PQ 10-bit BT.2020 output, film peak %.0f nits",
				v.transfer == ps5gpu::VideoTransfer::Pq ? "HDR10 (PQ)" : "HLG", v.peak_nits);
		else
			snprintf(b, sizeof(b), "%s picture on the GPU: tone mapping %s, white %d nits, film peak %.0f nits",
				v.transfer == ps5gpu::VideoTransfer::Pq ? "HDR10 (PQ)" : "HLG", kModes[std::max(0, std::min(o.tone_map, 3))],
				o.white_nits, v.peak_nits);
		if (logged != b)
		{
			logged = b;
			OrbisLog("[gpu] %s", b);
		}
	}
	return true;
}

void Hide()
{
	hwdec::CollectPools(ReleaseImport);
	InPlaceProgress(false);
	if (g_shown)
		ps5gpu::HideVideo();
	g_shown = false;
	g_shown_kind = "none";
}

const char* ShownKind()
{
	return g_shown_kind;
}

std::string TakeStats()
{
	if (!Active())
		return "";
	std::lock_guard<std::mutex> l(g_stats_mu);
	const double now = Now();
	if (g_stats_t0 == 0 || now - g_stats_t0 >= 0.5)
	{
		const ps5gpu::VideoStats s = ps5gpu::TakeVideoStats();
		char b[192];
		if (s.frames == 0)
			snprintf(b, sizeof(b), "GPU: no new picture, draw %.1f ms, present %.1f ms", s.gpu_ms, s.present_ms);
		else if (s.in_place && !s.copied)
			snprintf(b, sizeof(b), "GPU: upload in place (no copy), draw %.1f ms, present %.1f ms", s.gpu_ms, s.present_ms);
		else
			snprintf(b, sizeof(b), "GPU: upload %.1f ms (one copy%s), draw %.1f ms, present %.1f ms", s.copy_ms,
				s.in_place ? ", some in place" : "", s.gpu_ms, s.present_ms);
		g_stats_text = b;
		if (ps5gpu::HdrOutputActive()) g_stats_text += "; HDR10 PQ 10-bit";
		if (s.frames == 0)
			snprintf(b, sizeof(b), "GPU %.1f ms", s.gpu_ms);
		else if (s.in_place && !s.copied)
			snprintf(b, sizeof(b), "GPU %.1f ms, in place", s.gpu_ms);
		else
			snprintf(b, sizeof(b), "GPU %.1f ms, copy %.1f ms", s.gpu_ms, s.copy_ms);
		g_stats_short = b;
		g_stats_t0 = now;
	}
	return g_stats_text;
}

std::string ShortStats()
{
	if (TakeStats().empty())
		return "";
	std::lock_guard<std::mutex> l(g_stats_mu);
	return g_stats_short;
}
} // namespace gpu
} // namespace mpc
