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
// The library is there and its functions resolved.
bool Available();
// Off from the settings (Options > Output > "Hardware decoding").
void SetEnabled(bool on);
bool Enabled();

struct Decoder;
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
// "HEVC 3840x2160 10-bit (console decoder)" for the statistics.
std::string Describe(const Decoder* d);
void Close(Decoder* d);
} // namespace hwdec
} // namespace mpc
