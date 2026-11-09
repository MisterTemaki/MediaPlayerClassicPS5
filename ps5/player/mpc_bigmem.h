// Media Player Classic PS5: big pictures from the console's direct memory (1.6 test12; the approach is VLC for PS5's,
// by ZiZc3: pictures of 4 MiB and more are allocated in direct memory, not the app's small flexible heap).
//
// The PS5 leaves an app a small flexible heap (about 360 MiB at one start) and some 12 GiB of direct memory. FFmpeg's
// decoder frames (its get_buffer2) and the pictures copied from the console's decoder come from here when they are
// big: a block of direct memory each, kept on a free list by size when let go (an 8K picture a frame isn't mapped and
// unmapped 30 times a second), flexible memory when direct memory is full, FFmpeg's own allocation when neither can.
// Thread-safe (the decoder's frame threads ask from several threads).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

struct AVCodecContext;
struct AVFrame;

namespace mpc::bigmem
{
constexpr size_t kBigPicture = size_t(4) << 20; // pictures of this and more

// A block of at least len bytes (direct memory, else flexible, else nullptr), 64-byte aligned.
void* Alloc(size_t len);
// Back to the free list (kept for a picture of the same size) or unmapped.
void Free(void* p);
// Everything on the free list unmapped (a file closed).
void Trim();

// For AVCodecContext::get_buffer2 (a codec with AV_CODEC_CAP_DR1): a picture of kBigPicture or more in one block
// from Alloc, else avcodec_default_get_buffer2.
int GetBuffer2(AVCodecContext* ctx, AVFrame* frame, int flags);
// An AVFrame with its planes in one block from Alloc (a copy of a decoder's picture); nullptr when none can be had.
// (frame->format, width, height set by the caller; its buf[0] frees the block)
bool AllocFrame(AVFrame* frame);

// The direct memory free now (the largest run; SIZE_MAX when it can't be asked) and the whole of it.
size_t DirectFree();
size_t DirectSize();

struct Stats
{
	uint64_t direct_blocks = 0, direct_bytes = 0;     // in use now
	uint64_t flexible_blocks = 0, flexible_bytes = 0; // in use now
	uint64_t free_blocks = 0, free_bytes = 0;         // on the free list
	uint64_t allocs = 0, reuses = 0, fallbacks = 0;   // since the start: blocks mapped, taken from the list, FFmpeg's own
};
Stats GetStats();
} // namespace mpc::bigmem
