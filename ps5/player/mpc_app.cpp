// Media Player Classic PS5: the player's window (mpc_app.h) -- MPC-HC's CMainFrame: the title bar and the menu
// bar of a maximized Windows 10 window, the video, the seek bar (CPlayerSeekBar), the toolbar with the volume
// slider (CPlayerToolBar, CVolumeCtrl), the information and statistics bars (CPlayerInfoBar), the status bar
// (CPlayerStatusBar), the playlist (CPlayerPlaylistBar), the OSD (CVMROSD), full screen with its controls
// shown when the pointer moves, and the commands of MPC-HC's menus.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_app.h"
#include "mpc_tonemap.h"

#include "mpc_dialogs.h"
#include "mpc_disc.h"
#include "mpc_font.h"
#include "mpc_gdi.h"
#include "mpc_i18n.h"
#include "mpc_menu.h"
#include "mpc_player.h"
#include "mpc_vdec.h"
#include "mpc_res.h"
#include "mpc_settings.h"
#include "mpc_ui.h"
#include "mpc_update.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"
#include "ProsperoUpdate.h"
#include "ProsperoVideo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef MPCPS5_PS5_VERSION
#define MPCPS5_PS5_VERSION "dev"
#endif

MPC_INCBIN(mpc_cacert, MPC_ASSET_DIR "/data/cacert.pem")

namespace mpc
{
namespace
{
// ---- command ids (MPC-HC's resource.h names) ----
enum Cmd
{
	ID_NONE = 0,
	ID_FILE_OPENQUICK = 100,
	ID_FILE_OPENMEDIA,
	ID_FILE_OPENDVDBD,
	ID_FILE_OPENDEVICE,
	ID_FILE_OPENDIRECTORY,
	ID_FILE_OPENDISC,
	ID_FILE_CLOSE_AND_RESTORE,
	ID_FILE_SAVE_COPY,
	ID_FILE_SAVE_IMAGE,
	ID_FILE_SAVE_THUMBNAILS,
	ID_FILE_SUBTITLES_LOAD,
	ID_FILE_SUBTITLES_SAVE,
	ID_FILE_SUBTITLES_DOWNLOAD,
	ID_FILE_SUBTITLES_UPLOAD,
	ID_FILE_PROPERTIES,
	ID_FILE_EXIT,
	ID_RECENT_FILES_CLEAR,
	ID_VIEW_CAPTIONMENU = 200,
	ID_VIEW_SEEKER,
	ID_VIEW_CONTROLS,
	ID_VIEW_INFORMATION,
	ID_VIEW_STATISTICS,
	ID_VIEW_STATUS,
	ID_VIEW_SUBRESYNC,
	ID_VIEW_PLAYLIST,
	ID_VIEW_CAPTURE,
	ID_VIEW_NAVIGATION,
	ID_VIEW_DEBUGSHADERS,
	ID_VIEW_PRESETS_MINIMAL,
	ID_VIEW_PRESETS_COMPACT,
	ID_VIEW_PRESETS_NORMAL,
	ID_VIEW_FULLSCREEN,
	ID_VIEW_ZOOM_50,
	ID_VIEW_ZOOM_100,
	ID_VIEW_ZOOM_200,
	ID_VIEW_ZOOM_AUTOFIT,
	ID_VIEW_ZOOM_AUTOFIT_LARGER,
	ID_VIEW_TEARING_TEST,
	ID_VIEW_DISPLAY_RENDERER_STATS,
	ID_VIEW_OSD_DISPLAY_TIME,
	ID_VIEW_OSD_SHOW_FILENAME,
	ID_VIEW_RENDERER_OTHER, // the Direct3D / EVR settings, greyed
	ID_VIEW_VF_HALF,
	ID_VIEW_VF_NORMAL,
	ID_VIEW_VF_DOUBLE,
	ID_VIEW_VF_STRETCH,
	ID_VIEW_VF_FROMINSIDE,
	ID_VIEW_VF_ZOOM1,
	ID_VIEW_VF_ZOOM2,
	ID_VIEW_VF_FROMOUTSIDE,
	ID_VIEW_VF_COMPMONDESKARDIFF,
	ID_ASPECTRATIO_DAR,
	ID_ASPECTRATIO_4_3,
	ID_ASPECTRATIO_5_4,
	ID_ASPECTRATIO_16_9,
	ID_ASPECTRATIO_235_100,
	ID_ASPECTRATIO_185_100,
	ID_ASPECTRATIO_SAR,
	ID_VIEW_INCSIZE,
	ID_VIEW_DECSIZE,
	ID_VIEW_INCWIDTH,
	ID_VIEW_DECWIDTH,
	ID_VIEW_INCHEIGHT,
	ID_VIEW_DECHEIGHT,
	ID_PANSCAN_MOVERIGHT,
	ID_PANSCAN_MOVELEFT,
	ID_PANSCAN_MOVEUP,
	ID_PANSCAN_MOVEDOWN,
	ID_PANSCAN_CENTER,
	ID_VIEW_RESET,
	ID_ONTOP_DEFAULT,
	ID_ONTOP_ALWAYS,
	ID_ONTOP_WHILEPLAYING,
	ID_ONTOP_WHILEPLAYINGVIDEO,
	ID_VIEW_OPTIONS,
	ID_PLAY_PLAYPAUSE = 300,
	ID_PLAY_PLAY,
	ID_PLAY_PAUSE,
	ID_PLAY_STOP,
	ID_PLAY_FRAMESTEP,
	ID_PLAY_REPEAT_FOREVER,
	ID_PLAY_REPEAT_ONEFILE,
	ID_PLAY_REPEAT_WHOLEPLAYLIST,
	ID_PLAY_DECRATE,
	ID_PLAY_INCRATE,
	ID_PLAY_RESETRATE,
	ID_FILTERS,
	ID_SHADERS,
	ID_VOLUME_UP,
	ID_VOLUME_DOWN,
	ID_VOLUME_MUTE,
	ID_AFTERPLAYBACK_DONOTHING,
	ID_AFTERPLAYBACK_PLAYNEXT,
	ID_AFTERPLAYBACK_MONITOROFF,
	ID_AFTERPLAYBACK_EXIT,
	ID_AFTERPLAYBACK_STANDBY,
	ID_AFTERPLAYBACK_HIBERNATE,
	ID_AFTERPLAYBACK_SHUTDOWN,
	ID_AFTERPLAYBACK_LOGOFF,
	ID_AFTERPLAYBACK_LOCK,
	ID_SUBTITLES_ENABLE,
	ID_SUB_DELAY_DOWN,
	ID_SUB_DELAY_UP,
	ID_SUB_DELAY_RESET,
	ID_AUDIO_DELAY_DOWN,
	ID_AUDIO_DELAY_UP,
	ID_NAVIGATE_SKIPBACK = 400,
	ID_NAVIGATE_SKIPFORWARD,
	ID_NAVIGATE_GOTO,
	ID_NAVIGATE_TITLEMENU,
	ID_NAVIGATE_ROOTMENU,
	ID_NAVIGATE_SUBPICTUREMENU,
	ID_NAVIGATE_AUDIOMENU,
	ID_NAVIGATE_ANGLEMENU,
	ID_NAVIGATE_CHAPTERMENU,
	ID_FAVORITES_ADD = 500,
	ID_FAVORITES_ORGANIZE,
	ID_HELP_HOMEPAGE = 600,
	ID_HELP_CHECKFORUPDATE,
	ID_HELP_SHOWCOMMANDLINESWITCHES,
	ID_HELP_TOOLBARIMAGES,
	ID_HELP_DONATE,
	ID_HELP_ABOUT,
	ID_PLAYLIST_REMOVE = 700,
	ID_PLAYLIST_CLEAR,
	// dynamic ranges
	ID_RECENT_FIRST = 1000, // + n
	ID_AUDIO_FIRST = 2000, // + stream id
	ID_SUBTITLE_FIRST = 3000, // + track id (external: 1000+n)
	ID_SUBTITLE_OFF = 2999,
	ID_VIDEO_FIRST = 5000,
	ID_CHAPTER_FIRST = 6000,
	ID_FAVORITE_FIRST = 7000,
	ID_PLAYLIST_ITEM_FIRST = 8000,
	ID_DISC_FIRST = 9000, // File > Open Disc: + drive
	ID_TITLE_FIRST = 9100, // Navigate > Titles: + title index
};

constexpr int kW = 1920, kH = 1080;

struct SubCache
{
	std::string key;
	Image img;
	int x = 0, y = 0;
};

class MainFrame
{
public:
	explicit MainFrame(bool have_data) : have_data_(have_data) {}
	int Run(const std::string& first);

private:
	Settings& s_ = AppSettings();
	Player player_;
	InputMapper input_;
	bool have_data_;
	bool quit_ = false;

	// Help, Check for updates (mpc_update.h): what the updater was last seen doing, and who asked
	update::Phase upd_seen_ = update::Phase::Idle;
	bool upd_user_ = false, upd_auto_ = false, upd_installing_ = false;
	double upd_osd_ = 0;
	void CheckForUpdates();
	void OfferUpdate(const update::Status& st);
	void TickUpdate();
	void RestartForUpdate();

	// the playlist (CPlaylist)
	std::vector<std::string> playlist_;
	int pl_cur_ = -1, pl_sel_ = -1, pl_top_ = 0;
	bool pl_focus_ = false;
	std::vector<double> pl_durations_;

	// layout
	CRect r_title_, r_menubar_, r_video_area_, r_seek_, r_toolbar_, r_info_, r_stats_, r_status_, r_playlist_;
	std::vector<CRect> menu_items_;
	std::vector<std::string> menu_names_ = {"&File", "&View", "&Play", "&Navigate", "F&avorites", "&Help"};
	CRect tb_btn_[13];
	CRect tb_volume_;
	CRect cap_btn_[3];
	CRect video_rect_; // where the picture goes (may be larger than the area)

	// state
	int menubar_hot_ = -1, menubar_open_ = -1;
	bool menu_keyboard_ = false;
	int tb_hot_ = -1, tb_pressed_ = -1, cap_hot_ = -1, cap_pressed_ = -1;
	bool seek_drag_ = false, vol_drag_ = false, seek_hot_ = false;
	double seek_drag_pos_ = 0;
	double hover_t_ = 0;
	int hover_what_ = -1; // toolbar button / 100 volume / 200 seekbar
	std::string osd_;
	double osd_until_ = 0;
	// idle drawing (Run): on the console always; on the host only with MPCPS5_HOST_IDLE=1
#ifdef __PROSPERO__
	bool idle_allowed_ = true;
#else
	bool idle_allowed_ = getenv("MPCPS5_HOST_IDLE") != nullptr;
#endif
	bool idle_ = false;
	double last_activity_ = 0, last_paint_ = 0;
	unsigned long long idle_presents_ = 0;
	double last_click_t_ = 0;
	PlayState last_state_ = PlayState::Closed;
	std::string current_url_;
	double last_pos_save_ = 0;
	bool show_renderer_stats_ = false;
	bool subs_enabled_ = true;
	double rate_ = 1.0; // the playing rate (Play > Increase / Decrease / Reset Rate)
	double options_down_ = 0; // Options pressed, given another action than the menu bar: when (held: the menu bar)
	bool opened_ok_ = false;

	// video
	Image video_layer_;
	CRect video_layer_rect_;
	SubCache sub_cache_;

	void Layout();
	bool Fullscreen() const { return s_.fullscreen; }
	bool ControlsOverlayVisible() const;
	void Paint(CDC& dc);
	void PaintTitleBar(CDC& dc);
	void PaintMenuBar(CDC& dc);
	void PaintVideo(CDC& dc);
	void PaintSubtitles(CDC& dc);
	void PaintOsd(CDC& dc);
	void PaintSeekBar(CDC& dc, const CRect& r);
	void PaintToolBar(CDC& dc, const CRect& r);
	void PaintVolume(CDC& dc, const CRect& vr);
	void PaintInfoBar(CDC& dc, const CRect& r, const std::vector<std::pair<std::string, std::string>>& rows);
	void PaintStatusBar(CDC& dc, const CRect& r);
	void PaintPlaylist(CDC& dc, const CRect& r);
	void PaintTooltip(CDC& dc);
	std::vector<std::pair<std::string, std::string>> InfoRows() const;
	std::vector<std::pair<std::string, std::string>> StatsRows() const;
	int BarRows(bool info) const;

	void OnEvent(const InputEvent& ev);
	void OnKey(Key k, bool repeat = false);
	// Options > Player > Keys: what a button does in the window, and the first button doing an action (menus)
	void DoPadAction(PadAction a);
	void ShowOptions(OptionsPage page);
	void TickOptionsButton();
	std::string Accel(PadAction a, const char* combination = "") const;
	void NextTrack(TrackType t);
	void OnMouseMove(int x, int y);
	void OnLButtonDown(int x, int y);
	void OnLButtonUp(int x, int y);
	void OnRButtonDown(int x, int y);
	void OpenMenuBar(int i, bool keyboard);
	std::vector<MenuItem> BuildMenu(int which) const; // 0..5 the menu bar's, -1 the context menu
	std::vector<MenuItem> FileMenu() const;
	std::vector<MenuItem> ViewMenu(bool context) const;
	std::vector<MenuItem> PlayMenu() const;
	std::vector<MenuItem> NavigateMenu() const;
	std::vector<MenuItem> FavoritesMenu() const;
	std::vector<MenuItem> HelpMenu() const;
	std::vector<MenuItem> TrackMenu(TrackType t) const;
	void Command(int id);
	void Tick();

	void Open(const std::string& url, bool use_saved_pos = true);
	void OpenPlaylistItem(int i);
	void CloseMedia();
	void SavePosition();
	void Osd(const std::string& text, double seconds = 1.5);
	void Seek(double t);
	void Jump(double d);
	void SetVolume(int v);
	void ApplyAudio();
	void SkipBack();
	void SkipForward();
	bool Loaded() const;
	double SeekPos() const;
	std::string StatusText() const;
	std::string TimeText() const;
	std::string Title() const;
	CRect SeekChannel(const CRect& r) const;
	CRect SeekThumb(const CRect& r, double pos) const;
	double SeekPosAt(const CRect& r, int x) const;
	CRect VolumeChannel(const CRect& vr) const;
	CRect VolumeThumb(const CRect& vr) const;
	bool ButtonEnabled(int i) const;
	bool ButtonChecked(int i) const;
	int PlaylistRowH() const { return UiFont().Height() + ScaleY(2); }
	CRect PlaylistList() const;
	void LoadPlaylistFile(const std::string& path);
	// a file, a URL or a playlist file: a new playlist with it
	void OpenAny(const std::string& url)
	{
		if (IsPlaylistFile(url) && url.find("://") == std::string::npos)
		{
			LoadPlaylistFile(url);
			return;
		}
		playlist_ = {url};
		pl_durations_.assign(1, 0);
		pl_cur_ = 0;
		Open(url);
	}
	void AddToPlaylist(const std::string& url) { playlist_.push_back(url); }
};

// ===========================================================================================================
bool MainFrame::Loaded() const
{
	const PlayState st = player_.State();
	return st == PlayState::Playing || st == PlayState::Paused || st == PlayState::Stopped;
}

std::string MainFrame::Title() const
{
	// CMainFrame::OpenSetupWindowTitle
	const std::string app = "Media Player Classic Home Cinema";
	if (current_url_.empty() || player_.State() == PlayState::Closed)
		return app;
	std::string name = s_.title_bar == 0 ? current_url_ : FileName(current_url_);
	// a password in an smb:// address stays off the screen
	const size_t at = name.find('@');
	if (name.compare(0, 6, "smb://") == 0 && at != std::string::npos)
		name = "smb://" + name.substr(at + 1);
	if (s_.title_bar == 2)
		return name;
	return name + " - " + app;
}

void MainFrame::Osd(const std::string& text, double seconds)
{
	if (!s_.show_osd)
		return;
	if (text != osd_ || NowSeconds() >= osd_until_)
		OrbisLog("[osd] %s", text.c_str());
	osd_ = text;
	osd_until_ = NowSeconds() + seconds;
}

// ---- layout ----
int MainFrame::BarRows(bool info) const
{
	return int(info ? InfoRows().size() : StatsRows().size());
}

void MainFrame::Layout()
{
	const bool fs = Fullscreen();
	int top = 0;
	r_title_ = r_menubar_ = CRect();
	if (!fs && s_.show_caption_menu)
	{
		r_title_ = CRect(0, 0, kW, ScaleY(23));
		r_menubar_ = CRect(0, r_title_.bottom, kW, r_title_.bottom + MenuBarHeight());
		top = r_menubar_.bottom;
	}
	// the control bars, from the bottom up: status, statistics, information, toolbar, seek bar, playlist
	int bottom = kH;
	auto take = [&](CRect& r, bool show, int h) {
		if (!show || h <= 0)
		{
			r = CRect();
			return;
		}
		r = CRect(0, bottom - h, kW, bottom);
		bottom -= h;
	};
	const bool bars = !fs || ControlsOverlayVisible() || !s_.hide_controls_fullscreen;
	take(r_status_, bars && s_.show_status, std::max(ScaleY(24), UiFont().Height() + ScaleY(8)));
	const int sr = s_.show_stats ? BarRows(false) : 0, ir = s_.show_info ? BarRows(true) : 0;
	take(r_stats_, bars && s_.show_stats && sr, sr * ScaleY(17) + ScaleY(2) * 2);
	take(r_info_, bars && s_.show_info && ir, ir * ScaleY(17) + ScaleY(2) * 2);
	const int img = ScaleX(16);
	const int tb_h = ScaleY(2) + img + 6 + ScaleY(2);
	take(r_toolbar_, bars && s_.show_controls, tb_h);
	take(r_seek_, bars && s_.show_seekbar, ScaleY(20) + (s_.show_controls ? 0 : 2));
	take(r_playlist_, s_.show_playlist && !fs, ScaleY(170));
	r_video_area_ = fs ? CRect(0, 0, kW, kH) : CRect(0, top, kW, bottom);

	// caption buttons (maximized: 46 x the caption)
	const int bw = ScaleX(46);
	for (int i = 0; i < 3; i++)
		cap_btn_[i] = CRect(kW - (3 - i) * bw, 0, kW - (2 - i) * bw, r_title_.bottom);
	// menu bar items
	menu_items_.clear();
	int x = 0;
	for (const auto& n : menu_names_)
	{
		const int w = UiFont().Measure(StripPrefix(T(n))) + ScaleX(14);
		menu_items_.push_back(CRect(x, r_menubar_.top, x + w, r_menubar_.bottom - ScaleY(1)));
		x += w;
	}
	// the toolbar's buttons (CPlayerToolBar: sizeButton = image + (7, 6), borders (2, 2, 0, 1))
	const int btn_w = img + 7, btn_h = img + 6, sep_w = ScaleX(8);
	const bool is_sep[13] = {false, false, false, true, false, false, false, false, true, false, true, true, false};
	x = r_toolbar_.left + ScaleX(2);
	const int y = r_toolbar_.top + ScaleY(2);
	// the volume control: (right - 60 - inc, ..., right + 6), inc = MulDiv(thumb height, 50, 19) - 50
	const int thumb_h = ScaleY(19);
	const int inc = thumb_h * 50 / 19 - 50;
	tb_volume_ = CRect(r_toolbar_.right - 60 - inc - ScaleX(4), r_toolbar_.top, r_toolbar_.right - ScaleX(4), r_toolbar_.bottom);
	for (int i = 0; i < 13; i++)
	{
		int w = is_sep[i] ? sep_w : btn_w;
		if (i == 11) // the stretching separator: mute sits just left of the volume
			w = std::max(0, tb_volume_.left - btn_w - x);
		tb_btn_[i] = CRect(x, y, x + w, y + btn_h);
		x += w;
	}
}

bool MainFrame::ControlsOverlayVisible() const
{
	if (!Fullscreen())
		return true;
	if (!s_.hide_controls_fullscreen)
		return true;
	return input_.CursorVisible() || seek_drag_ || vol_drag_ || !Loaded();
}

// ---- seek bar (CPlayerSeekBar) ----
CRect MainFrame::SeekChannel(const CRect& client) const
{
	CRect r = client;
	r.top += 1;
	if (s_.show_controls)
		r.bottom += 2; // ADD_TO_BOTTOM_WITHOUT_CONTROLBAR
	r.DeflateRect(ScaleFloorX(8), ScaleFloorY(7) + 1, ScaleFloorX(8), ScaleFloorY(7) + 1);
	return r;
}

CRect MainFrame::SeekThumb(const CRect& client, double pos) const
{
	const CRect ch = SeekChannel(client);
	const double dur = player_.Duration();
	long x = ch.left;
	if (dur > 0)
	{
		long p = long(ch.Width() * std::max(0.0, std::min(pos, dur)) / dur);
		if (p >= ch.Width())
			p = ch.Width() - 1;
		x = ch.left + p;
	}
	const int sy = ScaleFloorY(5);
	const int sx = ch.Height() * 96 / g_dpi / 2 + sy; // TransposeScaledY(height) / 2 + s.cy
	return CRect(x + 1 - sx, ch.top - sy, x + sx, ch.bottom + sy);
}

double MainFrame::SeekPosAt(const CRect& client, int x) const
{
	const CRect ch = SeekChannel(client);
	const double dur = player_.Duration();
	if (dur <= 0 || ch.Width() <= 0)
		return 0;
	return std::max(0.0, std::min(dur, (x - ch.left) * dur / ch.Width()));
}

double MainFrame::SeekPos() const
{
	return seek_drag_ ? seek_drag_pos_ : player_.Position();
}

void MainFrame::PaintSeekBar(CDC& dc, const CRect& r)
{
	const bool enabled = Loaded() && player_.Duration() > 0 && player_.Seekable();
	const COLORREF dark = sys::GrayText, white = sys::Window, shadow = sys::Shadow, light = sys::Hilight, bkg = sys::BtnFace;
	dc.FillSolidRect(r, bkg);
	const CRect channel = SeekChannel(r);
	// Channel
	dc.FillSolidRect(channel, enabled ? white : bkg);
	CRect cr = channel;
	cr.InflateRect(1, 1);
	dc.Draw3dRect(cr, shadow, light);
	// Chapters
	if (enabled)
		for (const Chapter& c : player_.Chapters())
		{
			const double dur = player_.Duration();
			long p = long(channel.Width() * c.time / dur);
			if (p <= 0 || p >= channel.Width())
				continue;
			dc.FillSolidRect(CRect(channel.left + p, channel.top, channel.left + p + 1 + (channel.left + p + 1 < channel.right), channel.bottom), dark);
		}
	// Thumb (CreateThumb)
	CRect t = SeekThumb(r, SeekPos());
	CRect ri = t;
	{
		const int sx = ScaleFloorX(4) - 1, sy = ScaleFloorY(5) - (enabled ? 0 : 1);
		ri.DeflateRect(sx, sy, sx, sy);
	}
	CRect th = t;
	dc.Draw3dRect(th, light, 0);
	th.DeflateRect(0, 0, 1, 1);
	dc.Draw3dRect(th, light, shadow);
	th.DeflateRect(1, 1, 1, 1);
	dc.FillSolidRect(th, bkg);
	CRect rio = ri;
	rio.InflateRect(1, 1, 1, 1);
	dc.Draw3dRect(rio, shadow, bkg);
	if (enabled)
	{
		CRect w = ri;
		w.InflateRect(0, 1, 0, 1);
		dc.FillSolidRect(w, white);
		dc.SetPixel(w.CenterPoint().x, w.top, 0);
		dc.SetPixel(w.CenterPoint().x, w.bottom - 1, 0);
	}
	else
		dc.FillSolidRect(ri, bkg);
}

// ---- toolbar (CPlayerToolBar) ----
bool MainFrame::ButtonEnabled(int i) const
{
	const bool loaded = Loaded();
	switch (i)
	{
		case 0:
		case 1:
		case 2:
			return loaded;
		case 4:
		case 7:
			return loaded && (playlist_.size() > 1 || !player_.Chapters().empty() || pl_cur_ >= 0);
		case 5:
		case 6:
			return loaded;
		case 9:
			return loaded && player_.HasVideo();
		case 12:
			return true;
		default:
			return false;
	}
}

bool MainFrame::ButtonChecked(int i) const
{
	const PlayState st = player_.State();
	switch (i)
	{
		case 0:
			return st == PlayState::Playing;
		case 1:
			return st == PlayState::Paused;
		case 2:
			return st == PlayState::Stopped;
		case 12:
			return s_.mute;
		default:
			return false;
	}
}

void MainFrame::PaintToolBar(CDC& dc, const CRect& r)
{
	dc.FillSolidRect(r, sys::BtnFace);
	const int img = ScaleX(16);
	const Image& normal = res::Toolbar(img);
	const Image& disabled = res::ToolbarDisabled(img);
	for (int i = 0; i < 13; i++)
	{
		if (i == 3 || i == 8 || i == 10)
		{
			// a flat toolbar's separator: an etched line
			const CRect& b = tb_btn_[i];
			const int x = b.CenterPoint().x - 1;
			dc.VLine(x, b.top + ScaleY(2), b.bottom - ScaleY(2), sys::Shadow);
			dc.VLine(x + 1, b.top + ScaleY(2), b.bottom - ScaleY(2), sys::Hilight);
			continue;
		}
		if (i == 11)
			continue;
		const CRect& b = tb_btn_[i];
		const bool en = ButtonEnabled(i);
		const bool checked = ButtonChecked(i) && i != 12;
		const bool hot = en && tb_hot_ == i;
		const bool pressed = en && tb_pressed_ == i;
		if (pressed || (checked && hot))
		{
			dc.FillSolidRect(b, RGB(204, 228, 247));
			dc.FrameRect(b, RGB(153, 209, 255));
		}
		else if (checked)
		{
			dc.FillSolidRect(b, RGB(204, 232, 255));
			dc.FrameRect(b, RGB(153, 209, 255));
		}
		else if (hot)
		{
			dc.FillSolidRect(b, RGB(229, 243, 255));
			dc.FrameRect(b, RGB(204, 232, 255));
		}
		const int image = i == 12 ? (s_.mute ? 13 : 12) : i;
		const Image& sheet = en ? normal : disabled;
		if (!sheet.IsNull())
		{
			// one picture out of the strip
			Bitmap part = sheet.Bmp();
			part.px += image * img;
			part.w = std::min(img, sheet.Width() - image * img);
			const int x = b.left + (b.Width() - img) / 2, y = b.top + (b.Height() - img) / 2;
			dc.AlphaBlit(x, y, part);
		}
	}
	PaintVolume(dc, tb_volume_);
}

CRect MainFrame::VolumeChannel(const CRect& vr) const
{
	const int thumb_w = ScaleX(11) / 2;
	return CRect(vr.left + thumb_w + ScaleX(4), vr.top, vr.right - thumb_w - ScaleX(4), vr.bottom);
}

CRect MainFrame::VolumeThumb(const CRect& vr) const
{
	const CRect ch = VolumeChannel(vr);
	const int x = ch.left + ch.Width() * s_.volume / 100;
	const int th = ScaleY(19), tw = ScaleX(11);
	const int cy = vr.top + vr.Height() / 2 + ScaleY(1);
	return CRect(x - tw / 2, cy - th / 2, x - tw / 2 + tw, cy - th / 2 + th);
}

void MainFrame::PaintVolume(CDC& dc, const CRect& vr)
{
	// CVolumeCtrl::OnNMCustomdraw: the wedge, then the thumb
	const CRect ch = VolumeChannel(vr);
	const CRect thumb = VolumeThumb(vr);
	CRect rc(ch.left, thumb.top + 2, ch.right - 2, thumb.bottom - 2);
	dc.MoveTo(rc.right, rc.top);
	dc.LineTo(rc.right, rc.bottom, sys::Hilight);
	dc.LineTo(rc.left, rc.bottom, sys::Hilight);
	dc.LineTo(rc.right, rc.top, sys::Shadow);
	// the wedge's line is one pixel at 100 %: thicker at this scale, as Windows' pens scale
	for (int t = 1; t < ScaleFloorX(1); t++)
	{
		dc.MoveTo(rc.right - t, rc.top + t);
		dc.LineTo(rc.right - t, rc.bottom - t, sys::Hilight);
		dc.MoveTo(rc.left + t * 2, rc.bottom - t);
		dc.LineTo(rc.right - t, rc.bottom - t, sys::Hilight);
	}
	CRect r = thumb;
	r.bottom--;
	r.DeflateRect(0, 0, 1, 0);
	dc.Draw3dRect(r, sys::Hilight, 0);
	r.DeflateRect(0, 0, 1, 1);
	dc.Draw3dRect(r, sys::Hilight, sys::Shadow);
	r.DeflateRect(1, 1, 1, 1);
	dc.FillSolidRect(r, sys::BtnFace);
}

// ---- information / statistics bars (CPlayerInfoBar) ----
std::vector<std::pair<std::string, std::string>> MainFrame::InfoRows() const
{
	std::vector<std::pair<std::string, std::string>> rows;
	if (!Loaded())
		return rows;
	const std::string title = player_.Title();
	rows.push_back({"Title", title.empty() ? FileName(current_url_) : title});
	if (player_.HasVideo())
	{
		char b[128];
		snprintf(b, sizeof(b), "%dx%d (%.3f:1)", player_.VideoWidth(), player_.VideoHeight(), player_.VideoAspect());
		std::string v = b;
		for (const Track& t : player_.Tracks(TrackType::Video))
			if (t.id == player_.CurrentTrack(TrackType::Video))
				v = t.name;
		rows.push_back({"Video", v});
	}
	const std::string a = player_.AudioDescription();
	if (!a.empty())
		rows.push_back({"Audio", a});
	const int sub = player_.CurrentTrack(TrackType::Subtitle);
	if (sub >= 0)
		for (const Track& t : player_.Tracks(TrackType::Subtitle))
			if (t.id == sub)
				rows.push_back({"Subtitles", t.name});
	const std::vector<Chapter> ch = player_.Chapters();
	const double pos = player_.Position();
	for (int i = int(ch.size()) - 1; i >= 0; i--)
		if (ch[i].time <= pos + 0.01)
		{
			rows.push_back({"Chapter", std::to_string(i + 1) + ". " + ch[i].name});
			break;
		}
	return rows;
}

std::vector<std::pair<std::string, std::string>> MainFrame::StatsRows() const
{
	std::vector<std::pair<std::string, std::string>> rows;
	if (!Loaded())
		return rows;
	const PlayerStats st = player_.Stats();
	char b[160];
	if (player_.HasVideo())
	{
		snprintf(b, sizeof(b), T("%.3f (%.1f shown)"), st.fps, st.shown_fps);
		rows.push_back({"Frame rate", b});
		snprintf(b, sizeof(b), T("%llu dropped, %d queued"), (unsigned long long)st.dropped, st.queued_video);
		rows.push_back({"Frames", b});
		snprintf(b, sizeof(b), T("decode %.1f ms, scale %.1f ms"), st.decode_ms, st.convert_ms);
		rows.push_back({"Timing", b});
		rows.push_back({"Decoder", T(st.hw_decoding ? "PS5 video decoder (hardware)" : "FFmpeg (software)")});
	}
	snprintf(b, sizeof(b), "%lld kb/s", (long long)(st.bitrate / 1000));
	rows.push_back({"Bitrate", b});
	snprintf(b, sizeof(b), T("%d ms queued, %llu underruns"), int(ps5audio::Queued() * 1000 / ps5audio::kRate),
		(unsigned long long)st.underruns);
	rows.push_back({"Buffers", b});
	snprintf(b, sizeof(b), T("audio %+d ms, subtitles %+d ms"), int(player_.AudioDelay() * 1000), int(player_.SubtitleDelay() * 1000));
	rows.push_back({"Sync offset", b});
	return rows;
}

void MainFrame::PaintInfoBar(CDC& dc, const CRect& r, const std::vector<std::pair<std::string, std::string>>& rows)
{
	// CPlayerInfoBar: black, the labels right-aligned in a column, the values after them (CStatusLabel)
	dc.FillSolidRect(r, 0);
	const Font& f = UiFont();
	int w = ScaleX(100);
	for (const auto& row : rows)
		w = std::max(w, f.Measure(T(row.first) + ":"));
	const int sep = ScaleX(10), h = ScaleY(17);
	int y = r.top + ScaleY(2);
	for (const auto& row : rows)
	{
		CRect lr(r.left + 1, y, r.left + w - sep, y + h);
		DrawText(dc, f, T(row.first) + ":", lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, RGB(255, 255, 255));
		CRect vr(r.left + w + sep, y, r.right - 1, y + h);
		DrawText(dc, f, row.second, vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, RGB(255, 255, 255));
		y += h;
	}
}

// ---- status bar (CPlayerStatusBar) ----
std::string MainFrame::StatusText() const
{
	switch (player_.State())
	{
		case PlayState::Opening:
			return T("Opening...");
		case PlayState::Playing:
		{
			if (player_.Rate() != 1.0)
			{
				char b[64];
				snprintf(b, sizeof(b), T("Playing [%gx]"), player_.Rate());
				return b;
			}
			return T("Playing");
		}
		case PlayState::Paused:
			return T("Paused");
		case PlayState::Stopped:
			return T("Stopped");
		case PlayState::Failed:
			return T("Failed to open");
		default:
			return "";
	}
}

std::string MainFrame::TimeText() const
{
	if (!Loaded())
		return "";
	const double dur = player_.Duration();
	const bool hours = dur >= 3600;
	if (dur <= 0)
		return FormatTime(SeekPos(), hours);
	return FormatTime(SeekPos(), hours) + " / " + FormatTime(dur, hours);
}

void MainFrame::PaintStatusBar(CDC& dc, const CRect& client)
{
	CRect r = client;
	dc.Draw3dRect(r, sys::Shadow, sys::Hilight);
	r.DeflateRect(1, 1);
	dc.FillSolidRect(r, 0);
	const Font& f = UiFont();
	// the file type icon (m_type)
	const int is = ScaleX(16);
	if (Loaded() || player_.State() == PlayState::Opening)
	{
		const Image& ic = res::AppIcon(is);
		dc.AlphaBlit(client.left + 6, client.top + (client.Height() - is) / 2, ic.Bmp());
	}
	// the audio type picture, at the right
	int bmw = 0;
	if (Loaded())
	{
		const int ch = player_.AudioChannels();
		const Image& bm = res::AudioType(ch == 0 ? 0 : ch == 1 ? 1 : 2);
		if (!bm.IsNull())
		{
			const int w = ScaleX(bm.Width()), h = ScaleY(bm.Height());
			bmw = w;
			dc.StretchBlt(CRect(client.right - w - 1, client.CenterPoint().y - h / 2, client.right - 1, client.CenterPoint().y - h / 2 + h), bm.Bmp());
		}
	}
	CRect tr = client;
	tr.DeflateRect(11 + is, 5, bmw + 8, 4);
	const std::string time = TimeText();
	const int tw = f.Measure(time);
	CRect timer(tr.right - tw, tr.top, tr.right, tr.bottom);
	DrawText(dc, f, time, timer, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, RGB(255, 255, 255));
	CRect sr(tr.left, tr.top, timer.left - 1, tr.bottom);
	DrawText(dc, f, StatusText(), sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, RGB(255, 255, 255));
}

// ---- playlist (CPlayerPlaylistBar) ----
CRect MainFrame::PlaylistList() const
{
	CRect r = r_playlist_;
	r.top += ScaleY(20);
	r.DeflateRect(ScaleX(2), 0, ScaleX(2), ScaleY(2));
	return r;
}

void MainFrame::PaintPlaylist(CDC& dc, const CRect& r)
{
	dc.FillSolidRect(r, sys::BtnFace);
	// the sizing bar's caption: "Playlist", a close button
	CRect cap(r.left + ScaleX(2), r.top + ScaleY(2), r.right - ScaleX(2), r.top + ScaleY(18));
	CRect ct(cap.left + ScaleX(4), cap.top, cap.right - ScaleX(20), cap.bottom);
	DrawText(dc, UiFont(), T("Playlist"), ct, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, sys::WindowText);
	const int cx = cap.right - ScaleX(9), cy = cap.CenterPoint().y, s = ScaleX(4);
	for (int i = -s; i <= s; i++)
	{
		dc.SetPixel(cx + i, cy + i, sys::WindowText);
		dc.SetPixel(cx + i, cy - i, sys::WindowText);
	}
	const CRect lr = PlaylistList();
	dc.FillSolidRect(lr, sys::Window);
	dc.Draw3dRect(lr, sys::Shadow, sys::Hilight);
	const Font& f = UiFont();
	const int rh = PlaylistRowH();
	const int vis = std::max(1, (lr.Height() - 2) / rh);
	if (pl_sel_ >= 0)
	{
		if (pl_sel_ < pl_top_)
			pl_top_ = pl_sel_;
		if (pl_sel_ >= pl_top_ + vis)
			pl_top_ = pl_sel_ - vis + 1;
	}
	const CRect old = dc.GetClip();
	CRect clip = lr;
	clip.DeflateRect(1, 1);
	dc.SetClip(clip);
	int y = lr.top + 1;
	const int digits = int(std::log10(0.1 + playlist_.size())) + 1;
	for (int i = pl_top_; i < int(playlist_.size()) && i < pl_top_ + vis + 1; i++)
	{
		CRect item(lr.left + 1, y, lr.right - 1, y + rh);
		if (i == pl_sel_ && pl_focus_)
		{
			dc.FillSolidRect(item, RGB(204, 218, 241)); // 0xf1dacc
			dc.FrameRect(item, RGB(49, 106, 197)); // 0xc56a31
		}
		const COLORREF tc = i == pl_cur_ ? RGB(255, 0, 0) : RGB(0, 0, 0);
		std::string time;
		if (i < int(pl_durations_.size()) && pl_durations_[i] > 0)
			time = FormatTime(pl_durations_[i]);
		int timex = item.right;
		if (!time.empty())
		{
			const int tw = f.Measure(time);
			if (3 + tw + 3 < item.Width() / 2)
			{
				timex = item.right - (3 + tw + 3);
				f.Draw(dc, timex, item.top + (rh - f.Height()) / 2, time, tc);
			}
		}
		char num[16];
		snprintf(num, sizeof(num), "%0*d. ", digits, i + 1);
		const std::string text = FitText(f, std::string(num) + FileName(playlist_[i]), timex - item.left - 9);
		f.Draw(dc, item.left + 3, item.top + (rh - f.Height()) / 2, text, tc);
		y += rh;
	}
	dc.SetClip(old);
}

// ---- the video ----
void MainFrame::PaintVideo(CDC& dc)
{
	const CRect area = r_video_area_;
	dc.FillSolidRect(area, 0);
	const bool show_video = Loaded() && player_.HasVideo() && player_.HasPicture();
	if (!show_video)
	{
		// the logo (CChildView::OnEraseBkgnd): at its own size (here: scaled with the window), smaller if needed;
		// an audio file's cover art instead when it has one
		const Image* cover = Loaded() ? player_.CoverArt() : nullptr;
		const Image& img = cover ? *cover : res::Logo();
		if (img.IsNull())
			return;
		const double ar = double(img.Width()) / img.Height();
		int w = area.Width(), h = area.Height();
		if (!cover)
		{
			w = std::min(ScaleX(img.Width()), w);
			h = std::min(ScaleY(img.Height()), h);
		}
		double iw = h * ar, ih;
		if (w < iw)
		{
			iw = w;
			ih = iw / ar;
		}
		else
			ih = h;
		const int x = area.left + int(std::lround((area.Width() - iw) / 2.0));
		const int y = area.top + int(std::lround((area.Height() - ih) / 2.0));
		dc.StretchBlt(CRect(x, y, x + int(std::lround(iw)), y + int(std::lround(ih))), img.Bmp());
		return;
	}
	// the picture's rectangle (CMainFrame::GetVideoSize / MoveVideoWindow)
	const double par_ar = player_.VideoAspect();
	double ar = par_ar;
	switch (s_.aspect_ratio)
	{
		case 1:
			ar = 4.0 / 3;
			break;
		case 2:
			ar = 5.0 / 4;
			break;
		case 3:
			ar = 16.0 / 9;
			break;
		case 4:
			ar = 2.35;
			break;
		case 5:
			ar = 1.85;
			break;
		case 6:
			ar = double(player_.VideoWidth()) / std::max(1, player_.VideoHeight());
			break;
		default:
			break;
	}
	const int vh = player_.VideoHeight();
	const double vw = vh * ar;
	double w, h;
	const double aw = area.Width(), ah = area.Height();
	const double fit_w = std::min(aw, ah * ar), fit_h = fit_w / ar;
	const double fill_w = std::max(aw, ah * ar), fill_h = fill_w / ar;
	switch (s_.video_frame)
	{
		case VideoFrame::HalfSize:
			w = vw / 2;
			h = vh / 2.0;
			break;
		case VideoFrame::NormalSize:
			w = vw;
			h = vh;
			break;
		case VideoFrame::DoubleSize:
			w = vw * 2;
			h = vh * 2.0;
			break;
		case VideoFrame::Stretch:
			w = aw;
			h = ah;
			break;
		case VideoFrame::Zoom1:
			w = fit_w + (fill_w - fit_w) / 3;
			h = fit_h + (fill_h - fit_h) / 3;
			break;
		case VideoFrame::Zoom2:
			w = fit_w + (fill_w - fit_w) * 2 / 3;
			h = fit_h + (fill_h - fit_h) * 2 / 3;
			break;
		case VideoFrame::FromOutside:
			w = fill_w;
			h = fill_h;
			break;
		default:
			w = fit_w;
			h = fit_h;
			break;
	}
	w *= s_.zoom_x;
	h *= s_.zoom_y;
	const double cx = area.left + aw / 2 + s_.pan_x * std::max(0.0, (w - aw) / 2 + aw / 2) * 0.5;
	const double cy = area.top + ah / 2 + s_.pan_y * std::max(0.0, (h - ah) / 2 + ah / 2) * 0.5;
	video_rect_ = CRect(int(std::lround(cx - w / 2)), int(std::lround(cy - h / 2)), int(std::lround(cx + w / 2)),
		int(std::lround(cy + h / 2)));
	CRect vis = video_rect_;
	if (!vis.Intersect(area))
		return;
	// the visible part only (Double Size of a 4K picture would be 7680 wide): the source cropped to match
	if (video_layer_.Width() != vis.Width() || video_layer_.Height() != vis.Height())
		video_layer_.Create(vis.Width(), vis.Height());
	const bool moved = vis != video_layer_rect_;
	video_layer_rect_ = vis;
	if (vis == video_rect_)
		player_.RenderVideo(video_layer_.Data(), vis.Width(), vis.Width(), vis.Height(), moved);
	else
	{
		// render the whole into a scratch area only as big as needed: scale the crop ourselves
		static Image full;
		const int fw = std::min(video_rect_.Width(), 3840), fh = std::min(video_rect_.Height(), 2160);
		if (full.Width() != fw || full.Height() != fh)
			full.Create(fw, fh);
		player_.RenderVideo(full.Data(), fw, fw, fh, moved);
		const double sx = double(fw) / video_rect_.Width(), sy = double(fh) / video_rect_.Height();
		for (int y = 0; y < vis.Height(); y++)
		{
			const int syy = std::min(fh - 1, int((vis.top - video_rect_.top + y) * sy));
			const uint32_t* src = full.Data() + size_t(syy) * fw;
			uint32_t* dst = video_layer_.Data() + size_t(y) * vis.Width();
			for (int x = 0; x < vis.Width(); x++)
				dst[x] = src[std::min(fw - 1, int((vis.left - video_rect_.left + x) * sx))];
		}
	}
	dc.BitBlt(vis.left, vis.top, video_layer_.Bmp());
}

void MainFrame::PaintSubtitles(CDC& dc)
{
	if (!Loaded() || !subs_enabled_)
		return;
	{
		// ASS/SSA: libass's picture, as authored, over the video as shown (the Subtitles options don't apply)
		const CRect fr = player_.HasVideo() && player_.HasPicture() ? video_rect_ : r_video_area_;
		CRect vis = fr;
		if (!fr.IsRectEmpty() && vis.Intersect(r_video_area_))
		{
			const Image* img = nullptr;
			int x = 0, y = 0;
			bool changed = false;
			const CRect clip(vis.left - fr.left, vis.top - fr.top, vis.right - fr.left, vis.bottom - fr.top);
			if (player_.AssSubtitles(fr.Width(), fr.Height(), player_.VideoWidth(), player_.VideoHeight(), clip, &img, &x,
					&y, &changed))
			{
				if (img)
					dc.AlphaBlit(fr.left + x, fr.top + y, img->Bmp());
				return;
			}
		}
	}
	const SubtitleShow sub = player_.Subtitles();
	if (sub.Empty())
		return;
	CRect vr = player_.HasVideo() && player_.HasPicture() ? video_rect_ : r_video_area_;
	CRect area = r_video_area_;
	if (vr.IsRectEmpty())
		return;
	// bitmap subtitles: scaled from the video's coordinates
	if (!sub.pics.empty() && sub.ref_w > 0 && sub.ref_h > 0)
	{
		const double sx = double(vr.Width()) / sub.ref_w, sy = double(vr.Height()) / sub.ref_h;
		for (const auto& p : sub.pics)
		{
			CRect d(vr.left + int(p.x * sx), vr.top + int(p.y * sy), vr.left + int((p.x + p.img.Width()) * sx),
				vr.top + int((p.y + p.img.Height()) * sy));
			dc.StretchBlt(d, p.img.Bmp());
		}
	}
	if (sub.lines.empty())
		return;
	// text: MPC-HC's default style (VSFilter's: bold, white, a black outline and shadow; Options > Subtitles > Default
	// Style changes it), sized for a 288-line script as VSFilter sizes SRT text, so it scales with the picture
	const int ref_h = std::min(vr.Height(), area.Height());
	SubtitleStyle st;
	st.em = std::max(ScaleY(12), s_.sub_font_size * ref_h / 288);
	st.bold = s_.sub_bold;
	st.outline = s_.sub_outline > 0 ? std::max(1, s_.sub_outline * ref_h / 288) : 0;
	st.shadow = s_.sub_shadow > 0 ? std::max(1, s_.sub_shadow * ref_h / 288 / 2) : 0;
	st.color = RGB(s_.sub_color >> 16, s_.sub_color >> 8, s_.sub_color);
	st.outline_color = RGB(s_.sub_outline_color >> 16, s_.sub_outline_color >> 8, s_.sub_outline_color);
	st.box = s_.sub_border == 1;
	st.box_alpha = s_.sub_box_opacity * 255 / 100;
	char style[160];
	snprintf(style, sizeof(style), "%d|%d|%d|%d|%x|%x|%d|%d|%d|%s|", st.em, st.outline, st.shadow, int(st.bold), unsigned(st.color),
		unsigned(st.outline_color), int(st.box), st.box_alpha, s_.sub_margin, s_.sub_font.c_str());
	std::string key = style + std::to_string(vr.Width()) + (sub.top ? "T" : "B");
	for (const auto& l : sub.lines)
		key += "\n" + l;
	if (key != sub_cache_.key)
	{
		sub_cache_.key = key;
		const int maxw = std::max(100, std::min(vr.Width(), area.Width()) - 2 * st.em);
		const int pad = RenderSubtitleText(st, sub.lines, sub.italic, maxw, sub_cache_.img);
		const int iw = sub_cache_.img.Width(), ih = sub_cache_.img.Height();
		const int margin = std::max(ScaleY(8) * s_.sub_margin / 20, s_.sub_margin * ref_h / 288);
		const CRect place = s_.sub_position == 1 ? area : vr;
		sub_cache_.x = place.left + (place.Width() - iw) / 2;
		sub_cache_.y = sub.top ? place.top + margin - pad : std::min(place.bottom, area.bottom) - margin - ih + pad;
	}
	dc.AlphaBlit(sub_cache_.x, sub_cache_.y, sub_cache_.img.Bmp());
}

void MainFrame::PaintOsd(CDC& dc)
{
	const double now = NowSeconds();
	std::string text = now < osd_until_ ? osd_ : std::string();
	if (show_renderer_stats_ && Loaded())
	{
		const PlayerStats st = player_.Stats();
		char b[256];
		snprintf(b, sizeof(b), T("%dx%d  %.3f fps (%.1f shown)  dropped %llu  decode %.1f ms  scale %.1f ms"),
			player_.VideoWidth(), player_.VideoHeight(), st.fps, st.shown_fps, (unsigned long long)st.dropped,
			st.decode_ms, st.convert_ms);
		text = text.empty() ? b : text + "   " + b;
	}
	if (text.empty())
		return;
	// CVMROSD::DrawMessage: a box 10 px in, (32,40,48) with a (48,56,62) border, (224,224,224) text, Segoe UI 18 pt
	const Font& f = FontOfSize(18 * g_dpi / 72);
	const CRect area = r_video_area_;
	CRect rt(0, 0, f.Measure(text), f.Height());
	rt.InflateRect(10, 5);
	rt.MoveToXY(area.left + 10, area.top + 10);
	CRect lim = area;
	lim.DeflateRect(10, 10);
	rt.Intersect(lim);
	dc.FillSolidRect(rt, RGB(32, 40, 48));
	dc.FrameRect(rt, RGB(48, 56, 62));
	CRect tr = rt;
	tr.DeflateRect(10, 5);
	DrawText(dc, f, text, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, RGB(224, 224, 224));
}

void MainFrame::PaintTitleBar(CDC& dc)
{
	if (r_title_.IsRectEmpty())
		return;
	dc.FillSolidRect(r_title_, sys::Caption);
	const int is = ScaleX(16);
	dc.AlphaBlit(r_title_.left + ScaleX(8), r_title_.top + (r_title_.Height() - is) / 2, res::AppIcon(is).Bmp());
	CRect tr(r_title_.left + ScaleX(8) + is + ScaleX(6), r_title_.top, cap_btn_[0].left - ScaleX(8), r_title_.bottom);
	DrawText(dc, UiFont(), Title(), tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, sys::CaptionText);
	DrawCaptionButton(dc, cap_btn_[0], 0, cap_hot_ == 0, cap_pressed_ == 0, true);
	DrawCaptionButton(dc, cap_btn_[1], 2, cap_hot_ == 1, cap_pressed_ == 1, true);
	DrawCaptionButton(dc, cap_btn_[2], 3, cap_hot_ == 2, cap_pressed_ == 2, true);
}

void MainFrame::PaintMenuBar(CDC& dc)
{
	if (r_menubar_.IsRectEmpty())
		return;
	dc.FillSolidRect(r_menubar_, sys::MenuBar);
	for (size_t i = 0; i < menu_items_.size(); i++)
		DrawMenuBarItem(dc, menu_items_[i], T(menu_names_[i]), int(i) == menubar_hot_ && menubar_open_ < 0,
			int(i) == menubar_open_, menu_keyboard_);
}

void MainFrame::PaintTooltip(CDC& dc)
{
	if (!input_.CursorVisible() || hover_what_ < 0 || NowSeconds() - hover_t_ < 0.5 || WindowManager::Get().HasModal())
		return;
	std::string tip;
	static const char* const kTips[13] = {"Play", "Pause", "Stop", "", "Previous", "Decrease Rate", "Increase Rate", "Next", "",
		"Frame Step", "", "", "Mute"};
	if (hover_what_ < 13)
		tip = T(kTips[hover_what_]);
	else if (hover_what_ == 100)
		tip = std::to_string(s_.volume) + "%";
	else if (hover_what_ == 200 && Loaded() && player_.Duration() > 0)
		tip = FormatTime(SeekPosAt(r_seek_, input_.CursorX()), player_.Duration() >= 3600);
	if (tip.empty())
		return;
	const Font& f = UiFont();
	const int w = f.Measure(tip) + ScaleX(12), h = f.Height() + ScaleY(6);
	int x = input_.CursorX() - w / 2, y = input_.CursorY() - h - ScaleY(14);
	if (hover_what_ != 200)
	{
		x = input_.CursorX();
		y = input_.CursorY() + ScaleY(22);
	}
	x = std::max(0, std::min(kW - w, x));
	if (y + h > kH)
		y = input_.CursorY() - h - ScaleY(8);
	CRect r(x, y, x + w, y + h);
	dc.FillSolidRect(r, sys::TooltipBack);
	dc.FrameRect(r, sys::TooltipBorder);
	DrawText(dc, f, tip, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, sys::TooltipText);
}

void MainFrame::Paint(CDC& dc)
{
	Layout();
	PaintVideo(dc);
	PaintSubtitles(dc);
	PaintOsd(dc);
	PaintTitleBar(dc);
	PaintMenuBar(dc);
	if (!r_playlist_.IsRectEmpty())
		PaintPlaylist(dc, r_playlist_);
	if (!r_seek_.IsRectEmpty())
		PaintSeekBar(dc, r_seek_);
	if (!r_toolbar_.IsRectEmpty())
		PaintToolBar(dc, r_toolbar_);
	if (!r_info_.IsRectEmpty())
		PaintInfoBar(dc, r_info_, InfoRows());
	if (!r_stats_.IsRectEmpty())
		PaintInfoBar(dc, r_stats_, StatsRows());
	if (!r_status_.IsRectEmpty())
		PaintStatusBar(dc, r_status_);
	PaintTooltip(dc);
}

// ===========================================================================================================
// menus
std::vector<MenuItem> MainFrame::FileMenu() const
{
	using M = MenuItem;
	std::vector<M> recent;
	for (size_t i = 0; i < s_.recent.size() && i < 20; i++)
	{
		std::string shown = s_.recent[i];
		const size_t at = shown.find('@');
		if (shown.compare(0, 6, "smb://") == 0 && at != std::string::npos)
			shown = "smb://" + shown.substr(at + 1);
		recent.push_back(M::Cmd("&" + std::to_string((i + 1) % 10) + " " + shown, ID_RECENT_FIRST + int(i)));
	}
	if (recent.empty())
		recent.push_back(M::Cmd("<empty>", 0).Enable(false));
	else
	{
		recent.push_back(M::Sep());
		recent.push_back(M::Cmd("&Clear list", ID_RECENT_FILES_CLEAR));
	}
	const bool loaded = Loaded();
	std::vector<M> subs = {M::Cmd("&Load Subtitles...", ID_FILE_SUBTITLES_LOAD).Enable(loaded),
		M::Cmd("Save S&ubtitles...", ID_FILE_SUBTITLES_SAVE).Enable(false), M::Sep(),
		M::Cmd("&Download Subtitles...", ID_FILE_SUBTITLES_DOWNLOAD).Enable(false),
		M::Cmd("&Upload Subtitles...", ID_FILE_SUBTITLES_UPLOAD).Enable(false)};
	// the disc drives (mpc_disc.h): the console's own, USB drives
	std::vector<M> discs;
	const std::vector<disc::DriveState> drives = disc::Drives();
	for (size_t i = 0; i < drives.size() && i < 8; i++)
	{
		const disc::DriveState& d = drives[i];
		const bool can = d.kind == disc::Kind::Data || d.kind == disc::Kind::DvdVideo || d.kind == disc::Kind::BluRay ||
						 d.kind == disc::Kind::Encrypted;
		discs.push_back(M::Shown(disc::DriveName(d), ID_DISC_FIRST + int(i)).Enable(can));
	}
	if (discs.empty())
		discs.push_back(M::Cmd("<empty>", 0).Enable(false));
	return {M::Cmd("&Quick Open File...", ID_FILE_OPENQUICK), M::Sep(), M::Cmd("&Open File...", ID_FILE_OPENMEDIA),
		M::Cmd("Open &DVD/BD...", ID_FILE_OPENDVDBD), M::Cmd("Open De&vice...", ID_FILE_OPENDEVICE).Enable(false),
		M::Cmd("Open Dir&ectory...", ID_FILE_OPENDIRECTORY), M::Popup("O&pen Disc", discs),
		M::Popup("Recent &Files", recent), M::Cmd("&Close", ID_FILE_CLOSE_AND_RESTORE).Enable(loaded || player_.State() == PlayState::Opening), M::Sep(),
		M::Cmd("&Save a Copy...", ID_FILE_SAVE_COPY).Enable(false),
		M::Cmd("Save &Image...", ID_FILE_SAVE_IMAGE).Enable(loaded && player_.HasPicture()),
		M::Cmd("Save &Thumbnails...", ID_FILE_SAVE_THUMBNAILS).Enable(false), M::Sep(), M::Popup("S&ubtitles", subs), M::Sep(),
		M::Cmd("P&roperties", ID_FILE_PROPERTIES).Enable(loaded), M::Sep(), M::Cmd("E&xit", ID_FILE_EXIT)};
}

std::vector<MenuItem> MainFrame::ViewMenu(bool context) const
{
	using M = MenuItem;
	const bool fs = Fullscreen();
	std::vector<M> bars = {M::Cmd("Caption&&Menu", ID_VIEW_CAPTIONMENU).Check(s_.show_caption_menu),
		M::Cmd("See&k Bar", ID_VIEW_SEEKER).Check(s_.show_seekbar), M::Cmd("&Controls", ID_VIEW_CONTROLS).Check(s_.show_controls),
		M::Cmd("&Information", ID_VIEW_INFORMATION).Check(s_.show_info), M::Cmd("&Statistics", ID_VIEW_STATISTICS).Check(s_.show_stats),
		M::Cmd("St&atus", ID_VIEW_STATUS).Check(s_.show_status), M::Cmd("Su&bresync", ID_VIEW_SUBRESYNC).Enable(false),
		M::Cmd("Pla&ylist", ID_VIEW_PLAYLIST).Check(s_.show_playlist), M::Cmd("Captu&re", ID_VIEW_CAPTURE).Enable(false),
		M::Cmd("Na&vigation", ID_VIEW_NAVIGATION).Enable(false), M::Cmd("&Debug Shaders", ID_VIEW_DEBUGSHADERS).Enable(false),
		M::Popup("&Presets...", {M::Cmd("&Minimal", ID_VIEW_PRESETS_MINIMAL), M::Cmd("&Compact", ID_VIEW_PRESETS_COMPACT),
									M::Cmd("&Normal", ID_VIEW_PRESETS_NORMAL)})};
	const VideoFrame vf = s_.video_frame;
	std::vector<M> aspect = {M::Cmd("&Default (DAR)", ID_ASPECTRATIO_DAR).Radio(s_.aspect_ratio == 0),
		M::Cmd("&4:3", ID_ASPECTRATIO_4_3).Radio(s_.aspect_ratio == 1), M::Cmd("&5:4", ID_ASPECTRATIO_5_4).Radio(s_.aspect_ratio == 2),
		M::Cmd("&16:9", ID_ASPECTRATIO_16_9).Radio(s_.aspect_ratio == 3),
		M::Cmd("&235:100", ID_ASPECTRATIO_235_100).Radio(s_.aspect_ratio == 4),
		M::Cmd("1&85:100", ID_ASPECTRATIO_185_100).Radio(s_.aspect_ratio == 5),
		M::Cmd("Assume &square pixels (SAR)", ID_ASPECTRATIO_SAR).Radio(s_.aspect_ratio == 6)};
	M frame = M::Popup("Video &Frame",
		{M::Cmd("&Half Size", ID_VIEW_VF_HALF).Radio(vf == VideoFrame::HalfSize),
			M::Cmd("&Normal Size", ID_VIEW_VF_NORMAL).Radio(vf == VideoFrame::NormalSize),
			M::Cmd("&Double Size", ID_VIEW_VF_DOUBLE).Radio(vf == VideoFrame::DoubleSize),
			M::Cmd("&Stretch To Window", ID_VIEW_VF_STRETCH).Radio(vf == VideoFrame::Stretch),
			M::Cmd("Touch Window From &Inside", ID_VIEW_VF_FROMINSIDE).Radio(vf == VideoFrame::FromInside),
			M::Cmd("Zoom &1", ID_VIEW_VF_ZOOM1).Radio(vf == VideoFrame::Zoom1), M::Cmd("Zoom &2", ID_VIEW_VF_ZOOM2).Radio(vf == VideoFrame::Zoom2),
			M::Cmd("Touch Window From &Outside", ID_VIEW_VF_FROMOUTSIDE).Radio(vf == VideoFrame::FromOutside), M::Sep(),
			M::Popup("&Aspect Ratio", aspect), M::Cmd("&Correct Monitor/Desktop AR Diff", ID_VIEW_VF_COMPMONDESKARDIFF).Enable(false)});
	M panscan = M::Popup("Pa&n&&Scan", {M::Cmd("&Increase Size", ID_VIEW_INCSIZE), M::Cmd("&Decrease Size", ID_VIEW_DECSIZE),
										  M::Cmd("I&ncrease Width", ID_VIEW_INCWIDTH), M::Cmd("D&ecrease Width", ID_VIEW_DECWIDTH),
										  M::Cmd("In&crease Height", ID_VIEW_INCHEIGHT), M::Cmd("Decre&ase Height", ID_VIEW_DECHEIGHT), M::Sep(),
										  M::Cmd("Move &Right", ID_PANSCAN_MOVERIGHT), M::Cmd("Move &Left", ID_PANSCAN_MOVELEFT),
										  M::Cmd("Move &Up", ID_PANSCAN_MOVEUP), M::Cmd("Move &Down", ID_PANSCAN_MOVEDOWN),
										  M::Cmd("Cen&ter", ID_PANSCAN_CENTER), M::Sep(), M::Cmd("Re&set", ID_VIEW_RESET)});
	M renderer = M::Popup("R&enderer Settings",
		{M::Cmd("&Tearing Test", ID_VIEW_TEARING_TEST).Enable(false),
			M::Cmd("&Display Statistics", ID_VIEW_DISPLAY_RENDERER_STATS, Accel(PadAction::Statistics, "R3+Square")).Check(show_renderer_stats_),
			M::Cmd("Display Current T&ime", ID_VIEW_OSD_DISPLAY_TIME).Enable(Loaded()),
			M::Cmd("Show &File Name", ID_VIEW_OSD_SHOW_FILENAME).Enable(Loaded()), M::Sep(),
			M::Popup("&Output Range", {M::Cmd("&0 - 255", ID_VIEW_RENDERER_OTHER).Radio(true), M::Cmd("&16 - 235", ID_VIEW_RENDERER_OTHER).Enable(false)}),
			M::Popup("&Presentation", {M::Cmd("D3D Fullscreen &Mode", ID_VIEW_RENDERER_OTHER).Enable(false),
										   M::Cmd("10-bit &RGB Output", ID_VIEW_RENDERER_OTHER).Enable(false)}),
			M::Popup("&Color Management", {M::Cmd("&Enable", ID_VIEW_RENDERER_OTHER).Enable(false)}),
			M::Popup("&VSync", {M::Cmd("&VSync", ID_VIEW_RENDERER_OTHER).Check(true).Enable(false)})});
	M ontop = M::Popup("On &Top", {M::Cmd("&Default", ID_ONTOP_DEFAULT).Radio(true), M::Cmd("&Always", ID_ONTOP_ALWAYS).Enable(false),
									  M::Cmd("While &Playing", ID_ONTOP_WHILEPLAYING).Enable(false),
									  M::Cmd("While Playing &Video", ID_ONTOP_WHILEPLAYINGVIDEO).Enable(false)});
	M zoom = M::Popup("&Zoom", {M::Cmd("&50%", ID_VIEW_ZOOM_50), M::Cmd("&100%", ID_VIEW_ZOOM_100), M::Cmd("&200%", ID_VIEW_ZOOM_200),
								   M::Cmd("Auto &Fit", ID_VIEW_ZOOM_AUTOFIT), M::Cmd("Auto Fit (&Larger Only)", ID_VIEW_ZOOM_AUTOFIT_LARGER)});
	if (context)
	{
		std::vector<M> v = {ontop, M::Sep()};
		v.insert(v.end(), bars.begin(), bars.end());
		return v;
	}
	std::vector<M> v = bars;
	v.push_back(M::Sep());
	v.push_back(M::Cmd("F&ull Screen", ID_VIEW_FULLSCREEN, Accel(PadAction::FullScreen)).Check(fs));
	v.push_back(zoom);
	v.push_back(M::Sep());
	v.push_back(renderer);
	v.push_back(frame);
	v.push_back(panscan);
	v.push_back(M::Sep());
	v.push_back(ontop);
	v.push_back(M::Cmd("&Options...", ID_VIEW_OPTIONS, Accel(PadAction::OptionsWindow)));
	return v;
}

std::vector<MenuItem> MainFrame::TrackMenu(TrackType t) const
{
	using M = MenuItem;
	std::vector<M> v;
	const int cur = player_.CurrentTrack(t);
	const int base = t == TrackType::Audio ? ID_AUDIO_FIRST : t == TrackType::Subtitle ? ID_SUBTITLE_FIRST : ID_VIDEO_FIRST;
	if (t == TrackType::Subtitle)
	{
		v.push_back(M::Cmd("&Enabled", ID_SUBTITLES_ENABLE).Check(subs_enabled_ && cur >= 0));
		v.push_back(M::Sep());
	}
	for (const Track& tr : player_.Tracks(t))
		v.push_back(M::Cmd(tr.name, base + tr.id).Radio(tr.id == cur));
	if (t == TrackType::Subtitle)
	{
		v.push_back(M::Sep());
		char b[64];
		snprintf(b, sizeof(b), T("Delay: %+d ms"), int(std::lround(player_.SubtitleDelay() * 1000)));
		v.push_back(M::Shown(b, 0).Enable(false));
		v.push_back(M::Shown(Fmt(T("Delay +%d ms"), s_.sub_delay_step), ID_SUB_DELAY_UP));
		v.push_back(M::Shown(Fmt(T("Delay -%d ms"), s_.sub_delay_step), ID_SUB_DELAY_DOWN));
		v.push_back(M::Cmd("Reset delay", ID_SUB_DELAY_RESET));
	}
	if (t == TrackType::Audio)
	{
		v.push_back(M::Sep());
		char b[64];
		snprintf(b, sizeof(b), T("Delay: %+d ms"), int(std::lround(player_.AudioDelay() * 1000)));
		v.push_back(M::Shown(b, 0).Enable(false));
		v.push_back(M::Cmd("Delay +10 ms", ID_AUDIO_DELAY_UP));
		v.push_back(M::Cmd("Delay -10 ms", ID_AUDIO_DELAY_DOWN));
	}
	if (v.empty())
		v.push_back(M::Cmd("<empty>", 0).Enable(false));
	return v;
}

std::vector<MenuItem> MainFrame::PlayMenu() const
{
	using M = MenuItem;
	const bool loaded = Loaded();
	const PlayState st = player_.State();
	std::vector<M> after = {M::Cmd("Do no&thing", ID_AFTERPLAYBACK_DONOTHING).Radio(s_.after_playback == AfterPlayback::DoNothing),
		M::Cmd("Play &next file in the folder", ID_AFTERPLAYBACK_PLAYNEXT).Radio(s_.after_playback == AfterPlayback::PlayNext),
		M::Cmd("Turn off the &monitor", ID_AFTERPLAYBACK_MONITOROFF).Enable(false),
		M::Cmd("&Exit", ID_AFTERPLAYBACK_EXIT).Radio(s_.after_playback == AfterPlayback::Exit),
		M::Cmd("&Stand By", ID_AFTERPLAYBACK_STANDBY).Enable(false), M::Cmd("&Hibernate", ID_AFTERPLAYBACK_HIBERNATE).Enable(false),
		M::Cmd("Shut&down", ID_AFTERPLAYBACK_SHUTDOWN).Enable(false), M::Cmd("Log &Off", ID_AFTERPLAYBACK_LOGOFF).Enable(false),
		M::Cmd("&Lock", ID_AFTERPLAYBACK_LOCK).Enable(false)};
	return {M::Cmd("&Play/Pause", ID_PLAY_PLAYPAUSE, Accel(PadAction::PlayPause)).Enable(loaded).Check(st == PlayState::Playing),
		M::Cmd("&Stop", ID_PLAY_STOP).Enable(loaded), M::Cmd("Fra&me Step", ID_PLAY_FRAMESTEP, Accel(PadAction::FrameStep)).Enable(loaded && player_.HasVideo()),
		M::Popup("&Repeat", {M::Cmd("F&orever", ID_PLAY_REPEAT_FOREVER).Check(s_.repeat != 0), M::Sep(),
								M::Cmd("&File", ID_PLAY_REPEAT_ONEFILE).Radio(s_.repeat == 1),
								M::Cmd("&Playlist", ID_PLAY_REPEAT_WHOLEPLAYLIST).Radio(s_.repeat == 2)}),
		M::Sep(), M::Cmd("&Decrease Rate", ID_PLAY_DECRATE, Accel(PadAction::Slower)).Enable(loaded), M::Cmd("&Increase Rate", ID_PLAY_INCRATE, Accel(PadAction::Faster)).Enable(loaded),
		M::Cmd("R&eset Rate", ID_PLAY_RESETRATE, Accel(PadAction::NormalSpeed)).Enable(loaded), M::Sep(), M::Popup("&Filters", {M::Cmd("<empty>", 0).Enable(false)}),
		M::Popup("S&haders", {M::Cmd("<empty>", 0).Enable(false)}), M::Sep(),
		M::Popup("&Audio Track", TrackMenu(TrackType::Audio)).Enable(loaded),
		M::Popup("Su&btitle Track", TrackMenu(TrackType::Subtitle)).Enable(loaded),
		M::Popup("Vide&o Track", TrackMenu(TrackType::Video)).Enable(loaded), M::Sep(),
		M::Popup("&Volume", {M::Cmd("&Up", ID_VOLUME_UP, Accel(PadAction::VolumeUp)), M::Cmd("&Down", ID_VOLUME_DOWN, Accel(PadAction::VolumeDown)),
								M::Cmd("&Mute", ID_VOLUME_MUTE, Accel(PadAction::Mute)).Check(s_.mute)}),
		M::Popup("Af&ter Playback", after)};
}

std::vector<MenuItem> MainFrame::NavigateMenu() const
{
	using M = MenuItem;
	const bool loaded = Loaded();
	std::vector<M> v = {M::Cmd("&Previous", ID_NAVIGATE_SKIPBACK, Accel(PadAction::Previous)).Enable(loaded),
		M::Cmd("&Next", ID_NAVIGATE_SKIPFORWARD, Accel(PadAction::Next)).Enable(loaded),
		M::Cmd("&Go To...", ID_NAVIGATE_GOTO, Accel(PadAction::GoTo)).Enable(loaded && player_.Seekable()), M::Sep(),
		M::Cmd("&Title Menu", ID_NAVIGATE_TITLEMENU).Enable(false), M::Cmd("&Root Menu", ID_NAVIGATE_ROOTMENU).Enable(false),
		M::Cmd("&Subtitle Menu", ID_NAVIGATE_SUBPICTUREMENU).Enable(false), M::Cmd("&Audio Menu", ID_NAVIGATE_AUDIOMENU).Enable(false),
		M::Cmd("An&gle Menu", ID_NAVIGATE_ANGLEMENU).Enable(false), M::Cmd("&Chapter Menu", ID_NAVIGATE_CHAPTERMENU).Enable(false)};
	// a DVD-Video / Blu-ray: its titles (the disc's menus aren't shown: the main title plays first)
	const std::vector<DiscTitle> titles = player_.Titles();
	if (!titles.empty())
	{
		std::vector<M> tm;
		const int cur = player_.CurrentTitle();
		for (size_t i = 0; i < titles.size() && i < 99; i++)
			tm.push_back(M::Shown(Fmt(T("Title %d"), titles[i].number) + "  [" + FormatTime(titles[i].seconds) + "]",
				ID_TITLE_FIRST + int(i)).Radio(titles[i].number == cur));
		v.push_back(M::Sep());
		v.push_back(M::Popup("&Titles", tm));
	}
	const std::vector<Chapter> ch = player_.Chapters();
	if (!ch.empty())
	{
		std::vector<M> chm;
		const double pos = player_.Position();
		int cur = -1;
		for (int i = 0; i < int(ch.size()); i++)
			if (ch[i].time <= pos + 0.01)
				cur = i;
		for (int i = 0; i < int(ch.size()) && i < 999; i++)
			chm.push_back(M::Cmd(std::to_string(i + 1) + ". " + ch[i].name + "  [" + FormatTime(ch[i].time) + "]", ID_CHAPTER_FIRST + i).Radio(i == cur));
		v.push_back(M::Sep());
		v.push_back(M::Popup("C&hapters", chm));
	}
	return v;
}

std::vector<MenuItem> MainFrame::FavoritesMenu() const
{
	using M = MenuItem;
	std::vector<M> v = {M::Cmd("&Add to Favorites...", ID_FAVORITES_ADD).Enable(Loaded()),
		M::Cmd("&Organize Favorites...", ID_FAVORITES_ORGANIZE), M::Sep()};
	for (size_t i = 0; i < s_.favorites.size(); i++)
	{
		std::string name = s_.favorites[i].name;
		if (s_.favorites[i].pos > 0)
			name += "\t[" + FormatTime(s_.favorites[i].pos) + "]";
		M item = M::Cmd(s_.favorites[i].name, ID_FAVORITE_FIRST + int(i));
		if (s_.favorites[i].pos > 0)
			item.accel = FormatTime(s_.favorites[i].pos);
		v.push_back(item);
	}
	if (s_.favorites.empty())
		v.push_back(M::Cmd("<empty>", 0).Enable(false));
	return v;
}

std::vector<MenuItem> MainFrame::HelpMenu() const
{
	using M = MenuItem;
	return {M::Cmd("&Home Page", ID_HELP_HOMEPAGE), M::Cmd("Check for &updates", ID_HELP_CHECKFORUPDATE),
		M::Cmd("&Command Line Switches", ID_HELP_SHOWCOMMANDLINESWITCHES).Enable(false),
		M::Cmd("Download &Toolbar Images", ID_HELP_TOOLBARIMAGES).Enable(false), M::Cmd("&Donate", ID_HELP_DONATE).Enable(false),
		M::Sep(), M::Cmd("&About...", ID_HELP_ABOUT)};
}

std::vector<MenuItem> MainFrame::BuildMenu(int which) const
{
	using M = MenuItem;
	switch (which)
	{
		case 0:
			return FileMenu();
		case 1:
			return ViewMenu(false);
		case 2:
			return PlayMenu();
		case 3:
			return NavigateMenu();
		case 4:
			return FavoritesMenu();
		case 5:
			return HelpMenu();
		default:
			break;
	}
	// IDR_POPUP: the context menu
	const bool loaded = Loaded();
	const std::vector<M> play = PlayMenu();
	const std::vector<M> view = ViewMenu(false);
	std::vector<M> v;
	v.push_back(M::Popup("&File", FileMenu()));
	v.push_back(M::Sep());
	v.push_back(M::Cmd("&Play/Pause", ID_PLAY_PLAYPAUSE, Accel(PadAction::PlayPause)).Enable(loaded));
	v.push_back(M::Cmd("&Stop", ID_PLAY_STOP).Enable(loaded));
	v.push_back(play[3]); // Repeat
	v.push_back(M::Sep());
	v.push_back(M::Cmd("F&ull Screen", ID_VIEW_FULLSCREEN, Accel(PadAction::FullScreen)).Check(Fullscreen()));
	v.push_back(view[14]); // Zoom
	v.push_back(M::Sep());
	v.push_back(view[17]); // Video Frame
	v.push_back(view[18]); // Pan&Scan
	v.push_back(M::Sep());
	v.push_back(M::Popup("&Navigate", NavigateMenu()));
	v.push_back(M::Popup("F&avorites", FavoritesMenu()));
	v.push_back(M::Sep());
	v.push_back(play[9]); // Filters
	v.push_back(play[10]); // Shaders
	v.push_back(M::Sep());
	v.push_back(play[12]); // Audio Track
	v.push_back(play[13]); // Subtitle Track
	v.push_back(play[14]); // Video Track
	v.push_back(M::Sep());
	v.push_back(play[16]); // Volume
	v.push_back(play[17]); // After Playback
	v.push_back(M::Sep());
	v.push_back(M::Popup("&View", ViewMenu(true)));
	v.push_back(view[16]); // Renderer Settings
	v.push_back(M::Cmd("P&roperties", ID_FILE_PROPERTIES).Enable(loaded));
	v.push_back(M::Cmd("&Options...", ID_VIEW_OPTIONS, Accel(PadAction::OptionsWindow)));
	v.push_back(M::Sep());
	v.push_back(M::Cmd("E&xit", ID_FILE_EXIT));
	return v;
}

void MainFrame::OpenMenuBar(int i, bool keyboard)
{
	if (r_menubar_.IsRectEmpty())
	{
		// no menu bar (Caption&Menu off, full screen): the context menu in the middle
		TrackPopupMenu(BuildMenu(-1), kW / 2 - ScaleX(120), kH / 3, [this](int id) {
			if (id)
				Command(id);
		}, nullptr, keyboard);
		return;
	}
	const int n = int(menu_items_.size());
	i = (i % n + n) % n;
	menubar_open_ = i;
	menu_keyboard_ = keyboard;
	const CRect& r = menu_items_[i];
	TrackPopupMenu(BuildMenu(i), r.left, r.bottom,
		[this](int id) {
			menubar_open_ = -1;
			menu_keyboard_ = false;
			if (id)
				Command(id);
		},
		[this, i, keyboard](int dir) {
			menubar_open_ = -1;
			OpenMenuBar(i + dir, true);
			(void)keyboard;
		},
		keyboard);
}

// ===========================================================================================================
// commands
void MainFrame::SetVolume(int v)
{
	s_.volume = std::max(0, std::min(100, v));
	ApplyAudio();
}

void MainFrame::ApplyAudio()
{
	player_.SetVolume(s_.volume);
	player_.SetMute(s_.mute);
	player_.SetAmplify(1.0 + s_.audio_boost / 100.0);
	player_.SetNormalize(s_.audio_normalize, s_.audio_max_norm, s_.audio_norm_regain);
	// Options > Output: the next file opened uses the console's decoder or not; the rest applies at once
	hwdec::SetEnabled(s_.hw_decoding);
	player_.SetToneMapping(s_.hdr_tonemap, s_.hdr_peak);
	player_.SetSurround(s_.audio_output == 1);
}

void MainFrame::Seek(double t)
{
	if (!Loaded() || !player_.Seekable())
		return;
	const double dur = player_.Duration();
	if (dur > 0)
		t = std::min(t, dur);
	t = std::max(0.0, t);
	player_.Seek(t, true);
	Osd(FormatTime(t, dur >= 3600) + (dur > 0 ? " / " + FormatTime(dur, dur >= 3600) : ""));
}

void MainFrame::Jump(double d)
{
	Seek(player_.Position() + d);
}

void MainFrame::SkipBack()
{
	// chapters first, then the playlist (CMainFrame::OnNavigateSkip)
	const std::vector<Chapter> ch = player_.Chapters();
	const double pos = player_.Position();
	if (!ch.empty())
	{
		for (int i = int(ch.size()) - 1; i >= 0; i--)
			if (ch[i].time < pos - 3)
			{
				Seek(ch[i].time);
				Osd(Fmt(T("Chapter %d: %s"), i + 1, ch[i].name.c_str()));
				return;
			}
	}
	if (playlist_.size() > 1 && pl_cur_ > 0)
		OpenPlaylistItem(pl_cur_ - 1);
	else if (!current_url_.empty())
	{
		// the previous file in the folder
		const std::vector<std::string> files = ListFolderMedia(ParentFolder(current_url_));
		auto it = std::find(files.begin(), files.end(), current_url_);
		if (it != files.end() && it != files.begin())
			Open(*(it - 1));
		else
			Seek(0);
	}
}

void MainFrame::SkipForward()
{
	const std::vector<Chapter> ch = player_.Chapters();
	const double pos = player_.Position();
	for (int i = 0; i < int(ch.size()); i++)
		if (ch[i].time > pos + 1)
		{
			Seek(ch[i].time);
			Osd(Fmt(T("Chapter %d: %s"), i + 1, ch[i].name.c_str()));
			return;
		}
	if (playlist_.size() > 1 && pl_cur_ + 1 < int(playlist_.size()))
		OpenPlaylistItem(pl_cur_ + 1);
	else if (!current_url_.empty())
	{
		const std::vector<std::string> files = ListFolderMedia(ParentFolder(current_url_));
		auto it = std::find(files.begin(), files.end(), current_url_);
		if (it != files.end() && it + 1 != files.end())
			Open(*(it + 1));
		else
			Osd(T("No more media in the current folder."));
	}
}

void MainFrame::Command(int id)
{
	Settings& s = s_;
	const bool loaded = Loaded();
	if (id >= ID_RECENT_FIRST && id < ID_RECENT_FIRST + 20)
	{
		const int i = id - ID_RECENT_FIRST;
		if (i < int(s.recent.size()))
		{
			const std::string url = s.recent[i];
			playlist_ = {url};
			pl_durations_.assign(1, 0);
			pl_cur_ = 0;
			Open(url);
		}
		return;
	}
	if (id >= ID_AUDIO_FIRST && id < ID_AUDIO_FIRST + 999)
	{
		player_.SelectTrack(TrackType::Audio, id - ID_AUDIO_FIRST);
		return;
	}
	if (id >= ID_SUBTITLE_FIRST && id < ID_SUBTITLE_FIRST + 1999)
	{
		player_.SelectTrack(TrackType::Subtitle, id - ID_SUBTITLE_FIRST);
		subs_enabled_ = true;
		return;
	}
	if (id >= ID_VIDEO_FIRST && id < ID_VIDEO_FIRST + 999)
		return; // one video track at a time (the default one)
	if (id >= ID_CHAPTER_FIRST && id < ID_CHAPTER_FIRST + 999)
	{
		const std::vector<Chapter> ch = player_.Chapters();
		const int i = id - ID_CHAPTER_FIRST;
		if (i < int(ch.size()))
			Seek(ch[i].time);
		return;
	}
	if (id >= ID_DISC_FIRST && id < ID_DISC_FIRST + 8)
	{
		// File > Open Disc: a DVD-Video / Blu-ray plays (its main title); a data disc opens in the Open dialog
		const std::vector<disc::DriveState> drives = disc::Drives();
		const size_t i = size_t(id - ID_DISC_FIRST);
		if (i < drives.size())
		{
			const disc::DriveState& d = drives[i];
			const std::string u = disc::PlayUrl(d);
			if (!u.empty())
				OpenAny(u);
			else if (d.kind == disc::Kind::Data)
			{
				s_.last_dir = "disc://" + d.id;
				Command(ID_FILE_OPENQUICK);
			}
			else
				MessageBox(disc::DriveMessage(d), T("Open Disc"), MB_OK, 2);
		}
		return;
	}
	if (id >= ID_TITLE_FIRST && id < ID_TITLE_FIRST + 99)
	{
		// Navigate > Titles: the same disc, another title
		const std::vector<DiscTitle> titles = player_.Titles();
		const size_t i = size_t(id - ID_TITLE_FIRST);
		if (i < titles.size() && disc::IsTitleUrl(current_url_))
		{
			const std::string u = disc::TitleBase(current_url_) + "?title=" + std::to_string(titles[i].number);
			if (pl_cur_ >= 0 && pl_cur_ < int(playlist_.size()))
				playlist_[pl_cur_] = u;
			Open(u, false);
		}
		return;
	}
	if (id >= ID_FAVORITE_FIRST && id < ID_FAVORITE_FIRST + 999)
	{
		const int i = id - ID_FAVORITE_FIRST;
		if (i < int(s.favorites.size()))
		{
			const Favorite f = s.favorites[i];
			playlist_ = {f.url};
			pl_durations_.assign(1, 0);
			pl_cur_ = 0;
			SavePosition();
			current_url_ = f.url;
			player_.Open(f.url, f.pos);
			s.AddRecent(f.url);
			opened_ok_ = false;
		}
		return;
	}
	switch (id)
	{
		case ID_FILE_OPENQUICK:
			ShowFileBrowser(T("Open"), BrowseFilter::Media, [this](const std::string& path) {
				if (path.empty())
					return;
				s_.last_dir = ParentFolder(path);
				OpenAny(path);
			});
			break;
		case ID_FILE_OPENMEDIA:
			ShowOpenDialog([this](const std::string& url, const std::string& dub, bool add_only) {
				if (url.empty())
					return;
				if (add_only)
				{
					playlist_.push_back(url);
					pl_durations_.push_back(0);
					s_.show_playlist = true;
					return;
				}
				if (IsPlaylistFile(url) && url.find("://") == std::string::npos)
				{
					LoadPlaylistFile(url);
					return;
				}
				playlist_ = {url};
				pl_durations_.assign(1, 0);
				pl_cur_ = 0;
				Open(url);
				(void)dub;
			});
			break;
		case ID_FILE_OPENDVDBD:
			// a disc drive, or a folder with a DVD-Video (VIDEO_TS) or a Blu-ray (BDMV) in it
			ShowFileBrowser(T("Open DVD/BD"), BrowseFilter::Folder, [this](const std::string& dir) {
				if (dir.empty())
					return;
				const std::string u = disc::TitleUrlForFolder(dir);
				if (u.empty())
				{
					MessageBox(T("This folder has no DVD-Video (VIDEO_TS) or Blu-ray (BDMV) to play."), T("Open DVD/BD"), MB_OK, 2);
					return;
				}
				OpenAny(u);
			});
			break;
		case ID_FILE_OPENDIRECTORY:
			ShowFileBrowser(T("Open Directory"), BrowseFilter::Folder, [this](const std::string& dir) {
				if (dir.empty())
					return;
				const std::vector<std::string> files = ListFolderMedia(dir);
				if (files.empty())
				{
					MessageBox(T("There are no media files in this folder."), T("Open Directory"), MB_OK, 2);
					return;
				}
				playlist_ = files;
				pl_durations_.assign(files.size(), 0);
				s_.last_dir = dir;
				OpenPlaylistItem(0);
			});
			break;
		case ID_FILE_CLOSE_AND_RESTORE:
			CloseMedia();
			break;
		case ID_FILE_SAVE_IMAGE:
		{
			if (!loaded)
				break;
			char stamp[64];
			const time_t now = time(nullptr);
			struct tm tmv;
			localtime_r(&now, &tmv);
			strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tmv);
			std::string base = FileName(current_url_);
			const size_t dot = base.find_last_of('.');
			if (dot != std::string::npos)
				base = base.substr(0, dot);
			const std::string path = OrbisDir("screenshots") + "/" + base + "_snapshot_" +
									 FormatTime(player_.Position(), true) + "_[" + stamp + "].png";
			std::string fixed = path;
			std::replace(fixed.begin() + long(OrbisDir("screenshots").size() + 1), fixed.end(), ':', '.');
			std::string err;
			if (player_.SavePicture(fixed, &err))
				Osd(Fmt(T("Image saved: %s"), FileName(fixed).c_str()), 3);
			else
				MessageBox(T(err), T("Save Image"), MB_OK, 3);
			break;
		}
		case ID_FILE_SUBTITLES_LOAD:
			ShowFileBrowser(T("Open subtitle"), BrowseFilter::Subtitles, [this](const std::string& path) {
				if (path.empty())
					return;
				std::string err;
				if (player_.AddSubtitleFile(path, &err))
				{
					subs_enabled_ = true;
					Osd(Fmt(T("Subtitles loaded: %s"), FileName(path).c_str()));
				}
				else
					MessageBox(Fmt(T("Failed to load subtitle file \"%s\":\n%s"), FileName(path).c_str(), T(err).c_str()),
						"Media Player Classic Home Cinema", MB_OK, 3);
			});
			break;
		case ID_FILE_PROPERTIES:
			if (loaded)
				ShowPropertiesDialog(player_, current_url_);
			break;
		case ID_FILE_EXIT:
			quit_ = true;
			break;
		case ID_RECENT_FILES_CLEAR:
			s.recent.clear();
			break;
		case ID_VIEW_CAPTIONMENU:
			s.show_caption_menu = !s.show_caption_menu;
			break;
		case ID_VIEW_SEEKER:
			s.show_seekbar = !s.show_seekbar;
			break;
		case ID_VIEW_CONTROLS:
			s.show_controls = !s.show_controls;
			break;
		case ID_VIEW_INFORMATION:
			s.show_info = !s.show_info;
			break;
		case ID_VIEW_STATISTICS:
			s.show_stats = !s.show_stats;
			break;
		case ID_VIEW_STATUS:
			s.show_status = !s.show_status;
			break;
		case ID_VIEW_PLAYLIST:
			s.show_playlist = !s.show_playlist;
			pl_focus_ = s.show_playlist;
			if (pl_sel_ < 0)
				pl_sel_ = std::max(0, pl_cur_);
			break;
		case ID_VIEW_PRESETS_MINIMAL:
			s.show_caption_menu = false;
			s.show_seekbar = s.show_controls = s.show_info = s.show_stats = s.show_status = false;
			break;
		case ID_VIEW_PRESETS_COMPACT:
			s.show_caption_menu = true;
			s.show_seekbar = true;
			s.show_controls = s.show_info = s.show_stats = s.show_status = false;
			break;
		case ID_VIEW_PRESETS_NORMAL:
			s.show_caption_menu = s.show_seekbar = s.show_controls = s.show_status = true;
			s.show_info = s.show_stats = false;
			break;
		case ID_VIEW_FULLSCREEN:
			s.fullscreen = !s.fullscreen;
			break;
		case ID_VIEW_ZOOM_50:
			s.video_frame = VideoFrame::HalfSize;
			break;
		case ID_VIEW_ZOOM_100:
			s.video_frame = VideoFrame::NormalSize;
			break;
		case ID_VIEW_ZOOM_200:
			s.video_frame = VideoFrame::DoubleSize;
			break;
		case ID_VIEW_ZOOM_AUTOFIT:
		case ID_VIEW_ZOOM_AUTOFIT_LARGER:
			s.video_frame = VideoFrame::FromInside;
			break;
		case ID_VIEW_DISPLAY_RENDERER_STATS:
			show_renderer_stats_ = !show_renderer_stats_;
			break;
		case ID_VIEW_OSD_DISPLAY_TIME:
			Osd(TimeText(), 3);
			break;
		case ID_VIEW_OSD_SHOW_FILENAME:
			Osd(FileName(current_url_), 3);
			break;
		case ID_VIEW_VF_HALF:
		case ID_VIEW_VF_NORMAL:
		case ID_VIEW_VF_DOUBLE:
		case ID_VIEW_VF_STRETCH:
		case ID_VIEW_VF_FROMINSIDE:
		case ID_VIEW_VF_ZOOM1:
		case ID_VIEW_VF_ZOOM2:
		case ID_VIEW_VF_FROMOUTSIDE:
			s.video_frame = VideoFrame(id - ID_VIEW_VF_HALF);
			break;
		case ID_ASPECTRATIO_DAR:
		case ID_ASPECTRATIO_4_3:
		case ID_ASPECTRATIO_5_4:
		case ID_ASPECTRATIO_16_9:
		case ID_ASPECTRATIO_235_100:
		case ID_ASPECTRATIO_185_100:
		case ID_ASPECTRATIO_SAR:
		{
			s.aspect_ratio = id - ID_ASPECTRATIO_DAR;
			static const char* const kNames[] = {"Default (DAR)", "4:3", "5:4", "16:9", "235:100", "185:100", "Square pixels (SAR)"};
			Osd(Fmt(T("Aspect Ratio: %s"), T(kNames[s.aspect_ratio])));
			break;
		}
		case ID_VIEW_INCSIZE:
			s.zoom_x *= 1.02;
			s.zoom_y *= 1.02;
			break;
		case ID_VIEW_DECSIZE:
			s.zoom_x /= 1.02;
			s.zoom_y /= 1.02;
			break;
		case ID_VIEW_INCWIDTH:
			s.zoom_x *= 1.02;
			break;
		case ID_VIEW_DECWIDTH:
			s.zoom_x /= 1.02;
			break;
		case ID_VIEW_INCHEIGHT:
			s.zoom_y *= 1.02;
			break;
		case ID_VIEW_DECHEIGHT:
			s.zoom_y /= 1.02;
			break;
		case ID_PANSCAN_MOVERIGHT:
			s.pan_x = std::min(1.0, s.pan_x + 0.02);
			break;
		case ID_PANSCAN_MOVELEFT:
			s.pan_x = std::max(-1.0, s.pan_x - 0.02);
			break;
		case ID_PANSCAN_MOVEUP:
			s.pan_y = std::max(-1.0, s.pan_y - 0.02);
			break;
		case ID_PANSCAN_MOVEDOWN:
			s.pan_y = std::min(1.0, s.pan_y + 0.02);
			break;
		case ID_PANSCAN_CENTER:
			s.pan_x = s.pan_y = 0;
			break;
		case ID_VIEW_RESET:
			s.pan_x = s.pan_y = 0;
			s.zoom_x = s.zoom_y = 1;
			break;
		case ID_VIEW_OPTIONS:
			ShowOptions(OptionsPage::Last);
			break;
		case ID_PLAY_PLAYPAUSE:
			if (player_.State() == PlayState::Playing)
			{
				player_.Pause();
				Osd(T("Pause"));
			}
			else if (loaded)
			{
				player_.Play();
				Osd(T("Play"));
			}
			else if (!current_url_.empty() && player_.State() != PlayState::Opening)
				Open(current_url_);
			break;
		case ID_PLAY_PLAY:
			if (loaded)
			{
				player_.Play();
				Osd(T("Play"));
			}
			break;
		case ID_PLAY_PAUSE:
			if (loaded)
			{
				player_.Pause();
				Osd(T("Pause"));
			}
			break;
		case ID_PLAY_STOP:
			if (loaded)
			{
				player_.Stop();
				Osd(T("Stop"));
			}
			break;
		case ID_PLAY_FRAMESTEP:
			if (loaded)
				player_.FrameStep();
			break;
		case ID_PLAY_REPEAT_FOREVER:
			s.repeat = s.repeat ? 0 : 1;
			break;
		case ID_PLAY_REPEAT_ONEFILE:
			s.repeat = 1;
			break;
		case ID_PLAY_REPEAT_WHOLEPLAYLIST:
			s.repeat = 2;
			break;
		case ID_PLAY_DECRATE:
		case ID_PLAY_INCRATE:
		case ID_PLAY_RESETRATE:
		{
			// CMainFrame::OnPlayChangeRate / SetPlayingRate (a file, MPC-HC's default "auto" step): twice / half as fast;
			// under 0.125x it pauses instead; Reset Rate: 1x. Paused or stopped: it plays. The sound keeps its pitch
			// (FFmpeg's atempo). The fastest here: 8x (the decoders' limit on the console)
			if (!loaded)
				break;
			{
				const double want = id == ID_PLAY_RESETRATE ? 1.0 : id == ID_PLAY_INCRATE ? rate_ * 2 : rate_ / 2;
				if (want < 0.125)
				{
					if (player_.State() != PlayState::Paused)
						Command(ID_PLAY_PAUSE);
					break;
				}
				// as MPC-HC's SetPlayingRate: not running (paused, stopped), it plays
				if (player_.State() == PlayState::Paused || player_.State() == PlayState::Stopped)
					Command(ID_PLAY_PLAY);
				rate_ = std::min(8.0, want);
				player_.SetRate(rate_);
				OrbisLog("[ui] rate %.3fx", rate_);
				char b[64];
				snprintf(b, sizeof(b), T("Speed: %.2fx"), rate_);
				Osd(b);
			}
			break;
		}
		case ID_VOLUME_UP:
			// CVolumeCtrl::IncreaseVolume: up to the next step
			SetVolume(s.volume + 5 - s.volume % 5);
			Osd(Fmt(T("Volume: %d%%"), s.volume));
			break;
		case ID_VOLUME_DOWN:
			SetVolume(s.volume % 5 ? s.volume - s.volume % 5 : s.volume - 5);
			Osd(Fmt(T("Volume: %d%%"), s.volume));
			break;
		case ID_VOLUME_MUTE:
			s.mute = !s.mute;
			ApplyAudio();
			Osd(T(s.mute ? "Volume: Off" : "Volume: On"));
			break;
		case ID_AFTERPLAYBACK_DONOTHING:
			s.after_playback = AfterPlayback::DoNothing;
			break;
		case ID_AFTERPLAYBACK_PLAYNEXT:
			s.after_playback = AfterPlayback::PlayNext;
			break;
		case ID_AFTERPLAYBACK_EXIT:
			s.after_playback = AfterPlayback::Exit;
			break;
		case ID_SUBTITLES_ENABLE:
			subs_enabled_ = !subs_enabled_;
			if (subs_enabled_ && player_.CurrentTrack(TrackType::Subtitle) < 0)
			{
				const std::vector<Track> t = player_.Tracks(TrackType::Subtitle);
				if (!t.empty())
					player_.SelectTrack(TrackType::Subtitle, t.front().id);
			}
			break;
		case ID_SUB_DELAY_UP:
		case ID_SUB_DELAY_DOWN:
		case ID_SUB_DELAY_RESET:
		{
			s.sub_delay_ms = id == ID_SUB_DELAY_RESET ? 0 : s.sub_delay_ms + (id == ID_SUB_DELAY_UP ? s.sub_delay_step : -s.sub_delay_step);
			s.sub_delay_ms = std::max(-60000, std::min(60000, s.sub_delay_ms));
			player_.SetSubtitleDelay(s.sub_delay_ms / 1000.0);
			Osd(Fmt(T("Subtitle delay: %d ms"), s.sub_delay_ms));
			break;
		}
		case ID_AUDIO_DELAY_UP:
		case ID_AUDIO_DELAY_DOWN:
			s.audio_delay_ms += id == ID_AUDIO_DELAY_UP ? 10 : -10;
			player_.SetAudioDelay(s.audio_delay_ms / 1000.0);
			Osd(Fmt(T("Audio delay: %d ms"), s.audio_delay_ms));
			break;
		case ID_NAVIGATE_SKIPBACK:
			SkipBack();
			break;
		case ID_NAVIGATE_SKIPFORWARD:
			SkipForward();
			break;
		case ID_NAVIGATE_GOTO:
			if (loaded && player_.Seekable())
				ShowGoToDialog(player_.Position(), player_.Stats().fps, [this](double t) { Seek(t); });
			break;
		case ID_FAVORITES_ADD:
			if (loaded)
			{
				std::string name = player_.Title();
				if (name.empty())
					name = FileName(current_url_);
				ShowAddFavoriteDialog(name, [this](const std::string& n, bool remember) {
					Favorite f;
					f.name = n;
					f.url = current_url_;
					f.pos = remember ? player_.Position() : 0;
					s_.favorites.push_back(f);
					s_.Save();
				});
			}
			break;
		case ID_FAVORITES_ORGANIZE:
			ShowOrganizeFavoritesDialog();
			break;
		case ID_HELP_HOMEPAGE:
			MessageBox(T("MPC-HC's home page is https://mpc-hc.org/ and its code is at https://github.com/mpc-hc/mpc-hc.\n\n"
						 "This PS5 port: https://github.com/MisterTemaki"),
				T("Home Page"), MB_OK, 1);
			break;
		case ID_HELP_ABOUT:
			ShowAboutDialog();
			break;
		case ID_HELP_CHECKFORUPDATE:
			CheckForUpdates();
			break;
		case ID_PLAYLIST_REMOVE:
			if (pl_sel_ >= 0 && pl_sel_ < int(playlist_.size()))
			{
				playlist_.erase(playlist_.begin() + pl_sel_);
				if (pl_sel_ < int(pl_durations_.size()))
					pl_durations_.erase(pl_durations_.begin() + pl_sel_);
				if (pl_cur_ == pl_sel_)
					pl_cur_ = -1;
				else if (pl_cur_ > pl_sel_)
					pl_cur_--;
				pl_sel_ = std::min(pl_sel_, int(playlist_.size()) - 1);
			}
			break;
		case ID_PLAYLIST_CLEAR:
			playlist_.clear();
			pl_durations_.clear();
			pl_cur_ = pl_sel_ = -1;
			break;
		default:
			break;
	}
}

// ===========================================================================================================
// opening
void MainFrame::SavePosition()
{
	if (current_url_.empty() || !s_.remember_file_pos || !Loaded() || disc::IsTitleUrl(current_url_))
		return;
	const double pos = player_.Position(), dur = player_.Duration();
	// near the end (or the start): forget it, as MPC-HC does at the end of a file
	if (dur > 0 && (pos > dur - std::max(1.0, std::min(5.0, dur * 0.05)) || pos < 5))
		s_.SetFilePos(current_url_, 0);
	else if (dur > 0)
		s_.SetFilePos(current_url_, pos);
}

void MainFrame::Open(const std::string& url, bool use_saved_pos)
{
	SavePosition();
	MPCPS5_STAGE(Emu, "open");
	current_url_ = url;
	// (not for a disc's titles: dvd://cd0 names the drive, not the disc in it)
	const double pos = use_saved_pos && s_.remember_file_pos && !disc::IsTitleUrl(url) ? s_.FilePos(url) : 0;
	player_.Open(url, pos);
	opened_ok_ = false;
	sub_cache_.key.clear();
	rate_ = 1.0;
	s_.AddRecent(url);
	if (pl_cur_ < 0 || pl_cur_ >= int(playlist_.size()) || playlist_[pl_cur_] != url)
	{
		auto it = std::find(playlist_.begin(), playlist_.end(), url);
		if (it == playlist_.end())
		{
			playlist_.push_back(url);
			pl_durations_.push_back(0);
			it = playlist_.end() - 1;
		}
		pl_cur_ = int(it - playlist_.begin());
	}
	pl_sel_ = pl_cur_;
	OrbisLog("[ui] open %s at %.1f s", url.c_str(), pos);
}

void MainFrame::OpenPlaylistItem(int i)
{
	if (i < 0 || i >= int(playlist_.size()))
		return;
	pl_cur_ = i;
	Open(playlist_[i]);
}

void MainFrame::CloseMedia()
{
	SavePosition();
	player_.Close();
	current_url_.clear();
	video_layer_rect_ = CRect();
	sub_cache_.key.clear();
	s_.Save();
}

void MainFrame::LoadPlaylistFile(const std::string& path)
{
	// .m3u / .m3u8 / .pls: one entry per line (relative paths are next to the list)
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
	{
		MessageBox(Fmt(T("Can't open %s"), path.c_str()), T("Open"), MB_OK, 3);
		return;
	}
	const std::string dir = ParentFolder(path);
	std::vector<std::string> items;
	char line[4096];
	bool hls = false;
	while (fgets(line, sizeof(line), f))
	{
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' '))
			l.pop_back();
		if (l.size() >= 3 && (unsigned char)l[0] == 0xef)
			l = l.substr(3);
		if (l.compare(0, 7, "#EXT-X-") == 0)
			hls = true;
		if (l.empty() || l[0] == '#' || l[0] == '[')
			continue;
		const size_t eq = l.find('=');
		if (l.compare(0, 4, "File") == 0 && eq != std::string::npos)
			l = l.substr(eq + 1);
		else if (eq != std::string::npos && l.find("://") == std::string::npos)
			continue;
		if (l.find("://") == std::string::npos && l[0] != '/')
			l = dir + "/" + l;
		items.push_back(l);
	}
	fclose(f);
	if (hls || items.empty())
	{
		// an HLS stream saved as a file: FFmpeg plays it
		playlist_ = {path};
		pl_durations_.assign(1, 0);
		pl_cur_ = 0;
		Open(path);
		return;
	}
	playlist_ = items;
	pl_durations_.assign(items.size(), 0);
	OpenPlaylistItem(0);
}

// ---- Help, Check for updates -----------------------------------------------------------------------------
void MainFrame::CheckForUpdates()
{
	const update::Status st = update::GetStatus();
	switch (st.phase)
	{
		case update::Phase::Available: OfferUpdate(st); break;
		case update::Phase::Done: upd_seen_ = update::Phase::Idle; break; // TickUpdate asks again about the restart
		case update::Phase::Checking:
		case update::Phase::Downloading:
		case update::Phase::Installing: Osd(T("The update is on its way..."), 2); break;
		default:
			upd_user_ = true;
			update::StartCheck(false);
			Osd(T("Checking for updates..."), 3);
			break;
	}
}

void MainFrame::OfferUpdate(const update::Status& st)
{
	char size[32];
	snprintf(size, sizeof(size), "%.1f MB", st.size / 1048576.0);
	if (!have_data_)
	{
		MessageBox(Fmt(T("Media Player Classic PS5 %s is available (you have %s).\n\n"
						 "To install it, the app has to be unlocked: send MediaPlayerClassicPS5-v%s.elf with your payload "
						 "loader, then open the app again."),
					   st.latest.c_str(), MPCPS5_PS5_VERSION, MPCPS5_PS5_VERSION),
			T("Check for updates"), MB_OK, 1);
		return;
	}
	// the copy the console runs can't take it (an image, a read-only folder): said before anything is downloaded.
	// Looked for again if the helper didn't say at start (1.5.1), only when the user asked (Help, Check for updates):
	// asking a helper that was silent at start may wait up to 5 s, which the daily check at start mustn't
	if (upd_user_)
		mpcupdate::RefreshPlace();
	const AppPlace& place = mpcupdate::Place();
	if (!place.writable)
	{
		OrbisLog("[update] %s is available, but it can't be installed here: %s", st.latest.c_str(), place.reason.c_str());
		if (place.unsure)
			MessageBox(Fmt(T("Media Player Classic PS5 %s is available (you have %s).\n\nThe app can't tell for sure "
							 "which folder this copy runs from (%s), so it doesn't write an update anywhere. Update it the "
							 "way you put it there, with the new version's folder (from the release's zip)."),
						   st.latest.c_str(), MPCPS5_PS5_VERSION, place.source.c_str()),
				T("Check for updates"), MB_OK, 2);
		else if (place.image)
			MessageBox(Fmt(T("Media Player Classic PS5 %s is available (you have %s).\n\nThis copy runs from an image "
							 "that ShadowMount+ mounted (%s): the app can't write an update into it. Update the image the "
							 "way you made it, with the new version's folder (from the release's zip)."),
						   st.latest.c_str(), MPCPS5_PS5_VERSION, place.source.c_str()),
				T("Check for updates"), MB_OK, 2);
		else
			MessageBox(Fmt(T("Media Player Classic PS5 %s is available (you have %s).\n\nThis copy runs from %s, where the "
							 "app can't write: it can't be updated from here. Update that folder the way you put it there "
							 "(with the new version's folder from the release's zip, or its payload)."),
						   st.latest.c_str(), MPCPS5_PS5_VERSION, place.source.c_str()),
				T("Check for updates"), MB_OK, 2);
		return;
	}
	MessageBox(Fmt(T("Media Player Classic PS5 %s is available (you have %s).\n\nDownload it (%s) and install it now?"),
				   st.latest.c_str(), MPCPS5_PS5_VERSION, size),
		T("Check for updates"), MB_YESNO, 4, [this](int r) {
			if (r == IDYES && update::StartInstall())
			{
				upd_installing_ = true;
				Osd(T("Downloading the update..."), 2);
			}
		});
}

void MainFrame::RestartForUpdate()
{
	const std::string path = mpcupdate::EbootPath();
	OrbisLog("[update] restarting %s", path.c_str());
	SavePosition();
	s_.Save();
	OrbisLogClose();
	const int rc = sceSystemServiceLoadExec(path.c_str(), nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	OrbisLogOpen("boot-after-update");
	OrbisLog("[update] sceSystemServiceLoadExec(%s) -> %x", path.c_str(), unsigned(rc));
	MessageBox(T("Close Media Player Classic PS5 and open it again to use the new version."), T("Check for updates"), MB_OK, 1);
}

void MainFrame::TickUpdate()
{
	const update::Status st = update::GetStatus();
	if (st.phase != upd_seen_)
	{
		upd_seen_ = st.phase;
		switch (st.phase)
		{
			case update::Phase::UpToDate:
				OrbisLog("[update] up to date (%s)", MPCPS5_PS5_VERSION);
				if (upd_user_)
					MessageBox(Fmt(T("You are using the latest version of Media Player Classic PS5 (%s)."), MPCPS5_PS5_VERSION),
						T("Check for updates"), MB_OK, 1);
				upd_user_ = upd_auto_ = false;
				break;
			case update::Phase::Available:
				OrbisLog("[update] %s is available (%llu bytes)%s", st.latest.c_str(), (unsigned long long)st.size,
					upd_user_ || upd_auto_ ? ": offered" : "");
				if (upd_user_ || upd_auto_)
					OfferUpdate(st);
				upd_user_ = upd_auto_ = false;
				break;
			case update::Phase::Failed:
				if (upd_user_ || upd_installing_)
					MessageBox(Fmt(T("Media Player Classic PS5 could not be updated:\n%s"), T(st.error).c_str()) +
								   (upd_installing_ ? T("\n\nNothing was changed.") : ""),
						T("Check for updates"), MB_OK, 3);
				OrbisLog("[update] failed: %s", st.error.c_str());
				upd_user_ = upd_auto_ = upd_installing_ = false;
				break;
			case update::Phase::Done:
				OrbisLog("[update] %s installed (in %s): offering the restart", st.latest.c_str(), mpcupdate::Place().dir.c_str());
				upd_installing_ = false;
				mpcupdate::NoteInstalled(st.latest); // the next start checks it runs
				MessageBox(Fmt(T("Media Player Classic PS5 %s is installed.\n\nRestart the app now to use it?"), st.latest.c_str()),
					T("Check for updates"), MB_YESNO, 4, [this](int r) {
						if (r == IDYES)
							RestartForUpdate();
					});
				break;
			default: break;
		}
	}
	const double now = NowSeconds();
	if ((st.phase == update::Phase::Downloading || st.phase == update::Phase::Installing) && now - upd_osd_ > 0.5)
	{
		upd_osd_ = now;
		char b[96];
		if (st.phase == update::Phase::Downloading)
			snprintf(b, sizeof(b), T("Downloading the update: %d%%"), st.total ? int(st.done * 100 / st.total) : 0);
		else
			snprintf(b, sizeof(b), T("Installing the update: %d/%d"), int(st.done), int(st.total));
		Osd(b, 1.0);
	}
}

void MainFrame::Tick()
{
	TickUpdate();
	TickOptionsButton();
	const PlayState st = player_.State();
	if (st != last_state_)
	{
		if ((st == PlayState::Playing || st == PlayState::Paused) && !opened_ok_)
		{
			opened_ok_ = true;
			ApplyAudio();
			player_.SetSubtitleDelay(s_.sub_delay_ms / 1000.0);
			player_.SetAudioDelay(s_.audio_delay_ms / 1000.0);
			if (pl_cur_ >= 0 && pl_cur_ < int(pl_durations_.size()))
				pl_durations_[pl_cur_] = player_.Duration();
			const double pos = player_.Position();
			if (pos > 1)
				Osd(Fmt(T("Resumed at %s"), FormatTime(pos, player_.Duration() >= 3600).c_str()), 2.5);
			else
				Osd(FileName(current_url_), 2.0);
			s_.Save();
		}
		if (st == PlayState::Failed)
		{
			const std::string err = player_.Error();
			if (err == disc::EncryptedText())
				MessageBox(disc::EncryptedMessage(), "Media Player Classic Home Cinema", MB_OK, 3);
			else
				MessageBox(Fmt(T("Failed to open \"%s\":\n%s"), FileName(current_url_).c_str(), T(err).c_str()),
					"Media Player Classic Home Cinema", MB_OK, 3);
			ProsperoNotify(T("Media Player Classic PS5: could not open %s"), FileName(current_url_).c_str());
			player_.Close();
		}
		last_state_ = st;
	}
	// the disc it plays from taken out (or changed): stopped, and said so
	if (st != PlayState::Closed && st != PlayState::Failed && player_.DiscGeneration() != 0 &&
		(player_.DiscGone() || disc::Generation(current_url_) != player_.DiscGeneration()))
	{
		OrbisLog("[ui] the disc of %s was taken out: stopped", current_url_.c_str());
		player_.Close();
		last_state_ = PlayState::Closed;
		MessageBox(T("The disc was taken out: playback stopped."), "Media Player Classic Home Cinema", MB_OK, 2);
		return;
	}
	// the end of a file
	if (Loaded() && player_.Ended() && st == PlayState::Playing)
	{
		if (s_.remember_file_pos)
			s_.SetFilePos(current_url_, 0);
		if (s_.repeat == 1)
		{
			player_.Seek(0);
			player_.Play();
		}
		else if (pl_cur_ + 1 < int(playlist_.size()))
			OpenPlaylistItem(pl_cur_ + 1);
		else if (s_.repeat == 2 && !playlist_.empty())
			OpenPlaylistItem(0);
		else if (s_.after_playback == AfterPlayback::PlayNext)
		{
			const std::vector<std::string> files = ListFolderMedia(ParentFolder(current_url_));
			auto it = std::find(files.begin(), files.end(), current_url_);
			if (it != files.end() && it + 1 != files.end())
				Open(*(it + 1));
			else
			{
				player_.Pause();
				Osd(T("No more media in the current folder."), 3);
			}
		}
		else if (s_.after_playback == AfterPlayback::Exit)
			quit_ = true;
		else
			player_.Pause();
	}
	const double now = NowSeconds();
	if (Loaded() && now - last_pos_save_ > 10)
	{
		last_pos_save_ = now;
		SavePosition();
	}
}

// ===========================================================================================================
// input
void MainFrame::OnMouseMove(int x, int y)
{
	cap_hot_ = -1;
	for (int i = 0; i < 3; i++)
		if (!r_title_.IsRectEmpty() && cap_btn_[i].PtInRect(x, y))
			cap_hot_ = i;
	menubar_hot_ = -1;
	if (!r_menubar_.IsRectEmpty())
		for (size_t i = 0; i < menu_items_.size(); i++)
			if (menu_items_[i].PtInRect(x, y))
				menubar_hot_ = int(i);
	int what = -1;
	tb_hot_ = -1;
	if (!r_toolbar_.IsRectEmpty() && r_toolbar_.PtInRect(x, y))
	{
		for (int i = 0; i < 13; i++)
			if (tb_btn_[i].PtInRect(x, y) && i != 3 && i != 8 && i != 10 && i != 11)
				tb_hot_ = what = i;
		if (tb_volume_.PtInRect(x, y))
			what = 100;
	}
	seek_hot_ = !r_seek_.IsRectEmpty() && r_seek_.PtInRect(x, y);
	if (seek_hot_)
		what = 200;
	if (what != hover_what_ || what == 200)
	{
		if (what != hover_what_)
			hover_t_ = NowSeconds();
		hover_what_ = what;
	}
	if (seek_drag_)
	{
		seek_drag_pos_ = SeekPosAt(r_seek_, x);
		// MoveThumb: the picture follows while dragging (keyframes, fast)
		player_.Seek(seek_drag_pos_, false);
	}
	if (vol_drag_)
	{
		const CRect ch = VolumeChannel(tb_volume_);
		SetVolume(int(std::lround((x - ch.left) * 100.0 / std::max(1, ch.Width()))));
	}
}

void MainFrame::OnLButtonDown(int x, int y)
{
	for (int i = 0; i < 3; i++)
		if (!r_title_.IsRectEmpty() && cap_btn_[i].PtInRect(x, y))
		{
			cap_pressed_ = i;
			return;
		}
	if (!r_menubar_.IsRectEmpty())
		for (size_t i = 0; i < menu_items_.size(); i++)
			if (menu_items_[i].PtInRect(x, y))
			{
				OpenMenuBar(int(i), false);
				return;
			}
	if (!r_seek_.IsRectEmpty() && r_seek_.PtInRect(x, y))
	{
		if (Loaded() && player_.Duration() > 0 && player_.Seekable())
		{
			seek_drag_ = true;
			seek_drag_pos_ = SeekPosAt(r_seek_, x);
			player_.Seek(seek_drag_pos_, false);
		}
		return;
	}
	if (!r_toolbar_.IsRectEmpty() && r_toolbar_.PtInRect(x, y))
	{
		if (tb_volume_.PtInRect(x, y))
		{
			vol_drag_ = true;
			OnMouseMove(x, y);
			return;
		}
		for (int i = 0; i < 13; i++)
			if (tb_btn_[i].PtInRect(x, y) && ButtonEnabled(i) && i != 3 && i != 8 && i != 10 && i != 11)
				tb_pressed_ = i;
		return;
	}
	if (!r_playlist_.IsRectEmpty() && r_playlist_.PtInRect(x, y))
	{
		pl_focus_ = true;
		if (y < r_playlist_.top + ScaleY(20) && x > r_playlist_.right - ScaleX(22))
		{
			s_.show_playlist = false;
			pl_focus_ = false;
			return;
		}
		const CRect lr = PlaylistList();
		const int i = pl_top_ + (y - lr.top - 1) / PlaylistRowH();
		if (lr.PtInRect(x, y) && i >= 0 && i < int(playlist_.size()))
		{
			if (pl_sel_ == i)
				OpenPlaylistItem(i); // a second click plays it
			pl_sel_ = i;
		}
		return;
	}
	if (r_video_area_.PtInRect(x, y))
	{
		pl_focus_ = false;
		// MPC-HC's default mouse: a click plays/pauses, a double click toggles full screen
		const double now = NowSeconds();
		if (now - last_click_t_ < 0.4)
		{
			Command(ID_VIEW_FULLSCREEN);
			last_click_t_ = 0;
		}
		else
		{
			last_click_t_ = now;
			if (Loaded())
				Command(ID_PLAY_PLAYPAUSE);
		}
	}
}

void MainFrame::OnLButtonUp(int x, int y)
{
	if (cap_pressed_ >= 0)
	{
		const int b = cap_pressed_;
		cap_pressed_ = -1;
		if (cap_btn_[b].PtInRect(x, y))
		{
			if (b == 1)
				Command(ID_VIEW_FULLSCREEN);
			else if (b == 2)
				Command(ID_FILE_EXIT);
		}
	}
	if (seek_drag_)
	{
		seek_drag_ = false;
		player_.Seek(seek_drag_pos_, true);
	}
	vol_drag_ = false;
	if (tb_pressed_ >= 0)
	{
		const int b = tb_pressed_;
		tb_pressed_ = -1;
		if (tb_btn_[b].PtInRect(x, y))
		{
			static const int kCmds[13] = {ID_PLAY_PLAY, ID_PLAY_PAUSE, ID_PLAY_STOP, 0, ID_NAVIGATE_SKIPBACK, ID_PLAY_DECRATE,
				ID_PLAY_INCRATE, ID_NAVIGATE_SKIPFORWARD, 0, ID_PLAY_FRAMESTEP, 0, 0, ID_VOLUME_MUTE};
			if (kCmds[b])
				Command(kCmds[b]);
		}
	}
}

void MainFrame::OnRButtonDown(int x, int y)
{
	if (!r_playlist_.IsRectEmpty() && r_playlist_.PtInRect(x, y))
	{
		using M = MenuItem;
		const std::vector<M> items = {M::Cmd("&Open", 1).Enable(pl_sel_ >= 0), M::Sep(), M::Cmd("&Remove", ID_PLAYLIST_REMOVE).Enable(pl_sel_ >= 0),
			M::Cmd("C&lear", ID_PLAYLIST_CLEAR).Enable(!playlist_.empty())};
		TrackPopupMenu(items, x, y, [this](int id) {
			if (id == 1)
				OpenPlaylistItem(pl_sel_);
			else if (id)
				Command(id);
		});
		return;
	}
	TrackPopupMenu(BuildMenu(-1), x, y, [this](int id) {
		if (id)
			Command(id);
	});
}

namespace
{
// the window's keys and the buttons they come from (mpc_ui.cpp's InputMapper: Cross is Enter, Circle Escape)
PadButton ButtonOf(Key k)
{
	switch (k)
	{
		case Key::Enter:
			return PadButton::Cross;
		case Key::Escape:
			return PadButton::Circle;
		case Key::Triangle:
			return PadButton::Triangle;
		case Key::Square:
			return PadButton::Square;
		case Key::Left:
			return PadButton::Left;
		case Key::Right:
			return PadButton::Right;
		case Key::Up:
			return PadButton::Up;
		case Key::Down:
			return PadButton::Down;
		case Key::L1:
			return PadButton::L1;
		case Key::R1:
			return PadButton::R1;
		case Key::L2:
			return PadButton::L2;
		case Key::R2:
			return PadButton::R2;
		case Key::L3:
			return PadButton::L3;
		case Key::R3:
			return PadButton::R3;
		case Key::Menu:
			return PadButton::Options;
		case Key::StickLeft:
			return PadButton::StickLeft;
		case Key::StickRight:
			return PadButton::StickRight;
		default:
			return PadButton::Count;
	}
}

bool IsJump(PadAction a)
{
	return a >= PadAction::JumpBackMedium && a <= PadAction::JumpForwardSmall;
}

bool IsJumpBack(PadAction a)
{
	return a == PadAction::JumpBackMedium || a == PadAction::JumpBackLarge || a == PadAction::JumpBackSmall;
}
} // namespace

std::string MainFrame::Accel(PadAction a, const char* combination) const
{
	for (int i = 0; i < int(PadButton::Count); i++)
		if (s_.pad[i] == a)
			return PadButtonShortName(PadButton(i));
	// R3 + Square: while R3 keeps its default action
	if (*combination && s_.pad[int(PadButton::R3)] == DefaultPadAction(PadButton::R3))
		return combination;
	return "";
}

void MainFrame::NextTrack(TrackType t)
{
	if (!Loaded())
		return;
	const std::vector<Track> tracks = player_.Tracks(t);
	const int cur = t == TrackType::Subtitle && !subs_enabled_ ? -1 : player_.CurrentTrack(t);
	int at = -1;
	for (int i = 0; i < int(tracks.size()); i++)
		if (tracks[i].id == cur)
			at = i;
	if (t == TrackType::Subtitle)
	{
		// MPC-HC's Next Subtitle with On/Off: the next track; after the last one, off; then the first again
		if (tracks.empty() || (cur >= 0 && at + 1 >= int(tracks.size())))
		{
			subs_enabled_ = false;
			Osd(T("Subtitle: off"));
			OrbisLog("[ui] next subtitle: off");
			return;
		}
		const Track& n = tracks[at + 1];
		player_.SelectTrack(TrackType::Subtitle, n.id);
		subs_enabled_ = true;
		Osd(Fmt(T("Subtitle: %s"), n.name.c_str()));
		OrbisLog("[ui] next subtitle: %d", n.id);
		return;
	}
	if (tracks.empty())
		return;
	const Track& n = tracks[(at + 1) % int(tracks.size())];
	if (n.id != cur)
		player_.SelectTrack(t, n.id);
	Osd(Fmt(T("Audio: %s"), n.name.c_str()));
	OrbisLog("[ui] next audio track: %d", n.id);
}

void MainFrame::ShowOptions(OptionsPage page)
{
	OrbisLog("[ui] Options (page %d)", int(page));
	ShowOptionsDialog(
		[this] {
			ApplyAudio();
			player_.SetSubtitleDelay(s_.sub_delay_ms / 1000.0);
			player_.SetAudioDelay(s_.audio_delay_ms / 1000.0);
			input_.SetSpeed(s_.cursor_speed);
			input_.SetTouchpad(s_.touchpad_pointer);
			SetSubtitleFont(s_.sub_font);
			sub_cache_.key.clear();
			OrbisLogSetEnabled(s_.debug_logs); // Debug logs, at once (the helper reads mpc-hc.ini, saved below)
			if (g_dpi != s_.dpi)
			{
				g_dpi = s_.dpi;
				sub_cache_.key.clear();
			}
			s_.Save();
		},
		page);
}

// Options given another action (Options > Player > Keys): let go within half a second, it does that action; held
// half a second, it opens the menu bar instead
void MainFrame::TickOptionsButton()
{
	if (options_down_ <= 0)
		return;
	const PadAction a = s_.pad[int(PadButton::Options)];
	if (input_.Held(Key::Menu))
	{
		if (input_.HeldFor(Key::Menu) >= 0.5) // seen held (a hitch during a short press doesn't count as held)
		{
			options_down_ = 0;
			// a window opened meanwhile (a message...): the menu bar doesn't go over it
			if (WindowManager::Get().HasModal())
				return;
			OrbisLog("[ui] Options held: the menu bar");
			OpenMenuBar(0, true);
		}
		return;
	}
	options_down_ = 0;
	if (a != PadAction::None && a != PadAction::MenuBar && !WindowManager::Get().HasModal())
	{
		OrbisLog("[ui] %s: %s", PadButtonName(PadButton::Options), PadActionKey(a));
		DoPadAction(a);
	}
}

void MainFrame::DoPadAction(PadAction a)
{
	switch (a)
	{
		case PadAction::PlayPause:
			Command(ID_PLAY_PLAYPAUSE);
			break;
		case PadAction::Stop:
			Command(ID_PLAY_STOP);
			break;
		case PadAction::JumpBackMedium:
			Jump(-s_.jump_medium);
			break;
		case PadAction::JumpForwardMedium:
			Jump(s_.jump_medium);
			break;
		case PadAction::JumpBackLarge:
			Jump(-s_.jump_large);
			break;
		case PadAction::JumpForwardLarge:
			Jump(s_.jump_large);
			break;
		case PadAction::JumpBackSmall:
			Jump(-s_.jump_small);
			break;
		case PadAction::JumpForwardSmall:
			Jump(s_.jump_small);
			break;
		case PadAction::Previous:
			Command(ID_NAVIGATE_SKIPBACK);
			break;
		case PadAction::Next:
			Command(ID_NAVIGATE_SKIPFORWARD);
			break;
		case PadAction::VolumeUp:
			Command(ID_VOLUME_UP);
			break;
		case PadAction::VolumeDown:
			Command(ID_VOLUME_DOWN);
			break;
		case PadAction::Mute:
			Command(ID_VOLUME_MUTE);
			break;
		case PadAction::FullScreen:
			Command(ID_VIEW_FULLSCREEN);
			break;
		case PadAction::LeaveFullScreen:
			if (Fullscreen())
				Command(ID_VIEW_FULLSCREEN);
			break;
		case PadAction::GoTo:
			Command(ID_NAVIGATE_GOTO);
			break;
		case PadAction::ContextMenu:
			TrackPopupMenu(BuildMenu(-1), kW / 2 - ScaleX(120), kH / 4, [this](int id) {
				if (id)
					Command(id);
			}, nullptr, true);
			break;
		case PadAction::MenuBar:
			OpenMenuBar(0, true);
			break;
		case PadAction::SubDelayDown:
			Command(ID_SUB_DELAY_DOWN);
			break;
		case PadAction::SubDelayUp:
			Command(ID_SUB_DELAY_UP);
			break;
		case PadAction::NextSubtitle:
			NextTrack(TrackType::Subtitle);
			break;
		case PadAction::NextAudio:
			NextTrack(TrackType::Audio);
			break;
		case PadAction::AudioDelayDown:
			Command(ID_AUDIO_DELAY_DOWN);
			break;
		case PadAction::AudioDelayUp:
			Command(ID_AUDIO_DELAY_UP);
			break;
		case PadAction::Statistics:
			Command(ID_VIEW_DISPLAY_RENDERER_STATS);
			break;
		case PadAction::Faster:
			Command(ID_PLAY_INCRATE);
			break;
		case PadAction::Slower:
			Command(ID_PLAY_DECRATE);
			break;
		case PadAction::NormalSpeed:
			Command(ID_PLAY_RESETRATE);
			break;
		case PadAction::FrameStep:
			Command(ID_PLAY_FRAMESTEP);
			break;
		case PadAction::OpenFile:
			Command(ID_FILE_OPENMEDIA);
			break;
		case PadAction::Playlist:
			Command(ID_VIEW_PLAYLIST);
			break;
		case PadAction::NextAspectRatio:
			Command(ID_ASPECTRATIO_DAR + (s_.aspect_ratio + 1) % 7);
			break;
		case PadAction::OptionsWindow:
			ShowOptions(OptionsPage::Last);
			break;
		case PadAction::OptionsPlayer:
		case PadAction::OptionsKeys:
		case PadAction::OptionsPlayback:
		case PadAction::OptionsOutput:
		case PadAction::OptionsFullscreen:
		case PadAction::OptionsSubtitles:
		case PadAction::OptionsDefaultStyle:
		case PadAction::OptionsAudio:
		case PadAction::OptionsMisc:
			ShowOptions(OptionsPage(int(a) - int(PadAction::OptionsPlayer)));
			break;
		default:
			break;
	}
}

void MainFrame::OnKey(Key k, bool repeat)
{
	// the playlist, when it has the focus: Up/Down choose, Cross plays, Circle gives the focus back
	if (pl_focus_ && s_.show_playlist && !playlist_.empty())
	{
		switch (k)
		{
			case Key::Up:
				pl_sel_ = std::max(0, pl_sel_ - 1);
				return;
			case Key::Down:
				pl_sel_ = std::min(int(playlist_.size()) - 1, pl_sel_ + 1);
				return;
			case Key::Enter:
				if (!repeat)
					OpenPlaylistItem(pl_sel_);
				return;
			case Key::Escape:
				pl_focus_ = false;
				return;
			default:
				break;
		}
	}
	// Options: the menu bar (its default); given another action, that action when it is let go, and the menu bar still
	// when it is held half a second (nobody locks themselves out of the menus)
	if (k == Key::Menu)
	{
		if (repeat)
			return;
		const PadAction oa = s_.pad[int(PadButton::Options)];
		if (oa == PadAction::MenuBar || oa == PadAction::None)
		{
			if (oa == PadAction::MenuBar)
				OpenMenuBar(0, true);
			else
				options_down_ = NowSeconds(); // nothing of its own: held, still the menu bar
			return;
		}
		options_down_ = NowSeconds();
		return;
	}
	const PadButton b = ButtonOf(k);
	if (b == PadButton::Count)
		return;
	PadAction a = s_.pad[int(b)];
	// the combinations (as before the Keys page), while L2 / R2 / R3 keep their default actions: L2 or R2 held with
	// Left / Right (when they jump) jumps 20 s / 1 s that way; R3 held with Square: the statistics
	if ((b == PadButton::Left || b == PadButton::Right) && IsJump(a))
	{
		const bool back = IsJumpBack(a);
		if (input_.Held(Key::L2) && s_.pad[int(PadButton::L2)] == DefaultPadAction(PadButton::L2))
			a = back ? PadAction::JumpBackLarge : PadAction::JumpForwardLarge;
		else if (input_.Held(Key::R2) && s_.pad[int(PadButton::R2)] == DefaultPadAction(PadButton::R2))
			a = back ? PadAction::JumpBackSmall : PadAction::JumpForwardSmall;
	}
	if (b == PadButton::Square && input_.Held(Key::R3) && s_.pad[int(PadButton::R3)] == DefaultPadAction(PadButton::R3))
		a = PadAction::Statistics;
	if (repeat && !PadActionRepeats(a))
		return;
	if (a != PadAction::None && !repeat)
		OrbisLog("[ui] %s: %s", PadButtonName(b), PadActionKey(a));
	DoPadAction(a);
}

void MainFrame::OnEvent(const InputEvent& ev)
{
	switch (ev.type)
	{
		case InputEvent::MouseMove:
			OnMouseMove(ev.x, ev.y);
			break;
		case InputEvent::LButtonDown:
			OnLButtonDown(ev.x, ev.y);
			break;
		case InputEvent::LButtonUp:
			OnLButtonUp(ev.x, ev.y);
			break;
		case InputEvent::RButtonDown:
			OnRButtonDown(ev.x, ev.y);
			break;
		case InputEvent::Wheel:
		{
			// the wheel (the right stick): the playlist under the pointer, else the stick's keys (the volume, MPC-HC's
			// default); a stick held keeps stepping, which only the actions that repeat follow
			if (!r_playlist_.IsRectEmpty() && r_playlist_.PtInRect(ev.x, ev.y))
			{
				pl_top_ = std::max(0, std::min(pl_top_ - ev.wheel * 3, std::max(0, int(playlist_.size()) - 1)));
				break;
			}
			// ev.repeat: the stick was already pushed that way (InputMapper follows its axis), not a new push
			const PadButton b = ev.wheel > 0 ? PadButton::StickUp : PadButton::StickDown;
			const PadAction a = s_.pad[int(b)];
			if (ev.repeat && !PadActionRepeats(a))
				break;
			if (!ev.repeat && a != PadAction::None)
				OrbisLog("[ui] %s: %s", PadButtonName(b), PadActionKey(a));
			DoPadAction(a);
			break;
		}
		case InputEvent::KeyDown:
		{
			// L2/R2 with their default actions are the modifiers of Left/Right: pressed while a Left/Right that jumps is
			// held, they don't act themselves, and held they don't repeat (as before the Keys page); given another
			// action, they act and repeat as any button
			const bool trigger_default = (ev.key == Key::L2 && s_.pad[int(PadButton::L2)] == DefaultPadAction(PadButton::L2)) ||
										 (ev.key == Key::R2 && s_.pad[int(PadButton::R2)] == DefaultPadAction(PadButton::R2));
			const bool jump_held = (input_.Held(Key::Left) && IsJump(s_.pad[int(PadButton::Left)])) ||
								   (input_.Held(Key::Right) && IsJump(s_.pad[int(PadButton::Right)]));
			if (trigger_default && (jump_held || ev.repeat))
				break;
			OnKey(ev.key, ev.repeat);
			break;
		}
		default:
			break;
	}
}

// ===========================================================================================================
int MainFrame::Run(const std::string& first)
{
	s_.Load();
	g_dpi = s_.dpi;
	input_.SetSpeed(s_.cursor_speed);
	input_.SetTouchpad(s_.touchpad_pointer);
	OrbisLog("[ui] fonts folder: %d font(s) loaded", LoadExtraFonts(OrbisDir("fonts")));
	SetSubtitleFont(s_.sub_font); // a file no longer there: Selawik (logged)
	// the CA list for https:// streams
	{
		const std::string ca = OrbisRoot() + "/cacert.pem";
		if (FILE* f = fopen(ca.c_str(), "wb"))
		{
			fwrite(mpc_cacert_begin, 1, size_t(mpc_cacert_end - mpc_cacert_begin), f);
			fclose(f);
		}
	}
	// updates (Help, Check for updates); once a day the app looks by itself and offers a newer version
	mpcupdate::Init();
	if (have_data_ && mpcupdate::DailyCheckDue())
	{
		OrbisLog("[update] daily check");
		upd_auto_ = true;
		update::StartCheck(true);
	}
	WindowManager::Get().screen = CRect(0, 0, kW, kH);
	disc::Start(); // the disc drives (mpc_disc.h)
	ApplyAudio();
	if (!first.empty())
		OpenAny(first);
	if (!have_data_)
		MessageBox(Fmt(T("Media Player Classic PS5 has no access to the console's storage (/data) or USB drives.\n\nSend "
						 "MediaPlayerClassicPS5-v%s.elf with your payload loader (it unlocks the app), then open the app "
						 "again. Network streams still work."),
					   MPCPS5_PS5_VERSION),
			"Media Player Classic Home Cinema", MB_OK, 2);
	// an update installed before: is this the new version? (if not, what to do)
	if (have_data_)
	{
		const mpcupdate::AfterUpdate au = mpcupdate::CheckAfterUpdate();
		if (au.pending && au.ok)
			Osd(Fmt(T("Media Player Classic PS5 updated to %s."), MPCPS5_PS5_VERSION), 5);
		else if (au.pending)
			MessageBox(Fmt(T("Media Player Classic PS5 %s was installed in %s, but this is still %s: the console started "
							 "the old files.\n\nClose the app completely (on the home screen: Options on its card, Close "
							 "Application) and open it again. If it still says %s, ShadowMount+ still shows the old files: "
							 "run ShadowMount+ again from your Payload Manager (it registers the app again), or restart "
							 "the PS5.\n\nThe version in the home screen's app information only changes when ShadowMount+ "
							 "registers the app again."),
						   au.want.c_str(), au.dir.c_str(), MPCPS5_PS5_VERSION, MPCPS5_PS5_VERSION),
				T("Check for updates"), MB_OK, 2);
	}

	Bitmap screen{ps5video::Surface(), ps5video::kWidth, ps5video::kHeight, ps5video::kWidth};
	CDC dc(screen);
	const char* quit_after = getenv("MPCPS5_HOST_QUIT_AFTER");
	const double quit_at = quit_after ? NowSeconds() + atof(quit_after) : 0;
	const char* script = getenv("MPCPS5_HOST_SCRIPT"); // host tests: commands by frame, "frame:cmd;..."
	std::vector<std::pair<int, std::string>> steps;
	if (script)
	{
		std::string sc = script;
		size_t p = 0;
		while (p < sc.size())
		{
			size_t e = sc.find(';', p);
			if (e == std::string::npos)
				e = sc.size();
			const std::string item = sc.substr(p, e - p);
			const size_t c = item.find(':');
			if (c != std::string::npos)
				steps.push_back({atoi(item.substr(0, c).c_str()), item.substr(c + 1)});
			p = e + 1;
		}
	}
	int frame = 0;
	MPCPS5_STAGE(Shelf, "loop");
	while (!quit_)
	{
		if (quit_at > 0 && NowSeconds() >= quit_at)
			break;
		for (const auto& st : steps)
			if (st.first == frame)
			{
				const std::string& c = st.second;
				OrbisLog("[test] frame %d: %s", frame, c.c_str());
				static const std::pair<const char*, int> kNamed[] = {{"about", ID_HELP_ABOUT}, {"options", ID_VIEW_OPTIONS},
					{"open", ID_FILE_OPENMEDIA}, {"quickopen", ID_FILE_OPENQUICK}, {"props", ID_FILE_PROPERTIES},
					{"goto", ID_NAVIGATE_GOTO}, {"fullscreen", ID_VIEW_FULLSCREEN}, {"playlist", ID_VIEW_PLAYLIST},
					{"info", ID_VIEW_INFORMATION}, {"stats", ID_VIEW_STATISTICS}, {"pause", ID_PLAY_PAUSE},
					{"play", ID_PLAY_PLAY}, {"stop", ID_PLAY_STOP}, {"step", ID_PLAY_FRAMESTEP}, {"mute", ID_VOLUME_MUTE},
					{"volup", ID_VOLUME_UP}, {"voldown", ID_VOLUME_DOWN}, {"faster", ID_PLAY_INCRATE}, {"slower", ID_PLAY_DECRATE},
					{"close", ID_FILE_CLOSE_AND_RESTORE}, {"saveimage", ID_FILE_SAVE_IMAGE}, {"next", ID_NAVIGATE_SKIPFORWARD},
					{"prev", ID_NAVIGATE_SKIPBACK}, {"addfav", ID_FAVORITES_ADD}, {"loadsub", ID_FILE_SUBTITLES_LOAD},
					{"opendir", ID_FILE_OPENDIRECTORY}, {"opendvdbd", ID_FILE_OPENDVDBD}, {"disc0", ID_DISC_FIRST},
					{"disc1", ID_DISC_FIRST + 1}, {"title1", ID_TITLE_FIRST}, {"title2", ID_TITLE_FIRST + 1}, {"exit", ID_FILE_EXIT}};
				if (c.compare(0, 4, "cmd ") == 0)
				{
					int id = atoi(c.c_str() + 4);
					for (const auto& n : kNamed)
						if (c.substr(4) == n.first)
							id = n.second;
					Command(id);
				}
				else if (c.compare(0, 4, "key ") == 0)
				{
					// a pad button as the UI sees it: left right up down enter escape menu square triangle l1 r1 l2 r2 l3 r3
					static const std::pair<const char*, Key> kKeys[] = {{"left", Key::Left}, {"right", Key::Right}, {"up", Key::Up},
						{"down", Key::Down}, {"enter", Key::Enter}, {"escape", Key::Escape}, {"menu", Key::Menu},
						{"square", Key::Square}, {"triangle", Key::Triangle}, {"l1", Key::L1}, {"r1", Key::R1}, {"l2", Key::L2},
						{"r2", Key::R2}, {"l3", Key::L3}, {"r3", Key::R3}};
					for (const auto& kk : kKeys)
						if (c.substr(4) == kk.first)
						{
							InputEvent e{InputEvent::KeyDown};
							e.key = kk.second;
							if (!WindowManager::Get().Dispatch(e))
								OnEvent(e);
						}
				}
				else if (c.compare(0, 5, "open ") == 0)
					OpenAny(c.substr(5));
				else if (c.compare(0, 5, "seek ") == 0)
					Seek(atof(c.c_str() + 5));
				else if (c.compare(0, 6, "mouse ") == 0)
				{
					int x = 0, y = 0;
					sscanf(c.c_str() + 6, "%d,%d", &x, &y);
					input_.ShowCursorAt(x, y);
					InputEvent e{InputEvent::MouseMove};
					e.x = x;
					e.y = y;
					if (!WindowManager::Get().Dispatch(e))
						OnEvent(e);
				}
				else if (c == "click" || c == "rclick")
				{
					InputEvent d{c == "click" ? InputEvent::LButtonDown : InputEvent::RButtonDown};
					d.x = input_.CursorX();
					d.y = input_.CursorY();
					if (!WindowManager::Get().Dispatch(d))
						OnEvent(d);
					if (c == "click")
					{
						InputEvent u{InputEvent::LButtonUp};
						u.x = d.x;
						u.y = d.y;
						if (!WindowManager::Get().Dispatch(u))
							OnEvent(u);
					}
				}
				else if (c.compare(0, 6, "stall ") == 0)
					usleep(useconds_t(atoi(c.c_str() + 6)) * 1000); // a hitch: this frame takes that long
				else if (c == "quit")
					quit_ = true;
#ifndef __PROSPERO__
				else if (c.compare(0, 6, "eject ") == 0 || c.compare(0, 7, "insert ") == 0)
					disc::HostEject(c.substr(c.find(' ') + 1), c[0] == 'e'); // a disc taken out / put back (host tests)
#endif
				else if (c == "state")
				{
					const PlayerStats ps = player_.Stats();
					OrbisLog("[test] state %d pos %.2f dur %.2f video %d audio %d sub %d picture %d queued %d shown %.1f dropped %llu "
							 "decode %.1f scale %.1f underruns %llu hw %d",
						int(player_.State()), player_.Position(), player_.Duration(), player_.CurrentTrack(TrackType::Video),
						player_.CurrentTrack(TrackType::Audio), player_.CurrentTrack(TrackType::Subtitle), int(player_.HasPicture()),
						ps.queued_video, ps.shown_fps, (unsigned long long)ps.dropped, ps.decode_ms, ps.convert_ms,
						(unsigned long long)ps.underruns, int(ps.hw_decoding));
				}
			}
		std::vector<InputEvent> events;
		input_.Poll(events);
		for (const InputEvent& ev : events)
		{
			if (WindowManager::Get().Dispatch(ev))
			{
				if (ev.type == InputEvent::MouseMove)
					OnMouseMove(-1, -1); // nothing in the window is hot under a dialog
				continue;
			}
			OnEvent(ev);
		}
		WindowManager::Get().Tick();
		Tick();
		const bool new_picture = player_.UpdateVideo();
		// Idle: nothing playing or opening, no dialog or menu, no OSD, no pointer, nothing pressed for 2 s: the window
		// is drawn twice a second (the clock, the status) instead of at every refresh; the first input, a picture or
		// a playing file brings the full rate back at once. (Host tests: only with MPCPS5_HOST_IDLE=1, as their
		// screenshots are taken by frame number.)
		{
			const double now = NowSeconds();
			if (!events.empty() || new_picture)
				last_activity_ = now;
			const PlayState ps = player_.State();
			const bool still = idle_allowed_ && (ps == PlayState::Closed || ps == PlayState::Paused ||
													ps == PlayState::Stopped || ps == PlayState::Failed) &&
							   !WindowManager::Get().HasModal() && now >= osd_until_ && !input_.CursorVisible() &&
							   now - last_activity_ >= 2.0;
			if (still && now - last_paint_ < 0.5)
			{
				if (!idle_)
					OrbisLog("[ui] idle: the window drawn twice a second until something happens");
				idle_ = true;
				usleep(16000);
				frame++;
				continue;
			}
			if (!still && idle_)
			{
				idle_ = false;
				OrbisLog("[ui] active again: drawn at every refresh");
			}
			last_paint_ = now;
			idle_presents_ += still ? 1 : 0;
		}
		Paint(dc);
		WindowManager::Get().Paint(dc);
		if (input_.CursorVisible())
			DrawCursor(dc, input_.CursorX(), input_.CursorY());
		ps5video::Present(0, 0, ps5video::kWidth, ps5video::kHeight, true);
		frame++;
		if (frame % 3600 == 0 && Loaded())
		{
			const PlayerStats st = player_.Stats();
			OrbisLog("[ui] %s at %.1f s: %.1f fps shown (stream %.3f), %llu dropped, decode %.1f ms, scale %.1f ms, %llu underruns",
				FileName(current_url_).c_str(), player_.Position(), st.shown_fps, st.fps, (unsigned long long)st.dropped,
				st.decode_ms, st.convert_ms, (unsigned long long)st.underruns);
		}
	}
	SavePosition();
	player_.Close();
	disc::Stop();
	tonemap::Shutdown(); // the HDR workers stopped and joined (the player's threads are gone)
	s_.Save();
	return 0;
}
} // namespace

std::string EngineVersion()
{
	return "FFmpeg " + FFmpegVersion();
}

int RunApp(const std::string& first, bool have_data)
{
	MainFrame* f = new MainFrame(have_data);
	const int r = f->Run(first);
	delete f;
	return r;
}
} // namespace mpc
