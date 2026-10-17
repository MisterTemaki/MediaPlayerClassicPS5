// Media Player Classic PS5: H.264 and HEVC on the console's video decoder (mpc_vdec.h).
//
// After VLC for PS5's ps5/modules/ps5vdec.c (github.com/ZiZc3/VLC, GPL-2.0-or-later): the structures, the memory, the
// access-unit slots, the picture buffers and the recovery rules (keyframes after a seek, HEVC's RASL pictures, Dolby
// Vision's layers, resets after stream errors) are that module's, found on the console by its author; the comments
// say which run taught what.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_vdec.h"

#include "OrbisPaths.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

// ---- the console's decoder (libSceVideodec2) ----------------------------------------------------------------------
extern "C" {
struct VdecConfig
{
	uint64_t this_size;
	uint32_t resource_type; // 1: decoding on a compute queue
	uint32_t codec_type;
	uint32_t profile;
	uint32_t max_level;
	int32_t max_frame_width;
	int32_t max_frame_height;
	int32_t max_dpb_frame_count;
	uint32_t decode_pipeline_depth;
	void* compute_queue;
	uint64_t cpu_affinity_mask;
	int32_t cpu_thread_priority;
	bool optimize_progressive_video;
	bool check_memory_type;
	uint8_t reserved0;
	uint8_t reserved1;
	void* extra_config_info;
};
struct VdecMemory
{
	uint64_t this_size;
	uint64_t cpu_memory_size;
	void* cpu_memory;
	uint64_t gpu_memory_size;
	void* gpu_memory;
	uint64_t cpu_gpu_memory_size;
	void* cpu_gpu_memory;
	uint64_t max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
	uint32_t reserved0;
};
struct VdecInput
{
	uint64_t this_size;
	void* au_data;
	uint64_t au_size;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};
struct VdecOutput
{
	uint64_t this_size;
	bool is_valid;
	bool is_error_frame;
	uint8_t picture_count;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch; // in samples
	uint32_t frame_height;
	void* frame_buffer;
	uint64_t frame_buffer_size;
	uint32_t frame_format;
	uint32_t frame_pitch_in_bytes;
};
struct VdecFrameBuffer
{
	uint64_t this_size;
	void* frame_buffer;
	uint64_t frame_buffer_size;
	bool is_accepted;
};
struct VdecComputeMemory
{
	uint64_t this_size;
	uint64_t cpu_gpu_memory_size;
	void* cpu_gpu_memory;
};
struct VdecComputeConfig
{
	uint64_t this_size;
	uint16_t compute_pipe_id;
	uint16_t compute_queue_id;
	bool check_memory_type;
	uint8_t reserved0;
	uint16_t reserved1;
};
static_assert(sizeof(VdecConfig) == 0x48, "decoder config");
static_assert(sizeof(VdecMemory) == 0x48, "decoder memory");
static_assert(sizeof(VdecInput) == 0x30, "input");
static_assert(sizeof(VdecOutput) == 0x38, "output");
static_assert(sizeof(VdecFrameBuffer) == 0x20, "frame buffer");
static_assert(sizeof(VdecComputeMemory) == 0x18, "compute memory");
static_assert(sizeof(VdecComputeConfig) == 0x10, "compute config");

// Weak: an import the console didn't resolve reads as null (the library is loaded at run time, Prepare).
int sceVideodec2QueryComputeMemoryInfo(VdecComputeMemory* info) __attribute__((weak));
int sceVideodec2AllocateComputeQueue(const VdecComputeConfig* config, const VdecComputeMemory* memory, void** queue)
	__attribute__((weak));
int sceVideodec2QueryDecoderMemoryInfo(const VdecConfig* config, VdecMemory* memory) __attribute__((weak));
int sceVideodec2CreateDecoder(const VdecConfig* config, const VdecMemory* memory, void** decoder) __attribute__((weak));
int sceVideodec2DeleteDecoder(void* decoder) __attribute__((weak));
int sceVideodec2Decode(void* decoder, const VdecInput* input, VdecFrameBuffer* frame, VdecOutput* output)
	__attribute__((weak));
int sceVideodec2Flush(void* decoder, VdecFrameBuffer* frame, VdecOutput* output) __attribute__((weak));
int sceVideodec2Reset(void* decoder) __attribute__((weak));
int sceSysmoduleLoadModule(uint16_t id) __attribute__((weak));

int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t align, int mem_type,
	intptr_t* phys_out);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, intptr_t phys, size_t align);
int sceKernelReleaseDirectMemory(intptr_t phys, size_t len);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelMapFlexibleMemory(void** addr, size_t len, int prot, int flags);
int sceKernelReleaseFlexibleMemory(void* addr, size_t len);
int sceKernelMunmap(void* addr, size_t len);
}

namespace mpc
{
namespace hwdec
{
namespace
{
constexpr uint32_t kCodecAvc = 1;
constexpr uint32_t kCodecHevc = 974921;
constexpr uint16_t kSysmoduleVideodec2 = 207;
constexpr int kProtCpuRw = 0x03;
constexpr int kProtGpuRw = 0x30;
constexpr int kDirectType = 12; // the type the decoder's memory is known to work with
constexpr size_t kPage = 0x4000;
// Access units in turn: the decoder may still read one after Decode returns (a big picture), so a slot comes round
// again only after several others; three broke a 4K film at its first big picture.
constexpr int kAuSlots = 8;
constexpr size_t kAuSlotSize = size_t(4) << 20;
// The decoder keeps a picture buffer until that picture's turn to be shown: handed out again only once back.
constexpr int kMaxFrames = 16;
constexpr uint32_t kStreamErrFirst = 0x811D0300u, kStreamErrLast = 0x811D0304u;
constexpr int kMaxResyncs = 5;
constexpr size_t kMaxPending = 64;

std::atomic<bool> g_ready{false};
std::atomic<bool> g_enabled{true};
std::mutex g_queue_mu;
void* g_queue = nullptr;

struct Mem
{
	void* addr = nullptr;
	size_t len = 0;
	intptr_t start = -1; // direct memory; -1: flexible
};

size_t Round16k(size_t v)
{
	return (v + kPage - 1) & ~(kPage - 1);
}

bool MemDirect(Mem& m, size_t len, size_t align, int prot)
{
	m.len = Round16k(len);
	m.start = -1;
	m.addr = nullptr;
	align = std::max(align, kPage);
	if (sceKernelAllocateDirectMemory(0, int64_t(sceKernelGetDirectMemorySize()), m.len, align, kDirectType, &m.start) != 0)
	{
		m.start = -1;
		return false;
	}
	if (sceKernelMapDirectMemory(&m.addr, m.len, prot, 0, m.start, align) != 0)
	{
		sceKernelReleaseDirectMemory(m.start, m.len);
		m.start = -1;
		m.addr = nullptr;
		return false;
	}
	return true;
}

bool MemFlexible(Mem& m, size_t len)
{
	m.len = Round16k(len);
	m.start = -1;
	m.addr = nullptr;
	return sceKernelMapFlexibleMemory(&m.addr, m.len, kProtCpuRw, 0) == 0;
}

void MemFree(Mem& m)
{
	if (!m.addr)
		return;
	if (m.start >= 0)
	{
		sceKernelMunmap(m.addr, m.len);
		sceKernelReleaseDirectMemory(m.start, m.len);
	}
	else
		sceKernelReleaseFlexibleMemory(m.addr, m.len);
	m.addr = nullptr;
	m.start = -1;
}

bool Resolved()
{
	// volatile reads: weak imports, null when the console left them unresolved
	void* const fns[] = {(void*)sceVideodec2QueryComputeMemoryInfo, (void*)sceVideodec2AllocateComputeQueue,
		(void*)sceVideodec2QueryDecoderMemoryInfo, (void*)sceVideodec2CreateDecoder, (void*)sceVideodec2DeleteDecoder,
		(void*)sceVideodec2Decode, (void*)sceVideodec2Flush, (void*)sceVideodec2Reset};
	for (void* volatile f : fns)
		if (!f)
			return false;
	return true;
}

void* ComputeQueue()
{
	std::lock_guard<std::mutex> l(g_queue_mu);
	if (!g_queue)
	{
		static Mem queue_mem;
		VdecComputeMemory info = {};
		info.this_size = sizeof(info);
		int rc = sceVideodec2QueryComputeMemoryInfo(&info);
		if (rc == 0 && MemDirect(queue_mem, info.cpu_gpu_memory_size, kPage, kProtCpuRw | kProtGpuRw))
		{
			info.cpu_gpu_memory = queue_mem.addr;
			info.cpu_gpu_memory_size = queue_mem.len;
			VdecComputeConfig config = {};
			config.this_size = sizeof(config);
			rc = sceVideodec2AllocateComputeQueue(&config, &info, &g_queue);
			if (rc != 0)
			{
				g_queue = nullptr;
				MemFree(queue_mem);
			}
		}
		OrbisLog("[hwdec] compute queue: %s (%#x, %llu KiB)", g_queue ? "ready" : "FAILED", unsigned(rc),
			(unsigned long long)(info.cpu_gpu_memory_size >> 10));
	}
	return g_queue;
}

// HEVC NAL units the decoder can't take: Dolby Vision's own (62, 63) and any layer but the base one. Left out, a
// Dolby Vision film plays as the HDR10 its base layer is.
bool HevcSkip(const uint8_t* nal, size_t n)
{
	if (n < 2)
		return true;
	const unsigned type = (nal[0] >> 1) & 0x3f;
	const unsigned layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
	return type == 62 || type == 63 || layer != 0;
}

// Length-prefixed NAL units -> Annex B (4-byte start codes).
size_t ToAnnexB(uint8_t* dst, size_t room, const uint8_t* src, size_t size, unsigned len_size, bool hevc)
{
	size_t out = 0;
	while (size >= len_size)
	{
		uint32_t n = 0;
		for (unsigned i = 0; i < len_size; i++)
			n = (n << 8) | src[i];
		src += len_size;
		size -= len_size;
		if (n > size || out + 4 + n > room)
			break;
		if (!(hevc && HevcSkip(src, n)))
		{
			memcpy(dst + out, "\x00\x00\x00\x01", 4);
			memcpy(dst + out + 4, src, n);
			out += 4 + n;
		}
		src += n;
		size -= n;
	}
	return out;
}

// Annex B input: Dolby Vision's units are dropped the same way (start codes kept).
size_t CopyAnnexB(uint8_t* dst, size_t room, const uint8_t* src, size_t size, bool hevc)
{
	if (!hevc)
	{
		if (size > room)
			return 0;
		memcpy(dst, src, size);
		return size;
	}
	size_t out = 0, i = 0;
	while (i + 3 <= size)
	{
		size_t sc = 0;
		if (src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 1)
			sc = 3;
		else if (i + 4 <= size && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 0 && src[i + 3] == 1)
			sc = 4;
		if (!sc)
		{
			i++;
			continue;
		}
		size_t j = i + sc;
		size_t next = j;
		while (next + 3 <= size && !(src[next] == 0 && src[next + 1] == 0 && (src[next + 2] == 1 ||
			(next + 3 < size && src[next + 2] == 0 && src[next + 3] == 1))))
			next++;
		if (next + 3 > size)
			next = size;
		if (!HevcSkip(src + j, next - j))
		{
			if (out + 4 + (next - j) > room)
				return out;
			memcpy(dst + out, "\x00\x00\x00\x01", 4);
			memcpy(dst + out + 4, src + j, next - j);
			out += 4 + (next - j);
		}
		i = next;
	}
	return out;
}

unsigned ReadUe(const uint8_t* p, size_t n, size_t* bit)
{
	unsigned zeros = 0;
	while (*bit < n * 8 && !((p[*bit / 8] >> (7 - *bit % 8)) & 1) && zeros < 31)
	{
		zeros++;
		(*bit)++;
	}
	(*bit)++;
	unsigned v = 0;
	for (unsigned i = 0; i < zeros && *bit < n * 8; i++, (*bit)++)
		v = (v << 1) | ((p[*bit / 8] >> (7 - *bit % 8)) & 1);
	return (1u << zeros) - 1 + v;
}

// An access unit a decoder can start from (Annex B): an H.264 IDR or I picture, an HEVC IRAP (16-21).
bool IsKeyframe(const uint8_t* p, size_t n, uint32_t codec)
{
	for (size_t i = 0; i + 4 < n; i++)
	{
		if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
			continue;
		const uint8_t* nal = p + i + 3;
		const size_t left = n - i - 3;
		if (codec == kCodecHevc)
		{
			const unsigned type = (nal[0] >> 1) & 0x3f;
			if (type < 32)
				return type >= 16 && type <= 21;
		}
		else
		{
			const unsigned type = nal[0] & 0x1f;
			if (type == 5)
				return true;
			if (type == 1 && left > 1)
			{
				size_t bit = 0;
				ReadUe(nal + 1, left - 1, &bit);
				return ReadUe(nal + 1, left - 1, &bit) % 5 == 2;
			}
		}
		i += 2;
	}
	return false;
}

int HevcFirstSliceType(const uint8_t* p, size_t n)
{
	for (size_t i = 0; i + 4 < n; i++)
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
		{
			const unsigned type = (p[i + 3] >> 1) & 0x3f;
			if (type < 32)
				return int(type);
			i += 2;
		}
	return -1;
}
} // namespace

struct Decoder
{
	void* dec = nullptr;
	uint32_t codec = 0;
	Mem cpu, gpu, cpu_gpu;
	Mem au[kAuSlots];
	Mem frames;
	size_t frame_size = 0;
	unsigned frame_count = 0;
	bool frame_held[kMaxFrames] = {};
	unsigned au_next = 0;

	std::vector<uint8_t> headers; // parameter sets, Annex B
	bool send_headers = true;
	unsigned nal_length = 0; // avcC / hvcC packets: length bytes (0: Annex B)

	std::vector<int64_t> pending; // pictures come out in display order: each takes the earliest time waiting
	std::deque<AVFrame*> out;
	bool eof = false;

	unsigned decoded = 0, errors = 0, shown = 0, resyncs = 0, skipped = 0;
	bool failed = false, wait_keyframe = false, skip_rasl = true;
	int vis_w = 0, vis_h = 0;
	bool ten = false;
	// the bytes per sample the stream's pictures should come in: 1 (8-bit: H.264, HEVC Main), 2 (HEVC Main 10),
	// 0 when the stream doesn't say; and the first picture's layout, which the later ones must keep
	unsigned expect_bps = 0, seen_bps = 0;
	uint32_t seen_format = 0;
	// colour, from the stream
	AVColorPrimaries prim = AVCOL_PRI_UNSPECIFIED;
	AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
	AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
	AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
	AVRational sar = {0, 1};
	std::string desc;
	double copy_ms = 0, decode_ms = 0;
};

namespace
{
void GiveUp(Decoder* d, const char* why)
{
	if (d->failed)
		return;
	d->failed = true;
	OrbisLog("[hwdec] handing the video to FFmpeg: %s (%u decoded, %u errors)", why, d->decoded, d->errors);
}

void FrameBack(Decoder* d, const void* buffer)
{
	const uint8_t* base = static_cast<const uint8_t*>(d->frames.addr);
	const uint8_t* b = static_cast<const uint8_t*>(buffer);
	if (!b || !base || b < base || !d->frame_size)
		return;
	const size_t i = size_t(b - base) / d->frame_size;
	if (i < d->frame_count)
		d->frame_held[i] = false;
}

int64_t PendingTake(Decoder* d)
{
	if (d->pending.empty())
		return AV_NOPTS_VALUE;
	auto it = std::min_element(d->pending.begin(), d->pending.end());
	const int64_t t = *it;
	*it = d->pending.back();
	d->pending.pop_back();
	return t;
}

void PendingAdd(Decoder* d, int64_t t)
{
	if (d->pending.size() >= kMaxPending)
		d->pending.erase(d->pending.begin());
	d->pending.push_back(t);
}

// frame_format values seen on a console with a right picture, and their sample size (see Output). Empty until a
// console run reports them.
struct ConfirmedFormat
{
	uint32_t format;
	unsigned bytes_per_sample;
};
constexpr ConfirmedFormat kConfirmedFormats[] = {{0xffffffffu, 0}}; // (a placeholder no picture has)

// One decoded picture into an FFmpeg frame (NV12, or yuv420p10le with the 10 bits at the bottom as they come).
void Output(Decoder* d, const VdecOutput& o)
{
	const int64_t pts = PendingTake(d);
	if (o.is_error_frame || !o.frame_buffer)
	{
		d->errors++;
		FrameBack(d, o.frame_buffer);
		return;
	}
	const double t0 = av_gettime_relative() / 1e6;
	// The sample size comes from the picture's own row: frame_pitch is in samples, frame_pitch_in_bytes in bytes,
	// so their ratio is the bytes per sample (1: NV12, 2: 16-bit samples, 10 bits used) whatever the alignment.
	// Anything else, or a picture that doesn't fit the buffer the player handed in, isn't read: the video goes to
	// FFmpeg (from the next keyframe) rather than copying from outside the picture.
	const uint32_t pitch_px = o.frame_pitch, pitch_b = o.frame_pitch_in_bytes;
	const unsigned bps = pitch_px ? pitch_b / pitch_px : 0;
	const uint8_t* buf = static_cast<const uint8_t*>(o.frame_buffer);
	const uint8_t* mem = static_cast<const uint8_t*>(d->frames.addr);
	const uint64_t need = uint64_t(pitch_b) * (uint64_t(o.frame_height) + (uint64_t(o.frame_height) + 1) / 2);
	const char* bad = nullptr;
	if (!pitch_px || pitch_b % pitch_px != 0 || (bps != 1 && bps != 2))
		bad = "a picture layout the player doesn't know";
	else if (!o.frame_width || !o.frame_height || o.frame_width > pitch_px)
		bad = "a picture wider than its rows";
	else if (!mem || !d->frame_size || buf < mem || buf + d->frame_size > mem + d->frame_size * d->frame_count ||
			 size_t(buf - mem) % d->frame_size != 0)
		bad = "a picture outside the player's buffers";
	else if (need > o.frame_buffer_size || need > d->frame_size)
		bad = "a picture bigger than its buffer";
	else if (uint64_t((o.frame_width + 1) / 2) * 2 * bps > pitch_b)
		bad = "a chroma row wider than its pitch";
	else if (d->expect_bps && bps != d->expect_bps)
		bad = d->expect_bps == 1 ? "16-bit samples for an 8-bit stream" : "8-bit samples for a 10-bit stream";
	else if (d->seen_bps && (bps != d->seen_bps || o.frame_format != d->seen_format))
		bad = "the pictures' layout changed in the middle of the stream";
	if (bad)
	{
		OrbisLog("[hwdec] %s: %ux%u, pitch %u samples / %u bytes, format %u, buffer %llu of %zu bytes", bad,
			o.frame_width, o.frame_height, pitch_px, pitch_b, o.frame_format, (unsigned long long)o.frame_buffer_size,
			d->frame_size);
		d->errors++;
		FrameBack(d, o.frame_buffer);
		GiveUp(d, bad);
		return;
	}
	if (!d->seen_bps)
	{
		d->seen_bps = bps;
		d->seen_format = o.frame_format;
		// Sony's numbers for frame_format aren't public, and VLC for PS5 (whose console runs showed NV12 and 10-bit
		// pictures right) reads the layout from the pitch as this does. The numbers seen on a console go into
		// kConfirmedFormats once a boot.log shows them with a right picture; until then each new one is logged.
		bool known = false;
		for (const ConfirmedFormat& k : kConfirmedFormats)
			known = known || (k.format == o.frame_format && k.bytes_per_sample == bps);
		if (!known)
			OrbisLog("[hwdec] frame_format %u with %u-byte samples isn't on the confirmed list yet: layout from the "
					 "pitch (%s); please report how the picture looks",
				o.frame_format, bps, bps == 2 ? "16-bit samples, 10 bits used" : "NV12");
	}
	const bool ten = bps == 2;
	// only the picture's real size, not the decoder's padding (1088 rows for 1080)
	int w = d->vis_w > 0 && d->vis_w <= int(o.frame_width) ? d->vis_w : int(o.frame_width);
	int h = d->vis_h > 0 && d->vis_h <= int(o.frame_height) ? d->vis_h : int(o.frame_height);
	if (ten != d->ten || d->shown == 0)
	{
		d->ten = ten;
		OrbisLog("[hwdec] pictures %ux%u (shown %dx%d) %s, pitch %u bytes, format %u, buffer %llu bytes", o.frame_width,
			o.frame_height, w, h, ten ? "10-bit" : "NV12", pitch_b, o.frame_format,
			(unsigned long long)o.frame_buffer_size);
	}
	AVFrame* f = av_frame_alloc();
	if (f)
	{
		f->format = ten ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_NV12;
		f->width = w;
		f->height = h;
	}
	if (!f || av_frame_get_buffer(f, 64) < 0)
	{
		av_frame_free(&f);
		FrameBack(d, o.frame_buffer);
		return;
	}
	const uint8_t* src = static_cast<const uint8_t*>(o.frame_buffer);
	const size_t pitch = o.frame_pitch_in_bytes;
	const size_t luma_bytes = std::min(pitch, size_t(w) * (ten ? 2 : 1));
	for (int r = 0; r < h; r++)
		memcpy(f->data[0] + size_t(r) * f->linesize[0], src + size_t(r) * pitch, luma_bytes);
	// the chroma follows the decoder's luma rows (frame_height: 1088 for a padded 1080)
	const uint8_t* chroma = src + pitch * o.frame_height;
	const int crows = (h + 1) / 2;
	if (ten)
	{
		const int cw = (w + 1) / 2;
		for (int r = 0; r < crows; r++)
		{
			const uint16_t* s = reinterpret_cast<const uint16_t*>(chroma + size_t(r) * pitch);
			uint16_t* u = reinterpret_cast<uint16_t*>(f->data[1] + size_t(r) * f->linesize[1]);
			uint16_t* v = reinterpret_cast<uint16_t*>(f->data[2] + size_t(r) * f->linesize[2]);
			for (int i = 0; i < cw; i++)
			{
				u[i] = s[2 * i];
				v[i] = s[2 * i + 1];
			}
		}
	}
	else
	{
		const size_t cbytes = std::min(pitch, size_t((w + 1) / 2) * 2);
		for (int r = 0; r < crows; r++)
			memcpy(f->data[1] + size_t(r) * f->linesize[1], chroma + size_t(r) * pitch, cbytes);
	}
	FrameBack(d, o.frame_buffer);
	f->pts = pts;
	f->best_effort_timestamp = pts;
	f->pkt_dts = AV_NOPTS_VALUE;
	f->color_primaries = d->prim;
	f->color_trc = d->trc;
	f->colorspace = d->space;
	f->color_range = d->range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : d->range;
	f->sample_aspect_ratio = d->sar;
	d->out.push_back(f);
	d->shown++;
	d->copy_ms = d->copy_ms * 0.95 + (av_gettime_relative() / 1e6 - t0) * 1000 * 0.05;
	if (d->shown % 480 == 0)
		OrbisLog("[hwdec] %u pictures; decode %.1f ms, copy %.1f ms each", d->shown, d->decode_ms, d->copy_ms);
}

int NextFrame(Decoder* d, VdecFrameBuffer* fb)
{
	for (unsigned i = 0; i < d->frame_count; i++)
		if (!d->frame_held[i])
		{
			*fb = VdecFrameBuffer{};
			fb->this_size = sizeof(*fb);
			fb->frame_buffer = static_cast<uint8_t*>(d->frames.addr) + i * d->frame_size;
			fb->frame_buffer_size = d->frame_size;
			return int(i);
		}
	return -1;
}

void Drain(Decoder* d, bool show)
{
	for (int i = 0; i < 32; i++)
	{
		VdecFrameBuffer fb;
		const int slot = NextFrame(d, &fb);
		if (slot < 0)
			break;
		VdecOutput o = {};
		o.this_size = sizeof(o);
		const int rc = sceVideodec2Flush(d->dec, &fb, &o);
		if (fb.is_accepted)
			d->frame_held[slot] = true;
		if (rc != 0 || !o.is_valid)
			break;
		if (show)
			Output(d, o);
		else
		{
			PendingTake(d);
			FrameBack(d, o.frame_buffer);
		}
	}
}

void ReadHeaders(Decoder* d, const uint8_t* p, size_t n)
{
	if (!p || !n)
		return;
	if (p[0] != 1)
	{ // Annex B already
		d->headers.assign(p, p + n);
		return;
	}
	std::vector<uint8_t> out;
	auto add = [&](const uint8_t* nal, size_t len) {
		out.insert(out.end(), {0, 0, 0, 1});
		out.insert(out.end(), nal, nal + len);
	};
	if (d->codec == kCodecAvc && n >= 7)
	{ // avcC
		d->nal_length = (p[4] & 3) + 1;
		size_t at = 5;
		for (int list = 0; list < 2 && at < n; list++)
		{
			const unsigned count = list == 0 ? (p[at] & 0x1f) : p[at];
			at++;
			for (unsigned i = 0; i < count && at + 2 <= n; i++)
			{
				const size_t len = (size_t(p[at]) << 8) | p[at + 1];
				at += 2;
				if (at + len > n)
					break;
				add(p + at, len);
				at += len;
			}
		}
	}
	else if (d->codec == kCodecHevc && n >= 23)
	{ // hvcC
		d->nal_length = (p[21] & 3) + 1;
		const unsigned arrays = p[22];
		size_t at = 23;
		for (unsigned a = 0; a < arrays && at + 3 <= n; a++)
		{
			const unsigned count = (unsigned(p[at + 1]) << 8) | p[at + 2];
			at += 3;
			for (unsigned i = 0; i < count && at + 2 <= n; i++)
			{
				const size_t len = (size_t(p[at]) << 8) | p[at + 1];
				at += 2;
				if (at + len > n)
					break;
				if (!HevcSkip(p + at, len))
					add(p + at, len);
				at += len;
			}
		}
	}
	d->headers = std::move(out);
}

void FreeDecoder(Decoder* d)
{
	if (d->dec)
		sceVideodec2DeleteDecoder(d->dec);
	d->dec = nullptr;
	for (Mem& m : d->au)
		MemFree(m);
	MemFree(d->frames);
	MemFree(d->cpu_gpu);
	MemFree(d->gpu);
	MemFree(d->cpu);
}

int BringUp(Decoder* d, const VdecConfig& config, VdecMemory& mem)
{
	mem = VdecMemory{};
	mem.this_size = sizeof(mem);
	int rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem);
	if (rc != 0)
	{
		OrbisLog("[hwdec] memory query %#x", unsigned(rc));
		return rc;
	}
	const size_t align = std::max<size_t>(mem.frame_buffer_alignment, kPage);
	d->frame_size = (mem.max_frame_buffer_size + align - 1) & ~(align - 1);
	bool ok = (!mem.cpu_memory_size || MemFlexible(d->cpu, mem.cpu_memory_size)) &&
			  (!mem.gpu_memory_size || MemDirect(d->gpu, mem.gpu_memory_size, kPage, kProtCpuRw | kProtGpuRw)) &&
			  (!mem.cpu_gpu_memory_size || MemDirect(d->cpu_gpu, mem.cpu_gpu_memory_size, kPage, kProtCpuRw | kProtGpuRw)) &&
			  MemDirect(d->frames, d->frame_size * d->frame_count, align, kProtCpuRw | kProtGpuRw);
	for (int i = 0; ok && i < kAuSlots; i++)
		ok = MemDirect(d->au[i], kAuSlotSize, kPage, kProtCpuRw | kProtGpuRw);
	if (ok)
	{
		mem.cpu_memory = d->cpu.addr;
		mem.gpu_memory = d->gpu.addr;
		mem.cpu_gpu_memory = d->cpu_gpu.addr;
		rc = sceVideodec2CreateDecoder(&config, &mem, &d->dec);
		if (rc == 0)
			return 0;
		OrbisLog("[hwdec] create %#x", unsigned(rc));
		d->dec = nullptr;
	}
	else
	{
		OrbisLog("[hwdec] no memory (cpu %llu, gpu %llu, cpu+gpu %llu KiB, frames %u x %zu KiB)",
			(unsigned long long)(mem.cpu_memory_size >> 10), (unsigned long long)(mem.gpu_memory_size >> 10),
			(unsigned long long)(mem.cpu_gpu_memory_size >> 10), d->frame_count, d->frame_size >> 10);
		rc = -1;
	}
	FreeDecoder(d);
	return rc;
}
} // namespace

void Prepare()
{
	if (!sceSysmoduleLoadModule)
	{
		OrbisLog("[hwdec] off: sceSysmoduleLoadModule unresolved");
		return;
	}
	const int rc = sceSysmoduleLoadModule(kSysmoduleVideodec2);
	const bool resolved = rc == 0 && Resolved();
	OrbisLog("[hwdec] video decoder library %#x: %s", unsigned(rc),
		rc != 0 ? "not loaded" : resolved ? "functions resolved" : "functions still unresolved");
	if (!resolved)
		return;
	// the compute queue now, while still in the sandbox
	g_ready = ComputeQueue() != nullptr;
	OrbisLog("[hwdec] hardware decoding %s", g_ready ? "available" : "off");
}

bool Available()
{
	return g_ready;
}

void SetEnabled(bool on)
{
	g_enabled = on;
}

bool Enabled()
{
	return g_enabled;
}

Decoder* Open(const AVCodecParameters* par, std::string* why)
{
	auto no = [&](const char* r) -> Decoder* {
		if (why)
			*why = r;
		return nullptr;
	};
	if (!g_ready)
		return no("the console's decoder isn't available");
	if (!g_enabled)
		return no("turned off in the options");
	uint32_t codec, profile, level;
	if (par->codec_id == AV_CODEC_ID_H264)
	{
		// 8-bit 4:2:0 only: not High 10, 4:2:2, 4:4:4, CAVLC 4:4:4
		if (par->profile == AV_PROFILE_H264_HIGH_10 || par->profile == AV_PROFILE_H264_HIGH_10_INTRA ||
			par->profile == AV_PROFILE_H264_HIGH_422 || par->profile == AV_PROFILE_H264_HIGH_422_INTRA ||
			par->profile == AV_PROFILE_H264_HIGH_444 || par->profile == AV_PROFILE_H264_HIGH_444_PREDICTIVE ||
			par->profile == AV_PROFILE_H264_HIGH_444_INTRA || par->profile == AV_PROFILE_H264_CAVLC_444)
			return no("H.264 profile the decoder doesn't take");
		if (par->format != AV_PIX_FMT_NONE && par->format != AV_PIX_FMT_YUV420P && par->format != AV_PIX_FMT_YUVJ420P)
			return no("H.264 that isn't 8-bit 4:2:0");
		codec = kCodecAvc;
		profile = 100;
		level = 52;
	}
	else if (par->codec_id == AV_CODEC_ID_HEVC)
	{
		// Main and Main 10; not the range extensions
		if (par->profile > AV_PROFILE_HEVC_MAIN_10)
			return no("HEVC profile the decoder doesn't take");
		if (par->format != AV_PIX_FMT_NONE && par->format != AV_PIX_FMT_YUV420P && par->format != AV_PIX_FMT_YUV420P10LE &&
			par->format != AV_PIX_FMT_YUVJ420P)
			return no("HEVC that isn't 4:2:0");
		codec = kCodecHevc;
		profile = 2;
		level = 156; // 5.2: 4K films at 60-80 Mbit/s
	}
	else
		return no("not H.264 or HEVC");
	// interlaced pictures: FFmpeg, then bwdif (the console's decoder gives them as they are)
	if (par->field_order != AV_FIELD_UNKNOWN && par->field_order != AV_FIELD_PROGRESSIVE)
		return no("interlaced video (FFmpeg deinterlaces it)");
	const int w = par->width, h = par->height;
	if (w > 3840 || h > 2176)
		return no("bigger than 4K");
	const bool small = w && h && w <= 1920 && h <= 1088;
	void* queue = ComputeQueue();
	if (!queue)
		return no("no compute queue");

	Decoder* d = new Decoder;
	d->codec = codec;
	VdecConfig config = {};
	config.this_size = sizeof(config);
	config.resource_type = 1;
	config.codec_type = codec;
	config.profile = profile;
	config.max_level = level;
	config.max_frame_width = w ? int32_t((w + 15) & ~15) : 3840;
	config.max_frame_height = h ? int32_t((h + 15) & ~15) : 2176;
	config.max_dpb_frame_count = -1;    // the decoder sizes its picture store for the stream
	config.decode_pipeline_depth = 4;   // one at a time left it idle: 20 pictures a second for a 4K film needing 24
	config.compute_queue = queue;
	config.cpu_affinity_mask = 0x3f;
	config.cpu_thread_priority = 700;
	config.optimize_progressive_video = true;
	d->frame_count = small ? kMaxFrames : 10;
	VdecMemory mem;
	int rc = BringUp(d, config, mem);
	if (rc != 0 && config.decode_pipeline_depth > 1)
	{
		OrbisLog("[hwdec] pipeline depth %u failed (%#x): trying 1", config.decode_pipeline_depth, unsigned(rc));
		config.decode_pipeline_depth = 1;
		rc = BringUp(d, config, mem);
	}
	if (rc != 0 && config.max_level == 156)
	{
		OrbisLog("[hwdec] level 5.2 failed (%#x): trying 5.1", unsigned(rc));
		config.max_level = 153;
		rc = BringUp(d, config, mem);
	}
	if (rc != 0)
	{
		delete d;
		return no("the decoder couldn't be created");
	}
	ReadHeaders(d, par->extradata, size_t(par->extradata_size));
	d->vis_w = w;
	d->vis_h = h;
	if (codec == kCodecAvc || par->format == AV_PIX_FMT_YUV420P || par->format == AV_PIX_FMT_YUVJ420P ||
		(par->format == AV_PIX_FMT_NONE && par->profile == AV_PROFILE_HEVC_MAIN))
		d->expect_bps = 1;
	else if (par->format == AV_PIX_FMT_YUV420P10LE || par->profile == AV_PROFILE_HEVC_MAIN_10)
		d->expect_bps = 2;
	d->prim = par->color_primaries;
	d->trc = par->color_trc;
	d->space = par->color_space;
	d->range = par->color_range;
	d->sar = par->sample_aspect_ratio;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s %dx%d (console decoder)", codec == kCodecAvc ? "H.264" : "HEVC", w, h);
	d->desc = buf;
	OrbisLog("[hwdec] %s: profile %d, level %d (decoder level %u, pipeline %u), frames %u x %zu KiB, headers %zu bytes%s",
		buf, par->profile, par->level, config.max_level, config.decode_pipeline_depth, d->frame_count, d->frame_size >> 10,
		d->headers.size(), d->nal_length ? " (length-prefixed packets)" : "");
	return d;
}

int Send(Decoder* d, const AVPacket* pkt)
{
	if (d->failed)
		return AVERROR_EXTERNAL;
	if (!pkt || !pkt->data)
	{
		if (!d->eof)
			Drain(d, true);
		d->eof = true;
		return 0;
	}
	d->eof = false;
	const bool hevc = d->codec == kCodecHevc;
	const bool marked_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
	// the access unit into this turn's slot, made bigger first if it needs to be (a 4K keyframe can pass 4 MiB)
	Mem& slot = d->au[d->au_next];
	const size_t need = d->headers.size() + size_t(pkt->size) * (d->nal_length && d->nal_length < 4 ? 3 : 1) + 64;
	if (need > slot.len)
	{
		MemFree(slot);
		if (!MemDirect(slot, need + (size_t(1) << 20), kPage, kProtCpuRw | kProtGpuRw))
		{
			GiveUp(d, "no memory for an access unit");
			return AVERROR_EXTERNAL;
		}
		OrbisLog("[hwdec] access unit slot grown to %zu KiB", slot.len >> 10);
	}
	uint8_t* au = static_cast<uint8_t*>(slot.addr);
	size_t size = 0;
	if (d->send_headers && !d->headers.empty() && d->headers.size() < slot.len)
	{
		memcpy(au, d->headers.data(), d->headers.size());
		size = d->headers.size();
	}
	const bool annexb = pkt->size >= 4 && (!memcmp(pkt->data, "\x00\x00\x00\x01", 4) || !memcmp(pkt->data, "\x00\x00\x01", 3));
	if (d->nal_length && !annexb)
		size += ToAnnexB(au + size, slot.len - size, pkt->data, size_t(pkt->size), d->nal_length, hevc);
	else
		size += CopyAnnexB(au + size, slot.len - size, pkt->data, size_t(pkt->size), hevc);
	const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;

	// after a seek or a stream error: nothing until a keyframe (MP4 doesn't always mark them: looked at too)
	if (d->wait_keyframe)
	{
		if (!marked_key && !IsKeyframe(au, size, d->codec))
		{
			if (++d->skipped >= 600)
				GiveUp(d, "no keyframe after a seek or a stream error");
			return 0;
		}
		OrbisLog("[hwdec] going on from a keyframe (%u skipped)", d->skipped);
		d->wait_keyframe = false;
		d->skip_rasl = true;
		d->skipped = 0;
	}
	// after a restart point, HEVC's RASL pictures refer to pictures the decoder never got: left out
	if (d->skip_rasl && hevc)
	{
		const int type = HevcFirstSliceType(au, size);
		if (type == 8 || type == 9)
		{
			d->skipped++;
			return 0;
		}
		if (type >= 0 && (type < 16 || type > 21))
			d->skip_rasl = false;
	}
	VdecInput in = {};
	in.this_size = sizeof(in);
	in.au_data = au;
	in.au_size = size;
	in.pts_data = uint64_t(pts);
	in.dts_data = UINT64_MAX;
	VdecFrameBuffer fb;
	const int frame = NextFrame(d, &fb);
	if (frame < 0)
	{
		GiveUp(d, "out of picture buffers");
		return AVERROR_EXTERNAL;
	}
	VdecOutput o = {};
	o.this_size = sizeof(o);
	const double t0 = av_gettime_relative() / 1e6;
	const int rc = sceVideodec2Decode(d->dec, &in, &fb, &o);
	d->decode_ms = d->decode_ms * 0.95 + (av_gettime_relative() / 1e6 - t0) * 1000 * 0.05;
	d->au_next = (d->au_next + 1) % kAuSlots;
	if (fb.is_accepted)
		d->frame_held[frame] = true;
	if (rc != 0)
	{
		d->errors++;
		if (d->errors <= 5)
			OrbisLog("[hwdec] decode %#x (access unit %u, %zu bytes)", unsigned(rc), d->decoded, size);
		if (d->shown == 0 && d->errors >= 3)
		{
			GiveUp(d, "the decoder refuses this video");
			return AVERROR_EXTERNAL;
		}
		if (unsigned(rc) >= kStreamErrFirst && unsigned(rc) <= kStreamErrLast)
		{
			if (d->resyncs++ >= kMaxResyncs)
			{
				GiveUp(d, "stream errors keep coming");
				return AVERROR_EXTERNAL;
			}
			const int r = sceVideodec2Reset(d->dec);
			OrbisLog("[hwdec] stream error %#x: reset (%#x), waiting for a keyframe (%u of %d)", unsigned(rc), unsigned(r),
				d->resyncs, kMaxResyncs);
			d->pending.clear();
			d->send_headers = true;
			d->wait_keyframe = true;
			memset(d->frame_held, 0, sizeof(d->frame_held));
		}
		else if (d->errors >= 30)
		{
			GiveUp(d, "the decoder refuses this video");
			return AVERROR_EXTERNAL;
		}
		return 0;
	}
	d->send_headers = false;
	d->decoded++;
	PendingAdd(d, pts);
	if (o.is_valid)
		Output(d, o);
	if (d->decoded == 1 || d->decoded == 100)
		OrbisLog("[hwdec] %u access units in, %u pictures out, %u errors", d->decoded, d->shown, d->errors);
	return 0;
}

int Receive(Decoder* d, AVFrame* frame)
{
	if (!d->out.empty())
	{
		AVFrame* f = d->out.front();
		d->out.pop_front();
		av_frame_move_ref(frame, f);
		av_frame_free(&f);
		return 0;
	}
	if (d->failed)
		return AVERROR_EXTERNAL;
	return d->eof ? AVERROR_EOF : AVERROR(EAGAIN);
}

void Flush(Decoder* d)
{
	for (AVFrame* f : d->out)
		av_frame_free(&f);
	d->out.clear();
	d->eof = false;
	if (d->failed)
		return;
	Drain(d, false);
	const int rc = sceVideodec2Reset(d->dec);
	if (rc != 0)
		OrbisLog("[hwdec] reset %#x", unsigned(rc));
	d->pending.clear();
	d->send_headers = true;
	// after a reset the decoder holds nothing and takes nothing but a keyframe
	memset(d->frame_held, 0, sizeof(d->frame_held));
	d->wait_keyframe = true;
	d->skipped = 0;
}

bool Failed(const Decoder* d)
{
	return d->failed;
}

std::string Describe(const Decoder* d)
{
	return d->desc;
}

void Close(Decoder* d)
{
	if (!d)
		return;
	OrbisLog("[hwdec] closed: %u access units, %u pictures, %u errors", d->decoded, d->shown, d->errors);
	for (AVFrame* f : d->out)
		av_frame_free(&f);
	FreeDecoder(d);
	delete d;
}
} // namespace hwdec
} // namespace mpc
