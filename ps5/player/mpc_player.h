// Media Player Classic PS5: the playback engine.
//
// On Windows MPC-HC builds a DirectShow graph: a source filter, LAV Splitter and LAV Video/Audio (FFmpeg
// inside), its audio renderer and a video renderer (EVR-CP). Here the same work is done with FFmpeg's
// libraries directly, laid out as ffplay and VLC do it:
//   - an opener thread: avformat_open_input (local files, smb:// through libsmb2 -- mpc_smb.h --, http(s),
//     HLS...), stream info, the default tracks, the decoders;
//   - a demux thread: packets into one queue per decoder; a seek flushes them (a new "serial" -- ffplay's
//     word -- so whatever was decoded before is thrown away);
//   - a video decoder thread (FFmpeg's own frame threads inside), decoded pictures into a short queue;
//   - an audio thread: decode, libavfilter (atempo for the playback rate, the downmix to stereo, 48 kHz S16),
//     into the console's sound ring (ProsperoAudio.h); what it pushes keeps the clock;
//   - subtitles decoded as their packets come (every subtitle track, so switching shows lines at once),
//     plus external .srt/.ass/.ssa/.vtt/.sub files (ANSI files are read as Windows-1252, as MPC-HC does).
// The UI thread asks, once per screen refresh, for the picture that belongs to the clock (UpdateVideo) and has
// it scaled into the screen (RenderVideo, libswscale, bicubic -- EVR-CP's default resizer).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_gdi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mpc
{
enum class TrackType
{
	Video = 0,
	Audio,
	Subtitle,
	Count
};

struct Track
{
	int id = -1; // the stream's index, or 1000+n for external subtitle files
	std::string name; // as MPC-HC's menus show it: "English [eng] (AAC LC, 48000 Hz, 5.1)"
	std::string lang;
	bool external = false;
	bool is_default = false;
};

struct Chapter
{
	double time = 0;
	std::string name;
};

enum class PlayState
{
	Closed,
	Opening,
	Playing,
	Paused,
	Stopped,
	Failed
};

// One subtitle line or picture, as shown now.
struct SubtitleShow
{
	std::vector<std::string> lines; // text subtitles (formatting tags removed); italic per line
	std::vector<bool> italic;
	bool top = false; // ASS \an7-9 / \an8: at the top
	// bitmap subtitles (PGS, DVD, DVB): pictures in the video's coordinates (ref_w x ref_h)
	struct Pic
	{
		Image img;
		int x = 0, y = 0;
	};
	std::vector<Pic> pics;
	int ref_w = 0, ref_h = 0;
	bool Empty() const { return lines.empty() && pics.empty(); }
};

struct PlayerStats
{
	double fps = 0; // the stream's frame rate
	double shown_fps = 0; // pictures shown per second, measured
	uint64_t dropped = 0;
	double decode_ms = 0;
	double convert_ms = 0;
	int64_t bitrate = 0;
	int queued_video = 0;
	double buffered_s = 0;
	uint64_t underruns = 0;
};

class Player
{
public:
	Player();
	~Player();
	Player(const Player&) = delete;
	Player& operator=(const Player&) = delete;

	// Opens in the background (State() is Opening until it is done); playback starts at start_s.
	void Open(const std::string& url, double start_s = 0, bool start_paused = false);
	void Close();
	PlayState State() const;
	std::string Error() const; // why Failed
	std::string Url() const;
	bool Ended() const; // played to the end (the state is then Stopped)

	void Play();
	void Pause();
	void TogglePause();
	void Stop(); // to the start, paused, the first picture shown (MPC-HC's Stop)
	void Seek(double seconds, bool exact = true);
	void FrameStep();
	double Position() const;
	double Duration() const;
	bool Seekable() const;

	void SetVolume(int percent); // 0..100, MPC-HC's slider (a log curve, as CPlayerToolBar::GetVolume)
	void SetMute(bool mute);
	void SetAmplify(double gain); // the audio switcher's boost (1.0 = none)
	void SetRate(double rate); // 0.125..8
	double Rate() const;
	void SetAudioDelay(double seconds);
	double AudioDelay() const;
	void SetSubtitleDelay(double seconds);
	double SubtitleDelay() const;

	std::vector<Track> Tracks(TrackType type) const;
	int CurrentTrack(TrackType type) const; // -1: none (subtitles off)
	void SelectTrack(TrackType type, int id);
	bool AddSubtitleFile(const std::string& path, std::string* error = nullptr);
	std::vector<Chapter> Chapters() const;

	bool HasVideo() const;
	bool AudioOnly() const;
	int VideoWidth() const;
	int VideoHeight() const;
	double VideoAspect() const; // display aspect ratio (with the stream's pixel aspect)
	const Image* CoverArt() const; // an audio file's attached picture
	// The UI thread, once per refresh: takes the picture the clock has reached. True when it changed.
	bool UpdateVideo();
	// Scales the current picture into dst (w x h, pitch in pixels); false when there is none.
	bool RenderVideo(uint32_t* dst, int pitch, int w, int h, bool force = false);
	bool HasPicture() const;
	// Saves the current picture (File > Save Image), at the video's own size, as PNG.
	bool SavePicture(const std::string& path, std::string* error);
	SubtitleShow Subtitles() const;

	// File > Properties: the details lines (MediaInfo tab), and a short "format" line
	std::vector<std::pair<std::string, std::string>> Details() const;
	std::string Title() const; // the file's title tag, or ""
	std::string AudioDescription() const; // "AAC 48000Hz stereo 128kbps" for the status bar
	int AudioChannels() const;
	PlayerStats Stats() const;

	struct Impl;

private:
	std::unique_ptr<Impl> m;
};

// FFmpeg's version, for the About box
std::string FFmpegVersion();
// formats a time as MPC-HC's status bar does: "01:23" or "1:02:03"
std::string FormatTime(double seconds, bool force_hours = false);
} // namespace mpc
