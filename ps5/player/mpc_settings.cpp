// Media Player Classic PS5: the settings (mpc_settings.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_settings.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
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

// ---- passwords kept out of mpc-hc.ini ----------------------------------------------------------------------------
// An address with a password (smb://user:secret@server/share..., as the Add network location dialog makes it) is
// written to mpc-hc.ini without it: smb://user@server/share... The password goes to credentials.ini beside it
// (owner-only permissions), one line per "scheme://user@server", and comes back into the addresses when the
// settings are read. So mpc-hc.ini, the file people send when asking for help, carries no password, and a server
// removed from the list takes its password with it (credentials.ini holds only what the saved addresses use).
// It isn't encryption: on a jailbroken console anything that can read /data can read credentials.ini.
namespace
{
// host tests: MPCPS5_HOST_SETTINGS_FAIL=credentials|settings makes writing that file fail
bool FailForTest(const char* what)
{
#ifndef __PROSPERO__
	const char* f = getenv("MPCPS5_HOST_SETTINGS_FAIL");
	return f && strcmp(f, what) == 0;
#else
	(void)what;
	return false;
#endif
}

std::string CredentialsPath()
{
	return OrbisRoot() + "/credentials.ini";
}

// url = scheme://[auth@]host...: the key (scheme://user@host), the password as written (still %-encoded), and the
// address without it. False when there's no user:password in it.
bool SplitPassword(const std::string& url, std::string& key, std::string& secret, std::string& bare)
{
	const size_t sch = url.find("://");
	if (sch == std::string::npos)
		return false;
	const size_t auth = sch + 3;
	size_t end = url.find('/', auth);
	if (end == std::string::npos)
		end = url.size();
	const size_t at = url.rfind('@', end);
	if (at == std::string::npos || at < auth)
		return false;
	const size_t colon = url.find(':', auth);
	if (colon == std::string::npos || colon > at)
		return false;
	secret = url.substr(colon + 1, at - colon - 1);
	bare = url.substr(0, colon) + url.substr(at);
	size_t host_end = url.find('/', at);
	key = url.substr(0, colon) + url.substr(at, (host_end == std::string::npos ? url.size() : host_end) - at);
	return true;
}

// the key of an address without a password (scheme://user@host), "" when it has no user or has a password
std::string KeyOf(const std::string& url)
{
	const size_t sch = url.find("://");
	if (sch == std::string::npos)
		return "";
	const size_t auth = sch + 3;
	size_t end = url.find('/', auth);
	if (end == std::string::npos)
		end = url.size();
	const size_t at = url.rfind('@', end);
	if (at == std::string::npos || at < auth || url.find(':', auth) < at)
		return "";
	return url.substr(0, end);
}

std::string Bare(const std::string& url, std::map<std::string, std::string>* store)
{
	std::string key, secret, bare;
	if (!SplitPassword(url, key, secret, bare))
		return url;
	if (store && !secret.empty())
		(*store)[key] = secret;
	return bare;
}

std::string WithPassword(const std::string& url, const std::map<std::string, std::string>& store)
{
	const std::string key = KeyOf(url);
	if (key.empty())
		return url;
	auto it = store.find(key);
	if (it == store.end())
		return url;
	const size_t at = url.rfind('@', key.size());
	return url.substr(0, at) + ":" + it->second + url.substr(at);
}

std::map<std::string, std::string> ReadCredentials()
{
	std::map<std::string, std::string> m;
	FILE* f = fopen(CredentialsPath().c_str(), "r");
	if (!f)
		return m;
	char line[4096];
	while (fgets(line, sizeof(line), f))
	{
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
			l.pop_back();
		const size_t tab = l.find('\t');
		if (tab != std::string::npos && l[0] != ';')
			m[l.substr(0, tab)] = l.substr(tab + 1);
	}
	fclose(f);
	return m;
}

// credentials.ini replaced as a whole (a temporary file, synced, then renamed over it), or removed when there is
// nothing to keep. False, and the old file left as it was, when any step fails.
bool WriteCredentials(const std::map<std::string, std::string>& m)
{
	const std::string path = CredentialsPath();
	if (m.empty())
		return unlink(path.c_str()) == 0 || errno == ENOENT;
	const std::string tmp = path + ".tmp";
	const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return false;
	fchmod(fd, 0600);
	FILE* f = fdopen(fd, "w");
	if (!f)
	{
		close(fd);
		unlink(tmp.c_str());
		return false;
	}
	bool ok = fprintf(f, "; Media Player Classic PS5: the passwords of the network addresses in mpc-hc.ini "
						 "(address<TAB>password)\n") > 0;
	for (const auto& kv : m)
		ok = ok && fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str()) > 0;
	ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	ok = ok && !FailForTest("credentials") && rename(tmp.c_str(), path.c_str()) == 0;
	if (!ok)
		unlink(tmp.c_str());
	return ok;
}
} // namespace

std::string WithStoredPassword(const std::string& url)
{
	std::string key, secret, bare;
	if (SplitPassword(url, key, secret, bare))
		return url; // it has its own
	return WithPassword(url, ReadCredentials());
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
	// the passwords: back into the addresses from credentials.ini; an mpc-hc.ini of 1.1.1 or before still has them
	// in the addresses, so it's written again at once without them
	bool inline_secret = false;
	auto seen = [&inline_secret](const std::string& u) {
		std::string k, sec, b;
		if (SplitPassword(u, k, sec, b) && !sec.empty())
			inline_secret = true;
	};
	const auto store = ReadCredentials();
	auto fix = [&](std::string& u) {
		seen(u);
		u = WithPassword(u, store);
	};
	for (std::string& u : recent)
		fix(u);
	for (FilePosition& p : positions)
		fix(p.url);
	for (Favorite& fv : favorites)
		fix(fv.url);
	for (NetServer& sv : servers)
		fix(sv.url);
	fix(last_dir);
	if (inline_secret)
	{
		OrbisLog("[settings] passwords moved from mpc-hc.ini to credentials.ini");
		Save();
	}
}

void Settings::Save() const
{
	const std::string path = SettingsPath();
	const std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return;
	std::map<std::string, std::string> store; // the passwords the addresses below carry
	auto bare = [&store](const std::string& u) { return Escape(Bare(u, &store)); };
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
		debug_logs ? 1 : 0, bare(last_dir).c_str());
	fprintf(f, "[Recent File List]\n");
	for (size_t i = 0; i < recent.size(); i++)
		fprintf(f, "File%zu=%s\n", i + 1, bare(recent[i]).c_str());
	fprintf(f, "[Positions]\n");
	for (size_t i = 0; i < positions.size(); i++)
		fprintf(f, "FilePos%zu=%s|%.3f\n", i, bare(positions[i].url).c_str(), positions[i].pos);
	fprintf(f, "[Favorites]\n");
	for (size_t i = 0; i < favorites.size(); i++)
		fprintf(f, "Favorite%zu=%s|%s|%.3f\n", i, Escape(favorites[i].name).c_str(), bare(favorites[i].url).c_str(),
			favorites[i].pos);
	fprintf(f, "[Network]\n");
	for (size_t i = 0; i < servers.size(); i++)
		fprintf(f, "Server%zu=%s|%s\n", i, Escape(servers[i].name).c_str(), bare(servers[i].url).c_str());
	// (not fsync'd: Save runs on the window's thread, when a file opens; a cut-off write leaves the old file, as
	// the rename comes last)
	bool ok = fflush(f) == 0 && !ferror(f);
	ok = fclose(f) == 0 && ok;
	if (!ok)
	{
		unlink(tmp.c_str());
		OrbisLog("[settings] couldn't write %s: the settings on the disk are left as they were", tmp.c_str());
		return;
	}
	// The two files change together without a password ever being only in memory:
	// 1. credentials.ini gets the passwords the new mpc-hc.ini needs, plus the ones it has now (the old mpc-hc.ini
	//    may still need those); if that fails, mpc-hc.ini isn't replaced (the old one, and the old passwords, stay);
	// 2. mpc-hc.ini is replaced;
	// 3. credentials.ini is cut down to what the new mpc-hc.ini needs (if that fails, a few spare passwords stay).
	// (credentials.ini is only written when its passwords change: most saves don't touch it)
	const std::map<std::string, std::string> had = ReadCredentials();
	std::map<std::string, std::string> both = had;
	for (const auto& kv : store)
		both[kv.first] = kv.second;
	if (both != had && !WriteCredentials(both))
	{
		unlink(tmp.c_str());
		OrbisLog("[settings] couldn't write credentials.ini: mpc-hc.ini left as it was (no password lost)");
		return;
	}
	if (FailForTest("settings") || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		OrbisLog("[settings] couldn't replace %s (errno %d): left as it was", path.c_str(), errno);
		return;
	}
	if (both != store && !WriteCredentials(store))
		OrbisLog("[settings] couldn't trim credentials.ini: it keeps passwords no longer used");
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
