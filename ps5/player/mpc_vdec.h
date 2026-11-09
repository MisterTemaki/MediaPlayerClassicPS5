// Media Player Classic PS5: H.264 and HEVC on the console's video decoder (libSceVideodec2), ahead of FFmpeg.
//
// A 4K HEVC 10-bit film is a few pictures a second on the CPU; the console's decoder plays it in real time. This is
// VLC for PS5's ps5vdec module (github.com/ZiZc3/VLC, ps5/modules/ps5vdec.c, GPL-2.0-or-later), brought over from
// VLC's decoder interface to FFmpeg's packets and frames: the same calls, memory and recovery rules, learned on the
// console by its author.
//
//   - the library (sysmodule 207) and the decoders' compute queue are made at start, before the app leaves its
//     sandbox (Prepare): after it both are refused (0x80020063, 0x811D0111);
//   - per video: the decoder's memory and pictures in direct memory, access units in Annex B with the parameter
//     sets in front of the first one (avcC / hvcC packets are converted);
//   - pictures come out in display order as NV12 (8-bit) or 16-bit samples with the 10 bits at the bottom; they are
//     copied into ordinary FFmpeg frames (NV12, or yuv420p10le), so everything after the decoder is unchanged;
//   - what it can't take (other codecs, H.264 High 10/4:2:2, bigger than 4K) stays with FFmpeg; a decoder that fails
//     after starting hands the video back to FFmpeg from the next keyframe (Failed()).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <string>

namespace mpc
{
namespace hwdec
{
// At start, before the jailbreak: loads the library, makes the compute queue. Logs what happened.
void Prepare();
// (test12, the GPU start's variants 3 and 4) the compute queues made at the start are released before the GPU is set up:
// the console's decoder is off for this run (a queue made later fails). QueuesExist: whether any is there now.
void ReleaseQueues(const char* why);
bool QueuesExist();
// The library is there and its functions resolved.
bool Available();
// Off from the settings (Options > Output > "Hardware decoding").
void SetEnabled(bool on);
bool Enabled();

struct Decoder;
// 1.6: pictures handed out in place (the GPU draws them: mpc_gpu.h): each FFmpeg frame then points into the decoder's
// own picture buffer instead of a copy (NV12, or P010-sized words with the 10 bits at the bottom: ZeroCopyOf says so),
// and the buffer is the decoder's again only once every reference to the frame is gone. A picture goes out in place
// only while the decoder still has the most buffers it has held at once, plus one for its next picture and a margin;
// the others are copied (into the same semi-planar layout), so it never runs out.
// Takes effect for decoders opened afterwards; off by default (the CPU path).
void SetZeroCopy(bool on);
bool ZeroCopy();
// A frame handed out in place: where its picture lies (the whole buffer block, for the GPU's import), and how
struct ZeroCopyInfo
{
	uint32_t magic;
	void* pool;           // the block's owner (kept alive by the frame)
	uint8_t* base;        // the decoder's picture buffers, all of them
	size_t bytes;
	bool lsb10;           // 16-bit words, 10 bits at the bottom (else NV12)
};
const ZeroCopyInfo* ZeroCopyOf(const AVFrame* f);
// The GPU's import of a block (ps5gpu::HostMemory*), kept with it: null until set; `failed` once it couldn't be made
void* PoolImport(const ZeroCopyInfo* z, bool* failed);
void SetPoolImport(const ZeroCopyInfo* z, void* import, bool failed);
// Blocks no frame and no decoder holds any more: their import let go (`release`, on the GPU's thread), then the memory
// freed. Called by the thread that draws (mpc_gpu.cpp), each frame.
void CollectPools(void (*release)(void* import));
// (final review 2) The GPU is going: every block still in use (an open decoder's) lets go of its import now (`release`)
// and is marked as not importable, so its pictures are copied from then on.
void DropImports(void (*release)(void* import));
// A frame of the CPU's: an in-place 10-bit picture copied into P010 (the bits at the top); any other frame referenced as
// it is. nullptr: no memory.
AVFrame* CpuCopy(const AVFrame* f);
// At the app's end: the copy threads (a big picture's copy is split over them) stopped and joined
void StopCopyHelpers();

// A decoder for this stream, or nullptr (codec, profile or size it doesn't take; no memory). why: the reason.
Decoder* Open(const AVCodecParameters* par, std::string* why);
// avcodec_send_packet's contract: 0, AVERROR(EAGAIN) (receive first), AVERROR_EOF (drained); pkt nullptr: drain.
int Send(Decoder* d, const AVPacket* pkt);
// avcodec_receive_frame's: 0 with a frame (pts in the packets' time base), AVERROR(EAGAIN), AVERROR_EOF.
int Receive(Decoder* d, AVFrame* frame);
// After a seek: what's inside is dropped, decoding goes on from the next keyframe.
void Flush(Decoder* d);
// It gave up (the decoder refused the stream): the caller takes the video back to FFmpeg.
bool Failed(const Decoder* d);
// (review L-b) What a wait for a picture buffer asks the player: `stop` (it quits, or seeks: the access unit is dropped,
// no waiting on), `paused` (pictures are copied then, not handed out in place: they would hold buffers while nothing is
// shown). Called on the decoding thread.
struct Hooks
{
	bool (*stop)(void* ctx) = nullptr;
	bool (*paused)(void* ctx) = nullptr;
	void* ctx = nullptr;
};
void SetHooks(Decoder* d, const Hooks& h);
// "HEVC 3840x2160 10-bit (console decoder)" for the statistics.
std::string Describe(const Decoder* d);
void Close(Decoder* d);
} // namespace hwdec
} // namespace mpc
