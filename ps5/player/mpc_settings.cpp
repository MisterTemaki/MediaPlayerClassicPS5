// Media Player Classic PS5: the settings (mpc_settings.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_settings.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace mpc
{
namespace
{
std::string Escape(const std::string& s)
{
	std::string o;
	for (char c : s)
	{
		if (c == '\\')
			o += "\\\\";
		else if (c == '\n')
			o += "\\n";
		else if (c == '|')
			o += "\\p";
		else
			o += c;
	}
	return o;
}

std::string Unescape(const std::string& s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '\\' && i + 1 < s.size())
		{
			const char n = s[++i];
			o += n == 'n' ? '\n' : n == 'p' ? '|' : n;
		}
		else
			o += s[i];
	}
	return o;
}

std::vector<std::string> SplitBar(const std::string& s)
{
	std::vector<std::string> out;
	size_t start = 0;
	for (;;)
	{
		const size_t p = s.find('|', start);
		out.push_back(Unescape(s.substr(start, p == std::string::npos ? std::string::npos : p - start)));
		if (p == std::string::npos)
			break;
		start = p + 1;
	}
	return out;
}

int Clamp(int v, int lo, int hi)
{
	return std::max(lo, std::min(hi, v));
}
} // namespace

Settings& AppSettings()
{
	static Settings s;
	return s;
}

std::string SettingsPath()
{
	return OrbisRoot() + "/mpc-hc.ini";
}

void Settings::Load()
{
	// off by default; the host tests turn the default on (orbis_paths.cpp reads the same variable)
	if (const char* d = getenv("MPCPS5_LOGS_DEFAULT"))
		debug_logs = atoi(d) != 0;
	FILE* f = fopen(SettingsPath().c_str(), "r");
	if (!f)
		return;
	char line[8192];
	while (fgets(line, sizeof(line), f))
	{
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
			l.pop_back();
		const size_t eq = l.find('=');
		if (eq == std::string::npos || l[0] == ';' || l[0] == '[')
			continue;
		const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
		const int n = atoi(v.c_str());
		const bool b = n != 0;
		if (k == "RememberFilePos")
			remember_file_pos = b;
		else if (k == "TitleBarTextStyle")
			title_bar = Clamp(n, 0, 2);
		else if (k == "ShowOSD")
			show_osd = b;
		else if (k == "KeepHistory")
			keep_history = b;
		else if (k == "ShowSeekBar")
			show_seekbar = b;
		else if (k == "ShowControls")
			show_controls = b;
		else if (k == "ShowStatus")
			show_status = b;
		else if (k == "ShowInfo")
			show_info = b;
		else if (k == "ShowStatistics")
			show_stats = b;
		else if (k == "ShowPlaylist")
			show_playlist = b;
		else if (k == "ShowCaptionMenu")
			show_caption_menu = b;
		else if (k == "FullScreen")
			fullscreen = b;
		else if (k == "HideFullscreenControls")
			hide_controls_fullscreen = b;
		else if (k == "VideoFrame")
			video_frame = VideoFrame(Clamp(n, 0, 7));
		else if (k == "AspectRatio")
			aspect_ratio = Clamp(n, 0, 6);
		else if (k == "Volume")
			volume = Clamp(n, 0, 100);
		else if (k == "Mute")
			mute = b;
		else if (k == "Loop")
			repeat = Clamp(n, 0, 2);
		else if (k == "AfterPlayback")
			after_playback = AfterPlayback(Clamp(n, 0, 2));
		else if (k == "JumpDistS")
			jump_small = Clamp(n, 1, 600);
		else if (k == "JumpDistM")
			jump_medium = Clamp(n, 1, 600);
		else if (k == "JumpDistL")
			jump_large = Clamp(n, 1, 3600);
		else if (k == "AutoloadSubtitles")
			auto_load_subtitles = b;
		else if (k == "AudioBoost")
			audio_boost = Clamp(n, 0, 300);
		else if (k == "AudioDelay")
			audio_delay_ms = Clamp(n, -10000, 10000);
		else if (k == "AudioOutput")
			audio_output = Clamp(n, 0, 1);
		else if (k == "HardwareDecoding")
			hw_decoding = b;
		else if (k == "HdrToneMapping")
			hdr_tonemap = Clamp(n, 0, 3);
		else if (k == "HdrPeak")
			hdr_peak = Clamp(n, 100, 400);
		else if (k == "SubFontSize")
			sub_font_size = Clamp(n, 8, 72);
		else if (k == "SubBold")
			sub_bold = b;
		else if (k == "SubOutline")
			sub_outline = Clamp(n, 0, 8);
		else if (k == "SubMargin")
			sub_margin = Clamp(n, 0, 200);
		else if (k == "SubPosition")
			sub_position = Clamp(n, 0, 1);
		else if (k == "SubDelay")
			sub_delay_ms = Clamp(n, -60000, 60000);
		else if (k == "Dpi")
			dpi = n == 96 || n == 120 || n == 144 || n == 168 || n == 192 ? n : 144;
		else if (k == "CursorSpeed")
			cursor_speed = Clamp(n, 1, 5);
		else if (k == "Language")
			language = Clamp(n, 0, 4);
		else if (k == "debug_logs")
			debug_logs = b;
		else if (k == "LastDir")
			last_dir = Unescape(v);
		else if (k.compare(0, 4, "File") == 0 && k.size() > 4 && isdigit(static_cast<unsigned char>(k[4])))
			recent.push_back(Unescape(v));
		else if (k.compare(0, 7, "FilePos") == 0)
		{
			const auto parts = SplitBar(v);
			if (parts.size() == 2)
				positions.push_back({parts[0], atof(parts[1].c_str())});
		}
		else if (k.compare(0, 8, "Favorite") == 0)
		{
			const auto parts = SplitBar(v);
			if (parts.size() == 3)
				favorites.push_back({parts[0], parts[1], atof(parts[2].c_str())});
		}
		else if (k.compare(0, 6, "Server") == 0)
		{
			const auto parts = SplitBar(v);
			if (parts.size() == 2)
				servers.push_back({parts[0], parts[1]});
		}
	}
	fclose(f);
	if (recent.size() > 20)
		recent.resize(20);
}

void Settings::Save() const
{
	const std::string path = SettingsPath();
	const std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return;
	fprintf(f, "; Media Player Classic PS5 settings (MPC-HC's names where it has one)\n[Settings]\n");
	fprintf(f, "RememberFilePos=%d\nTitleBarTextStyle=%d\nShowOSD=%d\nKeepHistory=%d\n", remember_file_pos, title_bar,
		show_osd, keep_history);
	fprintf(f, "ShowSeekBar=%d\nShowControls=%d\nShowStatus=%d\nShowInfo=%d\nShowStatistics=%d\nShowPlaylist=%d\n",
		show_seekbar, show_controls, show_status, show_info, show_stats, show_playlist);
	fprintf(f, "ShowCaptionMenu=%d\nFullScreen=%d\nHideFullscreenControls=%d\nVideoFrame=%d\nAspectRatio=%d\n",
		show_caption_menu, fullscreen, hide_controls_fullscreen, int(video_frame), aspect_ratio);
	fprintf(f, "Volume=%d\nMute=%d\nLoop=%d\nAfterPlayback=%d\nJumpDistS=%d\nJumpDistM=%d\nJumpDistL=%d\n", volume, mute,
		repeat, int(after_playback), jump_small, jump_medium, jump_large);
	fprintf(f, "AutoloadSubtitles=%d\nAudioBoost=%d\nAudioDelay=%d\nAudioOutput=%d\n", auto_load_subtitles, audio_boost,
		audio_delay_ms, audio_output);
	fprintf(f, "HardwareDecoding=%d\nHdrToneMapping=%d\nHdrPeak=%d\n", hw_decoding, hdr_tonemap, hdr_peak);
	fprintf(f, "SubFontSize=%d\nSubBold=%d\nSubOutline=%d\nSubMargin=%d\nSubPosition=%d\nSubDelay=%d\n", sub_font_size,
		sub_bold, sub_outline, sub_margin, sub_position, sub_delay_ms);
	fprintf(f, "Dpi=%d\nCursorSpeed=%d\nLanguage=%d\ndebug_logs=%d\nLastDir=%s\n", dpi, cursor_speed, language,
		debug_logs ? 1 : 0, Escape(last_dir).c_str());
	fprintf(f, "[Recent File List]\n");
	for (size_t i = 0; i < recent.size(); i++)
		fprintf(f, "File%zu=%s\n", i + 1, Escape(recent[i]).c_str());
	fprintf(f, "[Positions]\n");
	for (size_t i = 0; i < positions.size(); i++)
		fprintf(f, "FilePos%zu=%s|%.3f\n", i, Escape(positions[i].url).c_str(), positions[i].pos);
	fprintf(f, "[Favorites]\n");
	for (size_t i = 0; i < favorites.size(); i++)
		fprintf(f, "Favorite%zu=%s|%s|%.3f\n", i, Escape(favorites[i].name).c_str(), Escape(favorites[i].url).c_str(),
			favorites[i].pos);
	fprintf(f, "[Network]\n");
	for (size_t i = 0; i < servers.size(); i++)
		fprintf(f, "Server%zu=%s|%s\n", i, Escape(servers[i].name).c_str(), Escape(servers[i].url).c_str());
	fclose(f);
	rename(tmp.c_str(), path.c_str());
}

void Settings::AddRecent(const std::string& url)
{
	if (!keep_history)
		return;
	recent.erase(std::remove(recent.begin(), recent.end(), url), recent.end());
	recent.insert(recent.begin(), url);
	if (recent.size() > 20)
		recent.resize(20);
}

double Settings::FilePos(const std::string& url) const
{
	for (const auto& p : positions)
		if (p.url == url)
			return p.pos;
	return 0;
}

void Settings::SetFilePos(const std::string& url, double pos)
{
	positions.erase(std::remove_if(positions.begin(), positions.end(), [&](const FilePosition& p) { return p.url == url; }),
		positions.end());
	if (pos > 0)
		positions.insert(positions.begin(), {url, pos});
	if (positions.size() > 50)
		positions.resize(50);
}
} // namespace mpc
