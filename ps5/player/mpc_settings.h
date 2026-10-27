// Media Player Classic PS5: the settings (CAppSettings), kept in <data>/mpc-hc.ini as key=value lines --
// MPC-HC's "Store settings to .ini file" mode, with its own key names where there is one.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
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

// Options > Player > Keys: the controller's buttons in the window (no pointer showing, no menu or dialog open) and
// the action each one does. Options (the menu bar), the pointer's Cross / Square clicks and the menus' and dialogs'
// buttons stay as they are.
enum class PadButton : int
{
	Cross,
	Circle,
	Triangle,
	Square,
	Left, // the D-pad
	Right,
	Up,
	Down,
	L1,
	R1,
	L2,
	R2,
	L3,
	R3,
	Options, // 1.5: bindable; given another action, held for half a second it still opens the menu bar
	StickUp, // the right stick (the mouse wheel)
	StickDown,
	StickLeft, // 1.5: pushed left / right, as the D-pad (the pointer is the left stick)
	StickRight,
	Count
};

enum class PadAction : int
{
	None,
	PlayPause,
	Stop,
	JumpBackMedium, // Settings::jump_medium (5 s)
	JumpForwardMedium,
	JumpBackLarge, // jump_large (20 s)
	JumpForwardLarge,
	JumpBackSmall, // jump_small (1 s)
	JumpForwardSmall,
	Previous, // Navigate > Previous: the chapter, then the file
	Next,
	VolumeUp,
	VolumeDown,
	Mute,
	FullScreen, // on/off
	LeaveFullScreen,
	GoTo,
	ContextMenu,
	MenuBar,
	SubDelayDown, // by Options > Subtitles' delay interval
	SubDelayUp,
	NextSubtitle, // the next track, then off, then the first
	NextAudio,
	AudioDelayDown, // 10 ms
	AudioDelayUp,
	Statistics, // View > Renderer Settings > Display Statistics
	// 1.5
	Faster, // Play > Increase Rate (MPC-HC: twice as fast)
	Slower, // Play > Decrease Rate (half)
	NormalSpeed, // Play > Reset Rate
	FrameStep, // Play > Frame Step
	OpenFile, // File > Open File...
	Playlist, // View > Playlist on/off
	NextAspectRatio, // View > Aspect Ratio: the next one
	OptionsWindow, // View > Options... (the page it was last on)
	OptionsPlayer, // View > Options... at a page
	OptionsKeys,
	OptionsPlayback,
	OptionsOutput,
	OptionsFullscreen,
	OptionsSubtitles,
	OptionsDefaultStyle,
	OptionsAudio,
	OptionsMisc,
	Count
};

// Options' pages, in the tree's order (ShowOptionsDialog's page)
enum class OptionsPage : int
{
	Last = -1, // the one it was on (MPC-HC opens where it was left)
	Player,
	Keys,
	Playback,
	Output,
	Fullscreen,
	Subtitles,
	DefaultStyle,
	AudioSwitcher,
	Miscellaneous,
	Count
};

// today's mapping (the README's table)
PadAction DefaultPadAction(PadButton b);
// the names in Options > Player > Keys ("D-pad Left"), in English (T() them)
const char* PadButtonName(PadButton b);
// the short name beside a menu item ("Left")
const char* PadButtonShortName(PadButton b);
// the action's name in mpc-hc.ini ("PlayPause")
const char* PadActionKey(PadAction a);
// the action as the Keys page shows it, in the current language ("Jump back 5 s")
std::string PadActionText(PadAction a);
// whether holding the button repeats it (jumps, volume, chapters, delays)
bool PadActionRepeats(PadAction a);

// An address with a user and no password (smb://user@server/...): the one kept in credentials.ini for that user
// and server put back in; else the address as it is.
std::string WithStoredPassword(const std::string& url);

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
	// Options > Audio Switcher "Normalize", "Max amplification" (percent, 100..1000), "Regain volume": MPC-HC's defaults
	bool audio_normalize = false;
	int audio_max_norm = 400;
	bool audio_norm_regain = true;
	int audio_delay_ms = 0;
	int audio_output = 0; // 0 stereo (5.1/7.1 mixed down), 1 surround: the console's 8-channel output
	// Output (video)
	bool hw_decoding = true; // H.264 / HEVC on the console's video decoder (mpc_vdec.h)
	int hdr_tonemap = 1;     // HDR10 / HLG on an SDR picture: 0 off (as it comes), 1 BT.2390, 2 Hable, 3 Reinhard
	int hdr_peak = 250;      // the picture's white after tone mapping, in nits (100..400)
	// Subtitles (Default Style): the text formats other than ASS/SSA; sizes in a 288-line script's pixels, as
	// VSFilter scales SRT text with the picture
	std::string sub_font; // a file in <data>/fonts ("name.ttf", "name.ttc#1"); "" Selawik, "Roboto" the app's own
	int sub_font_size = 18; // points at the video's height of 1080 / 1.5 (MPC-HC's default text style: Arial 18)
	bool sub_bold = true;
	int sub_border = 0; // Border Style: 0 outline, 1 opaque box
	int sub_outline = 2; // the outline's width (the box's border)
	int sub_shadow = 3; // the shadow's depth (half a script pixel per step; 0: none)
	uint32_t sub_color = 0xffffff; // the text's colour, 0xRRGGBB
	uint32_t sub_outline_color = 0x000000; // the outline's, or the box's
	int sub_box_opacity = 75; // percent
	int sub_margin = 20; // the bottom (or top) margin (VSFilter's default: 20)
	int sub_position = 0; // 0 in the video, 1 at the bottom of the window
	int sub_delay_ms = 0;
	int sub_delay_step = 100; // Options > Subtitles "Delay interval": Play > Subtitle Track's and the keys' step
	// Options > Player > Keys
	PadAction pad[int(PadButton::Count)];
	// Interface
	int dpi = 144; // 96 / 120 / 144 / 192 (100 % .. 200 %)
	int cursor_speed = 3; // 1..5
	bool touchpad_pointer = true; // 1.5: the DualSense touchpad moves the pointer, its click clicks (Options > Player > Keys)
	int options_x = -1, options_y = -1; // 1.5: where the Options window was moved to (-1: centred)
	int language = 0; // Options > Player > Language: 0 English, 1 Spanish, 2 Portuguese (Brazil), 3 French, 4 German
	// boot.log and the others in <data>/logs: off unless mpc-hc.ini says debug_logs=1 (orbis_paths.cpp reads the
	// same line: the installer and the helper follow it); Options > Miscellaneous "Debug logs", applied at once
	bool debug_logs = false;
	// Lists
	std::vector<std::string> recent; // MRU, newest first
	std::vector<FilePosition> positions;
	std::vector<Favorite> favorites;
	std::vector<NetServer> servers;
	std::string last_dir;

	Settings();
	void Load();
	void Save() const;
	void AddRecent(const std::string& url);
	double FilePos(const std::string& url) const;
	void SetFilePos(const std::string& url, double pos);
};

Settings& AppSettings();
std::string SettingsPath();
} // namespace mpc
