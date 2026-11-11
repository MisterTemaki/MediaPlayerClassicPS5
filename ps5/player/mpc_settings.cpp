// Media Player Classic PS5: the settings (mpc_settings.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_settings.h"

#include "mpc_i18n.h"
#include "mpc_credentials.h"

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

// Legacy plaintext files are read only for migration into the encrypted credential storage.
namespace
{
// Host tests: MPCPS5_HOST_SETTINGS_FAIL=settings preserves the previous settings file.
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
	return credentials::Split(url, key, secret, bare);
}

struct Secrets : std::map<std::string, std::string>
{
	~Secrets() { for (auto& entry : *this) credentials::Wipe(entry.second); }
};
std::string Bare(const std::string& url, std::map<std::string, std::string>* store)
{
	std::string key, secret, bare;
	if (!SplitPassword(url, key, secret, bare)) return url;
	if (store && !secret.empty()) store->emplace(key, secret);
	credentials::Wipe(secret);
	return bare;
}

// A credentials file: its passwords, and its generation ("; generation N", the first line; 0 in 1.2.1's files).
// exists: whether the file was there.
bool legacy_read_failed = false;
std::map<std::string, std::string> ReadCredentials(const std::string& path, int* gen = nullptr, bool* exists = nullptr)
{
	Secrets values;
	if (gen) *gen = 0;
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	if (exists) *exists = fd >= 0;
	if (fd < 0)
	{
		if (errno != ENOENT) legacy_read_failed = true;
		return {};
	}
	struct stat info = {};
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 1024 * 1024)
	{ close(fd); legacy_read_failed = true; return {}; }
	std::string bytes(static_cast<size_t>(info.st_size), '\0');
	size_t done = 0;
	while (done < bytes.size())
	{
		const ssize_t count = read(fd, &bytes[0] + done, bytes.size() - done);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) break;
		done += static_cast<size_t>(count);
	}
	close(fd);
	bool valid = done == bytes.size();
	for (size_t at = 0; valid && at < bytes.size();)
	{
		const size_t end = bytes.find('\n', at);
		std::string text = bytes.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = end == std::string::npos ? bytes.size() : end + 1;
		if (!text.empty() && text.back() == '\r') text.pop_back();
		if (text.size() > 8193 || text.find('\0') != std::string::npos) valid = false;
		else if (text.compare(0, 13, "; generation ") == 0)
		{ if (gen) *gen = atoi(text.c_str() + 13); }
		else if (!text.empty() && text.front() != ';')
		{
			const size_t tab = text.find('\t');
			if (tab == std::string::npos || tab == 0 || values.size() >= 256) valid = false;
			else values[text.substr(0, tab)] = text.substr(tab + 1);
		}
		credentials::Wipe(text);
	}
	credentials::Wipe(bytes);
	if (!valid) { legacy_read_failed = true; return {}; }
	std::map<std::string, std::string> result;
	result.swap(values);
	return result;
}

void SyncFolder(const std::string& dir)
{
	const int fd = open(dir.c_str(), O_RDONLY);
	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
}
} // namespace

std::string WithStoredPassword(const std::string& url)
{
	return credentials::Get().Resolve(url);
}

namespace
{
struct PadButtonInfo
{
	const char* name; // the Keys page
	const char* short_name; // beside a menu item
	const char* ini; // mpc-hc.ini: Key<ini>=
	PadAction def;
};
const PadButtonInfo kPadButtons[int(PadButton::Count)] = {
	{"Cross", "Cross", "Cross", PadAction::PlayPause},
	{"Circle", "Circle", "Circle", PadAction::LeaveFullScreen},
	{"Triangle", "Triangle", "Triangle", PadAction::FullScreen},
	{"Square", "Square", "Square", PadAction::ContextMenu},
	{"D-pad Left", "Left", "Left", PadAction::JumpBackMedium},
	{"D-pad Right", "Right", "Right", PadAction::JumpForwardMedium},
	{"D-pad Up", "Up", "Up", PadAction::VolumeUp},
	{"D-pad Down", "Down", "Down", PadAction::VolumeDown},
	{"L1", "L1", "L1", PadAction::Previous},
	{"R1", "R1", "R1", PadAction::Next},
	{"L2", "L2", "L2", PadAction::JumpBackLarge},
	{"R2", "R2", "R2", PadAction::JumpForwardLarge},
	{"L3", "L3", "L3", PadAction::GoTo},
	{"R3", "R3", "R3", PadAction::Mute},
	{"Options", "Options", "Options", PadAction::MenuBar},
	{"Right stick up", "Right stick up", "StickUp", PadAction::VolumeUp},
	{"Right stick down", "Right stick down", "StickDown", PadAction::VolumeDown},
	{"Right stick left", "Right stick left", "StickLeft", PadAction::JumpBackSmall},
	{"Right stick right", "Right stick right", "StickRight", PadAction::JumpForwardSmall},
};
const char* const kPadActionKeys[int(PadAction::Count)] = {"None", "PlayPause", "Stop", "JumpBack", "JumpForward",
	"JumpBackLarge", "JumpForwardLarge", "JumpBackSmall", "JumpForwardSmall", "Previous", "Next", "VolumeUp", "VolumeDown",
	"Mute", "FullScreen", "LeaveFullScreen", "GoTo", "ContextMenu", "MenuBar", "SubDelayDown", "SubDelayUp",
	"NextSubtitle", "NextAudio", "AudioDelayDown", "AudioDelayUp", "Statistics", "Faster", "Slower", "NormalSpeed",
	"FrameStep", "OpenFile", "Playlist", "NextAspectRatio", "Options", "OptionsPlayer", "OptionsKeys", "OptionsPlayback",
	"OptionsOutput", "OptionsFullscreen", "OptionsSubtitles", "OptionsDefaultStyle", "OptionsAudio", "OptionsMisc"};

// RRGGBB or #RRGGBB (as the Default Style page shows a colour not in its palette)
uint32_t ParseColor(std::string v, uint32_t def)
{
	if (!v.empty() && v[0] == '#')
		v.erase(0, 1);
	if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
		return def;
	return uint32_t(strtoul(v.c_str(), nullptr, 16));
}
} // namespace

PadAction DefaultPadAction(PadButton b)
{
	return int(b) >= 0 && b < PadButton::Count ? kPadButtons[int(b)].def : PadAction::None;
}

const char* PadButtonName(PadButton b)
{
	return int(b) >= 0 && b < PadButton::Count ? kPadButtons[int(b)].name : "";
}

const char* PadButtonShortName(PadButton b)
{
	return int(b) >= 0 && b < PadButton::Count ? kPadButtons[int(b)].short_name : "";
}

const char* PadActionKey(PadAction a)
{
	return int(a) >= 0 && a < PadAction::Count ? kPadActionKeys[int(a)] : "None";
}

std::string PadActionText(PadAction a)
{
	const Settings& s = AppSettings();
	switch (a)
	{
		case PadAction::None:
			return T("(none)");
		case PadAction::PlayPause:
			return T("Play/Pause");
		case PadAction::Stop:
			return T("Stop");
		case PadAction::JumpBackMedium:
			return Fmt(T("Jump back %d s"), s.jump_medium);
		case PadAction::JumpForwardMedium:
			return Fmt(T("Jump forward %d s"), s.jump_medium);
		case PadAction::JumpBackLarge:
			return Fmt(T("Jump back %d s"), s.jump_large);
		case PadAction::JumpForwardLarge:
			return Fmt(T("Jump forward %d s"), s.jump_large);
		case PadAction::JumpBackSmall:
			return Fmt(T("Jump back %d s"), s.jump_small);
		case PadAction::JumpForwardSmall:
			return Fmt(T("Jump forward %d s"), s.jump_small);
		case PadAction::Previous:
			return T("Previous chapter or file");
		case PadAction::Next:
			return T("Next chapter or file");
		case PadAction::VolumeUp:
			return T("Volume up");
		case PadAction::VolumeDown:
			return T("Volume down");
		case PadAction::Mute:
			return T("Mute");
		case PadAction::FullScreen:
			return T("Full screen on/off");
		case PadAction::LeaveFullScreen:
			return T("Leave full screen");
		case PadAction::GoTo:
			return T("Go To...");
		case PadAction::ContextMenu:
			return T("The context menu");
		case PadAction::MenuBar:
			return T("The menu bar");
		case PadAction::SubDelayDown:
			return Fmt(T("Subtitle delay -%d ms"), s.sub_delay_step);
		case PadAction::SubDelayUp:
			return Fmt(T("Subtitle delay +%d ms"), s.sub_delay_step);
		case PadAction::NextSubtitle:
			return T("Next subtitle track (and off)");
		case PadAction::NextAudio:
			return T("Next audio track");
		case PadAction::AudioDelayDown:
			return T("Audio delay -10 ms");
		case PadAction::AudioDelayUp:
			return T("Audio delay +10 ms");
		case PadAction::Statistics:
			return T("Display statistics");
		case PadAction::Faster:
			return T("Faster (Increase Rate)");
		case PadAction::Slower:
			return T("Slower (Decrease Rate)");
		case PadAction::NormalSpeed:
			return T("Normal speed (Reset Rate)");
		case PadAction::FrameStep:
			return T("Frame Step");
		case PadAction::OpenFile:
			return T("Open File...");
		case PadAction::Playlist:
			return T("Playlist on/off");
		case PadAction::NextAspectRatio:
			return T("Next aspect ratio");
		case PadAction::OptionsWindow:
			return T("Options...");
		case PadAction::OptionsPlayer:
		case PadAction::OptionsKeys:
		case PadAction::OptionsPlayback:
		case PadAction::OptionsOutput:
		case PadAction::OptionsFullscreen:
		case PadAction::OptionsSubtitles:
		case PadAction::OptionsDefaultStyle:
		case PadAction::OptionsAudio:
		case PadAction::OptionsMisc:
		{
			static const char* const kPages[] = {"Player", "Keys", "Playback", "Output", "Fullscreen", "Subtitles",
				"Default Style", "Audio Switcher", "Miscellaneous"};
			return Fmt(T("Options: %s"), T(kPages[int(a) - int(PadAction::OptionsPlayer)]));
		}
		default:
			return "";
	}
}

bool PadActionRepeats(PadAction a)
{
	switch (a)
	{
		case PadAction::JumpBackMedium:
		case PadAction::JumpForwardMedium:
		case PadAction::JumpBackLarge:
		case PadAction::JumpForwardLarge:
		case PadAction::JumpBackSmall:
		case PadAction::JumpForwardSmall:
		case PadAction::Previous:
		case PadAction::Next:
		case PadAction::VolumeUp:
		case PadAction::VolumeDown:
		case PadAction::SubDelayDown:
		case PadAction::SubDelayUp:
		case PadAction::AudioDelayDown:
		case PadAction::AudioDelayUp:
		case PadAction::FrameStep:
			return true;
		default:
			return false;
	}
}

Settings::Settings()
{
	// debug logs: off by default; the host tests turn the default on (orbis_paths.cpp reads the same variable)
	if (const char* d = getenv("MPCPS5_LOGS_DEFAULT"))
		debug_logs = atoi(d) != 0;
	for (int i = 0; i < int(PadButton::Count); i++)
		pad[i] = DefaultPadAction(PadButton(i));
}

void Settings::Load()
{
	legacy_read_failed = false;
	// off by default; the host tests turn the default on (orbis_paths.cpp reads the same variable)
	if (const char* d = getenv("MPCPS5_LOGS_DEFAULT"))
		debug_logs = atoi(d) != 0;
	FILE* f = fopen(SettingsPath().c_str(), "r");
	if (!f)
		return;
	int cred_gen = 0; // [Credentials] Generation: the credentials file this mpc-hc.ini goes with
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
		else if (k == "DarkMode")
			dark_mode = v == "1";
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
		else if (k == "HideFullscreenControlsPolicy") // 1.5.3: missing in older files: when the pointer moves
			fs_controls_show = n == kShowWhenHovered ? kShowWhenHovered : kShowWhenMoved;
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
		else if (k == "AudioNormalize")
			audio_normalize = b;
		else if (k == "AudioMaxNormFactor")
			audio_max_norm = Clamp(n, 100, 1000);
		else if (k == "AudioNormalizeRecover")
			audio_norm_regain = b;
		else if (k == "AudioDelay")
			audio_delay_ms = Clamp(n, -10000, 10000);
		else if (k == "AudioOutput")
			audio_output = Clamp(n, 0, 1);
		else if (k == "HardwareDecoding")
			hw_decoding = b;
		else if (k == "DecodeMode")
			decode_mode = Clamp(n, 0, 2);
		else if (k == "HdrToneMapping")
			hdr_tonemap = Clamp(n, 0, 3);
		else if (k == "HdrPeak")
			hdr_peak = Clamp(n, 100, 400);
		else if (k == "HdrOutput")
			hdr_output = b;
		else if (k == "VideoRenderer")
			video_renderer = Clamp(n, 0, 1);
		else if (k == "GpuStart")
			gpu_start = Clamp(n, 0, 1);
		else if (k == "VideoResizer")
			video_resizer = Clamp(n, 0, 1);
		else if (k == "SubFontSize")
			sub_font_size = Clamp(n, 8, 72);
		else if (k == "SubBold")
			sub_bold = b;
		else if (k == "SubOutline")
			sub_outline = Clamp(n, 0, 8);
		else if (k == "SubMarginBottom") // (1.2.3's SubMargin did nothing: left alone)
			sub_margin = Clamp(n, 0, 200);
		else if (k == "SubFont")
			sub_font = Unescape(v);
		else if (k == "SubBorderStyle")
			sub_border = Clamp(n, 0, 1);
		else if (k == "SubShadow")
			sub_shadow = Clamp(n, 0, 8);
		else if (k == "SubColor")
			sub_color = ParseColor(v, sub_color);
		else if (k == "SubOutlineColor")
			sub_outline_color = ParseColor(v, sub_outline_color);
		else if (k == "SubBoxOpacity")
			sub_box_opacity = Clamp(n, 10, 100);
		else if (k == "SubDelayInterval")
			sub_delay_step = Clamp(n, 10, 10000);
		else if (k.compare(0, 3, "Key") == 0 && k.size() > 3)
		{
			for (int i = 0; i < int(PadButton::Count); i++)
				if (k.compare(3, std::string::npos, kPadButtons[i].ini) == 0)
					for (int a = 0; a < int(PadAction::Count); a++)
						if (v == kPadActionKeys[a])
							pad[i] = PadAction(a);
		}
		else if (k == "SubPosition")
			sub_position = Clamp(n, 0, 1);
		else if (k == "SubDelay")
			sub_delay_ms = Clamp(n, -60000, 60000);
		else if (k == "Dpi")
			dpi = n == 96 || n == 120 || n == 144 || n == 168 || n == 192 ? n : 144;
		else if (k == "CursorSpeed")
			cursor_speed = Clamp(n, 1, 5);
		else if (k == "TouchpadPointer")
			touchpad_pointer = b;
		else if (k == "OptionsX")
			options_x = Clamp(n, -1, 1919);
		else if (k == "OptionsY")
			options_y = Clamp(n, -1, 1079);
		else if (k == "Language")
			language = Clamp(n, 0, 4);
		else if (k == "Generation")
			cred_gen = n;
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
	// Do not put credentials back into UI/model addresses. Preserve legacy files until the user
	// has committed a verified encrypted copy.
	Secrets legacy;
	auto strip = [&](std::string& url) {
		std::string clean = Bare(url, &legacy);
		credentials::Wipe(url);
		url = std::move(clean);
	};
	for (auto& server : servers) { strip(server.url); strip(server.name); }
	for (auto& url : recent) strip(url);
	for (auto& favorite : favorites) { strip(favorite.url); strip(favorite.name); }
	for (auto& position : positions) strip(position.url);
	strip(last_dir);
	int main_generation = 0, new_generation = 0;
	bool has_new = false;
	Secrets old, pending;
	static_cast<std::map<std::string, std::string>&>(old) = ReadCredentials(CredentialsPath(), &main_generation);
	static_cast<std::map<std::string, std::string>&>(pending) = ReadCredentials(CredentialsPath() + ".new", &new_generation, &has_new);
	const auto& selected = has_new && new_generation == cred_gen && main_generation != cred_gen ? pending : old;
	for (const auto& entry : selected) legacy.emplace(entry);
	credentials::Get().ImportLegacy(legacy);
}

bool Settings::Save() const
{
	if (!credentials::Get().Pending())
	{
		// A caller may save a fresh Settings object without Load. Do not discard an existing legacy file.
		for (const auto& file : {CredentialsPath(), CredentialsPath() + ".new", CredentialsPath() + ".tmp"})
		{
			Secrets legacy;
			static_cast<std::map<std::string, std::string>&>(legacy) = ReadCredentials(file);
			credentials::Get().ImportLegacy(legacy);
		}
	}
	if (legacy_read_failed)
	{
		OrbisLog("[settings] legacy credentials could not be read; all original files were kept");
		return false;
	}

	Secrets store;
	for (const auto& server : servers) { Bare(server.url, &store); Bare(server.name, &store); }
	for (const auto& url : recent) Bare(url, &store);
	for (const auto& favorite : favorites) { Bare(favorite.url, &store); Bare(favorite.name, &store); }
	for (const auto& position : positions) Bare(position.url, &store);
	Bare(last_dir, &store);
	credentials::Get().Stage(store);
	std::string error;
	if (!credentials::Get().Commit(error))
	{
		OrbisLog("[settings] %s Settings and legacy credentials were kept.", error.c_str());
		return false;
	}
	const std::string path = SettingsPath(), tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f) return false;
	auto bare = [](const std::string& u) { return Escape(Bare(u, nullptr)); };
	fprintf(f, "; Media Player Classic PS5 settings (MPC-HC's names where it has one)\n[Settings]\n");
	fprintf(f, "DarkMode=%d\n", dark_mode ? 1 : 0);
	fprintf(f, "RememberFilePos=%d\nTitleBarTextStyle=%d\nShowOSD=%d\nKeepHistory=%d\n", remember_file_pos, title_bar,
		show_osd, keep_history);
	fprintf(f, "ShowSeekBar=%d\nShowControls=%d\nShowStatus=%d\nShowInfo=%d\nShowStatistics=%d\nShowPlaylist=%d\n",
		show_seekbar, show_controls, show_status, show_info, show_stats, show_playlist);
	fprintf(f, "ShowCaptionMenu=%d\nFullScreen=%d\nHideFullscreenControls=%d\nHideFullscreenControlsPolicy=%d\nVideoFrame=%d\n"
			   "AspectRatio=%d\n",
		show_caption_menu, fullscreen, hide_controls_fullscreen, fs_controls_show, int(video_frame), aspect_ratio);
	fprintf(f, "Volume=%d\nMute=%d\nLoop=%d\nAfterPlayback=%d\nJumpDistS=%d\nJumpDistM=%d\nJumpDistL=%d\n", volume, mute,
		repeat, int(after_playback), jump_small, jump_medium, jump_large);
	fprintf(f, "AutoloadSubtitles=%d\nAudioBoost=%d\nAudioDelay=%d\nAudioOutput=%d\n", auto_load_subtitles, audio_boost,
		audio_delay_ms, audio_output);
	fprintf(f, "AudioNormalize=%d\nAudioMaxNormFactor=%d\nAudioNormalizeRecover=%d\n", audio_normalize, audio_max_norm,
		audio_norm_regain);
	fprintf(f, "HardwareDecoding=%d\nHdrToneMapping=%d\nHdrPeak=%d\nDecodeMode=%d\n", hw_decoding, hdr_tonemap, hdr_peak,
		decode_mode);
	fprintf(f, "VideoRenderer=%d\nVideoResizer=%d\nGpuStart=%d\n", video_renderer, video_resizer, gpu_start);
	fprintf(f, "HdrOutput=%d\n", hdr_output ? 1 : 0);
	fprintf(f, "SubFont=%s\nSubFontSize=%d\nSubBold=%d\nSubBorderStyle=%d\nSubOutline=%d\nSubShadow=%d\n",
		Escape(sub_font).c_str(), sub_font_size, sub_bold, sub_border, sub_outline, sub_shadow);
	fprintf(f, "SubColor=%06X\nSubOutlineColor=%06X\nSubBoxOpacity=%d\nSubMarginBottom=%d\nSubPosition=%d\nSubDelay=%d\n",
		unsigned(sub_color & 0xffffff), unsigned(sub_outline_color & 0xffffff), sub_box_opacity, sub_margin, sub_position,
		sub_delay_ms);
	fprintf(f, "SubDelayInterval=%d\n", sub_delay_step);
	fprintf(f, "Dpi=%d\nCursorSpeed=%d\nLanguage=%d\ndebug_logs=%d\nLastDir=%s\n", dpi, cursor_speed, language,
		debug_logs ? 1 : 0, bare(last_dir).c_str());
	fprintf(f, "TouchpadPointer=%d\nOptionsX=%d\nOptionsY=%d\n", touchpad_pointer ? 1 : 0, options_x, options_y);
	fprintf(f, "[Keys]\n");
	for (int i = 0; i < int(PadButton::Count); i++)
		fprintf(f, "Key%s=%s\n", kPadButtons[i].ini, PadActionKey(pad[i]));
	fprintf(f, "[Recent File List]\n");
	for (size_t i = 0; i < recent.size(); i++)
		fprintf(f, "File%zu=%s\n", i + 1, bare(recent[i]).c_str());
	fprintf(f, "[Positions]\n");
	for (size_t i = 0; i < positions.size(); i++)
		fprintf(f, "FilePos%zu=%s|%.3f\n", i, bare(positions[i].url).c_str(), positions[i].pos);
	fprintf(f, "[Favorites]\n");
	for (size_t i = 0; i < favorites.size(); i++)
		fprintf(f, "Favorite%zu=%s|%s|%.3f\n", i, bare(favorites[i].name).c_str(), bare(favorites[i].url).c_str(),
			favorites[i].pos);
	fprintf(f, "[Network]\n");
	for (size_t i = 0; i < servers.size(); i++)
		fprintf(f, "Server%zu=%s|%s\n", i, bare(servers[i].name).c_str(), bare(servers[i].url).c_str());
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && !ferror(f);
	ok = fclose(f) == 0 && ok;
	if (!ok || FailForTest("settings") || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		OrbisLog("[settings] settings could not be saved; legacy credentials were kept");
		return false;
	}
	SyncFolder(OrbisRoot());
	std::vector<std::string> referenced = recent;
	for (const auto& server : servers) referenced.push_back(server.url);
	for (const auto& favorite : favorites) referenced.push_back(favorite.url);
	for (const auto& position : positions) referenced.push_back(position.url);
	referenced.push_back(last_dir);
	if (!credentials::Get().Retain(referenced, error))
	{
		OrbisLog("[settings] %s Legacy credentials were kept.", error.c_str());
		return false;
	}
	// Remove old plaintext only after verified encryption and password-free settings are committed.
	bool cleaned = true;
	for (const auto& legacy : {CredentialsPath(), CredentialsPath() + ".new", CredentialsPath() + ".tmp"})
	{
		Secrets remaining;
		static_cast<std::map<std::string, std::string>&>(remaining) = ReadCredentials(legacy);
		if (legacy_read_failed) { cleaned = false; continue; }
		if (unlink(legacy.c_str()) != 0 && errno != ENOENT)
		{
			OrbisLog("[settings] a legacy credential file could not be removed");
			cleaned = false;
		}
	}
	SyncFolder(OrbisRoot());
	return cleaned;
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
