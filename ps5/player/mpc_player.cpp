// Media Player Classic PS5: the playback engine (mpc_player.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_player.h"

#include "mpc_ass.h"
#include "mpc_disc.h"
#include "mpc_i18n.h"
#include "mpc_res.h"
#include "mpc_smb.h"
#include "mpc_tonemap.h"
#include "mpc_vdec.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"

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
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
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
	bool abort = false;

	void Put(AVPacket* p, int serial)
	{
		std::lock_guard<std::mutex> l(mu);
		q.push_back({p, serial});
		if (p)
		{
			bytes += size_t(p->size) + sizeof(*p);
			duration += p->duration;
		}
		cv.notify_all();
	}
	void Flush()
	{
		std::lock_guard<std::mutex> l(mu);
		for (auto& e : q)
			av_packet_free(&e.pkt);
		q.clear();
		bytes = 0;
		duration = 0;
		cv.notify_all();
	}
	void Abort()
	{
		std::lock_guard<std::mutex> l(mu);
		abort = true;
		cv.notify_all();
	}
	// 1 got one, 0 timed out, -1 aborted
	int Get(QPacket& out, int timeout_ms)
	{
		std::unique_lock<std::mutex> l(mu);
		if (!cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return abort || !q.empty(); }))
			return 0;
		if (abort)
			return -1;
		out = q.front();
		q.pop_front();
		if (out.pkt)
		{
			bytes -= size_t(out.pkt->size) + sizeof(*out.pkt);
			duration -= out.pkt->duration;
		}
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
	void Set(double p)
	{
		std::lock_guard<std::mutex> l(mu);
		pts = p;
		at = NowS();
	}
	double Get()
	{
		std::lock_guard<std::mutex> l(mu);
		return paused ? pts : pts + (NowS() - at) * rate;
	}
	void SetPaused(bool p)
	{
		std::lock_guard<std::mutex> l(mu);
		if (paused == p)
			return;
		const double now = NowS();
		if (!paused)
			pts += (now - at) * rate;
		at = now;
		paused = p;
	}
	void SetRate(double r)
	{
		std::lock_guard<std::mutex> l(mu);
		const double now = NowS();
		if (!paused)
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
};

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
	// HDR on the SDR picture (mpc_tonemap.h): Options > Output
	std::atomic<int> tm_mode{tonemap::Bt2390};
	std::atomic<int> tm_white{250};
	std::vector<uint16_t> hdr_buf;
	std::atomic<int> hdr_shown{0}; // 0 SDR, 1 PQ, 2 HLG: what the last picture was

	bool Seekable_() const
	{
		const PlayState s = state;
		return (s == PlayState::Playing || s == PlayState::Paused || s == PlayState::Stopped) && seekable;
	}
	AVCodecContext* vdec = nullptr;
	// the console's video decoder (mpc_vdec.h) for H.264 / HEVC; vdec (FFmpeg) when it can't or gives up
	hwdec::Decoder* hw = nullptr;
	std::atomic<bool> hw_active{false};
	AVCodecContext* adec = nullptr;
	std::map<int, AVCodecContext*> sdecs;
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
	double seek_to = 0;
	bool seek_exact = true;
	std::atomic<double> skip_until{-1e18}; // frames before this (in the current serial) are not shown
	std::atomic<bool> show_after_seek{false};

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
	AVFrame* cur = nullptr;
	double cur_pts = 0;
	int cur_serial = -1;
	uint64_t cur_id = 0;
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
	std::vector<std::pair<int, std::string>> external_names;
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

	~Impl() { Shutdown(); }

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

	AVCodecContext* OpenDecoder(int idx, bool threads)
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
			c->thread_count = 8; // the console's game cores
			c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
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
		std::string open_url = url;
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
			OrbisLog("[player] video decoder: %s", hw ? hwdec::Describe(hw).c_str() : ("FFmpeg (" + why + ")").c_str());
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
		ps5audio::SetPaused(start_paused);
		state = start_paused ? PlayState::Paused : PlayState::Playing;

		demuxer = std::thread([this] { DemuxThread(); });
		if (vdec)
			vthread = std::thread([this] { VideoThread(); });
		if (adec)
			athread = std::thread([this] { AudioThread(); });
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
		static const char* const exts[] = {".srt", ".ass", ".ssa", ".vtt", ".sub", ".pt-BR.srt", ".pt.srt", ".por.srt",
			".en.srt", ".eng.srt"};
		bool any = false;
		for (const char* e : exts)
		{
			const std::string p = base + e;
			if (url.compare(0, 6, "smb://") == 0 ? smb::Exists(p) : url.compare(0, 7, "disc://") == 0 ? disc::Exists(p) : OrbisIsFile(p))
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
				}
			}
			if (do_seek)
			{
				target = std::max(0.0, duration > 0 ? std::min(target, duration) : target);
				const int64_t ts = int64_t(target * AV_TIME_BASE) + (ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0);
				const int r = avformat_seek_file(ic, -1, INT64_MIN, ts, ts, 0);
				if (r < 0)
					OrbisLog("[player] seek to %.2f failed: %s", target, AvErr(r).c_str());
				vq.Flush();
				aq.Flush();
				serial++;
				skip_until = exact ? target - 0.001 : -1e18;
				ps5audio::Clear();
				clock.Set(target);
				eof = false;
				ended = false;
				show_after_seek = true;
				{
					std::lock_guard<std::mutex> l(fmu);
					for (auto& f : frames)
						av_frame_free(&f.f);
					frames.clear();
					fcv.notify_all();
				}
			}
			// enough queued? (10 s or 64 MiB)
			const bool full = (vq.bytes + aq.bytes) > (64u << 20) ||
							  ((vstream < 0 || vq.Count() > 300) && (astream < 0 || aq.Count() > 500));
			if (full || eof)
			{
				av_usleep(10000);
				if (eof && vdone_serial == serial && (astream < 0 || adone_serial == serial) && !ended)
					ended = true;
				else if (eof && vstream < 0 && astream >= 0 && adone_serial == serial && !ended)
					ended = true;
				continue;
			}
			const int r = av_read_frame(ic, pkt);
			if (r < 0 && disc_gen && disc::Generation(url) != disc_gen)
			{
				// the disc was taken out: not the end of the film (the UI stops it and says so)
				if (!disc_gone)
					OrbisLog("[player] the disc was taken out while playing %s", url.c_str());
				disc_gone = true;
				av_usleep(20000);
				continue;
			}
			if (r < 0)
			{
				if (r == AVERROR_EOF || (ic->pb && avio_feof(ic->pb)))
				{
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
					av_usleep(10000);
				continue;
			}
			const int idx = pkt->stream_index;
			if (idx == vstream)
				vq.Put(av_packet_clone(pkt), serial);
			else if (idx == astream)
				aq.Put(av_packet_clone(pkt), serial);
			else
			{
				auto it = sdecs.find(idx);
				if (it != sdecs.end())
					DecodeSubtitle(idx, it->second, pkt);
			}
			av_packet_unref(pkt);
		}
		av_packet_free(&pkt);
	}

	void StoreSubtitle(int track, const AVSubtitle& sub, double pts, int ref_w, int ref_h)
	{
		SubItem item;
		item.start = pts + sub.start_display_time / 1000.0;
		item.open_end = sub.end_display_time == 0 || sub.end_display_time == UINT32_MAX;
		item.end = item.open_end ? 1e18 : pts + sub.end_display_time / 1000.0;
		item.show.ref_w = ref_w;
		item.show.ref_h = ref_h;
		for (unsigned i = 0; i < sub.num_rects; i++)
		{
			const AVSubtitleRect* r = sub.rects[i];
			if (r->type == SUBTITLE_ASS || r->type == SUBTITLE_TEXT)
			{
				const std::string text = r->type == SUBTITLE_ASS ? AssEventText(r->ass) : std::string(r->text ? r->text : "");
				AssToText(text, item.show.lines, item.show.italic, item.show.top);
			}
			else if (r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0 && r->data[0] && r->data[1])
			{
				SubtitleShow::Pic pic;
				pic.x = r->x;
				pic.y = r->y;
				std::vector<uint8_t> rgba(size_t(r->w) * r->h * 4);
				const uint32_t* pal = reinterpret_cast<const uint32_t*>(r->data[1]);
				for (int y = 0; y < r->h; y++)
					for (int x = 0; x < r->w; x++)
					{
						const uint32_t c = pal[r->data[0][y * r->linesize[0] + x]]; // 0xAARRGGBB
						uint8_t* o = &rgba[(size_t(y) * r->w + x) * 4];
						o[0] = (c >> 16) & 0xff;
						o[1] = (c >> 8) & 0xff;
						o[2] = c & 0xff;
						o[3] = c >> 24;
					}
				pic.img.FromRGBA(rgba.data(), r->w, r->h);
				item.show.pics.push_back(std::move(pic));
			}
		}
		std::lock_guard<std::mutex> l(smu);
		std::vector<SubItem>& list = subs[track];
		// a line that was shown "until the next one" ends here
		for (auto it = list.rbegin(); it != list.rend(); ++it)
			if (it->open_end && it->start <= item.start)
			{
				it->end = item.start;
				it->open_end = false;
				break;
			}
		if (item.show.Empty())
			return; // an empty one only ends the last (PGS)
		// keep the list sorted by start; replace an identical start (a seek replays packets)
		auto pos = std::lower_bound(list.begin(), list.end(), item.start,
			[](const SubItem& a, double s) { return a.start < s; });
		if (pos != list.end() && std::fabs(pos->start - item.start) < 0.001)
			*pos = std::move(item);
		else
			list.insert(pos, std::move(item));
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
		if (pts + dur < skip_until.load())
		{
			av_frame_free(&f);
			return;
		}
		std::unique_lock<std::mutex> l(fmu);
		fcv.wait(l, [&] { return quit || frames.size() < kMaxFrames || serial != my_serial; });
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
			const int r = vq.Get(qp, 100);
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
		clock.Set(end_pts - queued - audio_delay);
		return true;
	}

	void AudioThread()
	{
		AVFrame* frame = av_frame_alloc();
		AVFrame* out = av_frame_alloc();
		int dec_serial = -1;
		double out_pos = 0; // stream time at the end of what has been pushed
		bool have_pos = false;
		int nopts_drop = 0; // frames without a timestamp dropped before the first timed one
		norm_factor = -1; // Normalize starts again at the max (a file, another track)
		while (!quit && !a_stop)
		{
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
				nopts_drop = 0;
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
					// After a seek out_pos still holds the position before it: a first
					// frame without a timestamp (a DVD title's first AC-3 packet after a
					// seek has none) would set the clock back to that position. Wait for
					// a timed frame; a stream with no timestamps starts at the clock.
					if (++nopts_drop <= 32)
					{
						av_frame_unref(frame);
						continue;
					}
					out_pos = clock.Get();
				}
				double pts = frame->pts != AV_NOPTS_VALUE ? frame->pts * av_q2d(st->time_base) - StreamStart() : out_pos;
				const double fdur = frame->nb_samples / double(frame->sample_rate);
				if (pts + fdur < skip_until.load())
				{
					av_frame_unref(frame);
					continue;
				}
				if (!have_pos)
				{
					out_pos = pts;
					have_pos = true;
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
		av_frame_free(&cur);
		hwdec::Close(hw);
		hw = nullptr;
		hw_active = false;
		avcodec_free_context(&vdec);
		avcodec_free_context(&adec);
		for (auto& s : sdecs)
			avcodec_free_context(&s.second);
		sdecs.clear();
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
		ps5audio::SetPaused(false);
	}

	bool AddSubtitleFile(const std::string& path, std::string* err)
	{
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
	Impl* impl = m.get();
	m->opener = std::thread([impl] { impl->OpenThread(); });
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
	m->ic->streams[m->astream]->discard = AVDISCARD_ALL;
	m->ic->streams[id]->discard = AVDISCARD_DEFAULT;
	m->adec = c;
	m->astream = id;
	Impl* impl = m.get();
	m->athread = std::thread([impl] { impl->AudioThread(); });
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
	if (s.frames.empty())
		return false;
	bool take = false;
	if (s.show_after_seek || s.cur_serial != ser)
	{
		take = true;
		s.show_after_seek = false;
		if (s.paused)
			s.clock.Set(s.frames.front().pts);
	}
	else if (s.step_req)
	{
		take = true;
		s.step_req = false;
		s.clock.Set(s.frames.front().pts);
	}
	else if (!s.paused)
	{
		const double clock = s.clock.Get();
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

bool Player::HasPicture() const
{
	return m->cur != nullptr;
}

bool Player::RenderVideo(uint32_t* dst, int pitch, int w, int h, bool force)
{
	Impl& s = *m;
	AVFrame* f = s.cur;
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
		s.ctx_hdr != hdr)
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
	AVFrame* f = m->cur;
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
	st.dropped = m->dropped;
	st.decode_ms = m->decode_ms;
	st.hw_decoding = m->hw_active;
	st.convert_ms = m->convert_ms;
	st.bitrate = m->ic ? m->ic->bit_rate : 0;
	{
		std::lock_guard<std::mutex> l(m->fmu);
		st.queued_video = int(m->frames.size());
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
