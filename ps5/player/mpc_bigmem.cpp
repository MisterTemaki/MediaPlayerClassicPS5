// Media Player Classic PS5: big pictures from the console's direct memory (mpc_bigmem.h)
// SPDX-License-Identifier: MIT
#include "mpc_bigmem.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace mpc::bigmem
{
namespace
{
constexpr size_t kUnit = 0x10000; // direct memory's unit: 64 KiB blocks, 64 KiB aligned
constexpr size_t kAlign = 64;     // what FFmpeg's SIMD wants of a plane
constexpr size_t kFreeMax = size_t(1536) << 20; // the free list at most (an 8K film's few pictures are about 600 MiB)
constexpr size_t kFreeBlocksMax = 24;

enum class Kind
{
	Direct,
	Flexible
};
struct Block
{
	void* addr;
	size_t len; // rounded to kUnit
	intptr_t phys;
	Kind kind;
};
std::mutex g_mu;
std::vector<Block> g_used, g_free;
Stats g_stats;

size_t RoundUp(size_t len)
{
	return (len + kUnit - 1) & ~(kUnit - 1);
}

// the console's own direct memory, CPU-only (type 12, as VLC for PS5 takes it), else any (type 0), else the main
// range; then flexible memory
bool Map(size_t len, Block* out)
{
	const int64_t dsize = int64_t(sceKernelGetDirectMemorySize());
	for (int type : {12, 0})
	{
		intptr_t phys = 0;
		int r = sceKernelAllocateDirectMemory(0, dsize, len, kUnit, type, &phys);
		if (r != 0)
			r = sceKernelAllocateMainDirectMemory(len, kUnit, type, &phys);
		if (r != 0)
			continue;
		void* addr = nullptr;
		r = sceKernelMapDirectMemory(&addr, len, 0x3 /* read | write */, 0, phys, kUnit);
		if (r != 0 || !addr)
		{
			sceKernelReleaseDirectMemory(phys, len);
			continue;
		}
		*out = {addr, len, phys, Kind::Direct};
		return true;
	}
	void* addr = nullptr;
	if (sceKernelMapNamedFlexibleMemory(&addr, len, 0x3, 0, "mpcps5 picture") == 0 && addr)
	{
		*out = {addr, len, 0, Kind::Flexible};
		return true;
	}
	return false;
}

void Unmap(const Block& b)
{
	if (b.kind == Kind::Direct)
		sceKernelReleaseDirectMemory(b.phys, b.len); // (it unmaps too)
	else
		munmap(b.addr, b.len);
}

void Account(const Block& b, int64_t sign)
{
	uint64_t* blocks = b.kind == Kind::Direct ? &g_stats.direct_blocks : &g_stats.flexible_blocks;
	uint64_t* bytes = b.kind == Kind::Direct ? &g_stats.direct_bytes : &g_stats.flexible_bytes;
	*blocks += uint64_t(sign);
	*bytes += uint64_t(sign) * b.len;
}

void FreeCb(void*, uint8_t* data)
{
	Free(data);
}
} // namespace

void* Alloc(size_t want)
{
	const size_t len = RoundUp(want);
	std::lock_guard<std::mutex> l(g_mu);
	// the free list: a block of the same size (pictures of one film are all alike)
	for (size_t i = 0; i < g_free.size(); i++)
		if (g_free[i].len == len)
		{
			Block b = g_free[i];
			g_free.erase(g_free.begin() + long(i));
			g_stats.free_blocks--;
			g_stats.free_bytes -= b.len;
			g_used.push_back(b);
			Account(b, 1);
			g_stats.reuses++;
			return b.addr;
		}
	Block b;
	if (!Map(len, &b))
	{
		// everything free unmapped, one more try (a new size: an old film's pictures were holding the memory)
		if (g_free.empty())
			return nullptr;
		for (const Block& f : g_free)
			Unmap(f);
		g_free.clear();
		g_stats.free_blocks = g_stats.free_bytes = 0;
		if (!Map(len, &b))
			return nullptr;
	}
	g_used.push_back(b);
	Account(b, 1);
	g_stats.allocs++;
	return b.addr;
}

void Free(void* p)
{
	if (!p)
		return;
	std::lock_guard<std::mutex> l(g_mu);
	for (size_t i = 0; i < g_used.size(); i++)
		if (g_used[i].addr == p)
		{
			Block b = g_used[i];
			g_used.erase(g_used.begin() + long(i));
			Account(b, -1);
			if (g_free.size() >= kFreeBlocksMax || g_stats.free_bytes + b.len > kFreeMax)
			{
				Unmap(b);
				return;
			}
			g_free.push_back(b);
			g_stats.free_blocks++;
			g_stats.free_bytes += b.len;
			return;
		}
	OrbisLog("[bigmem] Free(%p): not a block of ours", p);
}

void Trim()
{
	std::lock_guard<std::mutex> l(g_mu);
	for (const Block& f : g_free)
		Unmap(f);
	g_free.clear();
	g_stats.free_blocks = g_stats.free_bytes = 0;
}

namespace
{
// the planes' layout of a picture as FFmpeg's default pool lays it out (avcodec_align_dimensions2's padded size,
// linesizes aligned for it, each plane kAlign-aligned with 16 bytes of slack); total: what one block needs
bool Layout(AVCodecContext* ctx, int width, int height, AVPixelFormat fmt, int ls[4], size_t sizes[4], size_t* total)
{
	int w = width, h = height;
	int lsalign[AV_NUM_DATA_POINTERS] = {};
	if (ctx)
		avcodec_align_dimensions2(ctx, &w, &h, lsalign);
	else
	{
		w = FFALIGN(w, 64);
		h = FFALIGN(h, 2);
		for (int& a : lsalign)
			a = int(kAlign);
	}
	bool unaligned;
	do
	{
		if (av_image_fill_linesizes(ls, fmt, w) < 0)
			return false;
		w += w & ~(w - 1); // (the next power-of-two step, as FFmpeg does, until every linesize is aligned)
		unaligned = false;
		for (int i = 0; i < 4; i++)
			if (ls[i] && lsalign[i] && ls[i] % lsalign[i])
				unaligned = true;
	} while (unaligned);
	ptrdiff_t pls[4] = {ls[0], ls[1], ls[2], ls[3]};
	if (av_image_fill_plane_sizes(sizes, fmt, h, pls) < 0)
		return false;
	*total = 0;
	for (int i = 0; i < 4; i++)
		if (sizes[i])
			*total += sizes[i] + 16 + kAlign;
	return *total > 0;
}

uint8_t* AlignUp(uint8_t* p)
{
	return reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + kAlign - 1) & ~uintptr_t(kAlign - 1));
}

bool Fill(AVFrame* f, int ls[4], const size_t sizes[4], size_t total)
{
	uint8_t* block = static_cast<uint8_t*>(Alloc(total));
	if (!block)
		return false;
	f->buf[0] = av_buffer_create(block, int(total), FreeCb, nullptr, 0);
	if (!f->buf[0])
	{
		Free(block);
		return false;
	}
	uint8_t* at = block;
	for (int i = 0; i < 4; i++)
	{
		if (!sizes[i])
		{
			f->data[i] = nullptr;
			f->linesize[i] = 0;
			continue;
		}
		at = AlignUp(at);
		f->data[i] = at;
		f->linesize[i] = ls[i];
		at += sizes[i] + 16;
	}
	f->extended_data = f->data;
	return true;
}
} // namespace

int GetBuffer2(AVCodecContext* ctx, AVFrame* f, int flags)
{
	const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
	int ls[4] = {};
	size_t sizes[4] = {};
	size_t total = 0;
	if (ctx->codec_type != AVMEDIA_TYPE_VIDEO || !d || (d->flags & AV_PIX_FMT_FLAG_HWACCEL) || f->width <= 0 ||
		f->height <= 0 || !Layout(ctx, f->width, f->height, AVPixelFormat(f->format), ls, sizes, &total) ||
		total < kBigPicture)
		return avcodec_default_get_buffer2(ctx, f, flags);
	if (!Fill(f, ls, sizes, total))
	{
		{
			std::lock_guard<std::mutex> l(g_mu);
			g_stats.fallbacks++;
		}
		return avcodec_default_get_buffer2(ctx, f, flags);
	}
	return 0;
}

bool AllocFrame(AVFrame* f)
{
	int ls[4] = {};
	size_t sizes[4] = {};
	size_t total = 0;
	if (f->width <= 0 || f->height <= 0 || !Layout(nullptr, f->width, f->height, AVPixelFormat(f->format), ls, sizes, &total))
		return false;
	if (total < kBigPicture)
		return av_frame_get_buffer(f, int(kAlign)) == 0;
	if (Fill(f, ls, sizes, total))
		return true;
	{
		std::lock_guard<std::mutex> l(g_mu);
		g_stats.fallbacks++;
	}
	return av_frame_get_buffer(f, int(kAlign)) == 0;
}

size_t DirectFree()
{
	int64_t start = 0;
	size_t size = 0;
	if (sceKernelAvailableDirectMemorySize(0, int64_t(sceKernelGetDirectMemorySize()), kUnit, &start, &size) != 0)
		return SIZE_MAX;
	return size;
}

size_t DirectSize()
{
	return sceKernelGetDirectMemorySize();
}

Stats GetStats()
{
	std::lock_guard<std::mutex> l(g_mu);
	return g_stats;
}
} // namespace mpc::bigmem
