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

// Windows 10's system colours (GetSysColor), the ones MPC-HC paints with.
namespace sys
{
constexpr COLORREF BtnFace = RGB(240, 240, 240); // COLOR_BTNFACE / COLOR_3DFACE
constexpr COLORREF Shadow = RGB(160, 160, 160); // COLOR_3DSHADOW
constexpr COLORREF Hilight = RGB(255, 255, 255); // COLOR_3DHILIGHT
constexpr COLORREF DkShadow = RGB(105, 105, 105); // COLOR_3DDKSHADOW
constexpr COLORREF Light = RGB(227, 227, 227); // COLOR_3DLIGHT
constexpr COLORREF GrayText = RGB(109, 109, 109); // COLOR_GRAYTEXT
constexpr COLORREF Window = RGB(255, 255, 255); // COLOR_WINDOW
constexpr COLORREF WindowText = RGB(0, 0, 0);
constexpr COLORREF Highlight = RGB(0, 120, 215); // COLOR_HIGHLIGHT
constexpr COLORREF HighlightText = RGB(255, 255, 255);
constexpr COLORREF HotTrack = RGB(0, 102, 204);
// Windows 10's themed menus (uxtheme "Menu" class)
constexpr COLORREF MenuBar = RGB(255, 255, 255);
constexpr COLORREF MenuBarHot = RGB(229, 243, 255);
constexpr COLORREF MenuBarHotBorder = RGB(204, 232, 255);
constexpr COLORREF MenuBarPushed = RGB(204, 232, 255);
constexpr COLORREF MenuBarPushedBorder = RGB(153, 209, 255);
constexpr COLORREF MenuPopup = RGB(242, 242, 242);
constexpr COLORREF MenuPopupBorder = RGB(204, 204, 204);
constexpr COLORREF MenuItemHot = RGB(145, 201, 247);
constexpr COLORREF MenuSeparator = RGB(215, 215, 215);
constexpr COLORREF MenuDisabled = RGB(109, 109, 109);
// Windows 10's title bar
constexpr COLORREF Caption = RGB(255, 255, 255);
constexpr COLORREF CaptionText = RGB(0, 0, 0);
constexpr COLORREF CaptionInactiveText = RGB(153, 153, 153);
constexpr COLORREF CaptionButtonHot = RGB(229, 229, 229);
constexpr COLORREF CaptionButtonPushed = RGB(204, 204, 204);
constexpr COLORREF CloseHot = RGB(232, 17, 35);
constexpr COLORREF ClosePushed = RGB(241, 112, 122);
constexpr COLORREF WindowFrame = RGB(100, 100, 100);
// Windows 10's themed push buttons, edits, lists
constexpr COLORREF ButtonFace = RGB(225, 225, 225);
constexpr COLORREF ButtonBorder = RGB(173, 173, 173);
constexpr COLORREF ButtonHotFace = RGB(229, 241, 251);
constexpr COLORREF ButtonHotBorder = RGB(0, 120, 215);
constexpr COLORREF ButtonPushedFace = RGB(204, 228, 247);
constexpr COLORREF ButtonPushedBorder = RGB(0, 84, 153);
constexpr COLORREF ButtonDisabledFace = RGB(204, 204, 204);
constexpr COLORREF ButtonDisabledBorder = RGB(191, 191, 191);
constexpr COLORREF EditBorder = RGB(122, 122, 122);
constexpr COLORREF EditBorderHot = RGB(23, 23, 23);
constexpr COLORREF EditBorderFocus = RGB(0, 120, 215);
constexpr COLORREF ListHot = RGB(229, 243, 255);
constexpr COLORREF ListSelected = RGB(204, 232, 255);
constexpr COLORREF ListSelectedBorder = RGB(153, 209, 255);
constexpr COLORREF TooltipBack = RGB(255, 255, 255);
constexpr COLORREF TooltipBorder = RGB(118, 118, 118);
constexpr COLORREF TooltipText = RGB(87, 87, 87);
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
