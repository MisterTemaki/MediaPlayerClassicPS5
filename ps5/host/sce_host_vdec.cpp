// Media Player Classic PS5 host build: a stand-in for the console's video decoder (libSceVideodec2) and
// libSceSysmodule, so player/mpc_vdec.cpp runs in the tests. FFmpeg decodes the access units it is given (Annex B, as
// the console's decoder takes them) and the pictures come out the way the console's do: in the frame buffers the
// player hands in, NV12 or 16-bit samples with the 10 bits at the bottom, a 256-byte pitch, the height padded to 16.
//
//   MPCPS5_HOST_HWDEC=1          the library loads (without it sceSysmoduleLoadModule fails: FFmpeg decodes)
//   MPCPS5_HOST_HWDEC_FAIL=n     from the n-th access unit on every decode fails with a stream error (the player
//                                resets it a few times, then hands the video back to FFmpeg)
//   MPCPS5_HOST_HWDEC_REFUSE=1   creating a decoder fails (FFmpeg from the start)
//   MPCPS5_HOST_HWDEC_ODD=pitch  from the 10th picture on, the output says a pitch in bytes that isn't 1 or 2 bytes
//                                a sample ("pitch": 3x the samples); =size: a buffer size too small for the
//                                picture; =format: another frame_format number; =depth: 16-bit samples for an
//                                8-bit stream (the player must not read any of these, and hands the video to FFmpeg)
//   MPCPS5_HOST_HWDEC_HOLD=n     1.6: the decoder keeps n picture buffers to itself, as the console's does for its
//                                pipeline and reordering: each buffer handed in is accepted, and a picture comes out in
//                                the oldest one only once more than n are held (in the one handed in, when n is 0)
//
// SPDX-License-Identifier: MIT

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

extern "C" {
struct HostVdecConfig
{
	uint64_t this_size;
	uint32_t resource_type, codec_type, profile, max_level;
	int32_t max_frame_width, max_frame_height, max_dpb_frame_count;
	uint32_t decode_pipeline_depth;
	void* compute_queue;
	uint64_t cpu_affinity_mask;
	int32_t cpu_thread_priority;
	bool optimize_progressive_video, check_memory_type;
	uint8_t reserved0, reserved1;
	void* extra_config_info;
};
struct HostVdecMemory
{
	uint64_t this_size, cpu_memory_size;
	void* cpu_memory;
	uint64_t gpu_memory_size;
	void* gpu_memory;
	uint64_t cpu_gpu_memory_size;
	void* cpu_gpu_memory;
	uint64_t max_frame_buffer_size;
	uint32_t frame_buffer_alignment, reserved0;
};
struct HostVdecInput
{
	uint64_t this_size;
	void* au_data;
	uint64_t au_size, pts_data, dts_data, attached_data;
};
struct HostVdecOutput
{
	uint64_t this_size;
	bool is_valid, is_error_frame;
	uint8_t picture_count;
	uint32_t codec_type, frame_width, frame_pitch, frame_height;
	void* frame_buffer;
	uint64_t frame_buffer_size;
	uint32_t frame_format, frame_pitch_in_bytes;
};
struct HostVdecFrameBuffer
{
	uint64_t this_size;
	void* frame_buffer;
	uint64_t frame_buffer_size;
	bool is_accepted;
};
struct HostVdecComputeMemory
{
	uint64_t this_size, cpu_gpu_memory_size;
	void* cpu_gpu_memory;
};
static_assert(sizeof(HostVdecConfig) == 0x48, "decoder config");
static_assert(sizeof(HostVdecOutput) == 0x38, "output");
}

namespace
{
struct HostDecoder
{
	AVCodecContext* c = nullptr;
	uint32_t codec = 0;
	int max_w = 0, max_h = 0;
	std::deque<AVFrame*> ready;
	std::deque<std::pair<void*, uint64_t>> held; // MPCPS5_HOST_HWDEC_HOLD: buffers accepted, not given back yet
	bool draining = false;
	unsigned aus = 0;
	uint32_t profile = 0;
	AVFrame* last = nullptr; // VP9: the last picture, given again for a block that shows no frame (as the console does)
};

int Pitch(int w)
{
	return (w * 2 + 255) & ~255; // room for 16-bit samples
}

int PadH(int h)
{
	return (h + 15) & ~15;
}

void Pull(HostDecoder* d)
{
	for (;;)
	{
		AVFrame* f = av_frame_alloc();
		if (avcodec_receive_frame(d->c, f) < 0)
		{
			av_frame_free(&f);
			return;
		}
		d->ready.push_back(f);
	}
}

// the oldest picture into fb, in the console decoder's layout
bool Give(HostDecoder* d, HostVdecFrameBuffer* fb, HostVdecOutput* out)
{
	if (d->ready.empty() || !fb || !fb->frame_buffer)
		return false;
	AVFrame* f = d->ready.front();
	d->ready.pop_front();
	const bool ten = f->format == AV_PIX_FMT_YUV420P10LE;
	const int w = f->width, h = f->height, ph = PadH(h);
	const int pitch_bytes = ten ? Pitch(w) : (w + 255) & ~255;
	uint8_t* base = static_cast<uint8_t*>(fb->frame_buffer);
	if (size_t(pitch_bytes) * ph * 3 / 2 > fb->frame_buffer_size)
	{
		av_frame_free(&f);
		return false;
	}
	memset(base, 0, size_t(pitch_bytes) * ph * 3 / 2);
	const int bpp = ten ? 2 : 1;
	for (int y = 0; y < h; y++)
		memcpy(base + size_t(y) * pitch_bytes, f->data[0] + size_t(y) * f->linesize[0], size_t(w) * bpp);
	uint8_t* chroma = base + size_t(pitch_bytes) * ph;
	for (int y = 0; y < (h + 1) / 2; y++)
	{
		const uint8_t* u = f->data[1] + size_t(y) * f->linesize[1];
		const uint8_t* v = f->data[2] + size_t(y) * f->linesize[2];
		uint8_t* dst = chroma + size_t(y) * pitch_bytes;
		for (int x = 0; x < (w + 1) / 2; x++)
		{
			memcpy(dst + (2 * x) * bpp, u + x * bpp, bpp);
			memcpy(dst + (2 * x + 1) * bpp, v + x * bpp, bpp);
		}
	}
	out->is_valid = true;
	out->is_error_frame = false;
	out->picture_count = 1;
	out->codec_type = d->codec;
	out->frame_width = uint32_t(w);
	out->frame_height = uint32_t(ph);
	out->frame_pitch = uint32_t(pitch_bytes / bpp);
	out->frame_pitch_in_bytes = uint32_t(pitch_bytes);
	out->frame_buffer = fb->frame_buffer;
	out->frame_buffer_size = fb->frame_buffer_size;
	out->frame_format = ten ? 2 : 1;
	static int given = 0;
	if (const char* odd = getenv("MPCPS5_HOST_HWDEC_ODD"))
		if (++given >= 10)
		{
			if (strcmp(odd, "pitch") == 0)
				out->frame_pitch_in_bytes = out->frame_pitch * 3;
			else if (strcmp(odd, "format") == 0)
				out->frame_format += 7; // the same layout under another format number
			else if (strcmp(odd, "depth") == 0 && !ten)
			{
				// an 8-bit stream's picture said to be 16-bit samples (half the samples per row)
				out->frame_pitch = uint32_t(pitch_bytes / 2);
				out->frame_width = std::min(out->frame_width, out->frame_pitch);
			}
			else
				out->frame_buffer_size = uint64_t(pitch_bytes) * ph / 2;
		}
	fb->is_accepted = true;
	av_frame_free(&f);
	return true;
}
} // namespace

extern "C" {
int sceSysmoduleLoadModule(uint16_t id)
{
	return id == 207 && getenv("MPCPS5_HOST_HWDEC") ? 0 : (int)0x80020063;
}

int sceKernelMapFlexibleMemory(void** addr, size_t len, int, int)
{
	*addr = aligned_alloc(0x4000, len);
	return *addr ? 0 : -1;
}
int sceKernelReleaseFlexibleMemory(void* addr, size_t)
{
	free(addr);
	return 0;
}
int sceKernelMunmap(void*, size_t)
{
	return 0; // the host's direct memory is freed by sceKernelReleaseDirectMemory
}

int sceVideodec2QueryComputeMemoryInfo(HostVdecComputeMemory* info)
{
	info->cpu_gpu_memory_size = 1 << 20;
	return 0;
}
int sceVideodec2AllocateComputeQueue(const void*, const HostVdecComputeMemory* memory, void** queue)
{
	*queue = memory->cpu_gpu_memory;
	return 0;
}
int sceVideodec2ReleaseComputeQueue(void*)
{
	return 0;
}
int sceVideodec2QueryDecoderMemoryInfo(const HostVdecConfig* config, HostVdecMemory* memory)
{
	// (test12) MPCPS5_HOST_HWDEC_SMALL_FAILS=1: a picture size under 1920x1088 fails the query with 0x811d0200, as the
	// console's does (test11's PS5 log: a 720x1280 phone video)
	if (getenv("MPCPS5_HOST_HWDEC_SMALL_FAILS") && (config->max_frame_width < 1920 || config->max_frame_height < 1088))
		return (int)0x811d0200;
	memory->cpu_memory_size = 1 << 20;
	memory->gpu_memory_size = 1 << 20;
	memory->cpu_gpu_memory_size = 1 << 20;
	memory->max_frame_buffer_size = uint64_t(Pitch(config->max_frame_width)) * PadH(config->max_frame_height) * 3 / 2;
	memory->frame_buffer_alignment = 0x10000;
	return 0;
}
int sceVideodec2CreateDecoder(const HostVdecConfig* config, const HostVdecMemory*, void** decoder)
{
	if (getenv("MPCPS5_HOST_HWDEC_REFUSE"))
		return (int)0x811D0101;
	// 1: H.264, 974921: HEVC, 2382845: VP9 (profile 0 or 2: 8 / 10-bit)
	if (config->codec_type == 2382845 && getenv("MPCPS5_HOST_HWDEC_NO_VP9"))
		return (int)0x811D0101;
	const AVCodec* codec = avcodec_find_decoder(config->codec_type == 1 ? AV_CODEC_ID_H264
												: config->codec_type == 2382845 ? AV_CODEC_ID_VP9
																				: AV_CODEC_ID_HEVC);
	if (!codec)
		return -1;
	HostDecoder* d = new HostDecoder;
	d->c = avcodec_alloc_context3(codec);
	d->c->thread_count = config->codec_type == 2382845 ? 1 : 2; // (VP9: a picture out of each block at once)
	d->codec = config->codec_type;
	d->profile = config->profile;
	d->max_w = config->max_frame_width;
	d->max_h = config->max_frame_height;
	if (avcodec_open2(d->c, codec, nullptr) < 0)
	{
		avcodec_free_context(&d->c);
		delete d;
		return -1;
	}
	*decoder = d;
	return 0;
}
int sceVideodec2DeleteDecoder(void* decoder)
{
	HostDecoder* d = static_cast<HostDecoder*>(decoder);
	for (AVFrame* f : d->ready)
		av_frame_free(&f);
	av_frame_free(&d->last);
	avcodec_free_context(&d->c);
	delete d;
	return 0;
}
int sceVideodec2Decode(void* decoder, const HostVdecInput* in, HostVdecFrameBuffer* fb, HostVdecOutput* out)
{
	HostDecoder* d = static_cast<HostDecoder*>(decoder);
	d->aus++;
	if (const char* f = getenv("MPCPS5_HOST_HWDEC_FAIL"))
		if (d->aus >= unsigned(atoi(f)))
			return (int)0x811D0301; // a stream error
	const uint8_t* au = static_cast<const uint8_t*>(in->au_data);
	if (d->codec == 2382845 && in->au_size && (au[0] >> 6) == 2)
	{
		// a VP9 frame of another profile than the decoder was made for: refused, as the console refuses them
		const uint32_t prof = ((au[0] >> 5) & 1) | (((au[0] >> 4) & 1) << 1);
		if (prof != d->profile)
			return (int)0x811D0300;
	}
	AVPacket* p = av_packet_alloc();
	p->data = static_cast<uint8_t*>(in->au_data);
	p->size = int(in->au_size);
	const int r = avcodec_send_packet(d->c, p);
	av_packet_free(&p);
	if (r < 0 && r != AVERROR(EAGAIN))
		return (int)0x811D0302;
	const size_t before = d->ready.size();
	Pull(d);
	if (d->codec == 2382845)
	{
		if (d->ready.size() > before)
		{
			av_frame_free(&d->last);
			d->last = av_frame_clone(d->ready.back());
		}
		else if (d->last && !(getenv("MPCPS5_HOST_VP9_HIDDEN") && !strcmp(getenv("MPCPS5_HOST_VP9_HIDDEN"), "none")))
			d->ready.push_back(av_frame_clone(d->last)); // a block that shows no frame still gives a picture
		// (MPCPS5_HOST_VP9_HIDDEN=none: as FFmpeg, nothing for it, its buffer not taken)
	}
	static const size_t hold = getenv("MPCPS5_HOST_HWDEC_HOLD") ? size_t(atoi(getenv("MPCPS5_HOST_HWDEC_HOLD"))) : 0;
	if (hold == 0)
	{
		Give(d, fb, out);
		return 0;
	}
	if (fb && fb->frame_buffer)
	{
		d->held.push_back({fb->frame_buffer, fb->frame_buffer_size});
		fb->is_accepted = true;
	}
	if (!d->ready.empty() && d->held.size() > hold)
	{
		HostVdecFrameBuffer oldest = {sizeof(oldest), d->held.front().first, d->held.front().second, false};
		d->held.pop_front();
		Give(d, &oldest, out);
	}
	return 0;
}
int sceVideodec2Flush(void* decoder, HostVdecFrameBuffer* fb, HostVdecOutput* out)
{
	HostDecoder* d = static_cast<HostDecoder*>(decoder);
	if (!d->draining)
	{
		avcodec_send_packet(d->c, nullptr);
		d->draining = true;
	}
	Pull(d);
	if (!d->held.empty() && !d->ready.empty())
	{
		// draining: the held buffers come back with the last pictures
		HostVdecFrameBuffer oldest = {sizeof(oldest), d->held.front().first, d->held.front().second, false};
		d->held.pop_front();
		Give(d, &oldest, out);
		return 0;
	}
	Give(d, fb, out);
	return 0;
}
int sceVideodec2Reset(void* decoder)
{
	HostDecoder* d = static_cast<HostDecoder*>(decoder);
	avcodec_flush_buffers(d->c);
	for (AVFrame* f : d->ready)
		av_frame_free(&f);
	d->ready.clear();
	d->held.clear();
	d->draining = false;
	return 0;
}
int sceVideodec2GetPictureInfo(const void*, void*, void*)
{
	return 0;
}
}
