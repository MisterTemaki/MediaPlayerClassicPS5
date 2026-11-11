// Media Player Classic PS5: the playback engine (mpc_player.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_player.h"
#include "mpc_settings.h"

#include "mpc_ass.h"
#include "mpc_disc.h"
#include "mpc_i18n.h"
#include "mpc_res.h"
#include "mpc_smb.h"
#include "mpc_tonemap.h"
#include "mpc_vdec.h"
#include "mpc_bigmem.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoThread.h"
#include "ProsperoSce.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <iterator>
#include <new>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace mpc
{
namespace
{
double NowS()
{
	return av_gettime_relative() / 1e6;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string Ext(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return "";
	return Lower(path.substr(dot + 1));
}

std::string AvErr(int e)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(e, buf, sizeof(buf));
	return buf;
}

// ---- packet queue (ffplay's PacketQueue) ----
struct QPacket
{
	AVPacket* pkt; // nullptr: end of stream (drain the decoder)
	int serial;
};

struct PacketQueue
{
	std::mutex mu;
	std::condition_variable cv;
	std::deque<QPacket> q;
	size_t bytes = 0;
	int64_t duration = 0; // in the stream's time base
	// (test7 review) kept as packets come and go, for answers without a walk over the queue: where the last one queued
	// ends, and the queued keyframes' times in order (pts, or dts without one; AV_NOPTS_VALUE without either)
	int64_t end_ts = AV_NOPTS_VALUE;
	std::deque<int64_t> keys;
	bool abort = false;

	static int64_t Ts(const AVPacket* p) { return p->pts != AV_NOPTS_VALUE ? p->pts : p->dts; }
	void Put(AVPacket* p, int serial)
	{
		std::lock_guard<std::mutex> l(mu);
		q.push_back({p, serial});
		if (p)
		{
			bytes += size_t(p->size) + sizeof(*p);
			duration += p->duration;
			const int64_t t = Ts(p);
			if (t != AV_NOPTS_VALUE && (end_ts == AV_NOPTS_VALUE || t + p->duration > end_ts))
				end_ts = t + p->duration;
			if (p->flags & AV_PKT_FLAG_KEY)
				keys.push_back(t);
		}
		cv.notify_all();
	}
	// (test6) the time the queued packets cover, in seconds: their durations, or the first one's time to the last one's
	// end (test7 review: no walk over the queue)
	double Seconds(AVRational tb)
	{
		std::lock_guard<std::mutex> l(mu);
		int64_t span = 0;
		if (!q.empty() && q.front().pkt && end_ts != AV_NOPTS_VALUE)
		{
			const int64_t t = Ts(q.front().pkt);
			if (t != AV_NOPTS_VALUE && end_ts > t)
				span = end_ts - t;
		}
		return double(std::max(duration, span)) * av_q2d(tb);
	}
	// (test7 review) the next queued keyframe's time (AV_NOPTS_VALUE: none queued, or one without a time)
	bool NextKey(int64_t* ts)
	{
		std::lock_guard<std::mutex> l(mu);
		if (keys.empty())
			return false;
		*ts = keys.front();
		return true;
	}
	void Flush()
	{
		std::lock_guard<std::mutex> l(mu);
		for (auto& e : q)
			av_packet_free(&e.pkt);
		q.clear();
		bytes = 0;
		duration = 0;
		end_ts = AV_NOPTS_VALUE;
		keys.clear();
		cv.notify_all();
	}
	void Abort()
	{
		std::lock_guard<std::mutex> l(mu);
		abort = true;
		cv.notify_all();
	}
	// 1 got one, 0 timed out, -1 aborted
	// busy (1.5.3): set before the packet leaves the queue, under its lock, so a packet is always either queued or
	// marked as being decoded
	int Get(QPacket& out, int timeout_ms, std::atomic<bool>* busy = nullptr)
	{
		std::unique_lock<std::mutex> l(mu);
		if (!cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return abort || !q.empty(); }))
			return 0;
		if (abort)
			return -1;
		if (busy)
			*busy = true;
		out = q.front();
		q.pop_front();
		if (out.pkt)
		{
			bytes -= size_t(out.pkt->size) + sizeof(*out.pkt);
			duration -= out.pkt->duration;
			if ((out.pkt->flags & AV_PKT_FLAG_KEY) && !keys.empty())
				keys.pop_front();
		}
		if (q.empty())
			end_ts = AV_NOPTS_VALUE;
		cv.notify_all();
		return 1;
	}
	size_t Count()
	{
		std::lock_guard<std::mutex> l(mu);
		return q.size();
	}
};

struct Clock
{
	std::mutex mu;
	double pts = 0, at = 0;
	double rate = 1;
	bool paused = true;
	bool held = false; // (test8) an exact seek waiting for its first picture: the clock stands at the target
	void Set(double p)
	{
		std::lock_guard<std::mutex> l(mu);
		pts = p;
		at = NowS();
	}
	double Get()
	{
		std::lock_guard<std::mutex> l(mu);
		return paused || held ? pts : pts + (NowS() - at) * rate;
	}
	void Freeze(bool& flag, bool v) // (under mu)
	{
		const bool was = paused || held;
		const double now = NowS();
		if (!was)
			pts += (now - at) * rate;
		at = now;
		flag = v;
	}
	void SetPaused(bool p)
	{
		std::lock_guard<std::mutex> l(mu);
		if (paused != p)
			Freeze(paused, p);
	}
	void SetHeld(bool h)
	{
		std::lock_guard<std::mutex> l(mu);
		if (held != h)
			Freeze(held, h);
	}
	void SetRate(double r)
	{
		std::lock_guard<std::mutex> l(mu);
		const double now = NowS();
		if (!paused && !held)
			pts += (now - at) * rate;
		at = now;
		rate = r;
	}
};

struct VFrame
{
	AVFrame* f;
	double pts;
	double dur;
	int serial;
};

struct SubItem
{
	double start = 0, end = 1e18;
	bool open_end = false; // shown until the next one
	SubtitleShow show;
	// 1.5.2: a line of an external bitmap subtitle file: the packets it is decoded from (ExtBitmap::pkts), and whether
	// its pictures are to be decoded (not yet, or let go by the budget)
	int first_pkt = -1, last_pkt = -1;
	bool need_pics = false;
	// a PGS line's epoch: the packet of the last presentation segment up to it that wasn't "normal" (an epoch start,
	// acquisition point or epoch continue: the decoder's palettes and objects begin again there); -1: none, the file's
	// start. A normal display set (a palette update, a new object on the epoch's palette) is drawn with what its epoch
	// defined before it, so its pictures are decoded from there (DecodeShown)
	int epoch_pkt = -1;
};

// 1.5.2: the compressed packets of an external bitmap subtitle file kept at most (a film's .sup is tens of MB), each
// counted with its overhead; and its lines (a film has ~1,500; a made-up file 100,000 tiny ones)
constexpr size_t kMaxExtBitmapBytes = size_t(128) << 20;
constexpr size_t kExtPacketOverhead = 256; // an AVPacket, its buffer's reference and padding
constexpr size_t kMaxExtBitmapLines = 20000;
// PGS segment types. A packet holds one segment from FFmpeg's sup and M2TS readers, a whole display set from MKV.
constexpr uint8_t kPgsPalette = 0x14, kPgsObject = 0x15, kPgsPresentation = 0x16, kPgsDisplayEnd = 0x80;

// what a PGS packet's segments are (pgssubdec.c reads them the same way)
struct PgsInfo
{
	int segments = 0;
	bool presentation = false; // a presentation segment: a display set begins
	bool epoch = false;        // ... whose composition state isn't "normal" (00): FFmpeg's decoder lets go of its
							   // palettes and objects there (parse_presentation_segment)
	bool clears = false;       // ... with no object: the display set takes the line off the screen
	bool palette = false, object = false, end = false;
	bool object_first = false; // the object's first fragment (its definition begins: older ones are replaced)
	int object_id = -1;
};

PgsInfo PgsScan(const AVPacket* p)
{
	PgsInfo g;
	const uint8_t* d = p ? p->data : nullptr;
	int n = p ? p->size : 0;
	while (d && n >= 3)
	{
		const int type = d[0], len = (d[1] << 8) | d[2];
		if (len > n - 3)
			break;
		g.segments++;
		if (type == kPgsPresentation && len >= 11)
		{
			g.presentation = true;
			g.epoch = g.epoch || (d[3 + 7] >> 6) != 0;
			g.clears = g.clears || d[3 + 10] == 0;
		}
		else if (type == kPgsPalette)
			g.palette = true;
		else if (type == kPgsObject && len >= 4)
		{
			g.object = true;
			g.object_id = (d[3] << 8) | d[4];
			g.object_first = g.object_first || (d[3 + 3] & 0x80) != 0;
		}
		else if (type == kPgsDisplayEnd)
			g.end = true;
		d += 3 + len;
		n -= 3 + len;
	}
	return g;
}

size_t PictureBytes(const SubtitleShow& s)
{
	size_t n = 0;
	for (const auto& p : s.pics)
		if (p.img)
			n += size_t(p.img->Width()) * size_t(p.img->Height()) * 4;
	return n;
}

// the bitmap subtitles' memory budget: 256 MiB (the host tests set a smaller one with MPCPS5_HOST_SUB_BUDGET_MB)
size_t SubPictureBudget()
{
#ifndef __PROSPERO__
	if (const char* e = getenv("MPCPS5_HOST_SUB_BUDGET_MB"))
		if (atoi(e) > 0)
			return size_t(atoi(e)) << 20;
#endif
	return size_t(256) << 20;
}

// lines that ended this long before the newest one decoded keep their pictures whatever the budget (a seek back a
// little finds them; further back, the demuxer decodes them again from the packets)
constexpr double kSubPictureKeep = 30;

const char* const kChannelNames[] = {"", "mono", "stereo", "2.1", "4.0", "5.0", "5.1", "6.1", "7.1"};

std::string ChannelText(int ch)
{
	if (ch >= 1 && ch <= 8)
		return T(kChannelNames[ch]);
	return Fmt(T("%d ch"), ch);
}

std::string LangName(const std::string& code)
{
	static const std::map<std::string, std::string> names = {{"eng", "English"}, {"en", "English"},
		{"por", "Portuguese"}, {"pt", "Portuguese"}, {"pob", "Portuguese (Brazil)"}, {"spa", "Spanish"},
		{"es", "Spanish"}, {"fre", "French"}, {"fra", "French"}, {"fr", "French"}, {"ger", "German"}, {"deu", "German"},
		{"de", "German"}, {"ita", "Italian"}, {"it", "Italian"}, {"jpn", "Japanese"}, {"ja", "Japanese"},
		{"chi", "Chinese"}, {"zho", "Chinese"}, {"zh", "Chinese"}, {"kor", "Korean"}, {"ko", "Korean"},
		{"rus", "Russian"}, {"ru", "Russian"}, {"dut", "Dutch"}, {"nld", "Dutch"}, {"pol", "Polish"},
		{"swe", "Swedish"}, {"nor", "Norwegian"}, {"dan", "Danish"}, {"fin", "Finnish"}, {"tur", "Turkish"},
		{"ara", "Arabic"}, {"heb", "Hebrew"}, {"hin", "Hindi"}, {"tha", "Thai"}, {"cze", "Czech"}, {"ces", "Czech"},
		{"hun", "Hungarian"}, {"gre", "Greek"}, {"ell", "Greek"}, {"rum", "Romanian"}, {"ron", "Romanian"},
		{"ukr", "Ukrainian"}, {"vie", "Vietnamese"}, {"ind", "Indonesian"}, {"cat", "Catalan"}};
	auto it = names.find(Lower(code));
	return it == names.end() ? code : T(it->second);
}

// Removes ASS override blocks; reports italics and top alignment.
void AssToText(const std::string& in, std::vector<std::string>& lines, std::vector<bool>& italic, bool& top)
{
	std::string cur;
	bool it = false, line_it = false;
	for (size_t i = 0; i < in.size(); i++)
	{
		const char c = in[i];
		if (c == '{')
		{
			const size_t end = in.find('}', i);
			if (end == std::string::npos)
				break;
			const std::string tag = in.substr(i + 1, end - i - 1);
			if (tag.find("\\i1") != std::string::npos)
				it = line_it = true;
			if (tag.find("\\i0") != std::string::npos)
				it = false;
			if (tag.find("\\an7") != std::string::npos || tag.find("\\an8") != std::string::npos ||
				tag.find("\\an9") != std::string::npos)
				top = true;
			i = end;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && (in[i + 1] == 'N' || in[i + 1] == 'n'))
		{
			lines.push_back(cur);
			italic.push_back(line_it);
			cur.clear();
			line_it = it;
			i++;
			continue;
		}
		if (c == '\\' && i + 1 < in.size() && in[i + 1] == 'h')
		{
			cur += ' ';
			i++;
			continue;
		}
		if (c == '\r')
			continue;
		if (c == '\n')
		{
			lines.push_back(cur);
			italic.push_back(line_it);
			cur.clear();
			line_it = it;
			continue;
		}
		cur += c;
	}
	lines.push_back(cur);
	italic.push_back(line_it);
	// drop empty lines at the ends
	while (!lines.empty() && lines.back().find_first_not_of(' ') == std::string::npos)
	{
		lines.pop_back();
		italic.pop_back();
	}
	while (!lines.empty() && lines.front().find_first_not_of(' ') == std::string::npos)
	{
		lines.erase(lines.begin());
		italic.erase(italic.begin());
	}
}

// the Text field of an ASS event ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text")
std::string AssEventText(const char* ass)
{
	if (!ass)
		return "";
	std::string s = ass;
	if (s.compare(0, 9, "Dialogue:") == 0)
	{
		// the old form: "Dialogue: Marked,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
		size_t p = 0;
		for (int i = 0; i < 9 && p != std::string::npos; i++)
			p = s.find(',', p + 1);
		return p == std::string::npos ? "" : s.substr(p + 1);
	}
	size_t p = std::string::npos;
	size_t from = 0;
	for (int i = 0; i < 8; i++)
	{
		p = s.find(',', from);
		if (p == std::string::npos)
			return s;
		from = p + 1;
	}
	return s.substr(from);
}

bool ValidUtf8(const std::vector<uint8_t>& d)
{
	size_t i = 0;
	while (i < d.size())
	{
		const uint8_t c = d[i];
		int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (n < 0 || i + n >= d.size() + (n ? 0 : 1))
			return false;
		for (int k = 1; k <= n; k++)
			if (i + k >= d.size() || (d[i + k] & 0xc0) != 0x80)
				return false;
		i += n + 1;
	}
	return true;
}

// Windows-1252 to UTF-8 (MPC-HC reads ANSI subtitles in the system code page; Western Europe and Brazil
// use 1252)
std::vector<uint8_t> Cp1252ToUtf8(const std::vector<uint8_t>& d)
{
	static const uint16_t hi[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
		0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
		0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
	std::vector<uint8_t> out;
	out.reserve(d.size() * 2);
	for (uint8_t c : d)
	{
		uint32_t cp = c;
		if (c >= 0x80 && c < 0xa0)
			cp = hi[c - 0x80];
		if (cp < 0x80)
			out.push_back(uint8_t(cp));
		else if (cp < 0x800)
		{
			out.push_back(uint8_t(0xc0 | (cp >> 6)));
			out.push_back(uint8_t(0x80 | (cp & 0x3f)));
		}
		else
		{
			out.push_back(uint8_t(0xe0 | (cp >> 12)));
			out.push_back(uint8_t(0x80 | ((cp >> 6) & 0x3f)));
			out.push_back(uint8_t(0x80 | (cp & 0x3f)));
		}
	}
	return out;
}

struct MemReader
{
	std::vector<uint8_t> data;
	size_t pos = 0;
	static int Read(void* opaque, uint8_t* buf, int size)
	{
		MemReader* m = static_cast<MemReader*>(opaque);
		const size_t n = std::min(size_t(size), m->data.size() - m->pos);
		if (n == 0)
			return AVERROR_EOF;
		memcpy(buf, m->data.data() + m->pos, n);
		m->pos += n;
		return int(n);
	}
	static int64_t Seek(void* opaque, int64_t offset, int whence)
	{
		MemReader* m = static_cast<MemReader*>(opaque);
		if (whence == AVSEEK_SIZE)
			return int64_t(m->data.size());
		whence &= ~AVSEEK_FORCE;
		int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? int64_t(m->pos) + offset : int64_t(m->data.size()) + offset;
		if (p < 0 || p > int64_t(m->data.size()))
			return AVERROR(EINVAL);
		m->pos = size_t(p);
		return p;
	}
};

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out, size_t max_bytes)
{
	if (path.compare(0, 6, "smb://") == 0)
		return smb::ReadFile(path, out, max_bytes);
	if (path.compare(0, 7, "disc://") == 0)
		return disc::ReadFileWithin(path, out, max_bytes, 5); // (File > Load Subtitle: on the UI thread)
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.insert(out.end(), buf, buf + n);
		if (out.size() > max_bytes)
			break;
	}
	fclose(f);
	return out.size() <= max_bytes;
}
} // namespace

struct Player::Impl
{
	// ---- state shared with the UI ----
	mutable std::mutex mu; // tracks, details, error, state
	std::atomic<PlayState> state{PlayState::Closed};
	std::string url, error;
	std::atomic<bool> ended{false};
	double start_at = 0;
	bool start_paused = false;

	// ---- FFmpeg ----
	AVFormatContext* ic = nullptr;
	AVIOContext* custom_io = nullptr; // smb://, disc://
	void* custom_io_owner = nullptr;
	bool custom_io_disc = false; // custom_io is a disc file's (disc::CloseAvio)
	// dvd:// and bluray:// (mpc_disc.h)
	disc::TitleOpen title_open;
	std::vector<DiscTitle> disc_titles;
	int disc_title = 0;
	std::vector<Chapter> disc_chapters; // Blu-ray (FFmpeg's DVD demuxer has the DVD's)
	std::atomic<uint64_t> disc_gen{0}; // the disc it plays from (0: not a drive)
	std::atomic<bool> disc_gone{false};
	std::atomic<int> vstream{-1}, astream{-1}, sstream{-1}; // sstream: -1 off, or a stream index / 1000+n
	std::atomic<bool> a_stop{false}; // the audio thread is asked to end (a track change)
	int ctx_w = 0, ctx_h = 0, ctx_sw = 0, ctx_sh = 0, ctx_fmt = -1; // the scaler's sizes
	bool ctx_hdr = false;                                            // the scaler makes 16-bit R'G'B' for tone mapping
	int ctx_chroma = -1; // 1.6: the chroma siting the context was made for
	// HDR on the SDR picture (mpc_tonemap.h): Options > Output
	std::atomic<int> tm_mode{tonemap::Bt2390};
	std::atomic<int> tm_white{250};
	std::vector<uint16_t> hdr_buf;
	std::atomic<int> hdr_shown{0}; // 0 SDR, 1 PQ, 2 HLG: what the last picture was
	std::atomic<double> display_delay{0}; // 1.6: SetDisplayDelay

	bool Seekable_() const
	{
		const PlayState s = state;
		return (s == PlayState::Playing || s == PlayState::Paused || s == PlayState::Stopped) && seekable;
	}
	AVCodecContext* vdec = nullptr;
	// the console's video decoder (mpc_vdec.h) for H.264 / HEVC; vdec (FFmpeg) when it can't or gives up
	hwdec::Decoder* hw = nullptr;
	std::atomic<int> hw_serial{-1}; // (review L-b) the serial the console decoder is decoding for
	std::atomic<bool> hw_active{false};
	AVCodecContext* adec = nullptr;
	std::map<int, AVCodecContext*> sdecs;
	// (the demuxer's thread) the PGS tracks waiting, after a seek, for a display set that begins an epoch (NewPgsDecoders)
	std::set<int> pgs_wait;
	// ... and those of them inside a clearing display set let through, until its end segment (M2TS: one segment a packet)
	std::set<int> pgs_clearing;
	std::vector<int> sub_streams;
	double duration = 0;
	bool seekable = true;
	Image cover;
	bool has_cover = false;

	// ---- threads ----
	std::thread opener, demuxer, vthread, athread;
	std::atomic<bool> quit{false};
	PacketQueue vq, aq;
	std::atomic<int> serial{0};
	std::atomic<bool> eof{false};
	std::atomic<int> vdone_serial{-1}, adone_serial{-1};

	// seeking
	std::mutex seek_mu;
	bool seek_req = false;
	// Seek() sets the clock at once, but the demuxer's seek (and serial++) comes a little later: until it is done,
	// frames from before it (still the current serial) mustn't set the clock back. seek_gen counts the seeks asked,
	// seek_done the ones the demuxer has done (serial already moved on)
	std::atomic<int> seek_gen{0}, seek_done{0};
	bool SeekPending() const { return seek_gen.load() != seek_done.load(); }
	double seek_to = 0;
	bool seek_exact = true;
	std::atomic<double> skip_until{-1e18}; // frames before this (in the current serial) are not shown
	std::atomic<double> serial_start{0}; // where the current serial starts: the last seek's target, 0 at the start
	std::atomic<bool> show_after_seek{false};
	// (test8 review) an exact seek holds the clock and the sound until its first picture (the target's) is shown, then
	// both go on from it together (mpv's hr-seek, VLC).
	// (test10 review: the keyframe finish and the decode-rate rule of test9 are gone) It holds until (a) the target's
	// picture is shown, (b) no picture can come (NoPictureCanCome: the video track ended there), or (c) kHoldMax of
	// holding (on hold_clock, standing while paused). At (c) it lets go where the decoder is: the latest picture decoded
	// before the target (kept in hold_last) is shown and the clock and the sound start at its time -- a little early,
	// never late, nothing left out, no flush; with no picture decoded at all yet (a slow source) the sound starts at the
	// target and the pictures before it count as late (hold_gaveup_serial), so the bounded keyframe skip catches up.
	// The sound isn't decoded while it holds (the audio thread waits before its packets), so it can start at either.
	static constexpr double kHoldMax = 3.0;
	std::atomic<int> hold_serial{-1};
	std::atomic<int> hold_gaveup_serial{-1}; // let go with no picture
	Clock hold_clock;                        // how long it has held (standing while paused)
	std::mutex hold_mu;                      // a seek's start against a release (skip_until, v_seek_target, hold_serial)
	// the latest picture decoded before the target (fmu). At the bound it is pushed to the front of `frames` and
	// taken by UpdateVideo's "show_after_seek || cur_serial != ser" branch -- show_after_seek stays true for the whole
	// hold (set at the seek, cleared only by that take), which is what makes that take, and the release, happen
	VFrame hold_last{nullptr, 0, 0, -1};
	static constexpr double kHoldNear = 1.0; // (test11) shown at the bound only within this of the target
	double HoldElapsed() { return hold_clock.Get(); }
	// (c), fmu held: true when the latest picture decoded was put up front, for UpdateVideo's take to let go with
	bool HoldBoundLocked(int ser)
	{
		if (hold_serial.load() != ser || HoldElapsed() <= kHoldMax)
			return false;
		// a picture is waiting to be taken (the target's, or the latest one put up front already): its take lets go
		if (!frames.empty())
			return false;
		// (test11 review) only a picture close to the target (within kHoldNear): one a whole GOP early would take the
		// sound back with it, and the keyframe skip would then jump both forward again a few seconds later
		if (hold_last.f && hold_last.serial == ser && v_seek_target.load() - hold_last.pts <= kHoldNear)
		{
			OrbisLog("[player] exact seek: no picture at the target in 3 s: on from the latest one decoded (%.2f s), the "
					 "sound with it",
				hold_last.pts);
			frames.push_front(hold_last);
			hold_last.f = nullptr;
			return true;
		}
		if (hold_last.f && hold_last.serial == ser)
			OrbisLog("[player] exact seek: the latest picture decoded (%.2f s) is more than %.0f s before the target: not shown",
				hold_last.pts, kHoldNear);
		ReleaseHold(ser, -1e18, "no picture in 3 s");
		return false;
	}
	// from the sound's wait (any thread but one holding fmu)
	void HoldCheck(int ser)
	{
		if (hold_serial.load() != ser || HoldElapsed() <= kHoldMax)
			return;
		std::lock_guard<std::mutex> l(fmu);
		HoldBoundLocked(ser); // (a picture put up front is taken, and the hold let go, by UpdateVideo)
	}
	// (second / third reviews; test9: the hold too) no picture can come for this serial, with no video left to decode
	// (none queued, none being decoded, no picture waiting): its reads have failed for a second with no read in progress
	// (a read that blocks, however long, never counts), or the sound read has run 10 s past both its first packet and
	// the last video packet (the video track ended there), paused or playing. (fmu held.)
	bool NoPictureCanCome(int ser)
	{
		if (!frames.empty() || vq.Count() != 0 || vbusy)
			return false;
		std::lock_guard<std::mutex> pl(prog_mu);
		if (prog_serial != ser)
			return false;
		if (fail_since > 0 && !in_read && NowS() - fail_since >= 1.0)
			return true;
		return apkt_max >= std::max(vpkt_max, apkt_first) + 10;
	}
	// true: this call let the hold go (pic: the first picture's time, or -1e18 for none). fmu held.
	bool ReleaseHold(int ser, double pic, const char* why)
	{
		{
			// (test10 review) where the sound starts is set before its wait ends (hold_serial last), and against a seek
			std::lock_guard<std::mutex> hl(hold_mu);
			if (ser < 0 || hold_serial.load() != ser)
				return false;
			if (pic > -1e17)
			{
				clock.Set(pic);
				// a first picture before the target (let go at (c)), or well past it (the decoder went on from a later
				// keyframe: the console's gave up and FFmpeg took over there): the sound starts with it
				if (pic + 0.1 < skip_until.load() || pic > skip_until.load() + 0.1)
				{
					skip_until = pic - 0.001;
					v_seek_target = pic;
				}
			}
			else
				hold_gaveup_serial = ser;
			av_frame_free(&hold_last.f);
			clock.SetHeld(false);
			hold_serial = -1;
		}
		if (pic > -1e17)
			OrbisLog("[player] exact seek: its first picture (%.2f s) after %.2f s; sound and picture go on from it together",
				pic, HoldElapsed());
		else
			OrbisLog("[player] exact seek: %s after %.2f s; the sound goes on without it", why, HoldElapsed());
		return true;
	}
	// 1.5.3 (second and third reviews): what the demuxer has read since the last seek, for SeekShown to tell a seek whose
	// picture can never come (it counts as done) from one still being worked on (never dropped):
	// - failing: since when av_read_frame has failed without the end flag (a demuxer error; the disc taken out) with
	//   none succeeding since, for that serial; a read in progress (in_read) never counts, however long it blocks. (A
	//   real I/O error on a share or a disc usually sets avio's end flag and takes the end path instead: the decoders
	//   drain to vdone_serial.)
	// - the sound and video packets read for the serial (seconds): the sound run 10 s past both its own first packet
	//   and the last video packet means the video track has ended there (an exact seek past its end, a long sound
	//   after it); from the first sound packet, not the target, so streams whose times are offset don't count at once
	// all under prog_mu (in_read too: set before a read, cleared with its outcome)
	std::mutex prog_mu;
	int prog_serial = -1;
	double fail_since = 0; // 0: the last read succeeded
	double vpkt_max = -1e18, apkt_first = 1e18, apkt_max = -1e18;
	bool in_read = false;
#ifndef __PROSPERO__
	int fault_step = 2; // MPCPS5_HOST_SEEK_READ_FAULT (the demuxer's own)
#endif
	std::atomic<bool> vbusy{false}; // the video thread is decoding a packet it took (PacketQueue::Get sets it)
	void ProgressReset()
	{
		std::lock_guard<std::mutex> l(prog_mu);
		prog_serial = serial.load();
		fail_since = 0;
		vpkt_max = apkt_max = -1e18;
		apkt_first = 1e18;
	}
	void ProgressReading(bool r) // a read starts / ends without a packet or a failure (the end)
	{
		std::lock_guard<std::mutex> l(prog_mu);
		in_read = r;
	}
	void ProgressFail()
	{
		std::lock_guard<std::mutex> l(prog_mu);
		in_read = false;
		if (fail_since == 0)
			fail_since = NowS();
	}
	void ProgressRead(int idx, const AVPacket* p)
	{
		const int64_t t = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
		std::lock_guard<std::mutex> l(prog_mu);
		in_read = false;
		fail_since = 0;
		if (t == AV_NOPTS_VALUE || idx < 0 || idx >= int(ic->nb_streams))
			return;
		const double sec = t * av_q2d(ic->streams[idx]->time_base) -
						   (ic->start_time != AV_NOPTS_VALUE ? ic->start_time / double(AV_TIME_BASE) : 0);
		if (idx == vstream)
			vpkt_max = std::max(vpkt_max, sec);
		else if (idx == astream)
		{
			apkt_first = std::min(apkt_first, sec);
			apkt_max = std::max(apkt_max, sec);
		}
	}

	// clock
	Clock clock;
	std::atomic<double> rate{1.0};
	std::atomic<double> audio_delay{0}, sub_delay{0};
	std::atomic<bool> paused{true};
	std::atomic<bool> step_req{false};

	// video frames
	std::mutex fmu;
	std::condition_variable fcv;
	std::deque<VFrame> frames;
	static constexpr size_t kMaxFrames = 6;
	size_t max_frames = kMaxFrames; // (1.6: 3 for a video too big for the memory otherwise)
	// 1.6: a message for the OSD (TakeNotice), and FFmpeg's faster decoding when it can't keep up (0 none, 1 no loop
	// filter, 2 no non-reference pictures either)
	std::string notice;
	int fast_level = 0;
	std::atomic<int> decode_mode{0}; // (test12) 0 Auto (faster when it can't keep up, back when it does), 1 Fast, 2 Full
	bool net_source = false;         // (test12) http(s) / smb: Auto doesn't decode faster for late pictures
	bool pictures_direct = false;    // (test12) the pictures come from direct memory (bigmem), not the heap
	uint64_t fast_seen = 0, fast_late = 0, fast_dropped = 0;
	int fast_serial = -1, fast_calm = 0;
	double fast_notice_at = -1e9;
	bool fast_skip_window = true;
	bool beyond_said = false;
	std::atomic<double> v_behind{0}; // (test4) how far the last decoded picture was behind the clock, seconds
	bool vskip_to_key = false;       // the video thread leaves packets out up to a keyframe
	unsigned vskipped = 0;
	std::atomic<double> v_last_pts{0};       // the last decoded picture's time
	std::atomic<double> v_seek_target{-1e18}; // the last seek's target: pictures before it aren't late
	double a_read = -1e18, v_read = -1e18;   // (demuxer) how far each stream has been read, seconds
	std::atomic<int> a_pushed_serial{-1};    // the serial the sound was last given to the console for (past skip_until)
	bool demux_over = false;                 // (demuxer) reading past the queues' normal limit for the sound
	int demux_said_serial = -1;              // (demuxer) the serial it last said so for
	size_t demux_peak = 0;                   // (demuxer) the most packet bytes queued at once
	double pic_estimate = 0;                 // (test7) the bytes FFmpeg's pictures may take (BigVideoFits), kept
	                                         // free of packets by the demuxer
	int cpu_fallback_threads = 8;
	bool cpu_reopen_fallback = false;        // FFmpeg's decoder with the heap's threads couldn't be opened for a
	                                         // fall-back: it is tried again if the console's decoder gives up
	std::vector<AVCodecContext*> vdec_retired; // decoders swapped out mid-film (freed at the close: others may read)
	bool cpu_lean_fallback = false;          // FFmpeg's decoder, opened for a fall-back from the console's, needs
	                                         // the short picture queue too
	AVFrame* cur = nullptr;
	double cur_pts = 0;
	int cur_serial = -1;
	uint64_t cur_id = 0;
	uint64_t pic_serial = 0; // 1.6: unique across files (Player::Picture)
	// 1.6: the current picture in the CPU's layout when it is the console decoder's own 10-bit one, in place (mpc_vdec.h)
	AVFrame* cur_cpu = nullptr;
	uint64_t cur_cpu_id = 0;
	AVFrame* CpuPicture()
	{
		const hwdec::ZeroCopyInfo* z = hwdec::ZeroCopyOf(cur);
		if (!z || !z->lsb10)
			return cur;
		if (!cur_cpu || cur_cpu_id != cur_id)
		{
			av_frame_free(&cur_cpu);
			cur_cpu = hwdec::CpuCopy(cur);
			cur_cpu_id = cur_id;
		}
		return cur_cpu;
	}
	// conversion
	SwsContext* sws = nullptr;
	uint64_t rendered_id = 0;
	const uint32_t* rendered_dst = nullptr;
	int rendered_w = 0, rendered_h = 0;
	// deinterlacing (bwdif) when the pictures say they are interlaced
	AVFilterGraph* vgraph = nullptr;
	AVFilterContext *vsrc = nullptr, *vsink = nullptr;
	int vg_w = 0, vg_h = 0, vg_fmt = -1;

	// audio
	AVFilterGraph* agraph = nullptr;
	AVFilterContext *asrc = nullptr, *asink = nullptr;
	int ag_rate = 0, ag_fmt = -1;
	AVChannelLayout ag_layout = {};
	double ag_tempo = 1.0;
	bool ag_surround = false;
	int ag_out_ch = 2; // what the graph gives: 2, 6 (5.1, sides) or 8 (7.1, FFmpeg's order)
	std::atomic<bool> surround{false};
	std::vector<int16_t> remap;
	std::atomic<int> volume{100};
	std::atomic<bool> mute{false};
	std::atomic<double> amplify{1.0};
	// Options > Audio Switcher > Normalize (MPC-HC's CAudioSwitcherFilter): set by the UI, read by the audio thread
	std::atomic<bool> norm_on{false};
	std::atomic<double> norm_max{4.0}; // Max amplification (400 % = 4.0)
	std::atomic<bool> norm_regain{true}; // Regain volume
	// the audio thread's own: the gain now (< 0: start again at the max at the next block), the setting last seen
	double norm_factor = -1;
	bool norm_was_on = false;
	std::vector<int16_t> norm_buf;

	// subtitles
	mutable std::mutex smu;
	std::map<int, std::vector<SubItem>> subs; // per track id
	// 1.5.2: the bitmap subtitles' pictures kept (PGS, VobSub, DVB: every track is decoded as it is demuxed), in bytes;
	// over the budget, the pictures of lines that ended well behind are let go (a Blu-ray remux's dozen PGS tracks
	// kept every picture of the film: gigabytes by its end)
	size_t sub_pic_bytes = 0;
	size_t sub_pic_budget = SubPictureBudget();
	double sub_trim_said = -1e18; // when (stream time) the log last said nothing could go
	std::vector<std::pair<int, std::string>> external_names;
	// 1.5.2: external bitmap subtitle files (.sup, VobSub .idx/.sub), by track: their packets kept (compressed), each
	// line's pictures decoded from them when it shows, and let go by the budget like an embedded track's (smu). Up to
	// 1.5.1 such a file went through the text conversion and never loaded.
	struct ExtBitmap
	{
		AVCodecContext* c = nullptr;
		std::vector<AVPacket*> pkts;
		int ref_w = 0, ref_h = 0;
		// PGS: each line's pictures from a decoder of its own (NewDecoder), its epoch replayed first -- c is left
		// wherever the last line decoded put it (at first the file's end: the last epoch's palettes and objects)
		bool pgs = false;
		const AVCodec* codec = nullptr;
		AVCodecParameters* par = nullptr;
		AVRational tb = {1, 90000};
		AVCodecContext* NewDecoder() const
		{
			AVCodecContext* n = codec && par ? avcodec_alloc_context3(codec) : nullptr;
			if (n && avcodec_parameters_to_context(n, par) >= 0)
			{
				n->pkt_timebase = tb;
				if (avcodec_open2(n, codec, nullptr) >= 0)
					return n;
			}
			avcodec_free_context(&n);
			return nullptr;
		}
		~ExtBitmap()
		{
			for (AVPacket*& p : pkts)
				av_packet_free(&p);
			avcodec_free_context(&c);
			avcodec_parameters_free(&par);
		}
	};
	std::map<int, std::unique_ptr<ExtBitmap>> ext_bitmaps;
	// ASS/SSA tracks: drawn by libass (mpc_ass.h), made the first time one turns up
	std::mutex ass_mu;
	std::unique_ptr<ass::Renderer> ass_r;
	std::set<int> ass_streams; // the embedded ASS/SSA tracks (set before the demuxer starts)
	std::atomic<bool> ic_ready{false}; // the file's streams are known: its attached fonts can be read
	bool ass_fonts_done = false;

	// stats
	std::atomic<uint64_t> dropped{0};
	std::atomic<double> decode_ms{0}, convert_ms{0};
	double shown_fps = 0;
	int shown_count = 0;
	double shown_t0 = 0;
	uint64_t seek_pictures = 0; // 1.5.3: seeks whose first picture was shown (under fmu)

	~Impl()
	{
		Shutdown();
		av_frame_free(&cur_cpu);
	}

	// ---------------------------------------------------------------------------------------------
	// libass: the renderer (made when first asked for), the file's attached fonts given to it once
	ass::Renderer* Ass(bool make)
	{
		std::lock_guard<std::mutex> l(ass_mu);
		if (!ass_r && make)
			ass_r.reset(new ass::Renderer);
		if (ass_r && !ass_fonts_done && ic_ready && ic)
		{
			ass_fonts_done = true;
			for (unsigned i = 0; i < ic->nb_streams; i++)
			{
				const AVStream* st = ic->streams[i];
				if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT || !st->codecpar->extradata ||
					st->codecpar->extradata_size <= 0)
					continue;
				const AVDictionaryEntry* fn = av_dict_get(st->metadata, "filename", nullptr, 0);
				const AVDictionaryEntry* mt = av_dict_get(st->metadata, "mimetype", nullptr, 0);
				const std::string name = fn ? fn->value : "font" + std::to_string(i);
				const std::string mime = mt ? Lower(mt->value) : "";
				const std::string ext = Ext(name);
				if (mime.find("font") == std::string::npos && mime.find("truetype") == std::string::npos &&
					mime.find("opentype") == std::string::npos && ext != "ttf" && ext != "otf" && ext != "ttc")
					continue;
				ass_r->AddFont(name, st->codecpar->extradata, size_t(st->codecpar->extradata_size));
				OrbisLog("[subs] attached font loaded: %s (%d bytes)", name.c_str(), st->codecpar->extradata_size);
			}
		}
		return ass_r.get();
	}

	// ---------------------------------------------------------------------------------------------
	static int Interrupt(void* p)
	{
		return static_cast<Impl*>(p)->quit.load() ? 1 : 0;
	}

	void SetFailed(const std::string& e)
	{
		{
			std::lock_guard<std::mutex> l(mu);
			error = e;
		}
		state = PlayState::Failed;
		OrbisLog("[player] failed: %s", e.c_str());
	}

	// (the demuxer's thread) after a seek, every PGS track's decoder made again, and given nothing until a display set
	// that begins an epoch (an epoch start or acquisition point), as a Blu-ray player does after a jump (1.6, the
	// audit). FFmpeg's decoder keeps its epoch's palettes and objects and has no flush: up to 1.5.4 a normal display set
	// read after a seek back (a fade's palette update, a new object on the epoch's palette) was drawn with what the
	// read-ahead had left -- a later epoch's palette or another line's picture -- and replaced the line decoded in
	// order. A new decoder alone isn't enough: a palette update read first leaves a palette of a colour or two, and the
	// next normal set's object came out see-through. Until the epoch begins, a line decoded in order stays as it was.
	void NewPgsDecoders()
	{
		for (auto& s : sdecs)
		{
			if (ic->streams[s.first]->codecpar->codec_id != AV_CODEC_ID_HDMV_PGS_SUBTITLE)
				continue;
			pgs_wait.insert(s.first);
			pgs_clearing.erase(s.first);
			if (AVCodecContext* c = OpenDecoder(s.first, false))
			{
				avcodec_free_context(&s.second);
				s.second = c;
			}
		}
	}

	// (the demuxer's thread) a subtitle packet decoded? A PGS track waiting after a seek (NewPgsDecoders) takes the
	// display set that begins an epoch, and meanwhile those that clear the screen (no object: they draw nothing, so need
	// no palette, and end the line shown -- review: a seek forward inside a line kept it on screen until the next one);
	// a normal set that draws is passed over. With one segment a packet (M2TS) a clearing set's other segments, up to
	// its end, go through with it.
	bool PgsPasses(int idx, const AVPacket* pkt)
	{
		if (!pgs_wait.count(idx))
			return true;
		const PgsInfo g = PgsScan(pkt);
		if (g.epoch)
		{
			pgs_wait.erase(idx); // the epoch the seek waited for
			pgs_clearing.erase(idx);
			return true;
		}
		if (g.presentation)
		{
			if (g.clears && !g.end)
				pgs_clearing.insert(idx);
			else
				pgs_clearing.erase(idx);
			return g.clears;
		}
		if (!pgs_clearing.count(idx))
			return false;
		if (g.end)
			pgs_clearing.erase(idx);
		return true;
	}

	void SetNotice(const std::string& n)
	{
		std::lock_guard<std::mutex> l(mu);
		notice = n;
	}

	// host tests: MPCPS5_HOST_NO_VSKIP=1 keeps the video thread from leaving packets out (the demuxer's way tested alone)
	static bool NoVideoSkip()
	{
#ifndef __PROSPERO__
		static const bool no = getenv("MPCPS5_HOST_NO_VSKIP") != nullptr;
		return no;
#else
		return false;
#endif
	}

	// 1.6: the heap the app has left (the console's malloc takes flexible memory)
	static size_t HeapLeft()
	{
		size_t s = 0;
		if (sceKernelAvailableFlexibleMemorySize(&s) != 0 || s == 0)
			return SIZE_MAX;
		return s;
	}

	// frame_threads: FFmpeg's frame threads (0: its slice threads only, for a video too big for the memory otherwise)
	AVCodecContext* OpenDecoder(int idx, bool threads, bool lean = false, int frame_threads = 8)
	{
		AVStream* st = ic->streams[idx];
		const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!codec)
			return nullptr;
		AVCodecContext* c = avcodec_alloc_context3(codec);
		if (!c)
			return nullptr;
		if (avcodec_parameters_to_context(c, st->codecpar) < 0)
		{
			avcodec_free_context(&c);
			return nullptr;
		}
		c->pkt_timebase = st->time_base;
		if (threads)
		{
			c->thread_count = lean ? 8 : frame_threads; // the console's game cores (fewer when the memory is short)
			// (lean: no frame threads, each of which holds pictures of its own)
			c->thread_type = lean ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
		}
		if (c->codec_type == AVMEDIA_TYPE_VIDEO && threads)
		{
			// (test12, VLC for PS5's way) big pictures from the console's direct memory, not the small heap
			if (codec->capabilities & AV_CODEC_CAP_DR1)
				c->get_buffer2 = bigmem::GetBuffer2;
			// Options > Playback > Decoding: Fast starts fast (no loop filter, FFmpeg's fast flag) and stays at least so
			if (decode_mode.load() == 1)
			{
				c->skip_loop_filter = AVDISCARD_ALL;
				c->flags2 |= AV_CODEC_FLAG2_FAST;
			}
		}
		AVDictionary* opts = nullptr;
		if (c->codec_type == AVMEDIA_TYPE_SUBTITLE)
			av_dict_set(&opts, "sub_text_format", "ass", 0);
		const int r = avcodec_open2(c, codec, &opts);
		av_dict_free(&opts);
		if (r < 0)
		{
			OrbisLog("[player] can't open the %s decoder: %s", codec->name, AvErr(r).c_str());
			avcodec_free_context(&c);
			return nullptr;
		}
		return c;
	}

	// 1.6 (test2: an 8K HEVC 10-bit film froze the app on FFmpeg): a picture over 4K on the CPU needs its decoder's
	// reference pictures, its frame threads' and the queue's, each up to 100 MB at 8K, from a heap the console keeps
	// small (360 MiB free at one start). When that doesn't fit the decoder runs without frame threads and with a
	// shorter queue; when even that doesn't fit, the film isn't opened, and the message says why. False: failed.
	// (test6 review) on_hw: the console's decoder plays it, FFmpeg's (vdec) is there for a fall-back mid-film; it gets
	// the threads the memory takes now, and the short queue is kept for the fall-back (cpu_lean_fallback)
	bool BigVideoFits(int v, const std::string& hw_why, bool on_hw = false)
	{
		const AVCodecParameters* p = ic->streams[v]->codecpar;
		const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(p->format));
		const int bits = d ? d->comp[0].depth : (p->profile == AV_PROFILE_HEVC_MAIN_10 || p->profile == AV_PROFILE_VP9_2 ? 10 : 8);
		const double chroma = d ? 1.0 + 2.0 / double((1 << d->log2_chroma_w) * (1 << d->log2_chroma_h)) : 1.5;
		const double frame = double(p->width) * p->height * chroma * (bits > 8 ? 2 : 1);
		// (test12, after VLC for PS5) pictures of 4 MiB and more come from the console's direct memory (some 12 GiB),
		// not the small heap: the budget is the direct memory free (the largest run), the heap's for smaller pictures
		pictures_direct = frame >= double(bigmem::kBigPicture);
		const size_t left_sz = pictures_direct ? bigmem::DirectFree() : HeapLeft();
		const double left = double(left_sz), budget = left * (pictures_direct ? 0.9 : 0.7);
		const int left_mib = left_sz == SIZE_MAX ? -1 : int(left / 1048576);
		const char* where = pictures_direct ? " of direct memory" : "";
		// (test5 review) the pictures held at once: the decoder's references (6), one being decoded per frame thread,
		// the queue and two for drawing
		auto need = [&](int threads, size_t queue) { return frame * (6 + threads + double(queue) + 2); };
		auto mib = [&](double b) { return int(b / 1048576 + 0.5); };
		// "above 4K" by pixel count: UHD 3840x2160 and DCI 4096x2160 are 4K; more than 4096x2304's pixels is beyond
		const bool big = int64_t(p->width) * p->height > int64_t(4096) * 2304;
		const char* on = on_hw ? "on the CPU should the console's decoder give up" : "on the CPU";
		pic_estimate = need(8, kMaxFrames);
		if (big && !on_hw)
		{
			OrbisLog("[player] %dx%d %d-bit, beyond the console's decoder (4K): FFmpeg on the CPU, its pictures from %s "
					 "(%d MiB free)",
				p->width, p->height, bits, pictures_direct ? "direct memory" : "the heap", left_mib);
			SetNotice(Fmt(T("%dx%d on the CPU (the console's decoder takes up to 4K): it may play slowly."), p->width, p->height));
		}
		if (need(8, kMaxFrames) <= budget)
			return true; // (8 frame threads)
		// fewer frame threads, as many as the memory takes; none (slice threads, 3 pictures queued) when not even 2
		int threads = 7;
		while (threads >= 2 && need(threads, kMaxFrames) > budget)
			threads--;
		if (threads < 2 && big && need(1, 3) > left)
		{
			// not even the lean decoder's pictures fit (direct memory full: another app holding it): the honest message
			// (test11) with about how fast the CPU would show it (the host's rate scaled to the console's cores)
			const int est_fps = std::max(1, int(30.0 * (7680.0 * 4320.0) / (double(p->width) * p->height) + 0.5));
			SetFailed(Fmt(T("This %dx%d %d-bit video can't be played here: the console's decoder takes up to 4K, and "
							"decoding it on the CPU needs about %d MiB of memory, more than the PS5 leaves the app (%d MiB). "
							"Even with the memory, the CPU would show it at about %d pictures a second."),
				p->width, p->height, bits, mib(need(1, 3)), left_mib, est_fps));
			OrbisLog("[player] (the console's decoder: %s)", hw_why.c_str());
			return false;
		}
		// (test7 review) the new one opened before the old is let go: on the console's decoder a failure here only
		// means no fall-back with these threads yet (tried again if it gives up), not the end of the film
		AVCodecContext* n = threads >= 2 ? OpenDecoder(v, true, false, threads) : OpenDecoder(v, true, true);
		pic_estimate = threads >= 2 ? need(threads, kMaxFrames) : need(1, 3);
		if (!n && on_hw)
		{
			OrbisLog("[player] FFmpeg's decoder for a fall-back (%s) couldn't be opened: the console's decoder plays "
					 "on, FFmpeg's is opened again should it give up",
				threads >= 2 ? "fewer frame threads" : "no frame threads");
			cpu_reopen_fallback = true;
			cpu_lean_fallback = threads < 2;
			cpu_fallback_threads = threads;
			return true;
		}
		avcodec_free_context(&vdec);
		vdec = n;
		if (threads >= 2)
		{
			OrbisLog("[player] %dx%d %d-bit %s: %d frame threads (about %d MiB of %d MiB free%s)", p->width, p->height,
				bits, on, threads, mib(need(threads, kMaxFrames)), left_mib, where);
		}
		else
		{
			// (test6 review) this opens even past the budget: the estimate counts every reference HEVC may keep and a
			// share of the memory is held back; only past all of it (above 4K) is it refused, above
			if (on_hw)
				cpu_lean_fallback = true;
			else
				max_frames = 3;
			OrbisLog("[player] %dx%d %d-bit %s: about %d MiB of %d MiB free%s; no frame threads, 3 pictures queued%s",
				p->width, p->height, bits, on, mib(need(1, 3)), left_mib, where,
				need(1, 3) > left ? " (warning: more than the memory left; it may run out of memory)"
				: need(1, 3) > budget ? " (warning: past the share kept for pictures; it may run short of memory)"
									  : "");
		}
		if (!vdec)
		{
			SetFailed(T("The video decoder couldn't be opened."));
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	void OpenThread()
	{
		disc::CancelScope cancel_reads(&quit); // Close: a disc being read gives up at once (mpc_disc.h)
		const double t0 = NowS();
		ic = avformat_alloc_context();
		ic->interrupt_callback.callback = Interrupt;
		ic->interrupt_callback.opaque = this;
		AVDictionary* opts = nullptr;
		const std::string low = Lower(url);
		const bool net = low.compare(0, 7, "http://") == 0 || low.compare(0, 8, "https://") == 0 ||
						 low.compare(0, 7, "rtmp://") == 0 || low.compare(0, 7, "rtsp://") == 0 ||
						 low.compare(0, 6, "udp://") == 0 || low.compare(0, 6, "rtp://") == 0;
		net_source = net || low.compare(0, 6, "smb://") == 0;
		if (net)
		{
			av_dict_set(&opts, "reconnect", "1", 0);
			av_dict_set(&opts, "reconnect_streamed", "1", 0);
			av_dict_set(&opts, "rw_timeout", "15000000", 0);
			av_dict_set(&opts, "user_agent", "MPC-HC PS5", 0);
			const std::string ca = OrbisRoot() + "/cacert.pem";
			if (OrbisIsFile(ca))
			{
				av_dict_set(&opts, "ca_file", ca.c_str(), 0);
				av_dict_set(&opts, "tls_verify", "1", 0);
			}
		}
		std::string open_url = WithStoredPassword(url);
		const AVInputFormat* force_fmt = nullptr;
		disc_gen = disc::Generation(url);
		if (disc::IsTitleUrl(url))
		{
			// a DVD-Video or Blu-ray title: what to open, from the disc's structure (this reads the disc)
			const bool ok = disc::OpenTitle(url, title_open);
			{
				std::lock_guard<std::mutex> l(mu);
				for (const auto& t : title_open.titles)
					disc_titles.push_back({t.number, t.seconds, t.chapters});
				disc_title = title_open.title;
				for (const auto& c : title_open.chapters)
					disc_chapters.push_back({c.time, c.name});
			}
			if (!ok)
			{
				av_dict_free(&opts);
				avformat_free_context(ic);
				ic = nullptr;
				SetFailed(title_open.error);
				return;
			}
			force_fmt = title_open.format;
			if (title_open.io)
			{
				ic->pb = title_open.io;
				ic->flags |= AVFMT_FLAG_CUSTOM_IO;
			}
			else
				open_url = title_open.ffmpeg_url;
			av_dict_copy(&opts, title_open.options, 0);
		}
		else if (low.compare(0, 7, "disc://") == 0)
		{
			std::string err;
			custom_io = disc::OpenAvio(url, &custom_io_owner, &err);
			if (!custom_io)
			{
				av_dict_free(&opts);
				avformat_free_context(ic);
				ic = nullptr;
				SetFailed(err.empty() ? "The file can't be opened from the disc." : err);
				return;
			}
			custom_io_disc = true;
			ic->pb = custom_io;
			ic->flags |= AVFMT_FLAG_CUSTOM_IO;
		}
		if (low.compare(0, 6, "smb://") == 0)
		{
			std::string err;
			custom_io = smb::OpenAvio(url, &custom_io_owner, &err);
			if (!custom_io)
			{
				av_dict_free(&opts);
				avformat_free_context(ic);
				ic = nullptr;
				SetFailed(err.empty() ? "Can't open the network file" : err);
				return;
			}
			ic->pb = custom_io;
			ic->flags |= AVFMT_FLAG_CUSTOM_IO;
		}
		// 1.6: a raw .yuv file holds no size or format: opened as raw video only when its name says its size ("3840x2160",
		// and "p010" / "10bit" / "nv12" for the format, "NNfps"); otherwise a message says what to do
		const bool raw_yuv = !force_fmt && !ic->pb && low.size() > 4 && low.compare(low.size() - 4, 4, ".yuv") == 0;
		if (raw_yuv)
		{
			const std::string name = low.substr(low.find_last_of('/') + 1);
			int rw = 0, rh = 0;
			for (size_t i = 0; i < name.size() && !rw; i++)
				if (isdigit((unsigned char)name[i]) && (i == 0 || !isdigit((unsigned char)name[i - 1])))
					if (sscanf(name.c_str() + i, "%dx%d", &rw, &rh) != 2 || rw < 16 || rh < 16 || rw > 8192 || rh > 8192)
						rw = rh = 0;
			if (!rw)
			{
				av_dict_free(&opts);
				avformat_free_context(ic);
				ic = nullptr;
				SetFailed(T("Raw .yuv files have no picture size or format inside; convert it to .mp4 or .mkv (or .y4m), or "
							"put its size in its name (e.g. clip_3840x2160_p010_24fps.yuv)."));
				return;
			}
			const char* pf = name.find("p010") != std::string::npos || name.find("10bit") != std::string::npos ||
									 name.find("10le") != std::string::npos
								 ? "yuv420p10le"
							 : name.find("nv12") != std::string::npos ? "nv12"
																	  : "yuv420p";
			std::string fps = "25";
			const size_t f = name.find("fps");
			if (f != std::string::npos)
			{
				// "24fps", "23.976fps", "29.97fps": the digits before it, and one point with digits before that
				// ("clip.60fps": 60; "x_1.5.24fps": 5.24)
				size_t b = f;
				while (b > 0 && isdigit((unsigned char)name[b - 1]))
					b--;
				if (b >= 2 && b < f && name[b - 1] == '.' && isdigit((unsigned char)name[b - 2]))
				{
					size_t c = b - 1;
					while (c > 0 && isdigit((unsigned char)name[c - 1]))
						c--;
					if (c == 0 || name[c - 1] != 'x') // ("..._320x180.60fps": 180 is the size's, the rate 60)
						b = c;
				}
				const std::string tok = name.substr(b, f - b);
				char* end = nullptr;
				const double v = tok.empty() ? 0 : strtod(tok.c_str(), &end);
				if (v >= 1 && v <= 240 && end)
					fps = tok.substr(0, size_t(end - tok.c_str()));
			}
			force_fmt = av_find_input_format("rawvideo");
			av_dict_set(&opts, "video_size", (std::to_string(rw) + "x" + std::to_string(rh)).c_str(), 0);
			av_dict_set(&opts, "pixel_format", pf, 0);
			av_dict_set(&opts, "framerate", fps.c_str(), 0);
			OrbisLog("[player] raw .yuv: %dx%d %s at %s fps, from its name", rw, rh, pf, fps.c_str());
		}
		int r = avformat_open_input(&ic, open_url.c_str(), force_fmt, &opts);
		av_dict_free(&opts);
		if (r < 0)
		{
			ic = nullptr; // freed by avformat_open_input
			// a title on a disc in a drive whose structure FFmpeg's demuxer (libdvdnav) can't follow: a damaged disc
			if (r != AVERROR_EXIT && !quit && disc::IsTitleUrl(url) && !disc::DriveOf(url).empty())
			{
				OrbisLog("[player] the disc's title can't be opened: %s", AvErr(r).c_str());
				SetFailed(disc::DamagedText());
			}
			else
				SetFailed(AvErr(r));
			return;
		}
		ic->flags |= AVFMT_FLAG_GENPTS;
		r = avformat_find_stream_info(ic, nullptr);
		if (r < 0)
			OrbisLog("[player] stream info: %s (going on)", AvErr(r).c_str());
		if (quit)
			return;
		ic_ready = true;
		duration = ic->duration > 0 ? ic->duration / double(AV_TIME_BASE) : 0;
		if (title_open.io)
		{
			// a Blu-ray title: its length and its tracks' languages from the playlist (the MPEG-TS has none)
			if (title_open.duration > 0)
				duration = title_open.duration;
			for (unsigned i = 0; i < ic->nb_streams; i++)
				for (const auto& pl : title_open.languages)
					if (ic->streams[i]->id == pl.first && !av_dict_get(ic->streams[i]->metadata, "language", nullptr, 0))
						av_dict_set(&ic->streams[i]->metadata, "language", pl.second.c_str(), 0);
		}
		seekable = ic->pb ? (ic->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0 || duration > 0 : duration > 0;

		// the tracks: the first default (or first) video and audio, MPC-HC's way
		int v = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (v >= 0 && (ic->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC))
		{
			// an audio file's cover
			const AVPacket& pic = ic->streams[v]->attached_pic;
			if (pic.data && pic.size > 0 && res::DecodeImage(pic.data, size_t(pic.size), cover))
				has_cover = true;
			else if (pic.data && pic.size > 0)
				OrbisLog("[player] the cover picture (%d bytes) can't be read, or is over 8192 pixels a side: not shown",
					pic.size);
			v = -1;
			for (unsigned i = 0; i < ic->nb_streams; i++)
				if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
					!(ic->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC))
				{
					v = int(i);
					break;
				}
		}
		const int a = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, v, nullptr, 0);
		if (v >= 0)
			vdec = OpenDecoder(v, true);
		if (vdec)
		{
			std::string why;
			hw = hwdec::Open(ic->streams[v]->codecpar, &why);
			hw_active = hw != nullptr;
			if (hw)
			{
				// (review L-b) a wait for a picture buffer ends when the player quits or seeks; pictures copied while paused
				hwdec::Hooks hk;
				hk.ctx = this;
				hk.stop = [](void* c) {
					Impl* s = static_cast<Impl*>(c);
					return s->quit.load() || s->serial.load() != s->hw_serial.load();
				};
				hk.paused = [](void* c) { return static_cast<Impl*>(c)->paused.load(); };
				hwdec::SetHooks(hw, hk);
			}
			OrbisLog("[player] video decoder: %s", hw ? hwdec::Describe(hw).c_str() : ("FFmpeg (" + why + ")").c_str());
			// (test6 review) with the console's decoder too: FFmpeg's takes over if it gives up mid-film
			if (!BigVideoFits(v, why, hw != nullptr))
				return;
			if (decode_mode.load() == 1)
			{
				fast_level = 1; // (test12) Decoding: Fast -- opened so (OpenDecoder)
				OrbisLog("[player] decoding: fast (Options > Playback): no loop filter, FFmpeg's fast flag, from the start");
			}
		}
		if (a >= 0)
			adec = OpenDecoder(a, false);
		{
			std::lock_guard<std::mutex> l(mu);
			vstream = vdec ? v : -1;
			astream = adec ? a : -1;
		}
		for (unsigned i = 0; i < ic->nb_streams; i++)
		{
			if (ic->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
				continue;
			if (AVCodecContext* c = OpenDecoder(int(i), false))
			{
				std::lock_guard<std::mutex> l(mu);
				sdecs[int(i)] = c;
				sub_streams.push_back(int(i));
			}
			// ASS/SSA: libass draws it, from the script's header and each packet as it comes
			const AVCodecParameters* p = ic->streams[i]->codecpar;
			if (sdecs.count(int(i)) && (p->codec_id == AV_CODEC_ID_ASS || p->codec_id == AV_CODEC_ID_SSA))
			{
				if (ass::Renderer* r = Ass(true))
				{
					r->NewTrack(int(i), p->extradata, p->extradata ? size_t(p->extradata_size) : 0);
					if (r->HasTrack(int(i)))
						ass_streams.insert(int(i));
				}
			}
		}
		// subtitles on by default when a track is marked default or forced (MPC-HC: "Auto-load subtitles")
		for (int s : sub_streams)
			if (ic->streams[s]->disposition & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED))
			{
				sstream = s;
				break;
			}
		if (sstream < 0 && !sub_streams.empty())
			sstream = sub_streams.front();
		for (unsigned i = 0; i < ic->nb_streams; i++)
			if (int(i) != vstream && int(i) != astream && !sdecs.count(int(i)))
				ic->streams[i]->discard = AVDISCARD_ALL;
		if (vstream < 0 && astream < 0)
		{
			SetFailed("No video or audio this player can decode");
			return;
		}
		if (disc::IsTitleUrl(url))
		{
			// a disc's title: its chapters and its tracks' languages, as Navigate and Play show them
			std::string langs;
			for (unsigned i = 0; i < ic->nb_streams; i++)
				if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
				{
					const AVDictionaryEntry* l = av_dict_get(ic->streams[i]->metadata, "language", nullptr, 0);
					langs += std::string(langs.empty() ? "" : ", ") + "#" + std::to_string(i) + " " +
							 (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? "audio" : "subtitles") + " " + (l ? l->value : "und");
				}
			OrbisLog("[player] disc title %d of %zu: %u chapter(s); %s", disc_title, disc_titles.size(),
				disc_chapters.empty() ? ic->nb_chapters : unsigned(disc_chapters.size()), langs.c_str());
		}
		OrbisLog("[player] opened %s in %.2f s: %s, %.1f s, video #%d %s %dx%d, audio #%d %s, %zu subtitle track(s)",
			url.c_str(), NowS() - t0, ic->iformat->name, duration, vstream.load(), vdec ? vdec->codec->name : "-",
			vdec ? vdec->width : 0, vdec ? vdec->height : 0, astream.load(), adec ? adec->codec->name : "-",
			sub_streams.size());

		// look for subtitle files next to a local video, as MPC-HC does
		AutoLoadSubtitles();

		if (start_at > 0 && seekable)
		{
			std::lock_guard<std::mutex> l(seek_mu);
			seek_req = true;
			seek_to = start_at;
			seek_exact = true;
		}
		else
			clock.Set(0);
		show_after_seek = true;
		paused = start_paused;
		clock.SetPaused(start_paused);
		hold_clock.SetPaused(start_paused);
		ps5audio::SetPaused(start_paused);
		state = start_paused ? PlayState::Paused : PlayState::Playing;

		// 1.5.2: a thread the console refuses fails the open (up to 1.5.1: std::terminate); the ones started stop
		// when the app closes the file
		try
		{
			demuxer = ps5::StartThread("demuxer", [this] { DemuxThread(); });
			if (vdec)
				vthread = ps5::StartThread("video", [this] { VideoThread(); });
			if (adec)
				athread = ps5::StartThread("audio", [this] { AudioThread(); });
		}
		catch (const std::system_error& e)
		{
			OrbisLog("[player] a new thread refused: %s", e.what());
			SetFailed("The console refused a new thread");
		}
	}

	void AutoLoadSubtitles()
	{
		if (url.find("://") != std::string::npos && url.compare(0, 6, "smb://") != 0 && url.compare(0, 7, "disc://") != 0)
			return;
		const size_t dot = url.find_last_of('.');
		const size_t slash = url.find_last_of('/');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
			return;
		const std::string base = url.substr(0, dot);
		// 1.5.2: .sup too; a VobSub pair through its .sub (AddSubtitleFile reads the .idx beside it)
		static const char* const exts[] = {".srt", ".ass", ".ssa", ".vtt", ".sub", ".sup", ".pt-BR.srt", ".pt.srt",
			".por.srt", ".en.srt", ".eng.srt"};
		bool any = false;
		for (const char* e : exts)
		{
			const std::string p = base + e;
			if (AnyExists(p))
			{
				std::string err;
				if (AddSubtitleFile(p, &err))
					any = true;
			}
		}
		if (any)
		{
			// an external file wins over embedded tracks (MPC-HC's priority)
			std::lock_guard<std::mutex> l(smu);
			for (auto& n : external_names)
			{
				sstream = n.first;
				break;
			}
		}
	}

	// ---------------------------------------------------------------------------------------------
	void DemuxThread()
	{
		disc::CancelScope cancel_reads(&quit);
		AVPacket* pkt = av_packet_alloc();
		int64_t last_sub_check = 0;
		(void)last_sub_check;
		while (!quit)
		{
			// a seek?
			bool do_seek = false;
			int taken_gen = 0;
			double target = 0;
			bool exact = true;
			{
				std::lock_guard<std::mutex> l(seek_mu);
				if (seek_req)
				{
					do_seek = true;
					target = seek_to;
					exact = seek_exact;
					seek_req = false;
					taken_gen = seek_gen.load();
				}
			}
			if (do_seek)
			{
#ifndef __PROSPERO__
				// host tests: MPCPS5_HOST_SLOW_SEEK=ms makes every seek take that long here (a slow source)
				static const int slow_ms = getenv("MPCPS5_HOST_SLOW_SEEK") ? atoi(getenv("MPCPS5_HOST_SLOW_SEEK")) : 0;
				for (int w = 0; w < slow_ms / 10 && !quit; w++)
					av_usleep(10000);
#endif
				target = std::max(0.0, duration > 0 ? std::min(target, duration) : target);
				const int64_t ts = int64_t(target * AV_TIME_BASE) + (ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0);
				const int r = avformat_seek_file(ic, -1, INT64_MIN, ts, ts, 0);
				if (r < 0)
					OrbisLog("[player] seek to %.2f failed: %s", target, AvErr(r).c_str());
				NewPgsDecoders();
				vq.Flush();
				aq.Flush();
				serial_start = target;
				a_read = v_read = -1e18;
				serial++;
				ps5audio::Clear();
				{
					// (test10 review) against a release of the hold before this seek (ReleaseHold)
					std::lock_guard<std::mutex> hl(hold_mu);
					v_seek_target = target;
					skip_until = exact ? target - 0.001 : -1e18;
					clock.Set(target);
					// (test8 review) an exact one waits for its first picture (only with a picture to wait for); a
					// keyframe seek doesn't: its first picture is the keyframe it lands on, the first one decoded, and
					// the held jumps and the seek bar's drag go by them
					if (exact && vstream >= 0)
					{
						hold_clock.Set(0);
						hold_gaveup_serial = -1;
						clock.SetHeld(true);
						ps5audio::HoldUntilPush();
						hold_serial = serial.load();
					}
					else
					{
						hold_serial = -1;
						clock.SetHeld(false);
					}
				}
				ProgressReset();
#ifndef __PROSPERO__
				fault_step = 0;
#endif
				seek_done = taken_gen;
				eof = false;
				ended = false;
				show_after_seek = true;
				{
					std::lock_guard<std::mutex> l(fmu);
					for (auto& f : frames)
						av_frame_free(&f.f);
					frames.clear();
					if (hold_last.serial != serial.load())
						av_frame_free(&hold_last.f);
					fcv.notify_all();
				}
			}
			// enough queued? (10 s or 64 MiB)
			// (test6 review) the demuxer never leaves packets out (two rounds showed it can't tell a starving sound from
			// a decoder still working toward a seek's target). When the sound is short -- what the audio side really holds
			// (not yet played, plus its packets) under a second, once a seek has settled (its first picture shown, and
			// sound of this serial given to the console past skip_until) and while the sound is still coming (the video
			// read isn't more than 2 s past it: a track that ended or a gap isn't short) -- it reads on past the normal
			// limit, up to a hard cap of 1.5 times it (96 MiB), and waits there. Video lateness is the video thread's
			// (its keyframe skip, as VLC and mpv do).
#ifndef __PROSPERO__
			// host tests: MPCPS5_HOST_DEMUX_MB=n: n MiB of queued packets is "full" (64 normally; the cap 1.5 times it)
			static const size_t limit = getenv("MPCPS5_HOST_DEMUX_MB") ? size_t(atoi(getenv("MPCPS5_HOST_DEMUX_MB"))) << 20
																	   : size_t(64) << 20;
#else
			constexpr size_t limit = size_t(64) << 20;
#endif
			size_t cap = limit * 3 / 2;
			const size_t queued = vq.bytes + aq.bytes;
			demux_peak = std::max(demux_peak, queued);
			bool audio_short = false;
			if (astream >= 0 && vstream >= 0 && !eof && !paused.load() && !SeekPending() && !show_after_seek.load() &&
				a_pushed_serial.load() == serial.load() && a_read > -1e17 && v_read - a_read < 2.0)
			{
				const double held = ps5audio::Queued() / double(ps5audio::kRate) * rate.load() +
									aq.Seconds(ic->streams[astream]->time_base);
				audio_short = held < 1.0;
			}
			const bool normal_full = (vq.bytes + aq.bytes) > limit ||
									 ((vstream < 0 || vq.Count() > 300) && (astream < 0 || aq.Count() > 500));
			if (audio_short && normal_full)
			{
				// (test7 review) never into the heap the pictures need: the cap is also what the heap leaves (measured now)
				// past FFmpeg's pictures' estimate and a margin, and never under the normal limit
				const size_t left = HeapLeft();
				const size_t keep = (pictures_direct ? 0 : size_t(pic_estimate)) + (size_t(32) << 20);
				if (left != SIZE_MAX)
					cap = std::max(limit, std::min(cap, left > keep ? left - keep : 0));
			}
			const bool full = audio_short ? queued > cap : normal_full;
			if (audio_short && normal_full)
				demux_over = true;
			else if (demux_over && !normal_full)
				demux_over = false;
			// (test8 review) said once per seek (and file), not at every toggle
			if (demux_over && demux_said_serial != serial.load())
			{
				demux_said_serial = serial.load();
				if (cap > limit)
					OrbisLog("[player] the sound is short behind a full video queue: reading on (up to %zu MiB queued)",
						cap >> 20);
				else
					OrbisLog("[player] the sound is short behind a full video queue: the heap leaves no room past %zu MiB "
							 "(%zu MiB free, about %zu MiB kept for pictures)",
						limit >> 20, HeapLeft() >> 20, size_t(pic_estimate) >> 20);
			}
			if (full || eof)
			{
				// (third review: neither counts as failing -- the end drains the decoders to vdone_serial, and full
				// queues with no video left show up as the sound run past the target)
				av_usleep(10000);
				if (eof && vdone_serial == serial && (astream < 0 || adone_serial == serial) && !ended)
					ended = true;
				else if (eof && vstream < 0 && astream >= 0 && adone_serial == serial && !ended)
					ended = true;
				continue;
			}
			ProgressReading(true);
#ifndef __PROSPERO__
			// host tests: MPCPS5_HOST_SEEK_READ_FAULT=ms -- after each seek, the first read fails at once and the next
			// one takes that long (a share retrying after an error)
			static const int fault_ms =
				getenv("MPCPS5_HOST_SEEK_READ_FAULT") ? atoi(getenv("MPCPS5_HOST_SEEK_READ_FAULT")) : 0;
			int r;
			if (fault_ms > 0 && fault_step == 0)
			{
				fault_step = 1;
				r = AVERROR(EIO);
			}
			else
			{
				if (fault_ms > 0 && fault_step == 1)
				{
					fault_step = 2;
					for (int w = 0; w < fault_ms / 10 && !quit; w++)
						av_usleep(10000);
				}
				r = av_read_frame(ic, pkt);
			}
#else
			const int r = av_read_frame(ic, pkt);
#endif
			if (r < 0 && disc_gen && disc::Generation(url) != disc_gen)
			{
				// the disc was taken out: not the end of the film (the UI stops it and says so)
				if (!disc_gone)
					OrbisLog("[player] the disc was taken out while playing %s", url.c_str());
				disc_gone = true;
				ProgressFail();
				av_usleep(20000);
				continue;
			}
			if (r < 0)
			{
				if (r == AVERROR_EOF || (ic->pb && avio_feof(ic->pb)))
				{
					ProgressReading(false);
					if (!eof)
					{
						if (vstream >= 0)
							vq.Put(nullptr, serial);
						if (astream >= 0)
							aq.Put(nullptr, serial);
						eof = true;
					}
				}
				else if (r == AVERROR_EXIT)
					break;
				else
				{
					ProgressFail(); // a demuxer error without the end flag (it is tried again)
					av_usleep(10000);
				}
				continue;
			}
			const int idx = pkt->stream_index;
			ProgressRead(idx, pkt);
			const int64_t pt = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			const double pts_s = pt != AV_NOPTS_VALUE && idx >= 0 && idx < int(ic->nb_streams)
									 ? pt * av_q2d(ic->streams[idx]->time_base) - StreamStart()
									 : -1e18;
			if (idx == astream && pts_s > -1e17)
				a_read = std::max(a_read, pts_s);
			if (idx == vstream)
			{
				if (pts_s > -1e17)
					v_read = std::max(v_read, pts_s);
				vq.Put(av_packet_clone(pkt), serial);
			}
			else if (idx == astream)
				aq.Put(av_packet_clone(pkt), serial);
			else
			{
				auto it = sdecs.find(idx);
				if (it != sdecs.end() && PgsPasses(idx, pkt))
					DecodeSubtitle(idx, it->second, pkt);
			}
			demux_peak = std::max(demux_peak, vq.bytes + aq.bytes);
			av_packet_unref(pkt);
		}
		av_packet_free(&pkt);
		if (demux_peak)
			OrbisLog("[player] demuxer: at most %zu KiB of packets queued at once", demux_peak >> 10);
	}

	static SubItem ItemTimes(const AVSubtitle& sub, double pts, int ref_w, int ref_h)
	{
		SubItem item;
		item.start = pts + sub.start_display_time / 1000.0;
		item.open_end = sub.end_display_time == 0 || sub.end_display_time == UINT32_MAX;
		item.end = item.open_end ? 1e18 : pts + sub.end_display_time / 1000.0;
		item.show.ref_w = ref_w;
		item.show.ref_h = ref_h;
		return item;
	}

	void StoreSubtitle(int track, const AVSubtitle& sub, double pts, int ref_w, int ref_h)
	{
		SubItem item = ItemTimes(sub, pts, ref_w, ref_h);
		ShowFromSubtitle(sub, item.show);
		InsertSubtitle(track, std::move(item));
	}

	// an external bitmap file's line: its times now, its pictures when it shows (DecodeShown)
	void StoreExternalBitmap(int track, const AVSubtitle& sub, double pts, int ref_w, int ref_h, int first_pkt,
		int last_pkt, int epoch_pkt)
	{
		SubItem item = ItemTimes(sub, pts, ref_w, ref_h);
		for (unsigned i = 0; i < sub.num_rects; i++)
			if (sub.rects[i]->type == SUBTITLE_BITMAP && sub.rects[i]->w > 0 && sub.rects[i]->h > 0)
				item.need_pics = true;
		item.first_pkt = first_pkt;
		item.last_pkt = last_pkt;
		item.epoch_pkt = epoch_pkt;
		InsertSubtitle(track, std::move(item));
	}

	// 1.5.2 (review): a bitmap subtitle's pictures are held in: over 4096 pixels a side, over twice the picture it goes
	// on (show.ref_w x ref_h, when known), or over 4096 x 4096 pixels for the line, a picture isn't made -- a 48 KB .sup
	// with one 8000 x 8000 line took ~1 GB -- and an allocation that fails drops the line instead of ending the app
	static constexpr int kMaxSubPicSide = 4096;
	static constexpr size_t kMaxSubLinePixels = size_t(4096) * 4096;
	static bool SubPictureFits(const AVSubtitleRect* r, const SubtitleShow& show, size_t* pixels)
	{
		if (r->w > kMaxSubPicSide || r->h > kMaxSubPicSide)
			return false;
		if (show.ref_w > 0 && show.ref_h > 0 && (r->w > 2 * show.ref_w + 16 || r->h > 2 * show.ref_h + 16))
			return false;
		*pixels += size_t(r->w) * size_t(r->h);
		return *pixels <= kMaxSubLinePixels;
	}

	static void ShowFromSubtitle(const AVSubtitle& sub, SubtitleShow& show)
	{
		try
		{
			ShowFromSubtitleRects(sub, show);
		}
		catch (const std::bad_alloc&)
		{
			OrbisLog("[subs] out of memory making a subtitle line's pictures: the line isn't shown");
			show.pics.clear();
		}
	}

	static void ShowFromSubtitleRects(const AVSubtitle& sub, SubtitleShow& show)
	{
		size_t pixels = 0;
		for (unsigned i = 0; i < sub.num_rects; i++)
		{
			const AVSubtitleRect* r = sub.rects[i];
			if (r->type == SUBTITLE_ASS || r->type == SUBTITLE_TEXT)
			{
				const std::string text = r->type == SUBTITLE_ASS ? AssEventText(r->ass) : std::string(r->text ? r->text : "");
				AssToText(text, show.lines, show.italic, show.top);
			}
			else if (r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0 && r->data[0] && r->data[1])
			{
				if (!SubPictureFits(r, show, &pixels))
				{
					static std::atomic<int> said{0};
					if (said++ < 20)
						OrbisLog("[subs] a bitmap subtitle picture of %dx%d (on a %dx%d picture): over the bounds, not shown",
							r->w, r->h, show.ref_w, show.ref_h);
					continue;
				}
				SubtitleShow::Pic pic;
				pic.x = r->x;
				pic.y = r->y;
				// straight into the picture, premultiplied (Image::FromRGBA's arithmetic): one allocation, not two
				auto img = std::make_shared<Image>(r->w, r->h);
				uint32_t* out = img->Data();
				const uint32_t* pal = reinterpret_cast<const uint32_t*>(r->data[1]);
				for (int y = 0; y < r->h; y++)
					for (int x = 0; x < r->w; x++)
					{
						const uint32_t c = pal[r->data[0][y * r->linesize[0] + x]]; // 0xAARRGGBB
						const uint32_t a = c >> 24;
						const uint32_t rr = (((c >> 16) & 0xff) * a + 127) / 255, gg = (((c >> 8) & 0xff) * a + 127) / 255,
									   bb = ((c & 0xff) * a + 127) / 255;
						out[size_t(y) * r->w + x] = rr | (gg << 8) | (bb << 16) | (a << 24);
					}
				pic.img = std::move(img);
				show.pics.push_back(std::move(pic));
			}
		}
	}

	void InsertSubtitle(int track, SubItem&& item)
	{
		std::lock_guard<std::mutex> l(smu);
		std::vector<SubItem>& list = subs[track];
		// a line that was shown "until the next one" ends here: one of the few that start before this one (1.5.2, review:
		// the whole list was scanned back when none was open -- O(n^2) over a file's lines, minutes for 100,000). The
		// scan starts where this one goes, not at the list's end: after a seek back the lines read ahead are later, and
		// a replayed line (stored again mid-list, open-ended) must still be ended by its clearing set
		auto from = std::upper_bound(list.begin(), list.end(), item.start, [](double v, const SubItem& a) { return v < a.start; });
		int back = 0;
		for (auto it = std::make_reverse_iterator(from); it != list.rend() && back < 64; ++it, ++back)
			if (it->open_end)
			{
				it->end = item.start;
				it->open_end = false;
				break;
			}
		if (item.show.Empty() && !item.need_pics)
			return; // an empty one only ends the last (PGS)
		const double start = item.start;
		const size_t bytes = PictureBytes(item.show);
		// keep the list sorted by start; replace an identical start (a seek replays packets)
		auto pos = std::lower_bound(list.begin(), list.end(), item.start,
			[](const SubItem& a, double s) { return a.start < s; });
		if (pos != list.end() && std::fabs(pos->start - item.start) < 0.001)
		{
			sub_pic_bytes -= std::min(sub_pic_bytes, PictureBytes(pos->show));
			*pos = std::move(item);
		}
		else
			list.insert(pos, std::move(item));
		sub_pic_bytes += bytes;
		if (bytes && sub_pic_bytes > sub_pic_budget)
			TrimSubtitlePictures(start);
	}

	// (smu held) over the budget: the pictures of every track's lines more than kSubPictureKeep s from `newest` (the
	// line just decoded) go -- those that ended before it, and, after a seek back, those that start long after it --
	// the furthest first, until half the budget is used; a line left with no text goes with them. A line let go is
	// decoded again when the film gets back to it (a seek replays the packets; an external file's from its packets kept,
	// DecodeShown).
	void TrimSubtitlePictures(double newest)
	{
		std::vector<std::pair<double, std::pair<int, size_t>>> old; // distance from newest, (track, index)
		for (auto& t : subs)
			for (size_t i = 0; i < t.second.size(); i++)
			{
				const SubItem& it = t.second[i];
				if (it.show.pics.empty())
					continue;
				if (!it.open_end && it.end < newest - kSubPictureKeep)
					old.push_back({newest - it.end, {t.first, i}});
				else if (it.start > newest + kSubPictureKeep)
					old.push_back({it.start - newest, {t.first, i}});
			}
		std::sort(old.begin(), old.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
		const size_t before = sub_pic_bytes;
		size_t dropped = 0;
		for (const auto& o : old)
		{
			if (sub_pic_bytes <= sub_pic_budget / 2)
				break;
			SubItem& item = subs[o.second.first][o.second.second];
			SubtitleShow& sh = item.show;
			sub_pic_bytes -= std::min(sub_pic_bytes, PictureBytes(sh));
			sh.pics.clear();
			sh.pics.shrink_to_fit();
			item.need_pics = item.first_pkt >= 0; // an external file's: decoded again from its packets when it shows
			dropped++;
		}
		// the lines with nothing left to show
		for (auto& t : subs)
			t.second.erase(std::remove_if(t.second.begin(), t.second.end(),
							   [](const SubItem& it) { return it.show.Empty() && !it.need_pics; }),
				t.second.end());
		if (dropped)
			OrbisLog("[subs] bitmap subtitles over %zu MiB (%zu MiB): %zu picture(s) of lines over %.0f s from %.1f s let go, "
					 "%zu MiB kept",
				sub_pic_budget >> 20, before >> 20, dropped, kSubPictureKeep, newest, sub_pic_bytes >> 20);
		else if (std::fabs(newest - sub_trim_said) > 60)
		{
			sub_trim_said = newest;
			OrbisLog("[subs] bitmap subtitles over %zu MiB (%zu MiB), all within %.0f s of %.1f s: kept",
				sub_pic_budget >> 20, before >> 20, kSubPictureKeep, newest);
		}
	}

	// (smu held) a PGS line's epoch before it, packets [from, to), into the line's decoder: the epoch's start (its
	// presentation segment: the screen's size), every palette segment (a palette is built entry by entry, a fade's
	// updates on top), and each object's latest definition (its first fragment and those after it: an older one is
	// replaced anyway). Windows, display ends (they would make pictures) and other presentations are passed over. Every
	// palette, whatever the epoch's length (review: 1.6's first build replayed the last 4,096 packets, and a line far
	// into a long epoch lost its palette and was never drawn); objects at most one per id (the decoder holds 64).
	static void ReplayEpoch(const ExtBitmap& x, AVCodecContext* c, int from, int to)
	{
		std::map<int, int> latest; // object id -> the packet of its latest first fragment before the line
		for (int k = to - 1; k >= from; k--)
		{
			const PgsInfo g = PgsScan(x.pkts[size_t(k)]);
			if (g.segments == 1 && g.object && g.object_first && !latest.count(g.object_id))
				latest[g.object_id] = k;
		}
		bool presentation = false;
		for (int k = from; k < to; k++)
		{
			const AVPacket* q = x.pkts[size_t(k)];
			const PgsInfo g = PgsScan(q);
			bool replay = false;
			if (g.segments > 1)
				replay = true; // not one segment a packet (not the sup reader's): as it is
			else if (g.palette)
				replay = true;
			else if (g.object)
			{
				auto l = latest.find(g.object_id);
				replay = l != latest.end() && k >= l->second;
			}
			else if (g.presentation)
			{
				replay = !presentation;
				presentation = true;
			}
			if (!replay || !q)
				continue;
			AVSubtitle one = {};
			int got = 0;
			if (avcodec_decode_subtitle2(c, &one, &got, q) >= 0 && got)
				avsubtitle_free(&one);
		}
	}

	// (smu held) an external bitmap file's lines showing at t whose pictures aren't in memory: decoded from their
	// packets, then the budget kept
	void DecodeShown(int track, double t)
	{
		auto e = ext_bitmaps.find(track);
		auto li = subs.find(track);
		if (e == ext_bitmaps.end() || li == subs.end())
			return;
		std::vector<SubItem>& list = li->second;
		auto end = std::upper_bound(list.begin(), list.end(), t, [](double v, const SubItem& a) { return v < a.start; });
		bool added = false;
		int seen = 0;
		for (auto i = end; i != list.begin() && seen < 64;)
		{
			--i;
			seen++;
			if (t >= i->start && t < i->end && i->need_pics)
			{
				i->need_pics = false; // tried once, whatever comes of it
				ExtBitmap& x = *e->second;
				// PGS (1.6, the audit): a decoder of its own, the line's epoch replayed into it first -- up to 1.5.4 the line's
				// own packets went to the file's one decoder, left at the file's end (or the line decoded last): a normal
				// display set took another epoch's palette or object (a fade in another colour, another line's picture)
				AVCodecContext* own = x.pgs ? x.NewDecoder() : nullptr;
				AVCodecContext* c = own ? own : x.c;
				const int npk = int(x.pkts.size());
				if (x.pgs && i->first_pkt >= 0 && i->epoch_pkt < i->first_pkt)
					ReplayEpoch(x, c, std::max(0, i->epoch_pkt), std::min(i->first_pkt, npk));
				AVSubtitle sub = {};
				bool have = false;
				for (int k = i->first_pkt; k >= 0 && k <= i->last_pkt && k < npk; k++)
				{
					AVSubtitle one = {};
					int got = 0;
					if (x.pkts[size_t(k)] && avcodec_decode_subtitle2(c, &one, &got, x.pkts[size_t(k)]) >= 0 && got)
					{
						if (have)
							avsubtitle_free(&sub);
						sub = one;
						have = true;
					}
				}
				avcodec_free_context(&own);
				if (!have)
					continue;
				SubtitleShow show;
				show.ref_w = i->show.ref_w;
				show.ref_h = i->show.ref_h;
				ShowFromSubtitle(sub, show);
				avsubtitle_free(&sub);
				i->show.pics = std::move(show.pics);
				sub_pic_bytes += PictureBytes(i->show);
				added = true;
			}
		}
		if (added && sub_pic_bytes > sub_pic_budget)
			TrimSubtitlePictures(t);
	}

	void DecodeSubtitle(int idx, AVCodecContext* c, AVPacket* pkt)
	{
		if (ass_streams.count(idx))
		{
			// libass's track: the packet as it is ("ReadOrder,Layer,Style,..."), its time from the file's start
			AVStream* st = ic->streams[idx];
			const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			if (ts == AV_NOPTS_VALUE || !pkt->data || pkt->size <= 0)
				return;
			const double start = ts * av_q2d(st->time_base) - StreamStart();
			const double dur = pkt->duration > 0 ? pkt->duration * av_q2d(st->time_base) : 5.0;
			if (ass::Renderer* r = Ass(false))
				r->AddEvent(idx, pkt->data, size_t(pkt->size), int64_t(std::llround(start * 1000)), int64_t(std::llround(dur * 1000)));
			return;
		}
		AVSubtitle sub = {};
		int got = 0;
		if (avcodec_decode_subtitle2(c, &sub, &got, pkt) >= 0 && got)
		{
			AVStream* st = ic->streams[idx];
			double pts = 0;
			if (sub.pts != AV_NOPTS_VALUE)
				pts = sub.pts / double(AV_TIME_BASE);
			else if (pkt->pts != AV_NOPTS_VALUE)
				pts = pkt->pts * av_q2d(st->time_base);
			if (ic->start_time != AV_NOPTS_VALUE)
				pts -= ic->start_time / double(AV_TIME_BASE);
			if (pkt->duration > 0 && sub.end_display_time == 0 && c->codec_descriptor &&
				(c->codec_descriptor->props & AV_CODEC_PROP_TEXT_SUB))
				sub.end_display_time = uint32_t(pkt->duration * av_q2d(st->time_base) * 1000);
			int rw = c->width, rh = c->height;
			if ((rw <= 0 || rh <= 0) && vdec)
			{
				rw = vdec->width;
				rh = vdec->height;
			}
			StoreSubtitle(idx, sub, pts, rw, rh);
			avsubtitle_free(&sub);
		}
	}

	double StreamStart() const
	{
		return ic && ic->start_time != AV_NOPTS_VALUE ? ic->start_time / double(AV_TIME_BASE) : 0;
	}

	// ---------------------------------------------------------------------------------------------
	bool SetupDeinterlace(const AVFrame* f)
	{
		if (vgraph && vg_w == f->width && vg_h == f->height && vg_fmt == f->format)
			return true;
		avfilter_graph_free(&vgraph);
		vgraph = avfilter_graph_alloc();
		char args[256];
		AVStream* st = ic->streams[vstream];
		snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", f->width,
			f->height, f->format, st->time_base.num, st->time_base.den, std::max(1, f->sample_aspect_ratio.num),
			std::max(1, f->sample_aspect_ratio.den));
		AVFilterContext* bw = nullptr;
		if (avfilter_graph_create_filter(&vsrc, avfilter_get_by_name("buffer"), "in", args, nullptr, vgraph) < 0 ||
			avfilter_graph_create_filter(&vsink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, vgraph) < 0 ||
			avfilter_graph_create_filter(&bw, avfilter_get_by_name("bwdif"), "bwdif", "mode=send_frame:deint=interlaced",
				nullptr, vgraph) < 0 ||
			avfilter_link(vsrc, 0, bw, 0) < 0 || avfilter_link(bw, 0, vsink, 0) < 0 ||
			avfilter_graph_config(vgraph, nullptr) < 0)
		{
			avfilter_graph_free(&vgraph);
			vg_fmt = -2; // don't try again for this format
			return false;
		}
		vg_w = f->width;
		vg_h = f->height;
		vg_fmt = f->format;
		OrbisLog("[player] interlaced video: deinterlacing with bwdif");
		return true;
	}

	void QueueFrame(AVFrame* f, int my_serial)
	{
		AVStream* st = ic->streams[vstream];
		const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		double pts = ts == AV_NOPTS_VALUE ? NAN : ts * av_q2d(st->time_base) - StreamStart();
		AVRational fr = av_guess_frame_rate(ic, st, f);
		double dur = fr.num && fr.den ? av_q2d(av_inv_q(fr)) : 1.0 / 25;
		if (f->duration > 0)
			dur = f->duration * av_q2d(st->time_base);
		static thread_local double last_pts = 0;
		if (std::isnan(pts))
			pts = last_pts + dur;
		last_pts = pts;
		const bool gave_up = hold_gaveup_serial.load() == my_serial;
		if (pts + dur < skip_until.load())
		{
			if (gave_up)
			{
				// (test9 review) an exact seek's hold let go with no picture: these are late now, the skip can help
				v_behind = clock.Get() + display_delay.load() * rate.load() - (pts + dur);
				v_last_pts = pts;
			}
			else if (hold_serial.load() == my_serial)
			{
				// (test10 review) the latest picture before an exact seek's target, for a let-go at the hold's bound
				std::lock_guard<std::mutex> l(fmu);
				if (hold_serial.load() == my_serial && serial == my_serial)
				{
					av_frame_free(&hold_last.f);
					hold_last = {f, pts, dur, my_serial};
					return;
				}
			}
			av_frame_free(&f);
			return;
		}
		// (test7 review) how far behind, for the keyframe skip, with either decoder; faster decoding is FFmpeg's
		{
			const double now = clock.Get() + display_delay.load() * rate.load();
			// (until a seek's first picture is shown the clock is still the old position's: nothing is behind yet)
			// and the pictures before a seek's target (a keyframe seek shows from the keyframe before it) aren't late;
			// (test9) after an exact seek's hold let go with no picture they are
			v_behind = gave_up ? now - (pts + dur)
					   : show_after_seek.load() || SeekPending() || pts < v_seek_target.load() - 0.001 ? 0.0
																									   : now - (pts + dur);
			v_last_pts = pts;
		}
		if (!hw)
			KeepUp(pts, dur, my_serial);
		std::unique_lock<std::mutex> l(fmu);
		fcv.wait(l, [&] { return quit || frames.size() < max_frames || serial != my_serial; });
		if (quit || serial != my_serial)
		{
			av_frame_free(&f);
			return;
		}
		frames.push_back({f, pts, dur, my_serial});
	}

	void VideoThread()
	{
		AVFrame* frame = av_frame_alloc();
		int dec_serial = -1;
		bool sw_wait_key = false; // the video came back to FFmpeg mid-stream: from the next keyframe
		auto send = [&](const AVPacket* pkt) -> int {
			if (hw)
				return hwdec::Send(hw, pkt);
			if (pkt && sw_wait_key)
			{
				if (!(pkt->flags & AV_PKT_FLAG_KEY))
					return 0;
				sw_wait_key = false;
			}
			return avcodec_send_packet(vdec, pkt);
		};
		auto receive = [&](AVFrame* f) -> int { return hw ? hwdec::Receive(hw, f) : avcodec_receive_frame(vdec, f); };
		auto flush = [&] {
			if (hw)
				hwdec::Flush(hw);
			else
				avcodec_flush_buffers(vdec);
		};
		while (!quit)
		{
			QPacket qp;
			vbusy = false; // (the last packet's pictures queued, or dropped)
			const int r = vq.Get(qp, 100, &vbusy);
			if (r < 0)
				break;
			if (r == 0)
				continue;
			if (qp.serial != serial)
			{
				av_packet_free(&qp.pkt);
				continue;
			}
			if (qp.serial != dec_serial)
			{
				flush();
				if (vgraph)
					avfilter_graph_free(&vgraph), vg_fmt = -1;
				dec_serial = qp.serial;
				hw_serial = dec_serial;
				vskip_to_key = false;
				v_behind = 0;
			}
			// (test4 review) FFmpeg far behind the clock (more than a second): the packets up to the next keyframe are
			// left out, so the video catches up instead of falling further behind (ffplay's way), the sound untouched.
			// (test5 review) The decoder flushed where it goes on (open-GOP leading pictures).
			// (test7 review) Whenever a keyframe is queued at all (a long GOP too: AV1 at 60 fps with 5 s between
			// keyframes), as long as it isn't more than 2 s past the clock (the picture would stand still longer than
			// it is behind; further behind, the next one is close enough); bounded by time, not packets: past 2.5 s
			// beyond the clock with no keyframe (an unmarked one), it goes on there. The console's decoder too: it is
			// reset there as after a seek (sceVideodec2Reset, then nothing but a keyframe), the calls every seek makes.
			// (test8 review) paused only keeps a skip from starting: one under way goes on (a frame step mustn't let the
			// packets it was leaving out through to a decoder that lost their references)
			if (qp.pkt)
			{
				const bool key = (qp.pkt->flags & AV_PKT_FLAG_KEY) != 0;
				const AVRational tb = ic->streams[vstream]->time_base;
				const int64_t pt = PacketQueue::Ts(qp.pkt);
				const double pts_s = pt != AV_NOPTS_VALUE ? pt * av_q2d(tb) - StreamStart() : -1e18;
				int64_t kt = AV_NOPTS_VALUE;
				if (!vskip_to_key && !paused.load() && v_behind.load() > 1.0 && !key && !NoVideoSkip() && vq.NextKey(&kt))
				{
					const double kts = kt != AV_NOPTS_VALUE ? kt * av_q2d(tb) - StreamStart() : -1e18;
					const double now = clock.Get();
					// (test8 review) and only for a keyframe at least half a second on: one right behind this packet
					// gains nothing for the reset it costs
					if (kts <= now + 2.0 && (kts < -1e17 || pts_s < -1e17 || kts - pts_s >= 0.5))
					{
						vskip_to_key = true;
						vskipped = 0;
						OrbisLog("[player] the video is %.1f s behind: left out up to the next keyframe (%s)", v_behind.load(),
							kts > -1e17 ? Fmt("%.1f s from here", kts - std::max(pts_s, 0.0)).c_str() : "its time unknown");
					}
				}
				if (vskip_to_key && !key && !(pts_s > -1e17 && pts_s > clock.Get() + 2.5))
				{
					vskipped++;
					av_packet_free(&qp.pkt);
					continue;
				}
				if (vskip_to_key)
				{
					OrbisLog("[player] ... %u packets left out; decoding on from %s", vskipped,
						key ? (hw ? "a keyframe (the console's decoder reset)" : "a keyframe (the decoder flushed)")
							: "here (2.5 s past the clock without a keyframe: the decoder flushed)");
					vskip_to_key = false;
					flush();
					if (vgraph) // (test6 review) the deinterlacer's buffered pictures are from before the skip
						avfilter_graph_free(&vgraph), vg_fmt = -1;
					v_behind = 0;
				}
			}
			const double t0 = NowS();
			int sr = send(qp.pkt); // nullptr: drain
			if (hw && hwdec::Failed(hw))
			{
				// the console's decoder gave up: FFmpeg from here, starting at a keyframe
				OrbisLog("[player] video decoder: FFmpeg from now on");
				AVFrame* left = av_frame_alloc();
				while (hwdec::Receive(hw, left) == 0)
				{
					AVFrame* copy = av_frame_alloc();
					av_frame_move_ref(copy, left);
					QueueFrame(copy, dec_serial);
				}
				av_frame_free(&left);
				hwdec::Close(hw);
				hw = nullptr;
				hw_active = false;
				if (cpu_reopen_fallback)
				{
					// (test7 review) the decoder for it couldn't be opened at the start: again now (the console's
					// decoder's memory is free); the one there stays alive until the close (others may read it)
					AVCodecContext* n = cpu_lean_fallback ? OpenDecoder(vstream, true, true)
													  : OpenDecoder(vstream, true, false, cpu_fallback_threads);
					if (!n)
					{
						OrbisLog("[player] ... FFmpeg's decoder couldn't be opened either: the video stops here");
						SetFailed(T("The video decoder couldn't be opened."));
						av_packet_free(&qp.pkt);
						break;
					}
					vdec_retired.push_back(vdec);
					vdec = n;
					cpu_reopen_fallback = false;
				}
				if (cpu_lean_fallback)
				{
					std::lock_guard<std::mutex> l(fmu);
					max_frames = 3;
				}
				OrbisLog("[player] ... FFmpeg with %d %s threads%s", vdec->thread_count,
					(vdec->active_thread_type & FF_THREAD_FRAME) ? "frame" : "slice",
					cpu_lean_fallback ? ", 3 pictures queued" : "");
				avcodec_flush_buffers(vdec);
				sw_wait_key = true;
				sr = send(qp.pkt);
			}
			av_packet_free(&qp.pkt);
			if (sr < 0 && sr != AVERROR(EAGAIN) && sr != AVERROR_EOF)
				continue;
			for (;;)
			{
				const int rr = receive(frame);
				if (rr == AVERROR_EOF)
				{
					vdone_serial = dec_serial;
					flush();
					break;
				}
				if (rr < 0)
					break;
#ifndef __PROSPERO__
				// host tests: MPCPS5_HOST_SLOW_DECODE=ms,until: FFmpeg's pictures take ms more each (a decoder slower
				// than real time) while their time is before `until` seconds (test7: the console decoder's stand-in's too)
				{
					static const char* slow = getenv("MPCPS5_HOST_SLOW_DECODE");
					double ms = 0, until = 0, from = -1e18; // (test11: an optional third value: only from that time on)
					if (slow && sscanf(slow, "%lf,%lf,%lf", &ms, &until, &from) >= 2)
					{
						const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
						const double t = ts != AV_NOPTS_VALUE ? ts * av_q2d(ic->streams[vstream]->time_base) - StreamStart() : -1e18;
						if (ts != AV_NOPTS_VALUE && t < until && t >= from)
							av_usleep(unsigned(ms * 1000));
					}
				}
#endif
				decode_ms = decode_ms * 0.95 + (NowS() - t0) * 1000 * 0.05;
				const bool interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
				if (interlaced && vg_fmt != -2 && SetupDeinterlace(frame))
				{
					if (av_buffersrc_add_frame(vsrc, frame) >= 0)
					{
						AVFrame* out = av_frame_alloc();
						while (av_buffersink_get_frame(vsink, out) >= 0)
						{
							QueueFrame(out, dec_serial);
							out = av_frame_alloc();
						}
						av_frame_free(&out);
					}
					av_frame_unref(frame);
					continue;
				}
				AVFrame* copy = av_frame_alloc();
				av_frame_move_ref(copy, frame);
				QueueFrame(copy, dec_serial);
			}
		}
		av_frame_free(&frame);
	}

	// 1.6: FFmpeg on the CPU that can't keep up (8K, AV1 at 4K...). (test4 review) Measured where it happens: each
	// decoded picture whose time has already passed when it comes out of the decoder (as well as the presenter's
	// drops), counted over windows of 30 pictures while playing; the first window after a seek, a resume or a new
	// file isn't counted (its catch-up). A third of a window late: faster decoding, a step at a time (no loop filter,
	// then no non-reference pictures, as VLC's "fast decoding" does); six calm windows in a row (at most one late):
	// a step back. Pictures are dropped, never the sound.
	void KeepUp(double pts, double dur, int ser)
	{
		if (!vdec)
			return;
		if (paused.load() || ser != fast_serial)
		{
			fast_serial = paused.load() ? -1 : ser;
			fast_seen = fast_late = 0;
			fast_dropped = dropped.load();
			fast_skip_window = true;
			return;
		}
		fast_seen++;
		if (v_behind.load() > 0.010)
			fast_late++;
		if (fast_seen < 30)
			return;
		const uint64_t d = dropped.load();
		// (test5 review) a picture late at the decoder is mostly the one the presenter then drops: the larger of the
		// two counts, not their sum
		const uint64_t late = std::max<uint64_t>(fast_late, d - fast_dropped);
		fast_seen = fast_late = 0;
		fast_dropped = d;
#ifndef __PROSPERO__
		if (getenv("MPCPS5_HOST_KEEPUP_LOG"))
			OrbisLog("[player] (test) window: %llu late or dropped in 30, level %d, calm %d%s", (unsigned long long)late, fast_level,
				fast_calm, fast_skip_window ? " (not counted)" : "");
#endif
		if (fast_skip_window)
		{
			fast_skip_window = false;
			return;
		}
		// (test12) Options > Playback > Decoding: Full never decodes faster; Auto doesn't on a network source (late
		// pictures there are the network's, as VLC for PS5 judges it); Fast starts at level 1 and never steps below it
		const int mode = decode_mode.load();
		const bool may_step_up = mode != 2 && !net_source;
		if (late >= 10)
		{
			fast_calm = 0;
			if (fast_level < 2 && may_step_up)
			{
				fast_level++;
				if (fast_level == 1)
				{
					vdec->skip_loop_filter = AVDISCARD_ALL;
					vdec->flags2 |= AV_CODEC_FLAG2_FAST; // (read per picture by the H.264 / HEVC decoders)
				}
				else
					vdec->skip_frame = AVDISCARD_NONREF;
				OrbisLog("[player] FFmpeg can't keep up (%llu late or dropped in 30 pictures): faster decoding, level %d (%s)",
					(unsigned long long)late, fast_level,
					fast_level == 1 ? "no loop filter" : "no loop filter, no non-reference pictures");
				if (NowS() - fast_notice_at > 30) // (at most one notice in 30 s)
				{
					fast_notice_at = NowS();
					SetNotice(T("Fast decoding: softer picture so this video keeps up."));
				}
			}
			else if (!beyond_said && late >= 20 && (fast_level == 2 || !may_step_up))
			{
				beyond_said = true;
				OrbisLog("[player] beyond real time on the CPU even with faster decoding (%llu late or dropped in 30 pictures)",
					(unsigned long long)late);
				SetNotice(T("This video is beyond what the PS5 can decode in real time: pictures are dropped, the sound plays on."));
			}
		}
		else if (late <= 1 && fast_level > (mode == 1 ? 1 : 0))
		{
			// six calm windows (about 7 s at 25 fps) before a step back: no back and forth between the levels
			if (++fast_calm >= 6)
			{
				fast_calm = 0;
				if (fast_level == 2)
					vdec->skip_frame = AVDISCARD_DEFAULT;
				else
				{
					vdec->skip_loop_filter = AVDISCARD_DEFAULT;
					vdec->flags2 &= ~AV_CODEC_FLAG2_FAST;
				}
				fast_level--;
				if (fast_level == 0)
					beyond_said = false;
				OrbisLog("[player] FFmpeg keeps up again: faster decoding, level %d%s", fast_level,
					fast_level == 0 ? " (full quality)" : "");
				if (fast_level == 0)
					SetNotice(T("Decoding at full quality again."));
			}
		}
		else
			fast_calm = 0;
	}

	// ---------------------------------------------------------------------------------------------
	bool SetupAudioGraph(const AVFrame* f, double tempo)
	{
		const bool surr = surround.load();
		if (agraph && ag_rate == f->sample_rate && ag_fmt == f->format && ag_tempo == tempo && ag_surround == surr &&
			av_channel_layout_compare(&ag_layout, &f->ch_layout) == 0)
			return true;
		// Surround on and a multichannel track: 5.1 or 7.1 to the console's 8-channel output; else stereo
		int out_ch = surr && f->ch_layout.nb_channels > 2 ? (f->ch_layout.nb_channels > 6 ? 8 : 6) : 2;
		if (out_ch > 2 && !ps5audio::SetChannels(8))
			out_ch = 2;
		if (out_ch == 2)
			ps5audio::SetChannels(2);
		avfilter_graph_free(&agraph);
		agraph = avfilter_graph_alloc();
		char layout[128] = "stereo";
		av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout));
		char args[256];
		snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d", f->sample_rate,
			av_get_sample_fmt_name(AVSampleFormat(f->format)), layout, f->sample_rate);
		AVFilterContext* last = nullptr;
		bool ok = avfilter_graph_create_filter(&asrc, avfilter_get_by_name("abuffer"), "in", args, nullptr, agraph) >= 0 &&
				  avfilter_graph_create_filter(&asink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr,
					  agraph) >= 0;
		last = asrc;
		// atempo takes 0.5..100: chain two for slower
		double t = tempo;
		int n = 0;
		while (ok && std::fabs(t - 1.0) > 1e-6 && n < 4)
		{
			const double step = t < 0.5 ? 0.5 : t;
			char a[64];
			snprintf(a, sizeof(a), "tempo=%f", step);
			AVFilterContext* at = nullptr;
			char name[32];
			snprintf(name, sizeof(name), "atempo%d", n);
			ok = avfilter_graph_create_filter(&at, avfilter_get_by_name("atempo"), name, a, nullptr, agraph) >= 0 &&
				 avfilter_link(last, 0, at, 0) >= 0;
			last = at;
			t /= step;
			n++;
		}
		AVFilterContext* rs = nullptr;
		AVFilterContext* fmt = nullptr;
		ok = ok &&
			 avfilter_graph_create_filter(&rs, avfilter_get_by_name("aresample"), "rs", "48000", nullptr, agraph) >= 0 &&
			 avfilter_graph_create_filter(&fmt, avfilter_get_by_name("aformat"), "fmt",
					 out_ch == 8 ? "sample_fmts=s16:sample_rates=48000:channel_layouts=7.1"
					 : out_ch == 6 ? "sample_fmts=s16:sample_rates=48000:channel_layouts=5.1(side)"
								   : "sample_fmts=s16:sample_rates=48000:channel_layouts=stereo",
					 nullptr, agraph) >= 0 &&
			 avfilter_link(last, 0, rs, 0) >= 0 && avfilter_link(rs, 0, fmt, 0) >= 0 &&
			 avfilter_link(fmt, 0, asink, 0) >= 0 && avfilter_graph_config(agraph, nullptr) >= 0;
		if (!ok)
		{
			avfilter_graph_free(&agraph);
			return false;
		}
		ag_rate = f->sample_rate;
		ag_fmt = f->format;
		av_channel_layout_uninit(&ag_layout);
		av_channel_layout_copy(&ag_layout, &f->ch_layout);
		ag_tempo = tempo;
		ag_surround = surr;
		norm_factor = -1; // another format: as MPC-HC's new output media type
		if (out_ch != ag_out_ch)
			OrbisLog("[player] sound: %d channels -> %s", f->ch_layout.nb_channels,
				out_ch == 8 ? "7.1 (8-channel output)" : out_ch == 6 ? "5.1 (8-channel output)" : "stereo");
		ag_out_ch = out_ch;
		return true;
	}

	double Gain() const
	{
		if (mute)
			return 0;
		const double v = volume / 100.0;
		return v * v * amplify; // CPlayerToolBar::GetVolume: 40 dB over the slider, log10 -> (v/100)^2
	}

	// MPC-HC's Normalize (CAudioSwitcherFilter::Transform, MPC-HC 1.7.13), on each block the audio graph gives
	// (s16, all channels interleaved, one gain for all of them): the block's peak first; when the gain would take that
	// peak past full scale it drops at once to what puts the peak at full scale (so nothing clips, with no look-ahead
	// beyond the block itself); else, with Regain volume, it rises by 6 % per second of sound while the peak stays under
	// 75 % of full scale; never above Max amplification. It starts at the max (a file, a seek, a track or format
	// change: MPC-HC's flush / new segment / new media type). Boost and the volume come after it, at the output.
	// Returns where the result is: `s` itself when Normalize is off.
	const int16_t* Normalize(const int16_t* s, int n, int ch, double dur, double end_pts)
	{
		const bool on = norm_on.load(std::memory_order_relaxed);
		if (on && !norm_was_on)
		{
			norm_factor = -1; // turned on: from the max (MPC-HC's SetNormalizeBoost)
			OrbisLog("[player] Normalize on from %.3f s of the stream", end_pts - dur);
		}
		else if (!on && norm_was_on)
			OrbisLog("[player] Normalize off from %.3f s of the stream", end_pts - dur);
		norm_was_on = on;
		if (!on || n <= 0)
			return s;
		const double max_factor = norm_max.load(std::memory_order_relaxed);
		if (norm_factor < 0)
			norm_factor = max_factor;
		const size_t count = size_t(n) * ch;
		int peak = 0;
		for (size_t i = 0; i < count; i++)
		{
			const int a = s[i] < 0 ? -int(s[i]) : int(s[i]);
			if (a > peak)
				peak = a;
		}
		const double sample_max = peak / 32767.0;
		if (norm_factor * sample_max > 1.0) // MPC-HC: m_normalizeFactor > 1.0 / sample_max
			norm_factor = 1.0 / sample_max;
		else if (norm_regain.load(std::memory_order_relaxed) && sample_max * norm_factor < 0.75)
			norm_factor += 0.06 * dur; // NORMALIZATION_REGAIN_STEP (+6 %/s), NORMALIZATION_REGAIN_THRESHOLD (0.75)
		if (norm_factor > max_factor)
			norm_factor = max_factor;
		// 16.16, rounded down: the peak times it stays within 32767; held to the 16-bit range all the same
		const int64_t g = int64_t(norm_factor * 65536.0);
		if (g == 65536)
			return s;
		if (norm_buf.size() < count)
			norm_buf.resize(count); // grows to the largest block once, then reused
		int16_t* d = norm_buf.data();
		for (size_t i = 0; i < count; i++)
		{
			const int64_t v = (int64_t(s[i]) * g) >> 16;
			d[i] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		return d;
	}

	// pushes s16 (ag_out_ch channels), waiting for room; false when the serial changed or the player stops
	// dur: the block's length in the stream's time (Normalize's regain step)
	bool PushAudio(const int16_t* s, int n, int my_serial, double end_pts, double dur)
	{
		// (test9 review) a start moved past this block (the first picture after the target): not played
		if (end_pts <= skip_until.load())
			return true;
		int ch = ag_out_ch;
		s = Normalize(s, n, ch, dur, end_pts);
		if (ch == 8)
		{
			// FFmpeg's 7.1 (FL FR FC LFE BL BR SL SR) -> the console's (L R C LFE Ls Rs Lb Rb)
			remap.resize(size_t(n) * 8);
			for (int i = 0; i < n; i++)
			{
				const int16_t* a = s + size_t(i) * 8;
				int16_t* b = &remap[size_t(i) * 8];
				b[0] = a[0], b[1] = a[1], b[2] = a[2], b[3] = a[3], b[4] = a[6], b[5] = a[7], b[6] = a[4], b[7] = a[5];
			}
			s = remap.data();
		}
		while (n > 0)
		{
			if (quit || a_stop || serial != my_serial)
				return false;
			const int w = ps5audio::Push(s, n, ch);
			s += size_t(w) * ch;
			n -= w;
			if (n > 0)
				av_usleep(5000);
		}
		const double queued = ps5audio::Queued() / double(ps5audio::kRate) * rate;
		if (serial == my_serial && !SeekPending())
			clock.Set(end_pts - queued - audio_delay);
		if (serial == my_serial)
			a_pushed_serial = my_serial;
		return true;
	}

	void AudioThread()
	{
		AVFrame* frame = av_frame_alloc();
		AVFrame* out = av_frame_alloc();
		int dec_serial = -1;
		double out_pos = 0; // stream time at the end of what has been pushed
		bool have_pos = false;
		bool guessed_pos = false; // out_pos set from where the serial starts (an untimed first frame)
		norm_factor = -1; // Normalize starts again at the max (a file, another track)
		while (!quit && !a_stop)
		{
			// (test8 / test10 reviews) an exact seek's sound waits for its first picture, not decoded yet: where it
			// starts (skip_until) is known when the hold lets go (paused or not; a new seek ends the wait)
			{
				const int ser = serial.load();
				if (hold_serial.load() == ser)
				{
					HoldCheck(ser);
					av_usleep(2000);
					continue;
				}
			}
			QPacket qp;
			const int r = aq.Get(qp, 100);
			if (r < 0)
				break;
			if (r == 0)
				continue;
			if (qp.serial != serial)
			{
				av_packet_free(&qp.pkt);
				continue;
			}
			if (qp.serial != dec_serial)
			{
				avcodec_flush_buffers(adec);
				avfilter_graph_free(&agraph);
				dec_serial = qp.serial;
				have_pos = false;
				guessed_pos = false;
				norm_factor = -1; // a seek: as MPC-HC's flush / new segment
			}
			const int sr = avcodec_send_packet(adec, qp.pkt);
			av_packet_free(&qp.pkt);
			if (sr < 0 && sr != AVERROR(EAGAIN) && sr != AVERROR_EOF)
				continue;
			for (;;)
			{
				const int rr = avcodec_receive_frame(adec, frame);
				if (rr == AVERROR_EOF)
				{
					adone_serial = dec_serial;
					avcodec_flush_buffers(adec);
					break;
				}
				if (rr < 0)
					break;
				AVStream* st = ic->streams[astream];
				if (frame->pts == AV_NOPTS_VALUE && !have_pos)
				{
					// A first frame without a timestamp (a DVD title's first AC-3 packet after
					// a seek has none; some files have none at all): it starts where the serial
					// does (the seek's target, 0 at the start), not at out_pos, which after a
					// seek still holds the position before it. A timed frame later puts it right.
					out_pos = serial_start.load();
					have_pos = true;
					guessed_pos = true;
				}
				double pts = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(st->time_base) - StreamStart() : out_pos;
				const double fdur = frame->nb_samples / double(frame->sample_rate);
				if (pts + fdur < skip_until.load())
				{
					av_frame_unref(frame);
					continue;
				}
				if (!have_pos || (guessed_pos && frame->pts != AV_NOPTS_VALUE))
				{
					out_pos = pts;
					have_pos = true;
					guessed_pos = false;
				}
				const double tempo = rate;
				if (!SetupAudioGraph(frame, tempo))
				{
					av_frame_unref(frame);
					continue;
				}
				if (av_buffersrc_add_frame(asrc, frame) < 0)
				{
					av_frame_unref(frame);
					continue;
				}
				while (av_buffersink_get_frame(asink, out) >= 0)
				{
					const double dur = out->nb_samples / double(ps5audio::kRate) * tempo;
					out_pos += dur;
					const bool ok =
						PushAudio(reinterpret_cast<const int16_t*>(out->data[0]), out->nb_samples, dec_serial, out_pos, dur);
					av_frame_unref(out);
					if (!ok)
						break;
				}
			}
		}
		av_frame_free(&frame);
		av_frame_free(&out);
	}

	// ---------------------------------------------------------------------------------------------
	void Shutdown()
	{
		quit = true;
		vq.Abort();
		aq.Abort();
		{
			// (review L-b) the queued pictures let go before the decoding thread is joined: pictures out in place hold
			// the console decoder's buffers, which it may be waiting for
			std::lock_guard<std::mutex> l(fmu);
			for (auto& f : frames)
				av_frame_free(&f.f);
			frames.clear();
		}
		fcv.notify_all();
		if (opener.joinable())
			opener.join();
		if (demuxer.joinable())
			demuxer.join();
		if (vthread.joinable())
			vthread.join();
		if (athread.joinable())
			athread.join();
		vq.Flush();
		aq.Flush();
		for (auto& f : frames)
			av_frame_free(&f.f);
		frames.clear();
		av_frame_free(&hold_last.f);
		av_frame_free(&cur);
		hwdec::Close(hw);
		hw = nullptr;
		hw_active = false;
		avcodec_free_context(&vdec);
		for (auto& c : vdec_retired)
			avcodec_free_context(&c);
		{
			// (test13) once per file: the teardown runs again from the destructor with nothing new to say
			static uint64_t said_allocs = 0, said_reuses = 0;
			const bigmem::Stats bs = bigmem::GetStats();
			if ((bs.allocs || bs.fallbacks) && (bs.allocs != said_allocs || bs.reuses != said_reuses))
				OrbisLog("[bigmem] pictures: %llu blocks mapped, %llu taken from the free list, %llu left to FFmpeg's own "
						 "allocation; in use now %llu direct (%llu MiB), %llu flexible",
					(unsigned long long)bs.allocs, (unsigned long long)bs.reuses, (unsigned long long)bs.fallbacks,
					(unsigned long long)bs.direct_blocks, (unsigned long long)(bs.direct_bytes >> 20),
					(unsigned long long)bs.flexible_blocks);
			said_allocs = bs.allocs;
			said_reuses = bs.reuses;
		}
		bigmem::Trim();
		vdec_retired.clear();
		cpu_reopen_fallback = cpu_lean_fallback = false;
		avcodec_free_context(&adec);
		for (auto& s : sdecs)
			avcodec_free_context(&s.second);
		sdecs.clear();
		pgs_wait.clear();
		pgs_clearing.clear();
		avfilter_graph_free(&agraph);
		avfilter_graph_free(&vgraph);
		av_channel_layout_uninit(&ag_layout);
		sws_freeContext(sws);
		sws = nullptr;
		if (ic)
			avformat_close_input(&ic);
		if (custom_io)
		{
			if (custom_io_disc)
				disc::CloseAvio(custom_io, custom_io_owner);
			else
				smb::CloseAvio(custom_io, custom_io_owner);
			custom_io = nullptr;
			custom_io_owner = nullptr;
		}
		disc::CloseTitle(title_open);
		ps5audio::Clear();
		// (test10 review) silence between files, not underruns (the next file's open sets it as it starts)
		ps5audio::SetPaused(true);
	}

	static bool AnyExists(const std::string& p)
	{
		return p.compare(0, 6, "smb://") == 0 ? smb::Exists(p) : p.compare(0, 7, "disc://") == 0 ? disc::Exists(p) : OrbisIsFile(p);
	}

	// 1.5.2: a .sup (Blu-ray PGS) or a VobSub pair (.idx + .sub): read by FFmpeg as it is, never through the text
	// conversion; each line's times now, its pictures when it shows (DecodeShown)
	bool AddBitmapSubtitleFile(const std::string& path, std::string* err)
	{
		const bool vobsub = Ext(path) == "idx";
		const bool on_smb = path.compare(0, 6, "smb://") == 0, on_disc = path.compare(0, 7, "disc://") == 0;
		if (vobsub && (on_smb || on_disc))
		{
			// FFmpeg's VobSub reader opens the .sub beside the .idx by itself, from a local path only
			OrbisLog("[player] %s: VobSub from a share or a disc isn't read (copy it to the console)", path.c_str());
			if (err)
				*err = "Not a subtitle file this player can read";
			return false;
		}
		AVFormatContext* sc = avformat_alloc_context();
		AVIOContext* io = nullptr;
		void* owner = nullptr;
		std::string ioerr;
		if (on_smb)
			io = smb::OpenAvio(path, &owner, &ioerr);
		else if (on_disc)
			io = disc::OpenAvio(path, &owner, &ioerr);
		if ((on_smb || on_disc) && !io)
		{
			avformat_free_context(sc);
			if (err)
				*err = ioerr.empty() ? "Can't read the file" : ioerr;
			return false;
		}
		if (io)
		{
			sc->pb = io;
			sc->flags |= AVFMT_FLAG_CUSTOM_IO;
		}
		bool ok = false;
		size_t kept = 0, lines = 0;
		double last = 0;
		bool cut = false, stopped = false;
		if (avformat_open_input(&sc, path.c_str(), av_find_input_format(vobsub ? "vobsub" : "sup"), nullptr) >= 0)
		{
			const int si = avformat_find_stream_info(sc, nullptr) >= 0
							   ? av_find_best_stream(sc, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0)
							   : AVERROR_STREAM_NOT_FOUND;
			const AVCodec* codec = si >= 0 ? avcodec_find_decoder(sc->streams[si]->codecpar->codec_id) : nullptr;
			auto x = std::make_unique<ExtBitmap>();
			x->c = codec ? avcodec_alloc_context3(codec) : nullptr;
			if (x->c && avcodec_parameters_to_context(x->c, sc->streams[si]->codecpar) >= 0)
			{
				AVStream* st = sc->streams[si];
				x->c->pkt_timebase = st->time_base;
				x->pgs = codec->id == AV_CODEC_ID_HDMV_PGS_SUBTITLE;
				x->codec = codec;
				x->tb = st->time_base;
				if (x->pgs && (x->par = avcodec_parameters_alloc()) && avcodec_parameters_copy(x->par, st->codecpar) < 0)
					avcodec_parameters_free(&x->par); // no decoder of a line's own: the file's one, as before
				if (avcodec_open2(x->c, codec, nullptr) >= 0)
				{
					x->ref_w = x->c->width;
					x->ref_h = x->c->height;
					int track;
					{
						std::lock_guard<std::mutex> l(smu);
						track = 1000 + int(external_names.size());
						const size_t slash = path.find_last_of('/');
						external_names.push_back({track, path.substr(slash == std::string::npos ? 0 : slash + 1)});
					}
					AVPacket* pkt = av_packet_alloc();
					int first = 0, epoch = -1;
					while (av_read_frame(sc, pkt) >= 0)
					{
						if (pkt->stream_index != si)
						{
							av_packet_unref(pkt);
							continue;
						}
						if (quit)
						{
							// the file closed meanwhile (1.5.2, review: Close waited for the whole file to be read)
							stopped = true;
							av_packet_unref(pkt);
							break;
						}
						if (kept + size_t(pkt->size) + kExtPacketOverhead > kMaxExtBitmapBytes || lines >= kMaxExtBitmapLines)
						{
							cut = true;
							av_packet_unref(pkt);
							break;
						}
						kept += size_t(pkt->size) + kExtPacketOverhead;
						x->pkts.push_back(av_packet_clone(pkt));
						const int k = int(x->pkts.size()) - 1;
						if (x->pgs && PgsScan(pkt).epoch)
							epoch = k;
						AVSubtitle sub = {};
						int got = 0;
						if (avcodec_decode_subtitle2(x->c, &sub, &got, pkt) >= 0 && got)
						{
							double pts = 0;
							if (sub.pts != AV_NOPTS_VALUE)
								pts = sub.pts / double(AV_TIME_BASE);
							else if (pkt->pts != AV_NOPTS_VALUE)
								pts = pkt->pts * av_q2d(st->time_base);
							for (unsigned r = 0; r < sub.num_rects; r++)
								if (sub.rects[r]->type == SUBTITLE_BITMAP && sub.rects[r]->w > 0 && sub.rects[r]->h > 0)
								{
									lines++; // a line with a picture (not the empty one that ends the last)
									break;
								}
							StoreExternalBitmap(track, sub, pts, x->ref_w, x->ref_h, first, k, epoch);
							avsubtitle_free(&sub);
							first = k + 1;
							last = pts;
						}
						av_packet_unref(pkt);
					}
					av_packet_free(&pkt);
					{
						std::lock_guard<std::mutex> l(smu);
						ext_bitmaps[track] = std::move(x);
					}
					ok = true;
				}
			}
			avformat_close_input(&sc);
		}
		else
			avformat_free_context(sc);
		if (io)
		{
			if (on_disc)
				disc::CloseAvio(io, owner);
			else
				smb::CloseAvio(io, owner);
		}
		if (!ok)
		{
			if (err)
				*err = "Not a subtitle file this player can read";
			return false;
		}
		if (stopped)
		{
			OrbisLog("[player] subtitles: %s: reading stopped, the file closed", path.c_str());
			return false;
		}
		OrbisLog("[player] subtitles loaded: %s (bitmaps: %zu line(s), %zu KB of packets kept%s)", path.c_str(), lines,
			kept >> 10, cut ? Fmt(", cut at %.0f s: over 128 MB or %zu lines", last, kMaxExtBitmapLines).c_str() : "");
		return true;
	}

	bool AddSubtitleFile(const std::string& path, std::string* err)
	{
		// 1.5.2: bitmap subtitle files read as they are (up to 1.5.1 they went through the text conversion below and
		// never loaded); a .sub with an .idx beside it is VobSub's picture file
		const std::string kind = Ext(path);
		if (kind == "sup" || kind == "idx")
			return AddBitmapSubtitleFile(path, err);
		if (kind == "sub")
		{
			const std::string idx = path.substr(0, path.size() - 3) + (path.compare(path.size() - 3, 3, "SUB") == 0 ? "IDX" : "idx");
			if (AnyExists(idx))
				return AddBitmapSubtitleFile(idx, err);
		}
		std::vector<uint8_t> data;
		if (!ReadWholeFile(path, data, 16u << 20))
		{
			if (err)
				*err = "Can't read the file";
			return false;
		}
		// the BOM goes; ANSI becomes UTF-8
		if (data.size() >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
			data.erase(data.begin(), data.begin() + 3);
		else if (data.size() >= 2 && ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff)))
		{
			// UTF-16: to UTF-8
			const bool le = data[0] == 0xff;
			std::vector<uint8_t> o;
			for (size_t i = 2; i + 1 < data.size(); i += 2)
			{
				uint32_t cp = le ? (data[i] | (data[i + 1] << 8)) : ((data[i] << 8) | data[i + 1]);
				if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < data.size())
				{
					const uint32_t lo = le ? (data[i + 2] | (data[i + 3] << 8)) : ((data[i + 2] << 8) | data[i + 3]);
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					i += 2;
				}
				if (cp < 0x80)
					o.push_back(uint8_t(cp));
				else if (cp < 0x800)
				{
					o.push_back(uint8_t(0xc0 | (cp >> 6)));
					o.push_back(uint8_t(0x80 | (cp & 0x3f)));
				}
				else if (cp < 0x10000)
				{
					o.push_back(uint8_t(0xe0 | (cp >> 12)));
					o.push_back(uint8_t(0x80 | ((cp >> 6) & 0x3f)));
					o.push_back(uint8_t(0x80 | (cp & 0x3f)));
				}
				else
				{
					o.push_back(uint8_t(0xf0 | (cp >> 18)));
					o.push_back(uint8_t(0x80 | ((cp >> 12) & 0x3f)));
					o.push_back(uint8_t(0x80 | ((cp >> 6) & 0x3f)));
					o.push_back(uint8_t(0x80 | (cp & 0x3f)));
				}
			}
			data.swap(o);
		}
		else if (!ValidUtf8(data))
			data = Cp1252ToUtf8(data);

		// .ass / .ssa: the whole script to libass; if it finds nothing in it, FFmpeg reads it as before
		const std::string ext = Ext(path);
		if (ext == "ass" || ext == "ssa")
		{
			ass::Renderer* r = Ass(true);
			int id;
			{
				std::lock_guard<std::mutex> l(smu);
				id = 1000 + int(external_names.size());
			}
			if (r && r->ReadScript(id, data))
			{
				{
					std::lock_guard<std::mutex> l(smu);
					const size_t slash = path.find_last_of('/');
					external_names.push_back({id, path.substr(slash == std::string::npos ? 0 : slash + 1)});
				}
				OrbisLog("[player] subtitles loaded: %s (ASS, libass)", path.c_str());
				return true;
			}
		}

		MemReader mem;
		mem.data = std::move(data);
		const int bufsize = 32768;
		uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufsize));
		AVIOContext* pb = avio_alloc_context(buf, bufsize, 0, &mem, MemReader::Read, nullptr, MemReader::Seek);
		AVFormatContext* sc = avformat_alloc_context();
		sc->pb = pb;
		sc->flags |= AVFMT_FLAG_CUSTOM_IO;
		const AVInputFormat* fmt = nullptr;
		if (ext == "srt")
			fmt = av_find_input_format("srt");
		else if (ext == "ass" || ext == "ssa")
			fmt = av_find_input_format("ass");
		else if (ext == "vtt")
			fmt = av_find_input_format("webvtt");
		bool ok = false;
		int track = 0;
		if (avformat_open_input(&sc, path.c_str(), fmt, nullptr) >= 0)
		{
			if (avformat_find_stream_info(sc, nullptr) >= 0 && sc->nb_streams > 0 &&
				sc->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
			{
				const AVCodec* codec = avcodec_find_decoder(sc->streams[0]->codecpar->codec_id);
				AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
				if (c && avcodec_parameters_to_context(c, sc->streams[0]->codecpar) >= 0)
				{
					c->pkt_timebase = sc->streams[0]->time_base;
					AVDictionary* o = nullptr;
					av_dict_set(&o, "sub_text_format", "ass", 0);
					if (avcodec_open2(c, codec, &o) >= 0)
					{
						{
							std::lock_guard<std::mutex> l(smu);
							track = 1000 + int(external_names.size());
							const size_t slash = path.find_last_of('/');
							external_names.push_back({track, path.substr(slash == std::string::npos ? 0 : slash + 1)});
						}
						AVPacket* pkt = av_packet_alloc();
						while (av_read_frame(sc, pkt) >= 0)
						{
							AVSubtitle sub = {};
							int got = 0;
							if (avcodec_decode_subtitle2(c, &sub, &got, pkt) >= 0 && got)
							{
								double pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(sc->streams[0]->time_base) : 0;
								if (pkt->duration > 0 && sub.end_display_time == 0)
									sub.end_display_time =
										uint32_t(pkt->duration * av_q2d(sc->streams[0]->time_base) * 1000);
								sub.start_display_time = 0;
								StoreSubtitle(track, sub, pts, 0, 0);
								avsubtitle_free(&sub);
							}
							av_packet_unref(pkt);
						}
						av_packet_free(&pkt);
						ok = true;
					}
					av_dict_free(&o);
				}
				avcodec_free_context(&c);
			}
			avformat_close_input(&sc);
		}
		else
			avformat_free_context(sc);
		av_freep(&pb->buffer);
		avio_context_free(&pb);
		if (!ok)
		{
			if (err)
				*err = "Not a subtitle file this player can read";
			return false;
		}
		OrbisLog("[player] subtitles loaded: %s", path.c_str());
		return true;
	}
};

// ===========================================================================================================
namespace
{
// FFmpeg's own messages (its default: stderr, from its level up), with any password in an address hidden first: its
// protocols print the addresses they open.
void AvLog(void* avcl, int level, const char* fmt, va_list vl)
{
	if (level > av_log_get_level())
		return;
	char line[1024];
	int prefix = 1;
	av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
	OrbisHidePasswords(line);
	fputs(line, stderr);
}
} // namespace

Player::Player() : m(new Impl)
{
	static const bool once = [] {
		av_log_set_callback(AvLog);
		return true;
	}();
	(void)once;
}
Player::~Player() = default;

// A new Impl for the next file, with the audio settings of the one before: the audio thread fills the ~340 ms ring
// before the app sees the file playing (and calls the setters again), so a fresh default here would play that start
// without Normalize (then jump to its level) or in stereo (then drop it when the 8-channel graph is made).
void Player::Renew()
{
	std::unique_ptr<Impl> n(new Impl);
	n->norm_on = m->norm_on.load();
	n->norm_max = m->norm_max.load();
	n->norm_regain = m->norm_regain.load();
	n->surround = m->surround.load();
	n->volume = m->volume.load();
	n->mute = m->mute.load();
	n->amplify = m->amplify.load();
	n->display_delay = m->display_delay.load();
	n->decode_mode = m->decode_mode.load();
	m = std::move(n);
}

void Player::Open(const std::string& url, double start_s, bool start_paused)
{
	Close();
	Renew();
	m->url = url;
	m->start_at = start_s;
	m->start_paused = start_paused;
	m->state = PlayState::Opening;
	m->clock.SetPaused(true);
	m->hold_clock.SetPaused(true);
	Impl* impl = m.get();
	try
	{
		m->opener = ps5::StartThread("opener", [impl] { impl->OpenThread(); });
	}
	catch (const std::system_error& e)
	{
		OrbisLog("[player] a new thread refused: %s", e.what());
		m->SetFailed("The console refused a new thread");
	}
}

void Player::Close()
{
	if (m->state != PlayState::Closed)
		OrbisLog("[player] closed %s", m->url.c_str());
	Renew();
}

PlayState Player::State() const
{
	return m->state;
}

std::string Player::Error() const
{
	std::lock_guard<std::mutex> l(m->mu);
	return m->error;
}

std::string Player::Url() const
{
	return m->url;
}

bool Player::Ended() const
{
	return m->ended;
}

void Player::Play()
{
	const PlayState s = m->state;
	if (s != PlayState::Paused && s != PlayState::Stopped && s != PlayState::Playing)
		return;
	if (m->ended)
		Seek(0);
	m->paused = false;
	m->clock.SetPaused(false);
	m->hold_clock.SetPaused(false);
	ps5audio::SetPaused(false);
	m->state = PlayState::Playing;
}

void Player::Pause()
{
	const PlayState s = m->state;
	if (s != PlayState::Playing && s != PlayState::Stopped)
		return;
	m->paused = true;
	m->clock.SetPaused(true);
	m->hold_clock.SetPaused(true);
	ps5audio::SetPaused(true);
	m->state = PlayState::Paused;
}

void Player::TogglePause()
{
	if (m->state == PlayState::Playing)
		Pause();
	else
		Play();
}

void Player::Stop()
{
	const PlayState s = m->state;
	if (s != PlayState::Playing && s != PlayState::Paused)
		return;
	m->paused = true;
	m->clock.SetPaused(true);
	m->hold_clock.SetPaused(true);
	ps5audio::SetPaused(true);
	Seek(0);
	m->state = PlayState::Stopped;
}

void Player::Seek(double seconds, bool exact)
{
	const PlayState s = m->state;
	if (s != PlayState::Playing && s != PlayState::Paused && s != PlayState::Stopped)
		return;
	if (!m->Seekable_())
		return;
	std::lock_guard<std::mutex> l(m->seek_mu);
	m->seek_req = true;
	m->seek_gen++;
	m->seek_to = std::max(0.0, seconds);
	m->seek_exact = exact;
	m->clock.Set(m->seek_to);
	m->ended = false;
}

void Player::FrameStep()
{
	if (m->state == PlayState::Playing)
		Pause();
	m->step_req = true;
}

double Player::Position() const
{
	const PlayState s = m->state;
	if (s == PlayState::Closed || s == PlayState::Opening || s == PlayState::Failed)
		return 0;
	double p = m->clock.Get();
	if (m->duration > 0)
		p = std::min(p, m->duration);
	return std::max(0.0, p);
}

double Player::Duration() const
{
	return m->duration;
}

bool Player::Seekable() const
{
	return m->Seekable_();
}

void Player::SetVolume(int percent)
{
	m->volume = std::max(0, std::min(100, percent));
	ps5audio::SetGain(m->Gain());
}

void Player::SetMute(bool mute)
{
	m->mute = mute;
	ps5audio::SetGain(m->Gain());
}

void Player::SetSurround(bool on)
{
	m->surround = on; // the audio graph is made again at the next frame
}

void Player::SetToneMapping(int mode, int white_nits)
{
	m->tm_mode = std::max(0, std::min(3, mode));
	m->tm_white = std::max(100, std::min(400, white_nits));
	m->rendered_id = 0; // drawn again
}

void Player::SetDecodeMode(int mode)
{
	m->decode_mode = std::max(0, std::min(2, mode));
}

int Player::HdrKind() const
{
	return m->cur ? m->hdr_shown.load() : 0;
}

void Player::SetAmplify(double gain)
{
	m->amplify = gain;
	ps5audio::SetGain(m->Gain());
}

void Player::SetNormalize(bool on, int max_percent, bool regain)
{
	m->norm_max = std::max(100, std::min(1000, max_percent)) / 100.0;
	m->norm_regain = regain;
	m->norm_on = on; // the audio thread takes it at its next block
}

void Player::SetRate(double rate)
{
	rate = std::max(0.125, std::min(8.0, rate));
	m->rate = rate;
	m->clock.SetRate(rate);
	// what is queued was made for the old rate: start again from here
	if (m->state == PlayState::Playing || m->state == PlayState::Paused)
		Seek(Position(), true);
}

double Player::Rate() const
{
	return m->rate;
}

void Player::SetAudioDelay(double s)
{
	m->audio_delay = s;
}

double Player::AudioDelay() const
{
	return m->audio_delay;
}

void Player::SetSubtitleDelay(double s)
{
	m->sub_delay = s;
}

double Player::SubtitleDelay() const
{
	return m->sub_delay;
}

std::vector<Track> Player::Tracks(TrackType type) const
{
	std::vector<Track> out;
	if (m->state == PlayState::Opening || !m->ic)
		return out;
	AVFormatContext* ic = m->ic;
	for (unsigned i = 0; i < ic->nb_streams; i++)
	{
		const AVStream* st = ic->streams[i];
		const AVCodecParameters* p = st->codecpar;
		if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
			continue;
		const bool want = (type == TrackType::Video && p->codec_type == AVMEDIA_TYPE_VIDEO) ||
						  (type == TrackType::Audio && p->codec_type == AVMEDIA_TYPE_AUDIO) ||
						  (type == TrackType::Subtitle && p->codec_type == AVMEDIA_TYPE_SUBTITLE && m->sdecs.count(int(i)));
		if (!want)
			continue;
		Track t;
		t.id = int(i);
		const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
		t.lang = lang ? lang->value : "";
		t.is_default = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
		std::string name;
		if (title && *title->value)
			name = title->value;
		if (lang && *lang->value && Lower(lang->value) != "und")
			name += (name.empty() ? "" : " - ") + LangName(lang->value) + " [" + lang->value + "]";
		const AVCodecDescriptor* d = avcodec_descriptor_get(p->codec_id);
		std::string codec = d ? d->name : "?";
		for (char& c : codec)
			c = char(toupper(static_cast<unsigned char>(c)));
		std::string info = codec;
		if (p->codec_type == AVMEDIA_TYPE_AUDIO)
			info += ", " + std::to_string(p->sample_rate) + " Hz, " + ChannelText(p->ch_layout.nb_channels);
		else if (p->codec_type == AVMEDIA_TYPE_VIDEO)
			info += ", " + std::to_string(p->width) + "x" + std::to_string(p->height);
		if (st->disposition & AV_DISPOSITION_FORCED)
			info += T(", forced");
		t.name = name.empty() ? info : name + " (" + info + ")";
		out.push_back(t);
	}
	if (type == TrackType::Subtitle)
	{
		std::lock_guard<std::mutex> l(m->smu);
		for (const auto& e : m->external_names)
		{
			Track t;
			t.id = e.first;
			t.name = e.second;
			t.external = true;
			out.push_back(t);
		}
	}
	return out;
}

int Player::CurrentTrack(TrackType type) const
{
	switch (type)
	{
		case TrackType::Video:
			return m->vstream;
		case TrackType::Audio:
			return m->astream;
		case TrackType::Subtitle:
			return m->sstream;
		default:
			return -1;
	}
}

void Player::SelectTrack(TrackType type, int id)
{
	if (type == TrackType::Subtitle)
	{
		m->sstream = id;
		return;
	}
	if (type != TrackType::Audio || !m->ic || id == m->astream || id < 0 || id >= int(m->ic->nb_streams))
		return;
	if (m->ic->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
		return;
	// stop the audio thread, open the new decoder, seek to here so the new track starts at once
	const double pos = Position();
	AVCodecContext* c = m->OpenDecoder(id, false);
	if (!c)
		return;
	m->a_stop = true;
	m->aq.Abort();
	if (m->athread.joinable())
		m->athread.join();
	avcodec_free_context(&m->adec);
	avfilter_graph_free(&m->agraph);
	m->aq.Flush();
	{
		std::lock_guard<std::mutex> l(m->aq.mu);
		m->aq.abort = false;
	}
	m->a_stop = false;
	// 1.5.2 (review): no track before (the first one's decoder didn't open, or a refused thread turned it off):
	// astream is -1, nothing to stop reading (streams[-1] wrote before the array)
	if (m->astream >= 0)
		m->ic->streams[m->astream]->discard = AVDISCARD_ALL;
	m->ic->streams[id]->discard = AVDISCARD_DEFAULT;
	m->adec = c;
	m->astream = id;
	Impl* impl = m.get();
	try
	{
		m->athread = ps5::StartThread("audio-track", [impl] { impl->AudioThread(); });
	}
	catch (const std::system_error& e)
	{
		// no thread for the new track: the film plays on without sound, as one with no audio track
		OrbisLog("[player] a new thread refused: %s: audio track %d off", e.what(), id);
		m->ic->streams[id]->discard = AVDISCARD_ALL;
		m->astream = -1;
		avcodec_free_context(&m->adec);
	}
	Seek(pos, true);
}

bool Player::AddSubtitleFile(const std::string& path, std::string* error)
{
	if (!m->AddSubtitleFile(path, error))
		return false;
	std::lock_guard<std::mutex> l(m->smu);
	m->sstream = m->external_names.back().first;
	return true;
}

std::vector<Chapter> Player::Chapters() const
{
	std::vector<Chapter> out;
	if (!m->ic || m->state == PlayState::Opening)
		return out;
	{
		std::lock_guard<std::mutex> l(m->mu);
		if (!m->disc_chapters.empty())
		{
			out = m->disc_chapters;
			for (size_t i = 0; i < out.size(); i++)
				if (out[i].name.empty())
					out[i].name = Fmt(T("Chapter %d"), int(i) + 1);
			return out;
		}
	}
	for (unsigned i = 0; i < m->ic->nb_chapters; i++)
	{
		const AVChapter* c = m->ic->chapters[i];
		Chapter ch;
		ch.time = c->start * av_q2d(c->time_base) - m->StreamStart();
		const AVDictionaryEntry* t = av_dict_get(c->metadata, "title", nullptr, 0);
		ch.name = t ? std::string(t->value) : Fmt(T("Chapter %d"), i + 1);
		out.push_back(ch);
	}
	return out;
}

std::vector<DiscTitle> Player::Titles() const
{
	std::lock_guard<std::mutex> l(m->mu);
	return m->disc_titles;
}

int Player::CurrentTitle() const
{
	std::lock_guard<std::mutex> l(m->mu);
	return m->disc_title;
}

bool Player::DiscGone() const
{
	return m->disc_gone;
}

uint64_t Player::DiscGeneration() const
{
	return m->disc_gen;
}

bool Player::HasVideo() const
{
	return m->vstream >= 0;
}

bool Player::AudioOnly() const
{
	const PlayState s = m->state;
	return (s == PlayState::Playing || s == PlayState::Paused || s == PlayState::Stopped) && m->vstream < 0 &&
		   m->astream >= 0;
}

int Player::VideoWidth() const
{
	return m->vdec ? m->vdec->width : 0;
}

int Player::VideoHeight() const
{
	return m->vdec ? m->vdec->height : 0;
}

double Player::VideoAspect() const
{
	if (!m->vdec || m->vdec->height <= 0)
		return 16.0 / 9.0;
	AVRational sar = m->cur && m->cur->sample_aspect_ratio.num ? m->cur->sample_aspect_ratio
															   : av_guess_sample_aspect_ratio(m->ic, m->ic->streams[m->vstream], nullptr);
	double a = double(m->vdec->width) / m->vdec->height;
	if (sar.num > 0 && sar.den > 0)
		a *= av_q2d(sar);
	return a;
}

const Image* Player::CoverArt() const
{
	return m->has_cover ? &m->cover : nullptr;
}

bool Player::UpdateVideo()
{
	Impl& s = *m;
	if (s.vstream < 0 || s.state == PlayState::Opening || s.state == PlayState::Closed)
		return false;
	std::lock_guard<std::mutex> l(s.fmu);
	const int ser = s.serial;
	while (!s.frames.empty() && s.frames.front().serial != ser)
	{
		av_frame_free(&s.frames.front().f);
		s.frames.pop_front();
		s.fcv.notify_all();
	}
	// (test8 review) an exact seek's hold ends without a picture when the video ended there (a seek past its last
	// picture), (test9 review) or none can come (SeekShown's rule: the video track ended before the target), or by its
	// bounds
	if (s.hold_serial.load() == ser && s.frames.empty())
	{
		if (s.vdone_serial == ser)
			s.ReleaseHold(ser, -1e18, "the video ended before the target");
		else if (s.NoPictureCanCome(ser))
			s.ReleaseHold(ser, -1e18, "no picture can come (the video track ended before the target)");
		else
			s.HoldBoundLocked(ser); // (c): the latest picture decoded put up front, or let go without one
	}
	if (s.frames.empty())
		return false;
	bool take = false;
	if (s.show_after_seek || s.cur_serial != ser)
	{
		take = true;
		s.show_after_seek = false;
		s.ReleaseHold(ser, s.frames.front().pts, "");
		s.seek_pictures++;
		if (s.paused && !s.SeekPending())
			s.clock.Set(s.frames.front().pts);
	}
	else if (s.step_req)
	{
		take = true;
		s.step_req = false;
		if (!s.SeekPending())
			s.clock.Set(s.frames.front().pts);
	}
	else if (!s.paused)
	{
		// (review L-d) the display delay is wall time: in the film's time it scales with the playback speed
		const double clock = s.clock.Get() + s.display_delay.load() * s.rate.load();
		// drop pictures that are late (their successor is already due)
		while (s.frames.size() >= 2 && s.frames[1].pts <= clock)
		{
			av_frame_free(&s.frames.front().f);
			s.frames.pop_front();
			s.dropped++;
		}
		take = s.frames.front().pts <= clock + 0.008;
	}
	if (!take)
		return false;
	av_frame_free(&s.cur);
	s.cur = s.frames.front().f;
	s.cur_pts = s.frames.front().pts;
	s.cur_serial = s.frames.front().serial;
	s.cur_id++;
	{
		static std::atomic<uint64_t> serials{0};
		s.pic_serial = ++serials;
	}
	s.frames.pop_front();
	s.fcv.notify_all();
	// pictures per second, measured
	const double now = NowS();
	if (s.shown_t0 == 0)
		s.shown_t0 = now;
	s.shown_count++;
	if (now - s.shown_t0 >= 1.0)
	{
		s.shown_fps = s.shown_count / (now - s.shown_t0);
		s.shown_count = 0;
		s.shown_t0 = now;
	}
	return true;
}

bool Player::SeekShown() const
{
	Impl& s = *m;
	const PlayState st = s.state;
	if (st != PlayState::Playing && st != PlayState::Paused && st != PlayState::Stopped)
		return true;
	if (s.SeekPending())
		return false; // the demuxer hasn't taken it yet
	if (s.vstream < 0)
		return true; // the sound: nothing to wait for
	std::lock_guard<std::mutex> l(s.fmu);
	const int ser = s.serial;
	// a picture from where it went shown, or the video ended there (a seek past the last picture)
	if ((s.cur_serial == ser && !s.show_after_seek) || s.vdone_serial == ser)
		return true;
	// (second / third reviews) or none can come, with no video left to decode (none queued, none being decoded, no
	// picture waiting): its reads have failed for a second with no read in progress (a read that blocks, however long,
	// never counts), or the sound read has run 10 s past both its first packet and the last video packet (the video
	// track ended there), paused or playing
	return s.NoPictureCanCome(ser);
}

bool Player::SeekTaken() const
{
	std::lock_guard<std::mutex> l(m->seek_mu);
	return !m->seek_req;
}

bool Player::HasPicture() const
{
	return m->cur != nullptr;
}

const AVFrame* Player::Picture(uint64_t* id) const
{
	Impl& s = *m;
	AVFrame* f = s.cur;
	if (f)
	{
		const tonemap::Transfer tr = tonemap::TransferOf(f);
		s.hdr_shown = tr == tonemap::Transfer::Pq ? 1 : tr == tonemap::Transfer::Hlg ? 2 : 0;
	}
	if (id)
		*id = s.pic_serial;
	return f;
}

std::string Player::TakeNotice()
{
	std::lock_guard<std::mutex> l(m->mu);
	std::string n;
	n.swap(m->notice);
	return n;
}

void Player::SetDisplayDelay(double seconds)
{
	m->display_delay = std::max(0.0, std::min(seconds, 0.1));
}

int Player::ToneMapMode() const
{
	return m->tm_mode;
}

int Player::ToneMapWhite() const
{
	return m->tm_white;
}

bool Player::RenderVideo(uint32_t* dst, int pitch, int w, int h, bool force)
{
	Impl& s = *m;
	AVFrame* f = s.CpuPicture();
	if (!f || w <= 0 || h <= 0)
		return false;
	if (!force && s.rendered_id == s.cur_id && s.rendered_dst == dst && s.rendered_w == w && s.rendered_h == h)
		return true;
	const double t0 = NowS();
	SwsContext* c = s.sws;
	// a new context when the size or the format changes (sws_getCachedContext can't set the threads)
	int& ctx_w = s.ctx_w; int& ctx_h = s.ctx_h; int& ctx_sw = s.ctx_sw; int& ctx_sh = s.ctx_sh; int& ctx_fmt = s.ctx_fmt;
	const tonemap::Transfer tr = tonemap::TransferOf(f);
	const bool hdr = tr != tonemap::Transfer::Sdr && s.tm_mode != tonemap::Off;
	s.hdr_shown = tr == tonemap::Transfer::Pq ? 1 : tr == tonemap::Transfer::Hlg ? 2 : 0;
	if (!c || ctx_w != w || ctx_h != h || ctx_sw != f->width || ctx_sh != f->height || ctx_fmt != f->format ||
		s.ctx_hdr != hdr || s.ctx_chroma != int(f->chroma_location))
	{
		sws_freeContext(c);
		c = sws_alloc_context();
		av_opt_set_int(c, "srcw", f->width, 0);
		av_opt_set_int(c, "srch", f->height, 0);
		av_opt_set_int(c, "src_format", f->format, 0);
		av_opt_set_int(c, "dstw", w, 0);
		av_opt_set_int(c, "dsth", h, 0);
		av_opt_set_int(c, "dst_format", hdr ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA, 0);
		av_opt_set_int(c, "sws_flags", SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, 0);
		av_opt_set_int(c, "threads", 6, 0);
		// 1.6 (review L6): the chroma where the stream sites it (MPEG-2 / H.264 / HEVC: "left"), as the GPU path does;
		// a stream that doesn't say keeps libswscale's default (centred), as up to 1.5.4
		if (f->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
		{
			int cx = 128, cy = 128;
			if (av_chroma_location_enum_to_pos(&cx, &cy, f->chroma_location) == 0)
			{
				av_opt_set_int(c, "src_h_chr_pos", cx, 0);
				av_opt_set_int(c, "src_v_chr_pos", cy, 0);
			}
		}
		if (sws_init_context(c, nullptr, nullptr) < 0)
		{
			sws_freeContext(c);
			s.sws = nullptr;
			return false;
		}
		// the colour matrix: BT.709 for HD, BT.601 for SD, as the stream says when it says
		int cs = f->colorspace;
		if (cs == AVCOL_SPC_UNSPECIFIED)
			cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
		if (cs == AVCOL_SPC_BT2020_NCL || cs == AVCOL_SPC_BT2020_CL)
			cs = SWS_CS_BT2020;
		int* inv = nullptr;
		int src_range = 0, *tbl = nullptr, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
		sws_getColorspaceDetails(c, &inv, &src_range, &tbl, &dst_range, &brightness, &contrast, &saturation);
		src_range = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
		const int* coef = sws_getCoefficients(cs);
		sws_setColorspaceDetails(c, coef, src_range, sws_getCoefficients(SWS_CS_DEFAULT), 1, brightness, contrast,
			saturation);
		s.sws = c;
		ctx_w = w;
		ctx_h = h;
		ctx_sw = f->width;
		ctx_sh = f->height;
		ctx_fmt = f->format;
		s.ctx_hdr = hdr;
		s.ctx_chroma = int(f->chroma_location);
		if (hdr)
			OrbisLog("[player] %s picture: tone mapped to SDR (%s, white %d nits, film peak %.0f nits)",
				tr == tonemap::Transfer::Pq ? "HDR10 (PQ)" : "HLG",
				s.tm_mode == tonemap::Hable ? "Hable" : s.tm_mode == tonemap::Reinhard ? "Reinhard" : "BT.2390",
				s.tm_white.load(), tonemap::SourcePeak(f));
	}
	AVFrame* out = av_frame_alloc();
	out->width = w;
	out->height = h;
	if (hdr)
	{
		// 16-bit R'G'B' first, then the tone mapping into the window
		s.hdr_buf.resize(size_t(w) * h * 4);
		out->format = AV_PIX_FMT_RGBA64LE;
		out->data[0] = reinterpret_cast<uint8_t*>(s.hdr_buf.data());
		out->linesize[0] = w * 8;
		out->buf[0] = av_buffer_create(out->data[0], s.hdr_buf.size() * 2, [](void*, uint8_t*) {}, nullptr, 0);
	}
	else
	{
		out->format = AV_PIX_FMT_RGBA;
		out->data[0] = reinterpret_cast<uint8_t*>(dst);
		out->linesize[0] = pitch * 4;
		// a buffer reference over our memory: without one sws_scale_frame allocates its own picture
		out->buf[0] = av_buffer_create(out->data[0], size_t(pitch) * 4 * h, [](void*, uint8_t*) {}, nullptr, 0);
	}
	int r = sws_scale_frame(c, out, f);
	av_frame_free(&out);
	if (hdr && r >= 0)
	{
		tonemap::Params tp;
		tp.mode = tonemap::Mode(s.tm_mode.load());
		tp.transfer = tr;
		tp.src_peak = tonemap::SourcePeak(f);
		tp.white = s.tm_white;
		tonemap::Convert(s.hdr_buf.data(), w, dst, pitch, w, h, tp);
	}
	s.convert_ms = s.convert_ms * 0.9 + (NowS() - t0) * 1000 * 0.1;
	s.rendered_id = s.cur_id;
	s.rendered_dst = dst;
	s.rendered_w = w;
	s.rendered_h = h;
	return r >= 0;
}

bool Player::SavePicture(const std::string& path, std::string* error)
{
	AVFrame* f = m->CpuPicture();
	if (!f)
	{
		if (error)
			*error = "No picture to save";
		return false;
	}
	const int w = f->width, h = f->height;
	SwsContext* c = sws_getContext(w, h, AVPixelFormat(f->format), w, h, AV_PIX_FMT_RGB24, SWS_BICUBIC, nullptr, nullptr, nullptr);
	const AVCodec* png = avcodec_find_encoder(AV_CODEC_ID_PNG);
	AVCodecContext* enc = png ? avcodec_alloc_context3(png) : nullptr;
	bool ok = false;
	if (c && enc)
	{
		AVFrame* rgb = av_frame_alloc();
		rgb->format = AV_PIX_FMT_RGB24;
		rgb->width = w;
		rgb->height = h;
		if (av_frame_get_buffer(rgb, 0) >= 0 && sws_scale_frame(c, rgb, f) >= 0)
		{
			enc->width = w;
			enc->height = h;
			enc->pix_fmt = AV_PIX_FMT_RGB24;
			enc->time_base = AVRational{1, 25};
			AVPacket* pkt = av_packet_alloc();
			if (avcodec_open2(enc, png, nullptr) >= 0 && avcodec_send_frame(enc, rgb) >= 0 &&
				avcodec_receive_packet(enc, pkt) >= 0)
			{
				if (FILE* fp = fopen(path.c_str(), "wb"))
				{
					ok = fwrite(pkt->data, 1, size_t(pkt->size), fp) == size_t(pkt->size);
					fclose(fp);
				}
			}
			av_packet_free(&pkt);
		}
		av_frame_free(&rgb);
	}
	avcodec_free_context(&enc);
	sws_freeContext(c);
	if (!ok && error)
		*error = Fmt(T("Can't write %s"), path.c_str());
	return ok;
}

SubtitleShow Player::Subtitles() const
{
	SubtitleShow out;
	const int track = m->sstream;
	if (track < 0)
		return out;
	const double t = Position() - m->sub_delay;
	std::lock_guard<std::mutex> l(m->smu);
	m->DecodeShown(track, t); // an external bitmap file's lines: their pictures now (1.5.2)
	auto it = m->subs.find(track);
	if (it == m->subs.end())
		return out;
	const std::vector<SubItem>& list = it->second;
	// the items that started before t (sorted by start); a few overlapping ones may show together
	auto end = std::upper_bound(list.begin(), list.end(), t, [](double v, const SubItem& a) { return v < a.start; });
	int seen = 0;
	for (auto i = end; i != list.begin() && seen < 64;)
	{
		--i;
		seen++;
		if (t >= i->start && t < i->end)
		{
			const SubtitleShow& s = i->show;
			out.lines.insert(out.lines.begin(), s.lines.begin(), s.lines.end());
			out.italic.insert(out.italic.begin(), s.italic.begin(), s.italic.end());
			out.top = out.top || s.top;
			for (const auto& p : s.pics)
				out.pics.push_back(p);
			if (s.ref_w > 0)
			{
				out.ref_w = s.ref_w;
				out.ref_h = s.ref_h;
			}
		}
	}
	if (out.ref_w <= 0 && m->vdec)
	{
		out.ref_w = m->vdec->width;
		out.ref_h = m->vdec->height;
	}
	return out;
}

bool Player::AssSubtitles(int frame_w, int frame_h, int storage_w, int storage_h, const CRect& clip, const Image** img,
	int* x, int* y, bool* changed)
{
	*img = nullptr;
	*changed = false;
	const int track = m->sstream;
	if (track < 0)
		return false;
	ass::Renderer* r = m->Ass(false);
	if (!r || !r->HasTrack(track))
		return false;
	const double t = Position() - m->sub_delay;
	*img = r->Render(track, int64_t(std::llround(t * 1000)), frame_w, frame_h, storage_w, storage_h, clip.left, clip.top,
		clip.right, clip.bottom, x, y, changed);
	return true;
}

std::vector<std::pair<std::string, std::string>> Player::Details() const
{
	std::vector<std::pair<std::string, std::string>> d;
	AVFormatContext* ic = m->ic;
	if (!ic || m->state == PlayState::Opening)
		return d;
	d.push_back({"Format", ic->iformat->long_name ? ic->iformat->long_name : ic->iformat->name});
	if (m->duration > 0)
		d.push_back({"Duration", FormatTime(m->duration, true)});
	if (ic->bit_rate > 0)
		d.push_back({"Overall bit rate", std::to_string(ic->bit_rate / 1000) + " kb/s"});
	if (ic->pb && avio_size(ic->pb) > 0)
	{
		char b[64];
		snprintf(b, sizeof(b), T("%.2f MiB (%lld bytes)"), avio_size(ic->pb) / 1048576.0, (long long)avio_size(ic->pb));
		d.push_back({"File size", b});
	}
	const AVDictionaryEntry* e = nullptr;
	while ((e = av_dict_get(ic->metadata, "", e, AV_DICT_IGNORE_SUFFIX)))
		d.push_back({e->key, e->value});
	for (unsigned i = 0; i < ic->nb_streams; i++)
	{
		const AVStream* st = ic->streams[i];
		const AVCodecParameters* p = st->codecpar;
		const AVCodecDescriptor* desc = avcodec_descriptor_get(p->codec_id);
		const char* kind = p->codec_type == AVMEDIA_TYPE_VIDEO	   ? "Video"
						   : p->codec_type == AVMEDIA_TYPE_AUDIO   ? "Audio"
						   : p->codec_type == AVMEDIA_TYPE_SUBTITLE ? "Text"
						   : p->codec_type == AVMEDIA_TYPE_ATTACHMENT ? "Attachment"
																	  : "Data";
		std::string v = desc ? (desc->long_name ? desc->long_name : desc->name) : "?";
		const char* prof = avcodec_profile_name(p->codec_id, p->profile);
		if (prof)
			v += std::string(" (") + prof + ")";
		char b[256];
		if (p->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
			snprintf(b, sizeof(b), ", %dx%d, %.3f fps, %s", p->width, p->height, fr.den ? av_q2d(fr) : 0.0,
				av_get_pix_fmt_name(AVPixelFormat(p->format)) ? av_get_pix_fmt_name(AVPixelFormat(p->format)) : "?");
			v += b;
		}
		else if (p->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			snprintf(b, sizeof(b), ", %d Hz, %s", p->sample_rate, ChannelText(p->ch_layout.nb_channels).c_str());
			v += b;
		}
		if (p->bit_rate > 0)
			v += ", " + std::to_string(p->bit_rate / 1000) + " kb/s";
		const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
		if (lang)
			v += std::string(", ") + LangName(lang->value);
		const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
		if (title)
			v += std::string(", \"") + title->value + "\"";
		d.push_back({std::string(kind) + " #" + std::to_string(i), v});
	}
	if (ic->nb_chapters)
		d.push_back({"Chapters", std::to_string(ic->nb_chapters)});
	return d;
}

std::string Player::Title() const
{
	if (!m->ic || m->state == PlayState::Opening)
		return "";
	const AVDictionaryEntry* t = av_dict_get(m->ic->metadata, "title", nullptr, 0);
	return t ? t->value : "";
}

std::string Player::AudioDescription() const
{
	if (!m->adec)
		return "";
	const AVCodecDescriptor* d = avcodec_descriptor_get(m->adec->codec_id);
	char b[128];
	snprintf(b, sizeof(b), "%s %dHz %s", d ? d->name : "?", m->adec->sample_rate,
		ChannelText(m->adec->ch_layout.nb_channels).c_str());
	std::string s = b;
	for (size_t i = 0; i < s.size() && s[i] != ' '; i++)
		s[i] = char(toupper(static_cast<unsigned char>(s[i])));
	if (m->adec->bit_rate > 0)
		s += " " + std::to_string(m->adec->bit_rate / 1000) + "kbps";
	return s;
}

int Player::AudioChannels() const
{
	return m->adec ? m->adec->ch_layout.nb_channels : 0;
}

PlayerStats Player::Stats() const
{
	PlayerStats st;
	if (m->vdec && m->ic && m->vstream >= 0)
	{
		const AVStream* s = m->ic->streams[m->vstream];
		const AVRational fr = s->avg_frame_rate.num ? s->avg_frame_rate : s->r_frame_rate;
		st.fps = fr.den ? av_q2d(fr) : 0;
	}
	st.shown_fps = m->shown_fps;
	{
		std::lock_guard<std::mutex> l(m->fmu);
		st.picture_s = m->cur ? m->cur_pts : -1;
	}
	st.dropped = m->dropped;
	st.decode_ms = m->decode_ms;
	st.hw_decoding = m->hw_active;
	st.convert_ms = m->convert_ms;
	st.bitrate = m->ic ? m->ic->bit_rate : 0;
	{
		std::lock_guard<std::mutex> l(m->fmu);
		st.queued_video = int(m->frames.size());
		st.seek_pictures = m->seek_pictures;
	}
	st.underruns = ps5audio::Underruns();
	return st;
}

std::string FFmpegVersion()
{
	return av_version_info();
}

std::string FormatTime(double seconds, bool force_hours)
{
	if (seconds < 0 || std::isnan(seconds))
		seconds = 0;
	const long t = long(seconds);
	char b[32];
	if (force_hours || t >= 3600)
		snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
	else
		snprintf(b, sizeof(b), "%02ld:%02ld", t / 60, t % 60);
	return b;
}
} // namespace mpc
