// Media Player Classic PS5: fonts and DrawText.
//
// Windows draws MPC-HC's window in Segoe UI 9 pt, the system's message, menu, caption and status font. Segoe UI
// can't be shipped; Selawik (Microsoft's open-source, metric-compatible stand-in for it, SIL OFL) can, so the
// window is drawn in Selawik, with Roboto (Apache-2.0) for the letters Selawik doesn't have (Greek, Cyrillic...)
// and, when the user puts one in <data>/fonts, any TrueType/OpenType font for the rest (Chinese, Japanese...).
// Rasterized with stb_truetype, grayscale anti-aliasing, kerned, cached per glyph.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_gdi.h"

#include <string>
#include <vector>

namespace mpc
{
// DrawText's flags, as in WinUser.h
enum : unsigned
{
	DT_LEFT = 0,
	DT_CENTER = 1,
	DT_RIGHT = 2,
	DT_VCENTER = 4,
	DT_BOTTOM = 8,
	DT_SINGLELINE = 0x20,
	DT_NOPREFIX = 0x800,
	DT_END_ELLIPSIS = 0x8000,
	DT_HIDEPREFIX = 0x100000,
	DT_WORDBREAK = 0x10,
	DT_CALCRECT = 0x400,
};

class Font
{
public:
	// em: the em size in pixels (a LOGFONT's -lfHeight)
	Font(int em, bool bold = false);
	int Em() const { return em_; }
	// the text cell: ascent + descent (tmHeight)
	int Height() const { return ascent_ + descent_; }
	int Ascent() const { return ascent_; }
	int Measure(const std::string& utf8) const;
	// one line at (x, y) = the top-left of the cell; returns the advance
	int Draw(CDC& dc, int x, int y, const std::string& utf8, COLORREF c, int opacity = 255) const;
	// with an outline (subtitles, OSD): the text drawn over a widened dark copy
	int DrawOutlined(CDC& dc, int x, int y, const std::string& utf8, COLORREF c, COLORREF outline, int width) const;
	// coverage into an 8-bit mask (max with what is there): subtitles are built from masks
	void DrawMask(uint8_t* mask, int w, int h, int pitch, int x, int y, const std::string& utf8) const;
	bool bold() const { return bold_; }

private:
	int em_;
	bool bold_;
	int ascent_ = 0, descent_ = 0;
};

// The window's fonts at the current scale (UiScale): Segoe UI 9 pt and friends.
Font& UiFont();
Font& UiBoldFont();
// any size (the OSD, subtitles): cached by em and weight
Font& FontOfSize(int em, bool bold = false);

// DrawText: & marks the accelerator (underlined unless DT_HIDEPREFIX; && is a &), ... when cut.
// Returns the height drawn; with DT_CALCRECT nothing is drawn and r gets the size.
int DrawText(CDC& dc, const Font& f, const std::string& text, CRect& r, unsigned flags, COLORREF c);
// the text without its & marks
std::string StripPrefix(const std::string& text);
// the accelerator letter of "&File" (lower case), 0 when none
char PrefixChar(const std::string& text);
// a string cut to fit max_px, with "..."
std::string FitText(const Font& f, const std::string& text, int max_px);
// wraps text into lines of at most max_px (at spaces; long words are cut)
std::vector<std::string> WrapText(const Font& f, const std::string& text, int max_px);

// the user's extra fonts: every .ttf/.otf/.ttc in dir, tried in order after Selawik and Roboto
void LoadExtraFonts(const std::string& dir);
} // namespace mpc
