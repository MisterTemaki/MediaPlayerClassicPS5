// Media Player Classic PS5: the settings (CAppSettings), kept in <data>/mpc-hc.ini as key=value lines --
// MPC-HC's "Store settings to .ini file" mode, with its own key names where there is one.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace mpc
{
enum class AfterPlayback
{
	DoNothing = 0,
	PlayNext, // the next file in the folder
	Exit,
};

enum class VideoFrame // View > Video Frame (dvs)
{
	HalfSize = 0,
	NormalSize,
	DoubleSize,
	Stretch,
	FromInside, // "Touch Window From Inside" (the default)
	Zoom1,
	Zoom2,
	FromOutside,
};

struct NetServer
{
	std::string name; // shown in the browser ("Living room PC")
	std::string url; // smb://user:pass@host/share
};

struct Favorite
{
	std::string name, url;
	double pos = 0;
};

struct FilePosition
{
	std::string url;
	double pos = 0;
};

struct Settings
{
	// Player
	bool remember_file_pos = true; // Options > Player > History "Remember file position"
	bool remember_playlist = false;
	int title_bar = 1; // 0 full path, 1 file name only, 2 don't prefix anything
	bool show_osd = true;
	bool keep_history = true;
	// View
	bool show_seekbar = true;
	bool show_controls = true;
	bool show_status = true;
	bool show_info = false;
	bool show_stats = false;
	bool show_playlist = false;
	bool show_caption_menu = true; // Caption&Menu
	bool fullscreen = false;
	bool hide_controls_fullscreen = true; // shown again when the cursor reaches the bottom
	VideoFrame video_frame = VideoFrame::FromInside;
	int aspect_ratio = 0; // 0 DAR, 1 4:3, 2 5:4, 3 16:9, 4 2.35, 5 1.85, 6 SAR
	double pan_x = 0, pan_y = 0, zoom_x = 1, zoom_y = 1; // Pan&Scan
	// Playback
	int volume = 100;
	bool mute = false;
	int repeat = 0; // 0 off, 1 forever (file), 2 playlist
	AfterPlayback after_playback = AfterPlayback::DoNothing;
	int jump_small = 1, jump_medium = 5, jump_large = 20; // seconds (MPC-HC's defaults: Shift, plain, Ctrl)
	bool auto_load_audio = true;
	bool auto_load_subtitles = true;
	// Audio
	int audio_boost = 0; // percent, Options > Audio Switcher "Boost"
	int audio_delay_ms = 0;
	int audio_output = 0; // 0 stereo (5.1/7.1 mixed down), 1 surround: the console's 8-channel output
	// Output (video)
	bool hw_decoding = true; // H.264 / HEVC on the console's video decoder (mpc_vdec.h)
	int hdr_tonemap = 1;     // HDR10 / HLG on an SDR picture: 0 off (as it comes), 1 BT.2390, 2 Hable, 3 Reinhard
	int hdr_peak = 250;      // the picture's white after tone mapping, in nits (100..400)
	// Subtitles (Default Style)
	int sub_font_size = 18; // points at the video's height of 1080 / 1.5 (MPC-HC's default text style: Arial 18)
	bool sub_bold = true;
	int sub_outline = 2;
	int sub_margin = 16; // bottom margin, pixels at 96 dpi
	int sub_position = 0; // 0 in the video, 1 at the bottom of the window
	int sub_delay_ms = 0;
	// Interface
	int dpi = 144; // 96 / 120 / 144 / 192 (100 % .. 200 %)
	int cursor_speed = 3; // 1..5
	int language = 0; // Options > Player > Language: 0 English, 1 Spanish, 2 Portuguese (Brazil), 3 French, 4 German
	// boot.log and the others in <data>/logs: off unless mpc-hc.ini says debug_logs=1 (orbis_paths.cpp reads the
	// same line; no menu item, the key is kept when the settings are saved)
	bool debug_logs = false;
	// Lists
	std::vector<std::string> recent; // MRU, newest first
	std::vector<FilePosition> positions;
	std::vector<Favorite> favorites;
	std::vector<NetServer> servers;
	std::string last_dir;

	void Load();
	void Save() const;
	void AddRecent(const std::string& url);
	double FilePos(const std::string& url) const;
	void SetFilePos(const std::string& url, double pos);
};

Settings& AppSettings();
std::string SettingsPath();
} // namespace mpc
