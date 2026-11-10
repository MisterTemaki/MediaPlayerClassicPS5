// Media Player Classic PS5: a small GDI.
//
// MPC-HC paints its window with GDI: FillSolidRect, Draw3dRect, SetPixel, BitBlt, MoveTo/LineTo, DrawText.
// This is the same set over a 32-bit pixel buffer, so the port of each painting routine (the seek bar's
// CPlayerSeekBar::OnPaint, the volume slider's CVolumeCtrl::OnNMCustomdraw, the status bar's
// CPlayerStatusBar::OnPaint...) reads like the original, call for call.
//
// Colours are COLORREFs, 0x00BBGGRR as Windows' RGB() makes them; the surface stores 0xAABBGGRR
// (A8B8G8R8), so a COLORREF is a pixel once its alpha byte is set.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mpc
{
typedef uint32_t COLORREF;
constexpr COLORREF RGB(int r, int g, int b)
{
	return uint32_t(r & 0xff) | (uint32_t(g & 0xff) << 8) | (uint32_t(b & 0xff) << 16);
}
inline int GetRValue(COLORREF c) { return c & 0xff; }
inline int GetGValue(COLORREF c) { return (c >> 8) & 0xff; }
inline int GetBValue(COLORREF c) { return (c >> 16) & 0xff; }

// MPC-HC's Windows 10 light colors and the port's dark palette.
namespace sys
{
// The UI thread selects the theme. Colors resolve when painted, so existing dialogs update on Apply.
inline bool dark_theme = false;
inline void SetDarkTheme(bool dark) { dark_theme = dark; }
inline bool IsDarkTheme() { return dark_theme; }
struct ThemeColor
{
	COLORREF light, dark;
	constexpr ThemeColor(COLORREF l, COLORREF d) : light(l), dark(d) {}
	operator COLORREF() const { return dark_theme ? dark : light; }
};
constexpr ThemeColor BtnFace{RGB(240, 240, 240), RGB(32, 32, 32)};
constexpr ThemeColor Shadow{RGB(160, 160, 160), RGB(70, 70, 70)};
constexpr ThemeColor Hilight{RGB(255, 255, 255), RGB(72, 72, 72)};
constexpr ThemeColor DkShadow{RGB(105, 105, 105), RGB(12, 12, 12)};
constexpr ThemeColor Light{RGB(227, 227, 227), RGB(42, 42, 42)};
constexpr ThemeColor GrayText{RGB(109, 109, 109), RGB(150, 150, 150)};
constexpr ThemeColor Window{RGB(255, 255, 255), RGB(24, 24, 24)};
constexpr ThemeColor WindowText{RGB(0, 0, 0), RGB(230, 230, 230)};
constexpr ThemeColor Highlight{RGB(0, 120, 215), RGB(0, 92, 160)};
constexpr ThemeColor HighlightText{RGB(255, 255, 255), RGB(255, 255, 255)};
constexpr ThemeColor HotTrack{RGB(0, 102, 204), RGB(100, 180, 250)};
// Windows 10's themed menus (uxtheme "Menu" class)
constexpr ThemeColor MenuBar{RGB(255, 255, 255), RGB(32, 32, 32)};
constexpr ThemeColor MenuBarHot{RGB(229, 243, 255), RGB(50, 65, 78)};
constexpr ThemeColor MenuBarHotBorder{RGB(204, 232, 255), RGB(72, 105, 132)};
constexpr ThemeColor MenuBarPushed{RGB(204, 232, 255), RGB(54, 88, 118)};
constexpr ThemeColor MenuBarPushedBorder{RGB(153, 209, 255), RGB(88, 144, 190)};
constexpr ThemeColor MenuPopup{RGB(242, 242, 242), RGB(40, 40, 40)};
constexpr ThemeColor MenuPopupBorder{RGB(204, 204, 204), RGB(74, 74, 74)};
constexpr ThemeColor MenuItemHot{RGB(145, 201, 247), RGB(50, 85, 115)};
constexpr ThemeColor MenuSeparator{RGB(215, 215, 215), RGB(78, 78, 78)};
constexpr ThemeColor MenuDisabled{RGB(109, 109, 109), RGB(150, 150, 150)};
constexpr ThemeColor MenuDisabledHot{RGB(230, 230, 230), RGB(52, 52, 52)};
// Windows 10's title bar
constexpr ThemeColor Caption{RGB(255, 255, 255), RGB(28, 28, 28)};
constexpr ThemeColor CaptionText{RGB(0, 0, 0), RGB(238, 238, 238)};
constexpr ThemeColor CaptionInactiveText{RGB(153, 153, 153), RGB(140, 140, 140)};
constexpr ThemeColor CaptionButtonHot{RGB(229, 229, 229), RGB(58, 58, 58)};
constexpr ThemeColor CaptionButtonPushed{RGB(204, 204, 204), RGB(76, 76, 76)};
constexpr COLORREF CloseHot = RGB(232, 17, 35);
constexpr COLORREF ClosePushed = RGB(241, 112, 122);
constexpr ThemeColor WindowFrame{RGB(100, 100, 100), RGB(82, 82, 82)};
// Windows 10's themed push buttons, edits, lists
constexpr ThemeColor ButtonFace{RGB(225, 225, 225), RGB(54, 54, 54)};
constexpr ThemeColor ButtonBorder{RGB(173, 173, 173), RGB(100, 100, 100)};
constexpr ThemeColor ButtonHotFace{RGB(229, 241, 251), RGB(60, 76, 90)};
constexpr ThemeColor ButtonHotBorder{RGB(0, 120, 215), RGB(90, 175, 245)};
constexpr ThemeColor ButtonPushedFace{RGB(204, 228, 247), RGB(42, 66, 86)};
constexpr ThemeColor ButtonPushedBorder{RGB(0, 84, 153), RGB(70, 145, 210)};
constexpr ThemeColor ButtonDisabledFace{RGB(204, 204, 204), RGB(43, 43, 43)};
constexpr ThemeColor ButtonDisabledBorder{RGB(191, 191, 191), RGB(72, 72, 72)};
constexpr ThemeColor EditBorder{RGB(122, 122, 122), RGB(96, 96, 96)};
constexpr ThemeColor EditBorderHot{RGB(23, 23, 23), RGB(160, 160, 160)};
constexpr ThemeColor EditBorderFocus{RGB(0, 120, 215), RGB(90, 175, 245)};
constexpr ThemeColor ListHot{RGB(229, 243, 255), RGB(44, 60, 75)};
constexpr ThemeColor ListSelected{RGB(204, 232, 255), RGB(47, 78, 104)};
constexpr ThemeColor ListSelectedBorder{RGB(153, 209, 255), RGB(78, 128, 172)};
constexpr ThemeColor TooltipBack{RGB(255, 255, 255), RGB(48, 48, 48)};
constexpr ThemeColor TooltipBorder{RGB(118, 118, 118), RGB(100, 100, 100)};
constexpr ThemeColor TooltipText{RGB(87, 87, 87), RGB(235, 235, 235)};
constexpr ThemeColor ScrollArrow{RGB(96, 96, 96), RGB(200, 200, 200)};
constexpr ThemeColor ScrollThumb{RGB(205, 205, 205), RGB(90, 90, 90)};
constexpr ThemeColor ControlMark{RGB(51, 51, 51), RGB(224, 224, 224)};
constexpr ThemeColor DisabledMark{RGB(204, 204, 204), RGB(115, 115, 115)};
constexpr ThemeColor GroupLine{RGB(220, 220, 220), RGB(85, 85, 85)};
constexpr ThemeColor GroupText{RGB(0, 70, 213), RGB(115, 180, 245)};
constexpr ThemeColor SliderTrack{RGB(231, 234, 234), RGB(50, 50, 50)};
constexpr ThemeColor SliderBorder{RGB(214, 214, 214), RGB(85, 85, 85)};
constexpr ThemeColor SliderHot{RGB(23, 23, 23), RGB(235, 235, 235)};
constexpr ThemeColor ListBorder{RGB(130, 135, 144), RGB(85, 85, 85)};
constexpr ThemeColor HeaderText{RGB(76, 96, 122), RGB(190, 205, 220)};
constexpr ThemeColor GridLine{RGB(229, 229, 229), RGB(64, 64, 64)};
constexpr ThemeColor UnfocusedSelection{RGB(217, 217, 217), RGB(64, 64, 64)};
constexpr ThemeColor SecondaryText{RGB(109, 109, 109), RGB(170, 170, 170)};
constexpr ThemeColor TabBorder{RGB(217, 217, 217), RGB(72, 72, 72)};
constexpr ThemeColor DialogFrame{RGB(24, 131, 215), RGB(90, 160, 220)};
constexpr ThemeColor PlaylistSelected{RGB(204, 218, 241), RGB(52, 78, 108)};
constexpr ThemeColor PlaylistSelectedBorder{RGB(49, 106, 197), RGB(90, 150, 220)};
constexpr ThemeColor PlaylistCurrent{RGB(255, 0, 0), RGB(255, 125, 125)};
} // namespace sys

struct CPoint
{
	int x = 0, y = 0;
};

struct CRect
{
	int left = 0, top = 0, right = 0, bottom = 0;
	CRect() = default;
	CRect(int l, int t, int r, int b) : left(l), top(t), right(r), bottom(b) {}
	int Width() const { return right - left; }
	int Height() const { return bottom - top; }
	bool IsRectEmpty() const { return right <= left || bottom <= top; }
	bool PtInRect(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
	void DeflateRect(int l, int t, int r, int b)
	{
		left += l;
		top += t;
		right -= r;
		bottom -= b;
	}
	void DeflateRect(int x, int y) { DeflateRect(x, y, x, y); }
	void InflateRect(int l, int t, int r, int b) { DeflateRect(-l, -t, -r, -b); }
	void InflateRect(int x, int y) { DeflateRect(-x, -y, -x, -y); }
	void OffsetRect(int dx, int dy)
	{
		left += dx;
		right += dx;
		top += dy;
		bottom += dy;
	}
	void MoveToXY(int x, int y) { OffsetRect(x - left, y - top); }
	void MoveToX(int x) { OffsetRect(x - left, 0); }
	void MoveToY(int y) { OffsetRect(0, y - top); }
	CPoint CenterPoint() const { return {(left + right) / 2, (top + bottom) / 2}; }
	bool Intersect(const CRect& o)
	{
		left = std::max(left, o.left);
		top = std::max(top, o.top);
		right = std::min(right, o.right);
		bottom = std::min(bottom, o.bottom);
		return !IsRectEmpty();
	}
	bool operator==(const CRect& o) const
	{
		return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
	}
	bool operator!=(const CRect& o) const { return !(*this == o); }
};

// A 32-bit picture: the screen, or a bitmap of ours.
struct Bitmap
{
	uint32_t* px = nullptr; // 0xAABBGGRR
	int w = 0, h = 0, pitch = 0; // pitch in pixels
	bool Valid() const { return px && w > 0 && h > 0; }
};

// The device context: a bitmap, a clip rectangle and a pen position.
class CDC
{
public:
	CDC() = default;
	explicit CDC(const Bitmap& b) : bmp_(b), clip_(0, 0, b.w, b.h) {}

	const Bitmap& Target() const { return bmp_; }
	void SetClip(const CRect& r)
	{
		clip_ = r;
		clip_.Intersect(CRect(0, 0, bmp_.w, bmp_.h));
	}
	void ResetClip() { clip_ = CRect(0, 0, bmp_.w, bmp_.h); }
	CRect GetClip() const { return clip_; }

	void FillSolidRect(const CRect& r, COLORREF c);
	void FillSolidRect(int x, int y, int w, int h, COLORREF c) { FillSolidRect(CRect(x, y, x + w, y + h), c); }
	// alpha: 0..255 over what is there
	void BlendRect(const CRect& r, COLORREF c, int alpha);
	// 1.6: transparent (0): the GPU's video picture shows there (ps5video::GpuActive)
	void ClearRect(const CRect& r);
	void Draw3dRect(const CRect& r, COLORREF topLeft, COLORREF bottomRight);
	void FrameRect(const CRect& r, COLORREF c) { Draw3dRect(r, c, c); }
	void SetPixel(int x, int y, COLORREF c);
	COLORREF GetPixel(int x, int y) const;
	void MoveTo(int x, int y)
	{
		pen_ = {x, y};
	}
	// as GDI: the last point is not drawn
	void LineTo(int x, int y, COLORREF c);
	void HLine(int x0, int x1, int y, COLORREF c) { FillSolidRect(CRect(x0, y, x1, y + 1), c); }
	void VLine(int x, int y0, int y1, COLORREF c) { FillSolidRect(CRect(x, y0, x + 1, y1), c); }
	// a dotted focus rectangle (DrawFocusRect)
	void DrawFocusRect(const CRect& r);

	// premultiplied-alpha bitmaps (ours) blended in; the whole of src at (x, y)
	void AlphaBlit(int x, int y, const Bitmap& src, int opacity = 255);
	// an opaque copy
	void BitBlt(int x, int y, const Bitmap& src);
	// a scaled copy (bilinear), opaque
	void StretchBlt(const CRect& dst, const Bitmap& src);
	// an 8-bit coverage mask drawn in one colour (text glyphs)
	void MaskBlit(int x, int y, const uint8_t* mask, int w, int h, int pitch, COLORREF c, int opacity = 255);

private:
	Bitmap bmp_;
	CRect clip_;
	CPoint pen_;
};

// An owned bitmap (premultiplied alpha).
class Image
{
public:
	Image() = default;
	Image(int w, int h) { Create(w, h); }
	void Create(int w, int h);
	void Clear(uint32_t px = 0);
	Bitmap Bmp() { return Bitmap{data_.empty() ? nullptr : data_.data(), w_, h_, w_}; }
	Bitmap Bmp() const { return Bitmap{const_cast<uint32_t*>(data_.data()), w_, h_, w_}; }
	int Width() const { return w_; }
	int Height() const { return h_; }
	bool IsNull() const { return data_.empty(); }
	uint32_t* Data() { return data_.data(); }
	// from RGBA bytes (straight alpha)
	void FromRGBA(const uint8_t* rgba, int w, int h);
	// a high-quality scaled copy (area/bilinear, premultiplied)
	Image Scaled(int w, int h) const;
	// ImageGrayer::Gray, MPC-HC's disabled toolbar images
	Image Grayed() const;

private:
	int w_ = 0, h_ = 0;
	std::vector<uint32_t> data_;
};
} // namespace mpc
