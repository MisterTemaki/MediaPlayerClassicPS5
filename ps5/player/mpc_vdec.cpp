// Media Player Classic PS5: H.264 and HEVC on the console's video decoder (mpc_vdec.h).
//
// After VLC for PS5's ps5/modules/ps5vdec.c (github.com/ZiZc3/VLC, GPL-2.0-or-later): the structures, the memory, the
// access-unit slots, the picture buffers and the recovery rules (keyframes after a seek, HEVC's RASL pictures, Dolby
// Vision's layers, resets after stream errors) are that module's, found on the console by its author; the comments
// say which run taught what.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_vdec.h"
#include "mpc_bigmem.h"

#include "OrbisPaths.h"
#include "ProsperoThread.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
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

// 1.6 (test2 on a console: "sceSysmoduleLoadModule unresolved"): ordinary (not weak) imports, as VLC for PS5 declares
// them; the console leaves an import it can't bind at 0 without refusing the title, so each one's slot is read before
// a call (Slot): the decoder's own functions get their addresses only once its library is loaded (Prepare).
int sceVideodec2QueryComputeMemoryInfo(VdecComputeMemory* info);
int sceVideodec2AllocateComputeQueue(const VdecComputeConfig* config, const VdecComputeMemory* memory, void** queue);
int sceVideodec2QueryDecoderMemoryInfo(const VdecConfig* config, VdecMemory* memory);
int sceVideodec2CreateDecoder(const VdecConfig* config, const VdecMemory* memory, void** decoder);
int sceVideodec2DeleteDecoder(void* decoder);
int sceVideodec2Decode(void* decoder, const VdecInput* input, VdecFrameBuffer* frame, VdecOutput* output);
int sceVideodec2Flush(void* decoder, VdecFrameBuffer* frame, VdecOutput* output);
int sceVideodec2Reset(void* decoder);
int sceVideodec2ReleaseComputeQueue(void* queue);
int sceSysmoduleLoadModule(uint16_t id);

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
constexpr uint32_t kZeroCopyMagic = 0x6d706330; // "mpc0"
// (review M-A) the margin a decoder keeps besides the most buffers it has held at once and its next picture's
constexpr unsigned kZeroCopySlack = 2;
std::atomic<bool> g_zero_copy{false};
constexpr uint32_t kCodecAvc = 1;
constexpr uint32_t kCodecHevc = 974921;
// VP9 (1.6, from what VLC for PS5's console runs found: README): profile 0 (8-bit) or 2 (10-bit) 4:2:0, level 62, on a
// compute queue of its own (a VP9 decoder made on the queue an H.264 or HEVC one had used was refused)
constexpr uint32_t kCodecVp9 = 2382845;
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

// An import's address as the console bound it (its GOT slot, read at run time: the compiler may not assume a declared
// function is there); 0 when it wasn't bound.
uintptr_t Slot(void (*fn)())
{
	uintptr_t v = reinterpret_cast<uintptr_t>(fn);
	__asm__ volatile("" : "+r"(v));
	return v;
}
#define MPC_SLOT(f) Slot(reinterpret_cast<void (*)()>(&f))

// The decoder's functions, each bound (`missing`: the first that isn't)
bool Resolved(const char** missing = nullptr)
{
	const struct
	{
		const char* name;
		uintptr_t at;
	} fns[] = {{"sceVideodec2QueryComputeMemoryInfo", MPC_SLOT(sceVideodec2QueryComputeMemoryInfo)},
		{"sceVideodec2AllocateComputeQueue", MPC_SLOT(sceVideodec2AllocateComputeQueue)},
		{"sceVideodec2QueryDecoderMemoryInfo", MPC_SLOT(sceVideodec2QueryDecoderMemoryInfo)},
		{"sceVideodec2CreateDecoder", MPC_SLOT(sceVideodec2CreateDecoder)},
		{"sceVideodec2DeleteDecoder", MPC_SLOT(sceVideodec2DeleteDecoder)},
		{"sceVideodec2Decode", MPC_SLOT(sceVideodec2Decode)}, {"sceVideodec2Flush", MPC_SLOT(sceVideodec2Flush)},
		{"sceVideodec2Reset", MPC_SLOT(sceVideodec2Reset)}};
	for (const auto& f : fns)
		if (!f.at)
		{
			if (missing)
				*missing = f.name;
			return false;
		}
	return true;
}

void* g_vp9_queue = nullptr;
bool g_vp9_queue_tried = false;

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

// VP9's own queue, made at the start too (a queue made later fails); null: VP9 shares the other
void* Vp9Queue()
{
	std::lock_guard<std::mutex> l(g_queue_mu);
	if (!g_vp9_queue_tried)
	{
		g_vp9_queue_tried = true;
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
			rc = sceVideodec2AllocateComputeQueue(&config, &info, &g_vp9_queue);
			if (rc != 0)
			{
				g_vp9_queue = nullptr;
				MemFree(queue_mem);
			}
		}
		OrbisLog("[hwdec] VP9 compute queue: %s (%#x)", g_vp9_queue ? "ready" : "FAILED (VP9 shares the other)",
			unsigned(rc));
	}
	return g_vp9_queue;
}

// VLC-PS5 queries these sizes while still sandboxed: after full access the console can refuse the same query
// with 0x811d0200. Keep successful answers, not allocations or decoder instances. Published before any worker starts.
struct DecoderMemoryQuery
{
	VdecConfig config;
	VdecMemory memory;
};
std::vector<DecoderMemoryQuery> g_memory_queries;

void PrepareMemoryQueries()
{
	g_memory_queries.clear();
	const struct { uint32_t codec, profile, level; } formats[] = {
		{kCodecAvc, 100, 52}, {kCodecHevc, 2, 156}, {kCodecHevc, 2, 153},
		{kCodecVp9, 0, 62}, {kCodecVp9, 2, 62}
	};
	for (const auto& f : formats)
	{
		const auto codec = f.codec, profile = f.profile, level = f.level;
		for (int size : {0, 1})
			for (uint32_t depth : {4u, 1u})
			{
				VdecConfig c = {};
				c.this_size = sizeof(c);
				c.resource_type = 1;
				c.codec_type = codec;
				c.profile = profile;
				c.max_level = level;
				c.max_frame_width = size ? 3840 : 1920;
				c.max_frame_height = size ? (codec == kCodecAvc ? 2176 : 2160) : 1088;
				c.max_dpb_frame_count = -1;
				c.decode_pipeline_depth = depth;
				c.compute_queue = codec == kCodecVp9 && g_vp9_queue ? g_vp9_queue : g_queue;
				c.cpu_affinity_mask = 0x3f;
				c.cpu_thread_priority = 700;
				c.optimize_progressive_video = true;
				VdecMemory m = {};
				m.this_size = sizeof(m);
				const int rc = sceVideodec2QueryDecoderMemoryInfo(&c, &m);
				OrbisLog("[hwdec] memory before full access: codec %u profile %u level %u %dx%d pipeline %u: %#x",
					codec, profile, level, c.max_frame_width, c.max_frame_height, depth, unsigned(rc));
				if (rc == 0)
					g_memory_queries.push_back({c, m});
			}
	}
}

bool CachedMemory(VdecConfig& config, VdecMemory& memory)
{
	const DecoderMemoryQuery* best = nullptr;
	for (const auto& q : g_memory_queries)
	{
		const auto& c = q.config;
		if (c.codec_type == config.codec_type && c.profile == config.profile && c.max_level == config.max_level &&
			c.decode_pipeline_depth == config.decode_pipeline_depth && c.compute_queue == config.compute_queue &&
			c.max_frame_width >= config.max_frame_width && c.max_frame_height >= config.max_frame_height &&
			(!best || int64_t(c.max_frame_width) * c.max_frame_height <
				int64_t(best->config.max_frame_width) * best->config.max_frame_height))
			best = &q;
	}
	if (!best)
		return false;
	config = best->config; // CreateDecoder must receive exactly the configuration whose memory was queried.
	memory = best->memory;
	return true;
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
// ---- VP9: a block's frames (one, or a superframe's), and the start of a frame's header ----
// A superframe ends with an index: a marker byte 110sssnn (nn + 1 frames, each size in sss + 1 bytes, little-endian),
// the sizes, the marker again. Returns the frames (0: an index that doesn't add up).
unsigned Vp9Frames(const uint8_t* p, size_t n, size_t off[8], size_t len[8])
{
	if (!n)
		return 0;
	const uint8_t m = p[n - 1];
	if ((m & 0xe0) == 0xc0)
	{
		const unsigned frames = (m & 7) + 1, bytes = ((m >> 3) & 3) + 1;
		const size_t index = 2 + size_t(bytes) * frames;
		if (n >= index && p[n - index] == m)
		{
			size_t at = 0;
			for (unsigned i = 0; i < frames; i++)
			{
				size_t size = 0;
				for (unsigned b = 0; b < bytes; b++)
					size |= size_t(p[n - index + 1 + i * bytes + b]) << (8 * b);
				if (!size || at + size > n - index)
					return 0;
				off[i] = at;
				len[i] = size;
				at += size;
			}
			return frames;
		}
	}
	off[0] = 0;
	len[0] = n;
	return 1;
}

// A frame's profile (0..3; 4: not a VP9 frame), key frame, shown (or an earlier one shown again); *bits: a key frame's
// bit depth (8, 10, 12; 0 when the header doesn't say)
unsigned Vp9Header(const uint8_t* p, size_t n, bool* key, bool* shows, int* bits = nullptr)
{
	*key = *shows = false;
	if (bits)
		*bits = 0;
	if (n < 1 || (p[0] >> 6) != 2) // frame_marker
		return 4;
	unsigned bit = 2;
	auto get = [&]() -> unsigned {
		const unsigned v = bit < n * 8 ? (p[bit / 8] >> (7 - bit % 8)) & 1 : 0;
		bit++;
		return v;
	};
	unsigned profile = get();
	profile |= get() << 1;
	if (profile == 3)
		get(); // reserved_zero
	if (get()) // show_existing_frame
	{
		*shows = true;
		return profile;
	}
	*key = get() == 0; // frame_type 0: KEY_FRAME
	*shows = get() != 0;
	if (*key && bits)
	{
		get(); // error_resilient_mode
		unsigned sync = 0;
		for (int i = 0; i < 24; i++)
			sync = (sync << 1) | get();
		if (sync == 0x498342) // frame_sync_code
			*bits = profile >= 2 ? (get() ? 12 : 10) : 8;
	}
	return profile;
}

bool IsKeyframe(const uint8_t* p, size_t n, uint32_t codec)
{
	if (codec == kCodecVp9)
	{
		size_t off[8], len[8];
		bool key = false, shows = false;
		if (Vp9Frames(p, n, off, len))
			Vp9Header(p + off[0], len[0], &key, &shows);
		return key;
	}
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

// 1.6: the decoder's picture buffers, shared with the frames handed out in place (SetZeroCopy). The decoder gives its
// block to the pool when it closes; the block is freed when the last frame goes (after the GPU's import of it, on the
// drawing thread: CollectPools).
struct Pool
{
	uint8_t* base = nullptr;
	size_t bytes = 0;
	size_t frame_size = 0;
	std::atomic<bool> ref[kMaxFrames] = {};
	Mem mem; // set when the decoder closes
	std::mutex mu;
	std::condition_variable freed; // review M2: a frame handed out in place let go (ReleaseRef)
	void* import = nullptr;
	bool import_failed = false;
	~Pool();
};
std::mutex g_grave_mu;
std::vector<std::pair<void*, Mem>> g_grave; // (import, memory) to free on the drawing thread
std::mutex g_pools_mu;
std::vector<std::weak_ptr<Pool>> g_pools; // (final review 2) the blocks in use, for DropImports

Pool::~Pool()
{
	if (!mem.addr)
		return;
	if (import)
	{
		std::lock_guard<std::mutex> l(g_grave_mu);
		g_grave.push_back({import, mem});
	}
	else
		MemFree(mem);
}

struct PoolRef
{
	std::shared_ptr<Pool> pool;
	unsigned slot;
};

void ReleaseRef(void* opaque, uint8_t*)
{
	PoolRef* r = static_cast<PoolRef*>(opaque);
	{
		std::lock_guard<std::mutex> l(r->pool->mu);
		r->pool->ref[r->slot] = false;
	}
	r->pool->freed.notify_all();
	delete r;
}

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
	std::shared_ptr<Pool> pool; // 1.6: frames handed out in place hold their buffer through it
	bool zero_copy = false;     // this decoder hands pictures out in place
	unsigned in_place = 0;
	unsigned depth = 4;         // decode_pipeline_depth
	unsigned max_held = 0;      // review M2: the most picture buffers the decoder itself has held at once
	bool draining = false;      // (review L-b) the stream's end: its last pictures copied (no buffer held by them)
	Hooks hooks;
	// review L1: the stream's mastering display and content light level, put on every picture (SourcePeak reads them)
	std::vector<uint8_t> mastering, light;
	unsigned sei_scans = 0; // access units looked through for them (HEVC's SEI) while the container gave none
	AVChromaLocation chroma_loc = AVCHROMA_LOC_UNSPECIFIED;
	unsigned au_next = 0;

	std::vector<uint8_t> headers; // parameter sets, Annex B
	bool send_headers = true;
	unsigned nal_length = 0; // avcC / hvcC packets: length bytes (0: Annex B)

	struct Pend
	{
		int64_t pts;
		bool hidden; // VP9: a block with no frame shown (its picture is dropped)
	};
	std::vector<Pend> pending; // pictures come out in display order: each takes the earliest time waiting (VP9: in turn)
	bool vp9_checked = false;  // VP9: the first frame's profile looked at (the decoder made again for it if needed)
	unsigned vp9_no_picture = 0; // VP9: blocks that gave no picture (no buffer taken)
	bool vp9_key_seen = false;   // VP9: the first key frame's bit depth looked at
	unsigned vp9_hidden_dropped = 0;
	uint32_t profile = 0;
	VdecConfig config = {};
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
	double copied_ms = 0; // (1.6) the copies' own time, summed
	unsigned copied = 0;
};

namespace
{
// (review L1) HEVC's HDR10 metadata from the stream itself: the prefix SEI messages mastering_display_colour_volume
// (137) and content_light_level_info (144), as FFmpeg's decoder reads them, into the frames' side data (the container
// often has none). An access unit in Annex B.
void ScanHevcSei(Decoder* d, const uint8_t* p, size_t n)
{
	for (size_t i = 0; i + 5 < n; i++)
	{
		if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
			continue;
		const uint8_t* nal = p + i + 3;
		size_t end = i + 3;
		while (end + 2 < n && !(p[end] == 0 && p[end + 1] == 0 && (p[end + 2] == 1 || p[end + 2] == 0)))
			end++;
		if (end + 2 >= n)
			end = n;
		i = end - 1;
		if (((nal[0] >> 1) & 0x3f) != 39) // prefix SEI
			continue;
		// the RBSP: the two header bytes off, emulation prevention bytes (00 00 03) dropped
		std::vector<uint8_t> r;
		for (const uint8_t* q = nal + 2; q < p + end; q++)
		{
			if (r.size() >= 2 && r[r.size() - 1] == 0 && r[r.size() - 2] == 0 && *q == 3)
				continue;
			r.push_back(*q);
		}
		size_t at = 0;
		while (at + 2 <= r.size() && r[at] != 0x80) // rbsp trailing bits
		{
			unsigned type = 0, size = 0;
			while (at < r.size() && r[at] == 0xff)
				type += 255, at++;
			if (at >= r.size())
				break;
			type += r[at++];
			while (at < r.size() && r[at] == 0xff)
				size += 255, at++;
			if (at >= r.size())
				break;
			size += r[at++];
			if (at + size > r.size())
				break;
			const uint8_t* b = r.data() + at;
			auto u16 = [&](size_t k) { return unsigned(b[k] << 8 | b[k + 1]); };
			auto u32 = [&](size_t k) { return uint32_t(b[k]) << 24 | uint32_t(b[k + 1]) << 16 | uint32_t(b[k + 2]) << 8 | b[k + 3]; };
			if (type == 137 && size >= 24 && d->mastering.empty())
			{
				AVMasteringDisplayMetadata m = {};
				static const int kRgb[3] = {2, 0, 1}; // the SEI's G, B, R into R, G, B (as FFmpeg's decoder does)
				for (int c = 0; c < 3; c++)
				{
					m.display_primaries[c][0] = av_make_q(int(u16(kRgb[c] * 4)), 50000);
					m.display_primaries[c][1] = av_make_q(int(u16(kRgb[c] * 4 + 2)), 50000);
				}
				m.white_point[0] = av_make_q(int(u16(12)), 50000);
				m.white_point[1] = av_make_q(int(u16(14)), 50000);
				m.max_luminance = av_make_q(int(u32(16) & 0x7fffffff), 10000);
				m.min_luminance = av_make_q(int(u32(20) & 0x7fffffff), 10000);
				m.has_primaries = m.has_luminance = 1;
				const uint8_t* mb = reinterpret_cast<const uint8_t*>(&m);
				d->mastering.assign(mb, mb + sizeof(m));
				OrbisLog("[hwdec] the stream's mastering display: up to %.0f nits", av_q2d(m.max_luminance));
			}
			else if (type == 144 && size >= 4 && d->light.empty())
			{
				AVContentLightMetadata l = {};
				l.MaxCLL = u16(0);
				l.MaxFALL = u16(2);
				const uint8_t* lb = reinterpret_cast<const uint8_t*>(&l);
				d->light.assign(lb, lb + sizeof(l));
				OrbisLog("[hwdec] the stream's content light level: MaxCLL %u, MaxFALL %u nits", l.MaxCLL, l.MaxFALL);
			}
			at += size;
		}
	}
}

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

// the time of the picture that came out; *hidden: VP9's picture of a block with no frame shown
int64_t PendingTake(Decoder* d, bool* hidden = nullptr)
{
	if (hidden)
		*hidden = false;
	if (d->pending.empty())
		return AV_NOPTS_VALUE;
	if (d->codec == kCodecVp9)
	{
		// VP9's pictures come out in the order its blocks went in, one per block, shown or not (VLC for PS5's runs)
		const Decoder::Pend p = d->pending.front();
		d->pending.erase(d->pending.begin());
		if (hidden)
			*hidden = p.hidden;
		return p.pts;
	}
	auto it = std::min_element(d->pending.begin(), d->pending.end(),
		[](const Decoder::Pend& a, const Decoder::Pend& b) { return a.pts < b.pts; });
	const int64_t t = it->pts;
	*it = d->pending.back();
	d->pending.pop_back();
	return t;
}

void PendingAdd(Decoder* d, int64_t t, bool hidden = false)
{
	if (d->pending.size() >= kMaxPending)
		d->pending.erase(d->pending.begin());
	d->pending.push_back({t, hidden});
}



// frame_format values seen on a console with a right picture, and their sample size (see Output). Empty until a
// console run reports them.
struct ConfirmedFormat
{
	uint32_t format;
	unsigned bytes_per_sample;
};
constexpr ConfirmedFormat kConfirmedFormats[] = {{0xffffffffu, 0}}; // (a placeholder no picture has)

// (1.6) Rows of a copy split over this thread and kCopyHelpers others (made once, at the first big picture)
constexpr int kCopyHelpers = 3;
struct CopyPool
{
	std::mutex mu;
	std::condition_variable go, done;
	const std::function<void(int, int)>* job = nullptr;
	int rows = 0, parts = 0, next = 0, finished = 0;
	uint64_t generation = 0;
	bool stop = false;
	std::vector<ps5::BigThread> threads; // (test4 review: named, a stack of their own, the crash report armed, joined)

	void Work(int part)
	{
		const int r0 = rows * part / parts, r1 = rows * (part + 1) / parts;
		(*job)(r0, r1);
	}
	void Helper()
	{
		uint64_t seen = 0;
		std::unique_lock<std::mutex> l(mu);
		for (;;)
		{
			go.wait(l, [&] { return stop || generation != seen; });
			if (stop)
				return;
			seen = generation;
			while (next < parts)
			{
				const int p = next++;
				l.unlock();
				Work(p);
				l.lock();
				if (++finished == parts)
					done.notify_all();
			}
		}
	}
};
CopyPool* g_copy_pool = nullptr;
std::mutex g_copy_mu; // one split copy at a time (the decoding thread's)

// (wait: false -- the drawing thread's CpuCopy: when the decoding thread's copy has the helpers, this one copies alone
// rather than wait for it)
void ParallelRows(int rows, const std::function<void(int, int)>& fn, bool wait = true)
{
	std::unique_lock<std::mutex> one(g_copy_mu, std::defer_lock);
	if (wait)
		one.lock();
	else if (!one.try_lock())
	{
		fn(0, rows);
		return;
	}
	if (!g_copy_pool)
	{
		g_copy_pool = new CopyPool; // (its threads wait for work until StopCopyHelpers)
		for (int i = 0; i < kCopyHelpers; i++)
		{
			ps5::BigThread t([] { g_copy_pool->Helper(); }, size_t(256) << 10, "copy");
			if (!t.joinable())
				break;
			g_copy_pool->threads.push_back(std::move(t));
		}
	}
	CopyPool& p = *g_copy_pool;
	std::unique_lock<std::mutex> l(p.mu);
	p.job = &fn;
	p.rows = rows;
	p.parts = int(p.threads.size()) + 1;
	p.next = 0;
	p.finished = 0;
	p.generation++;
	p.go.notify_all();
	while (p.next < p.parts)
	{
		const int part = p.next++;
		l.unlock();
		p.Work(part);
		l.lock();
		p.finished++;
	}
	p.done.wait(l, [&] { return p.finished == p.parts; });
	p.job = nullptr;
}

// A picture of the decoder's layout (luma rows of `pitch` bytes, then the chroma after `rows` luma rows) copied into an
// FFmpeg frame: NV12, or P010 (the 10 bits moved to the top). (review M-A) The same two planes as a picture handed out
// in place, so the GPU's plane images serve both and a film switching between them never makes new ones. nullptr: no
// memory.
AVFrame* CopyPicture(const uint8_t* src, size_t pitch, int rows, int w, int h, bool ten, bool wait = true)
{
	AVFrame* f = av_frame_alloc();
	if (f)
	{
		f->format = ten ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
		f->width = w;
		f->height = h;
	}
	if (!f || !bigmem::AllocFrame(f)) // (test12: a 4K copy is 12-25 MiB: direct memory, not the heap)
	{
		av_frame_free(&f);
		return nullptr;
	}
	// the chroma follows the decoder's luma rows (frame_height: 1088 for a padded 1080)
	const uint8_t* chroma = src + pitch * size_t(rows);
	const int crows = (h + 1) / 2;
	const size_t luma_bytes = std::min(pitch, size_t(w) * (ten ? 2 : 1));
	const size_t cbytes = std::min(pitch, size_t((w + 1) / 2) * 2 * (ten ? 2 : 1));
	auto rows_of = [&](uint8_t* dst, int dpitch, const uint8_t* s, int n, size_t bytes) {
		for (int r = 0; r < n; r++)
		{
			if (!ten)
			{
				memcpy(dst + size_t(r) * dpitch, s + size_t(r) * pitch, bytes);
				continue;
			}
			const uint16_t* a = reinterpret_cast<const uint16_t*>(s + size_t(r) * pitch);
			uint16_t* b = reinterpret_cast<uint16_t*>(dst + size_t(r) * dpitch);
			for (size_t i = 0; i < bytes / 2; i++)
				b[i] = uint16_t(a[i] << 6);
		}
	};
	// (1.6) a big picture's rows split over the copy threads (as VLC for PS5 splits its copy): luma and chroma rows as
	// one range, chroma after luma
	auto part = [&](int r0, int r1) {
		const int l1 = std::min(r1, h);
		if (r0 < l1)
			rows_of(f->data[0] + size_t(r0) * f->linesize[0], f->linesize[0], src + size_t(r0) * pitch, l1 - r0, luma_bytes);
		const int c0 = std::max(r0, h) - h, c1 = r1 - h;
		if (c0 < c1)
			rows_of(f->data[1] + size_t(c0) * f->linesize[1], f->linesize[1], chroma + size_t(c0) * pitch, c1 - c0, cbytes);
	};
#ifndef __PROSPERO__
	// host measurements: MPCPS5_HOST_COPY_SPLIT=0 copies on the decoding thread alone (as up to test2)
	static const bool split = !(getenv("MPCPS5_HOST_COPY_SPLIT") && *getenv("MPCPS5_HOST_COPY_SPLIT") == '0');
#else
	constexpr bool split = true;
#endif
	if (split && size_t(w) * h >= size_t(1280) * 720)
		ParallelRows(h + crows, part, wait);
	else
		part(0, h + crows);
	return f;
}

// 1.6: room to hand this picture out in place. (review M-A) Against the pictures already out in place only: with this
// one out too, the decoder must still have the most buffers it has held at once (max_held, measured with its next
// picture's buffer taken: what it needs to go on) and kZeroCopySlack more. The buffers it holds now are part of that
// count, not besides it. Not while paused (nothing is shown: they would stay held) nor at the stream's end.
bool ZeroCopyRoom(const Decoder* d)
{
#ifndef __PROSPERO__
	// host tests: MPCPS5_HOST_INPLACE_PAUSED=1: in place while paused too (a paused seek's picture read in place)
	static const bool paused_ok = getenv("MPCPS5_HOST_INPLACE_PAUSED") && *getenv("MPCPS5_HOST_INPLACE_PAUSED") == '1';
#else
	constexpr bool paused_ok = false;
#endif
	if (d->draining || (!paused_ok && d->hooks.paused && d->hooks.paused(d->hooks.ctx)))
		return false;
	unsigned refs = 0, held = 0;
	for (unsigned i = 0; i < d->frame_count; i++)
	{
		refs += d->pool->ref[i] ? 1 : 0;
		held += d->frame_held[i] ? 1 : 0;
	}
	return refs + std::max(d->max_held, held) + kZeroCopySlack + 1 <= d->frame_count;
}


// One decoded picture into an FFmpeg frame (NV12, or yuv420p10le with the 10 bits at the bottom as they come); 1.6: or
// handed out in place (SetZeroCopy), the frame pointing into the decoder's buffer.
void Output(Decoder* d, const VdecOutput& o)
{
	bool hidden = false;
	const int64_t pts = PendingTake(d, &hidden);
	if (hidden && !o.is_error_frame && o.frame_buffer)
	{
		// VP9: the picture of a block that shows no frame (a reference only): not shown
		d->vp9_hidden_dropped++;
		FrameBack(d, o.frame_buffer);
		return;
	}
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
	const uint8_t* src = static_cast<const uint8_t*>(o.frame_buffer);
	const size_t pitch = o.frame_pitch_in_bytes;
	AVFrame* f = nullptr;
	if (d->zero_copy && d->pool && ZeroCopyRoom(d))
	{
		// in place: the frame is the decoder's buffer; the slot is the decoder's again when the last reference goes
		const unsigned slot = unsigned(size_t(buf - mem) / d->frame_size);
		PoolRef* ref = new (std::nothrow) PoolRef{d->pool, slot};
		AVBufferRef* info = av_buffer_allocz(sizeof(ZeroCopyInfo));
		f = ref && info ? av_frame_alloc() : nullptr;
		AVBufferRef* b = f ? av_buffer_create(const_cast<uint8_t*>(src), d->frame_size, ReleaseRef, ref, 0) : nullptr;
		if (b)
		{
			d->pool->ref[slot] = true;
			ZeroCopyInfo* z = reinterpret_cast<ZeroCopyInfo*>(info->data);
			*z = {kZeroCopyMagic, d->pool.get(), d->pool->base, d->pool->bytes, ten};
			f->format = ten ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12; // (P010's word size; the bits: ZeroCopyInfo)
			f->width = w;
			f->height = h;
			f->data[0] = const_cast<uint8_t*>(src);
			f->linesize[0] = int(pitch);
			f->data[1] = const_cast<uint8_t*>(src) + pitch * o.frame_height;
			f->linesize[1] = int(pitch);
			f->buf[0] = b;
			f->opaque_ref = info;
			d->in_place++;
		}
		else
		{
			delete ref;
			av_buffer_unref(&info);
			av_frame_free(&f);
		}
	}
	if (!f)
	{
		const double c0 = av_gettime_relative() / 1e3;
		f = CopyPicture(src, pitch, int(o.frame_height), w, h, ten);
		d->copied_ms += av_gettime_relative() / 1e3 - c0;
		d->copied++;
	}
	FrameBack(d, o.frame_buffer);
	if (!f)
		return;
	f->pts = pts;
	f->best_effort_timestamp = pts;
	f->pkt_dts = AV_NOPTS_VALUE;
	f->color_primaries = d->prim;
	f->color_trc = d->trc;
	f->colorspace = d->space;
	f->color_range = d->range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : d->range;
	f->sample_aspect_ratio = d->sar;
	f->chroma_location = d->chroma_loc;
	if (d->mastering.size() == sizeof(AVMasteringDisplayMetadata))
		if (AVFrameSideData* sd = av_frame_new_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, d->mastering.size()))
			memcpy(sd->data, d->mastering.data(), d->mastering.size());
	if (d->light.size() == sizeof(AVContentLightMetadata))
		if (AVFrameSideData* sd = av_frame_new_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, d->light.size()))
			memcpy(sd->data, d->light.data(), d->light.size());
	d->out.push_back(f);
	d->shown++;
	d->copy_ms = d->copy_ms * 0.95 + (av_gettime_relative() / 1e6 - t0) * 1000 * 0.05;
	if (d->shown % 480 == 0)
		OrbisLog("[hwdec] %u pictures (%u in place); decode %.1f ms, copy %.1f ms each", d->shown, d->in_place, d->decode_ms,
			d->copy_ms);
}

int NextFrame(Decoder* d, VdecFrameBuffer* fb)
{
	for (unsigned i = 0; i < d->frame_count; i++)
		if (!d->frame_held[i] && !(d->pool && d->pool->ref[i]))
		{
			*fb = VdecFrameBuffer{};
			fb->this_size = sizeof(*fb);
			fb->frame_buffer = static_cast<uint8_t*>(d->frames.addr) + i * d->frame_size;
			fb->frame_buffer_size = d->frame_size;
			return int(i);
		}
	return -1;
}

// (review M2, L-b) Every buffer held, by the decoder or by pictures out in place: wait (up to 3 s) for one to be let go
// (they are as they are shown), looking every 50 ms whether the player quits or seeks. The slot, or -1.
int WaitFrame(Decoder* d, VdecFrameBuffer* fb)
{
	int frame = NextFrame(d, fb);
	if (frame >= 0 || !d->pool)
		return frame;
	for (int i = 0; i < 60 && frame < 0; i++)
	{
		if (d->hooks.stop && d->hooks.stop(d->hooks.ctx))
			return -1;
		// (final review 2) paused: nothing is shown, so no buffer can come back before playback goes on or a seek; the
		// wait goes on without counting toward its limit (a seek or the exit still end it at once)
		if (d->hooks.paused && d->hooks.paused(d->hooks.ctx))
			i = 0;
		bool any_ref = false;
		for (unsigned k = 0; k < d->frame_count; k++)
			any_ref = any_ref || d->pool->ref[k];
		if (!any_ref)
			return -1; // the decoder holds them all itself: nothing to wait for
		std::unique_lock<std::mutex> l(d->pool->mu);
		d->pool->freed.wait_for(l, std::chrono::milliseconds(50), [&] {
			for (unsigned k = 0; k < d->frame_count; k++)
				if (!d->frame_held[k] && !d->pool->ref[k])
					return true;
			return false;
		});
		l.unlock();
		frame = NextFrame(d, fb);
	}
	return frame;
}

void Drain(Decoder* d, bool show)
{
	// (review L-b) the stream's end: its last pictures copied (none left holding a buffer the next one needs), and a
	// buffer waited for when the shown ones hold them all, so none of them is lost
	d->draining = show;
	for (int i = 0; i < 32; i++)
	{
		VdecFrameBuffer fb;
		const int slot = show ? WaitFrame(d, &fb) : NextFrame(d, &fb);
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
	if (d->pool && d->frames.addr)
	{
		// frames handed out in place may still point into it: the pool frees it after the last one
		d->pool->mem = d->frames;
		d->frames = Mem();
	}
	d->pool.reset();
	MemFree(d->frames);
	MemFree(d->cpu_gpu);
	MemFree(d->gpu);
	MemFree(d->cpu);
}

int BringUp(Decoder* d, VdecConfig& config, VdecMemory& mem)
{
	mem = VdecMemory{};
	mem.this_size = sizeof(mem);
	int rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem);
	if (rc != 0)
	{
		OrbisLog("[hwdec] memory query %#x", unsigned(rc));
		if (!CachedMemory(config, mem))
			return rc;
		OrbisLog("[hwdec] using memory queried before full access: %dx%d, pipeline %u",
			config.max_frame_width, config.max_frame_height, config.decode_pipeline_depth);
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
	// (VLC for PS5's console runs: the library and the compute queue only before the helper's full access) called at the
	// start, before the jailbreak
	if (!MPC_SLOT(sceSysmoduleLoadModule))
	{
		OrbisLog("[hwdec] console decoder: off (sceSysmoduleLoadModule isn't bound in this eboot; the [imports] lines "
				 "after this one list what the console bound)");
		return;
	}
	const int rc = sceSysmoduleLoadModule(kSysmoduleVideodec2);
	const char* missing = nullptr;
	const bool resolved = rc == 0 && Resolved(&missing);
	if (!resolved)
	{
		if (rc != 0)
			OrbisLog("[hwdec] console decoder: off (its library didn't load: %#x)", unsigned(rc));
		else
			OrbisLog("[hwdec] console decoder: off (its library loaded, but %s isn't bound)", missing ? missing : "?");
		return;
	}
	// the compute queues now, while still in the sandbox
	g_ready = ComputeQueue() != nullptr;
	if (g_ready)
	{
		Vp9Queue();
		PrepareMemoryQueries();
	}
	OrbisLog("[hwdec] console decoder: %s", g_ready ? "on (H.264, HEVC 8 / 10-bit, VP9 8 / 10-bit, up to 4K)"
												  : "off (its compute queue couldn't be made)");
}

bool Available()
{
	return g_ready;
}

void ReleaseQueues(const char* why)
{
	std::lock_guard<std::mutex> l(g_queue_mu);
	int rc1 = 0, rc2 = 0;
	// (not required of the eboot: an unbound slot means no release -- the queues then just stay)
	const bool bound = MPC_SLOT(sceVideodec2ReleaseComputeQueue) != 0;
	if (g_vp9_queue)
		rc2 = bound ? sceVideodec2ReleaseComputeQueue(g_vp9_queue) : -1;
	if (g_queue)
		rc1 = bound ? sceVideodec2ReleaseComputeQueue(g_queue) : -1;
	OrbisLog("[hwdec] compute queues released (%#x, VP9 %#x): %s; the console's decoder is off for this run", unsigned(rc1),
		unsigned(rc2), why);
	g_queue = nullptr;
	g_vp9_queue = nullptr;
	g_vp9_queue_tried = true;
	g_ready = false;
}

bool QueuesExist()
{
	std::lock_guard<std::mutex> l(g_queue_mu);
	return g_queue != nullptr || g_vp9_queue != nullptr;
}

void SetEnabled(bool on)
{
	g_enabled = on;
}

bool Enabled()
{
	return g_enabled;
}

// The pool frames handed out in place hold (zero copy)
void MakePool(Decoder* d)
{
	if (!d->zero_copy)
		return;
	d->pool = std::make_shared<Pool>();
	{
		std::lock_guard<std::mutex> l(g_pools_mu);
		g_pools.erase(std::remove_if(g_pools.begin(), g_pools.end(), [](const std::weak_ptr<Pool>& w) { return w.expired(); }),
			g_pools.end());
		g_pools.push_back(d->pool);
	}
	d->pool->base = static_cast<uint8_t*>(d->frames.addr);
	d->pool->bytes = d->frames.len;
	d->pool->frame_size = d->frame_size;
	OrbisLog("[hwdec] pictures handed out in place (%u buffers of %zu KiB; the decoder keeps the most it holds, its "
			 "next picture's and %u more: the others copied)", d->frame_count, d->frame_size >> 10, kZeroCopySlack);
}

// VP9: the decoder made again for the profile the first frame has (a decoder made for the other refused every frame,
// in VLC for PS5's runs). Before any picture came out.
bool Vp9Rebuild(Decoder* d, uint32_t profile)
{
	if (d->dec)
		sceVideodec2DeleteDecoder(d->dec);
	d->dec = nullptr;
	for (Mem& m : d->au)
		MemFree(m);
	if (d->pool && d->frames.addr)
	{
		d->pool->mem = d->frames; // (freed with the pool: nothing holds it yet)
		d->frames = Mem();
	}
	d->pool.reset();
	MemFree(d->frames);
	MemFree(d->cpu_gpu);
	MemFree(d->gpu);
	MemFree(d->cpu);
	memset(d->frame_held, 0, sizeof(d->frame_held));
	VdecConfig c = d->config;
	c.profile = profile;
	c.max_level = 62;
	VdecMemory mem;
	int rc = -1;
	for (uint32_t depth : {4u, 1u})
	{
		c.decode_pipeline_depth = depth;
		rc = BringUp(d, c, mem);
		if (rc == 0)
			break;
	}
	OrbisLog("[hwdec] VP9 stream is profile %u (%s): decoder made again: %#x (pipeline %u)", profile,
		profile == 2 ? "10-bit" : "8-bit", unsigned(rc), c.decode_pipeline_depth);
	if (rc != 0)
		return false;
	d->config = c;
	d->profile = profile;
	d->depth = c.decode_pipeline_depth;
	d->au_next = 0;
	MakePool(d);
	return true;
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
	else if (par->codec_id == AV_CODEC_ID_VP9)
	{
		// profiles 0 (8-bit) and 2 (10-bit) 4:2:0; 1 and 3 (4:2:2, 4:4:4) stay with FFmpeg. The first frame says which
		// it is when the file doesn't (Send)
		if (par->profile == AV_PROFILE_VP9_1 || par->profile == AV_PROFILE_VP9_3)
			return no("VP9 4:2:2 / 4:4:4");
		// (profile 2 is 10 or 12-bit: the decoder takes 10)
		const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(AVPixelFormat(par->format));
		if ((pd && pd->comp[0].depth > 10) || par->bits_per_raw_sample > 10)
			return no("VP9 12-bit");
		codec = kCodecVp9;
		profile = par->profile == AV_PROFILE_VP9_2 ? 2 : 0;
		level = 62;
	}
	else
		return no("not H.264, HEVC or VP9");
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
	if (codec == kCodecVp9 && g_vp9_queue)
		queue = g_vp9_queue;

	Decoder* d = new Decoder;
	d->codec = codec;
	VdecConfig config = {};
	config.this_size = sizeof(config);
	config.resource_type = 1;
	config.codec_type = codec;
	config.profile = profile;
	config.max_level = level;
	// (test12; test11's PS5 log) a size under 1920x1088 fails the memory query (0x811d0200: a 720x1280 phone video),
	// 1080p never does -- VLC for PS5 saw the same and asks for room for 1920x1088 at least; so does this
	config.max_frame_width = std::max<int32_t>(w ? int32_t((w + 15) & ~15) : 3840, 1920);
	config.max_frame_height = std::max<int32_t>(h ? int32_t((h + 15) & ~15) : (codec == kCodecAvc ? 2176 : 2160), 1088);
	if (w && h && (w < 1920 || h < 1088))
		OrbisLog("[hwdec] %dx%d: the decoder is made with room for %dx%d (smaller pictures fail its memory query)", w, h,
			config.max_frame_width, config.max_frame_height);
	config.max_dpb_frame_count = -1;    // the decoder sizes its picture store for the stream
	config.decode_pipeline_depth = 4;   // one at a time left it idle: 20 pictures a second for a 4K film needing 24
	config.compute_queue = queue;
	config.cpu_affinity_mask = 0x3f;
	config.cpu_thread_priority = 700;
	config.optimize_progressive_video = true;
	// 1.6: pictures handed out in place hold buffers until they are shown: 4K gets all kMaxFrames then (10 if that
	// much memory isn't there; ZeroCopyRoom keeps what the decoder needs)
	d->zero_copy = g_zero_copy;
	d->frame_count = small || d->zero_copy ? kMaxFrames : 10;
	VdecMemory mem;
	int rc = BringUp(d, config, mem);
	if (rc != 0 && d->frame_count > 10)
	{
		OrbisLog("[hwdec] %u picture buffers failed (%#x): trying 10", d->frame_count, unsigned(rc));
		d->frame_count = 10;
		rc = BringUp(d, config, mem);
	}
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
	if (rc != 0 && codec == kCodecVp9)
	{
		// VP9's values one after another until one is taken: its profile as VP9 numbers it (the other one next), levels
		// as ten times VP9's
		static const uint32_t kLevels[] = {62, 61, 52, 51};
		const uint32_t profiles[2] = {profile, profile == 2 ? 0u : 2u};
		for (int pi = 0; pi < 2 && rc != 0; pi++)
			for (uint32_t lv : kLevels)
			{
				if (rc == 0)
					break;
				config.profile = profiles[pi];
				config.max_level = lv;
				config.decode_pipeline_depth = 1;
				rc = BringUp(d, config, mem);
				OrbisLog("[hwdec] VP9 profile %u level %u (pipeline 1): %#x", config.profile, lv, unsigned(rc));
			}
	}
	if (rc != 0)
	{
		delete d;
		return no("the decoder couldn't be created");
	}
	d->config = config;
	d->profile = config.profile;
	MakePool(d);
	if (codec != kCodecVp9) // (VP9's extra data is its codec features, not for the decoder)
		ReadHeaders(d, par->extradata, size_t(par->extradata_size));
	d->vis_w = w;
	d->vis_h = h;
	if (codec == kCodecVp9)
		d->expect_bps = 0; // (the first frame's profile says: Send)
	else if (codec == kCodecAvc || par->format == AV_PIX_FMT_YUV420P || par->format == AV_PIX_FMT_YUVJ420P ||
		(par->format == AV_PIX_FMT_NONE && par->profile == AV_PROFILE_HEVC_MAIN))
		d->expect_bps = 1;
	else if (par->format == AV_PIX_FMT_YUV420P10LE || par->profile == AV_PROFILE_HEVC_MAIN_10)
		d->expect_bps = 2;
	d->prim = par->color_primaries;
	d->trc = par->color_trc;
	d->space = par->color_space;
	d->range = par->color_range;
	d->sar = par->sample_aspect_ratio;
	d->chroma_loc = par->chroma_location;
	d->depth = config.decode_pipeline_depth;
	// (review L1) the HDR metadata the container carries (FFmpeg's decoder takes it from the stream's SEI; this one
	// hands out pictures only): without it every HDR10 film would be tone mapped from 1000 nits
	for (int i = 0; i < par->nb_coded_side_data; i++)
	{
		const AVPacketSideData& sd = par->coded_side_data[i];
		if (sd.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA && sd.size == sizeof(AVMasteringDisplayMetadata))
			d->mastering.assign(sd.data, sd.data + sd.size);
		else if (sd.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL && sd.size == sizeof(AVContentLightMetadata))
			d->light.assign(sd.data, sd.data + sd.size);
	}
	if (!d->mastering.empty() || !d->light.empty())
		OrbisLog("[hwdec] the stream's HDR metadata:%s%s", d->mastering.empty() ? "" : " mastering display",
			d->light.empty() ? "" : " content light level");
	char buf[128];
	snprintf(buf, sizeof(buf), "%s %dx%d (console decoder)", codec == kCodecAvc ? "H.264" : codec == kCodecHevc ? "HEVC" : "VP9",
		w, h);
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
	d->draining = false;
	const bool hevc = d->codec == kCodecHevc;
	const bool vp9 = d->codec == kCodecVp9;
	const bool marked_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
	if (vp9 && !d->vp9_checked)
	{
		// the stream's own profile from its first frame (the file rarely says), before the block is copied: a rebuild
		// frees the access unit slots
		d->vp9_checked = true;
		bool key = false, shows = false;
		const unsigned prof = Vp9Header(pkt->data, size_t(pkt->size), &key, &shows);
		if (prof == 1 || prof == 3)
		{
			GiveUp(d, "VP9 4:2:2 or 4:4:4");
			return AVERROR_EXTERNAL;
		}
		if (prof <= 2 && prof != d->profile && !Vp9Rebuild(d, prof))
		{
			GiveUp(d, "no VP9 decoder for this profile");
			return AVERROR_EXTERNAL;
		}
		if (prof <= 2)
			d->expect_bps = prof == 2 ? 2 : 1;
	}
	if (vp9 && !d->vp9_key_seen)
	{
		// (test5 review) the bit depth from the first key frame (its colour config): 12-bit isn't the decoder's
		size_t off[8], len[8];
		const unsigned frames = Vp9Frames(pkt->data, size_t(pkt->size), off, len);
		for (unsigned i = 0; i < frames; i++)
		{
			bool key = false, shows = false;
			int bits = 0;
			Vp9Header(pkt->data + off[i], len[i], &key, &shows, &bits);
			if (!key)
				continue;
			d->vp9_key_seen = true;
			if (bits == 12)
			{
				GiveUp(d, "VP9 12-bit");
				return AVERROR_EXTERNAL;
			}
			break;
		}
	}
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
	if (vp9)
	{
		// VP9's blocks go in as they are (a superframe whole: one picture comes out of it)
		memcpy(au, pkt->data, size_t(pkt->size));
		size = size_t(pkt->size);
	}
	else if (d->nal_length && !annexb)
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
	if (d->codec == kCodecHevc && (d->mastering.empty() || d->light.empty()) && d->sei_scans < 240)
	{
		d->sei_scans++;
		ScanHevcSei(d, static_cast<const uint8_t*>(au), size);
	}
	VdecInput in = {};
	in.this_size = sizeof(in);
	in.au_data = au;
	in.au_size = size;
	in.pts_data = uint64_t(pts);
	in.dts_data = UINT64_MAX;
	VdecFrameBuffer fb;
	int frame = NextFrame(d, &fb);
	if (frame < 0 && d->pool)
	{
		// (review M2) every buffer held by the decoder or by pictures handed out in place: wait for one of those to be
		// let go (they are as they are shown), and copy the pictures from now on rather than give the video up
		frame = WaitFrame(d, &fb);
		if (frame < 0 && d->hooks.stop && d->hooks.stop(d->hooks.ctx))
			return 0; // (review L-b) quitting or seeking: this access unit dropped, not waited on
		if (d->zero_copy)
		{
			d->zero_copy = false;
			OrbisLog("[hwdec] no picture buffer free (%u in place, the decoder held up to %u): pictures copied from now on, %s",
				d->in_place, d->max_held, frame >= 0 ? "decoding goes on" : "none came back");
		}
	}
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
	{
		unsigned held = 0;
		for (unsigned i = 0; i < d->frame_count; i++)
			held += d->frame_held[i];
		d->max_held = std::max(d->max_held, held);
	}
	if (rc != 0)
	{
		d->errors++;
		if (d->errors <= 5)
			OrbisLog("[hwdec] decode %#x (access unit %u, %zu bytes)", unsigned(rc), d->decoded, size);
		if (vp9)
		{
			// the chip decodes some VP9 encodes and refuses others, from the first refused frame on (VLC for PS5's runs):
			// FFmpeg at once, at the same place
			GiveUp(d, "a VP9 frame the console's decoder refuses");
			return AVERROR_EXTERNAL;
		}
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
	bool hidden = false;
	if (vp9)
	{
		// one picture per block, shown or not: a block none of whose frames is shown gives one to drop
		size_t off[8], len[8];
		const unsigned frames = Vp9Frames(au, size, off, len);
		bool shows = frames == 0;
		for (unsigned i = 0; i < frames; i++)
		{
			bool key = false, sh = false;
			Vp9Header(au + off[i], len[i], &key, &sh);
			shows = shows || sh;
		}
		hidden = !shows;
		// (test4 review) a block whose buffer the decoder didn't take and that gave no picture never gives one (a
		// picture needs a buffer): it gets no time of its own, so the times stay paired with the pictures whether
		// the decoder gives a picture for a block showing no frame (as VLC for PS5 found the console does) or not
		// (as FFmpeg doesn't)
		if (!fb.is_accepted && !o.is_valid)
		{
			d->vp9_no_picture++;
			if (d->vp9_no_picture == 1)
				OrbisLog("[hwdec] VP9: a block%s gave no picture (none expected from it)", hidden ? " showing no frame" : "");
			return 0;
		}
	}
	PendingAdd(d, pts, hidden);
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

void SetHooks(Decoder* d, const Hooks& h)
{
	if (d)
		d->hooks = h;
}

void SetZeroCopy(bool on)
{
	g_zero_copy = on;
}

bool ZeroCopy()
{
	return g_zero_copy;
}

const ZeroCopyInfo* ZeroCopyOf(const AVFrame* f)
{
	if (!f || !f->opaque_ref || f->opaque_ref->size != sizeof(ZeroCopyInfo))
		return nullptr;
	const ZeroCopyInfo* z = reinterpret_cast<const ZeroCopyInfo*>(f->opaque_ref->data);
	return z->magic == kZeroCopyMagic ? z : nullptr;
}

void* PoolImport(const ZeroCopyInfo* z, bool* failed)
{
	Pool* p = static_cast<Pool*>(z->pool);
	std::lock_guard<std::mutex> l(p->mu);
	if (failed)
		*failed = p->import_failed;
	return p->import;
}

void SetPoolImport(const ZeroCopyInfo* z, void* import, bool failed)
{
	Pool* p = static_cast<Pool*>(z->pool);
	std::lock_guard<std::mutex> l(p->mu);
	p->import = import;
	p->import_failed = failed;
}

void CollectPools(void (*release)(void* import))
{
	std::vector<std::pair<void*, Mem>> dead;
	{
		std::lock_guard<std::mutex> l(g_grave_mu);
		dead.swap(g_grave);
	}
	for (auto& e : dead)
	{
		if (release)
			release(e.first);
		MemFree(e.second);
	}
}

void DropImports(void (*release)(void* import))
{
	std::vector<std::shared_ptr<Pool>> live;
	{
		std::lock_guard<std::mutex> l(g_pools_mu);
		for (auto& w : g_pools)
			if (auto p = w.lock())
				live.push_back(p);
	}
	for (auto& p : live)
	{
		void* import = nullptr;
		{
			std::lock_guard<std::mutex> l(p->mu);
			import = p->import;
			p->import = nullptr;
			p->import_failed = true;
		}
		if (import && release)
			release(import);
	}
}

void StopCopyHelpers()
{
	std::lock_guard<std::mutex> one(g_copy_mu);
	if (!g_copy_pool)
		return;
	{
		std::lock_guard<std::mutex> l(g_copy_pool->mu);
		g_copy_pool->stop = true;
	}
	g_copy_pool->go.notify_all();
	g_copy_pool->threads.clear(); // (joined)
	delete g_copy_pool;
	g_copy_pool = nullptr;
}

AVFrame* CpuCopy(const AVFrame* f)
{
	const ZeroCopyInfo* z = ZeroCopyOf(f);
	if (!z || !z->lsb10)
		return f ? av_frame_clone(f) : nullptr;
	const size_t pitch = size_t(f->linesize[0]);
	const int rows = pitch ? int((f->data[1] - f->data[0]) / ptrdiff_t(pitch)) : 0;
	AVFrame* c = CopyPicture(f->data[0], pitch, rows, f->width, f->height, true, false);
	if (c)
		av_frame_copy_props(c, f);
	if (c)
		av_buffer_unref(&c->opaque_ref);
	return c;
}

void Close(Decoder* d)
{
	if (!d)
		return;
	OrbisLog("[hwdec] closed: %u access units, %u pictures (%u in place), %u errors; copies %.2f ms each (%u)", d->decoded,
		d->shown, d->in_place, d->errors, d->copied ? d->copied_ms / d->copied : 0.0, d->copied);
	if (d->codec == kCodecVp9)
		// (test5 review) for a console log: how the decoder treats VP9 blocks that show no frame
		OrbisLog("[hwdec] VP9: %u blocks gave no picture, %u pictures shown (blocks showing no frame given a picture "
				 "and dropped: %u)", d->vp9_no_picture, d->shown, d->vp9_hidden_dropped);
	for (AVFrame* f : d->out)
		av_frame_free(&f);
	FreeDecoder(d);
	delete d;
}
} // namespace hwdec
} // namespace mpc
