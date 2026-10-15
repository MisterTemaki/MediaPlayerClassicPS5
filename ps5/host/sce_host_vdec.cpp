// Media Player Classic PS5 host build: a stand-in for the console's video decoder (libSceVideodec2) and
// libSceSysmodule, so player/mpc_vdec.cpp runs in the tests. FFmpeg decodes the access units it is given (Annex B, as
// the console's decoder takes them) and the pictures come out the way the console's do: in the frame buffers the
// player hands in, NV12 or 16-bit samples with the 10 bits at the bottom, a 256-byte pitch, the height padded to 16.
//
//   MPCPS5_HOST_HWDEC=1          the library loads (without it sceSysmoduleLoadModule fails: FFmpeg decodes)
//   MPCPS5_HOST_HWDEC_FAIL=n     from the n-th access unit on every decode fails with a stream error (the player
//                                resets it a few times, then hands the video back to FFmpeg)
//   MPCPS5_HOST_HWDEC_REFUSE=1   creating a decoder fails (FFmpeg from the start)
//
// SPDX-License-Identifier: MIT

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

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
	bool draining = false;
	unsigned aus = 0;
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
	const AVCodec* codec = avcodec_find_decoder(config->codec_type == 1 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
	if (!codec)
		return -1;
	HostDecoder* d = new HostDecoder;
	d->c = avcodec_alloc_context3(codec);
	d->c->thread_count = 2;
	d->codec = config->codec_type;
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
	AVPacket* p = av_packet_alloc();
	p->data = static_cast<uint8_t*>(in->au_data);
	p->size = int(in->au_size);
	const int r = avcodec_send_packet(d->c, p);
	av_packet_free(&p);
	if (r < 0 && r != AVERROR(EAGAIN))
		return (int)0x811D0302;
	Pull(d);
	Give(d, fb, out);
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
	d->draining = false;
	return 0;
}
int sceVideodec2GetPictureInfo(const void*, void*, void*)
{
	return 0;
}
}
