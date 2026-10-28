// Media Player Classic PS5: the window system (mpc_ui.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_ui.h"

#include "mpc_i18n.h"

#include "OrbisPaths.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace mpc
{
double NowSeconds()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ===========================================================================================================
// input
namespace
{
struct BtnKey
{
	uint32_t bit;
	Key key;
	bool repeats;
};
const BtnKey kButtons[] = {
	{SCE_PAD_BUTTON_LEFT, Key::Left, true},
	{SCE_PAD_BUTTON_RIGHT, Key::Right, true},
	{SCE_PAD_BUTTON_UP, Key::Up, true},
	{SCE_PAD_BUTTON_DOWN, Key::Down, true},
	{SCE_PAD_BUTTON_CIRCLE, Key::Escape, false},
	{SCE_PAD_BUTTON_OPTIONS, Key::Menu, false},
	{SCE_PAD_BUTTON_TRIANGLE, Key::Triangle, false},
	{SCE_PAD_BUTTON_L1, Key::L1, true},
	{SCE_PAD_BUTTON_R1, Key::R1, true},
	{SCE_PAD_BUTTON_L2, Key::L2, true},
	{SCE_PAD_BUTTON_R2, Key::R2, true},
	{SCE_PAD_BUTTON_L3, Key::L3, false},
	{SCE_PAD_BUTTON_R3, Key::R3, false},
};
constexpr double kCursorHide = 3.0;
// the touchpad (1920 x 1080 units on the DualSense): how far a finger goes during a click before the pointer follows
constexpr double kPressSlop = 30;
// the most one poll adds to how long a button has been held (for its repeat): a few polls are needed to repeat
constexpr double kMaxHeldStep = 0.1;

double Axis(uint8_t v)
{
	const double d = (int(v) - 128) / 127.0;
	const double dead = 0.18;
	if (std::fabs(d) < dead)
		return 0;
	const double s = (std::fabs(d) - dead) / (1 - dead);
	return (d < 0 ? -1 : 1) * std::min(1.0, s);
}
} // namespace

bool InputMapper::CursorVisible() const
{
	return NowSeconds() - cursor_t_ < kCursorHide;
}

void InputMapper::ShowCursorAt(int x, int y)
{
	cx_ = x;
	cy_ = y;
	cursor_t_ = NowSeconds();
}

void InputMapper::HideCursor()
{
	cursor_t_ = -100;
}

double InputMapper::IdleSeconds() const
{
	return NowSeconds() - input_t_;
}

double InputMapper::HeldFor(Key k) const
{
	for (int i = 0; i < int(sizeof(kButtons) / sizeof(kButtons[0])); i++)
		if (kButtons[i].key == k)
			return (prev_ & kButtons[i].bit) ? held_[i] : 0;
	return 0;
}

bool InputMapper::Held(Key k) const
{
	for (const auto& b : kButtons)
		if (b.key == k)
			return (prev_ & b.bit) != 0;
	return false;
}

void InputMapper::Poll(std::vector<InputEvent>& out)
{
	ps5input::Poll();
	const ps5input::PadState p = ps5input::Snapshot(0);
	const uint32_t cur = p.raw_buttons;
	const double now = NowSeconds();
	static double last = now;
	const double dt = std::min(0.05, now - last);
	// held buttons repeat after being seen held this long; one poll counts for at most kMaxHeldStep, so a slow
	// frame (a hitch) while a button is pressed briefly doesn't make it repeat (R1 skipping two chapters)
	const double held_step = std::min(kMaxHeldStep, now - last);
	last = now;
	const bool cursor_was_shown = CursorVisible();

	// the pointer: the left stick, faster the further it is pushed
	const double ax = Axis(p.lx), ay = Axis(p.ly);
	if (ax != 0 || ay != 0)
	{
		const double speed = (500 + 350 * speed_) * dt;
		const double mag = std::sqrt(ax * ax + ay * ay);
		const double k = speed * mag * mag / std::max(mag, 1e-6);
		cx_ = std::max(0.0, std::min(1919.0, cx_ + ax * k));
		cy_ = std::max(0.0, std::min(1079.0, cy_ + ay * k));
		cursor_t_ = now;
		input_t_ = now;
		InputEvent e{InputEvent::MouseMove};
		e.x = int(cx_);
		e.y = int(cy_);
		out.push_back(e);
	}
	// the touchpad: a finger moving moves the pointer by how far it went (as a laptop's touchpad), faster for a quick
	// stroke; the first touch only marks where the finger is
	if (touchpad_ && p.touches > 0)
	{
		const double fx = p.touch_x[0], fy = p.touch_y[0];
		if (touch_click_ && press_still_)
		{
			const double px = fx - press_x_, py = fy - press_y_;
			if (px * px + py * py >= kPressSlop * kPressSlop)
				press_still_ = false; // a drag: the pointer follows from here
		}
		if (touching_ && p.touch_id[0] == touch_id_ && !(touch_click_ && press_still_))
		{
			const double dx = fx - touch_x_, dy = fy - touch_y_;
			const double dist = std::sqrt(dx * dx + dy * dy);
			if (dist > 0)
			{
				const double v = dist / std::max(dt, 1.0 / 120); // touch units a second
				const double gain = (0.5 + 0.25 * speed_) * (1 + std::min(1.5, std::max(0.0, (v - 400) / 1600)));
				cx_ = std::max(0.0, std::min(1919.0, cx_ + dx * gain));
				cy_ = std::max(0.0, std::min(1079.0, cy_ + dy * gain));
				cursor_t_ = now;
				input_t_ = now;
				InputEvent e{InputEvent::MouseMove};
				e.x = int(cx_);
				e.y = int(cy_);
				out.push_back(e);
			}
		}
		touching_ = true;
		touch_id_ = p.touch_id[0];
		touch_x_ = fx;
		touch_y_ = fy;
	}
	else
		touching_ = false;
	// the touchpad's click: a left click where the pointer is (two fingers on it: a right click). With the pointer
	// hidden it only shows it: where it was hidden may be anything by now (a title bar's close box...)
	{
		const bool down = (cur & SCE_PAD_BUTTON_TOUCH_PAD) != 0 && touchpad_;
		if (down && !touch_click_ && !cursor_was_shown)
		{
			touch_click_ = 3;
			cursor_t_ = now;
			input_t_ = now;
		}
		else if (down && !touch_click_)
		{
			touch_click_ = p.touches >= 2 ? 2 : 1;
			press_still_ = p.touches > 0;
			press_x_ = p.touch_x[0];
			press_y_ = p.touch_y[0];
			cursor_t_ = now;
			input_t_ = now;
			InputEvent e{touch_click_ == 2 ? InputEvent::RButtonDown : InputEvent::LButtonDown};
			e.x = int(cx_);
			e.y = int(cy_);
			out.push_back(e);
		}
		else if (!down && touch_click_)
		{
			if (touch_click_ != 3)
			{
				InputEvent e{touch_click_ == 2 ? InputEvent::RButtonUp : InputEvent::LButtonUp};
				e.x = int(cx_);
				e.y = int(cy_);
				out.push_back(e);
			}
			touch_click_ = 0;
			press_still_ = false;
			cursor_t_ = now;
		}
	}
	// the right stick's left / right: keys, held past half way (back under 40 % lets go), repeating as the D-pad
	{
		const int hx = int(p.rx) - 128, hy = int(p.ry) - 128;
		int h = stick_h_;
		if (stick_h_ == 0 && std::abs(hx) > 64 && std::abs(hx) > std::abs(hy))
			h = hx < 0 ? -1 : 1;
		else if (stick_h_ != 0 && (std::abs(hx) < 51 || (hx < 0) != (stick_h_ < 0)))
			h = 0;
		if (h != stick_h_)
		{
			stick_h_ = h;
			if (h)
			{
				InputEvent e{InputEvent::KeyDown};
				e.key = h < 0 ? Key::StickLeft : Key::StickRight;
				out.push_back(e);
				stick_held_ = 0;
				stick_repeat_at_ = 0.40;
				input_t_ = now;
			}
		}
		else if (h && (stick_held_ += held_step) >= stick_repeat_at_)
		{
			InputEvent e{InputEvent::KeyDown};
			e.key = h < 0 ? Key::StickLeft : Key::StickRight;
			e.repeat = true;
			out.push_back(e);
			stick_repeat_at_ = stick_held_ + 0.07;
			input_t_ = now;
		}
	}
	// the right stick: the wheel (not while it is held left / right)
	const double ry = stick_h_ ? 0 : Axis(p.ry);
	// a push of the stick: its first step is a press, the next ones (still pushed that way) repeats
	const int wdir = ry < 0 ? 1 : ry > 0 ? -1 : 0;
	if (wdir != wheel_dir_)
	{
		wheel_dir_ = wdir;
		wheel_sent_ = false;
		wheel_acc_ = 0;
	}
	if (ry != 0)
	{
		wheel_acc_ += -ry * dt * 12;
		input_t_ = now;
	}
	else
		wheel_acc_ = 0;
	while (std::fabs(wheel_acc_) >= 1)
	{
		InputEvent e{InputEvent::Wheel};
		e.x = int(cx_);
		e.y = int(cy_);
		e.wheel = wheel_acc_ > 0 ? 1 : -1;
		e.repeat = wheel_sent_;
		wheel_sent_ = true;
		wheel_acc_ -= e.wheel;
		out.push_back(e);
	}

	const uint32_t pressed = cur & ~prev_;
	const uint32_t released = prev_ & ~cur;
	if (pressed)
		input_t_ = now;
	// Cross and Square: clicks while the pointer shows, keys otherwise
	if (pressed & SCE_PAD_BUTTON_CROSS)
	{
		if (CursorVisible())
		{
			cross_click_ = true;
			cursor_t_ = now;
			InputEvent e{InputEvent::LButtonDown};
			e.x = int(cx_);
			e.y = int(cy_);
			out.push_back(e);
		}
		else
		{
			InputEvent e{InputEvent::KeyDown};
			e.key = Key::Enter;
			out.push_back(e);
			held_[14] = 0;
			repeat_at_[14] = 0.45;
		}
	}
	else if ((cur & SCE_PAD_BUTTON_CROSS) && !cross_click_ && repeat_at_[14] > 0 &&
			 (held_[14] += held_step) >= repeat_at_[14])
	{
		InputEvent e{InputEvent::KeyDown};
		e.key = Key::Enter;
		e.repeat = true;
		out.push_back(e);
		repeat_at_[14] = held_[14] + 0.12;
	}
	if (released & SCE_PAD_BUTTON_CROSS)
	{
		repeat_at_[14] = 0;
		if (cross_click_)
		{
			cross_click_ = false;
			InputEvent e{InputEvent::LButtonUp};
			e.x = int(cx_);
			e.y = int(cy_);
			out.push_back(e);
		}
	}
	if (pressed & SCE_PAD_BUTTON_SQUARE)
	{
		if (CursorVisible())
		{
			square_click_ = true;
			cursor_t_ = now;
			InputEvent e{InputEvent::RButtonDown};
			e.x = int(cx_);
			e.y = int(cy_);
			out.push_back(e);
		}
		else
		{
			InputEvent e{InputEvent::KeyDown};
			e.key = Key::Square;
			out.push_back(e);
		}
	}
	if ((released & SCE_PAD_BUTTON_SQUARE) && square_click_)
	{
		square_click_ = false;
		InputEvent e{InputEvent::RButtonUp};
		e.x = int(cx_);
		e.y = int(cy_);
		out.push_back(e);
	}
	for (int i = 0; i < int(sizeof(kButtons) / sizeof(kButtons[0])); i++)
	{
		const BtnKey& b = kButtons[i];
		if (pressed & b.bit)
		{
			InputEvent e{InputEvent::KeyDown};
			e.key = b.key;
			out.push_back(e);
			held_[i] = 0;
			repeat_at_[i] = 0.40;
		}
		else if ((cur & b.bit) && (held_[i] += held_step) >= repeat_at_[i] && b.repeats)
		{
			InputEvent e{InputEvent::KeyDown};
			e.key = b.key;
			e.repeat = true;
			out.push_back(e);
			repeat_at_[i] = held_[i] + 0.07;
		}
	}
	prev_ = cur;
}

void DrawCursor(CDC& dc, int x, int y)
{
	// Windows' arrow (aero_arrow at 100 %): 1 = black outline, 2 = white
	static const char* const kArrow[] = {
		"1          ",
		"11         ",
		"121        ",
		"1221       ",
		"12221      ",
		"122221     ",
		"1222221    ",
		"12222221   ",
		"122222221  ",
		"1222222221 ",
		"12222222221",
		"122222111111",
		"1221221     ",
		"121 1221    ",
		"11  1221    ",
		"1    1221   ",
		"     1221   ",
		"      11    ",
	};
	const int s = std::max(1, ScaleX(100) / 100);
	const double f = ScaleFactor();
	for (int r = 0; r < int(sizeof(kArrow) / sizeof(kArrow[0])); r++)
		for (int c = 0; kArrow[r][c]; c++)
		{
			const char ch = kArrow[r][c];
			if (ch == ' ')
				continue;
			const int x0 = x + int(c * f), y0 = y + int(r * f);
			const int x1 = x + int((c + 1) * f), y1 = y + int((r + 1) * f);
			dc.FillSolidRect(CRect(x0, y0, std::max(x1, x0 + 1), std::max(y1, y0 + 1)), ch == '1' ? RGB(0, 0, 0) : RGB(255, 255, 255));
		}
	(void)s;
}

// ===========================================================================================================
// drawing helpers
namespace
{
void DrawCheckMark(CDC& dc, const CRect& box, COLORREF c)
{
	// Windows 10's check: a short and a long stroke, two pixels thick at 100 %
	const double f = box.Width() / 13.0;
	auto P = [&](double x, double y) { return CPoint{box.left + int(x * f), box.top + int(y * f)}; };
	const CPoint a = P(3, 6.5), b = P(5.5, 9), c2 = P(10, 4);
	const int th = std::max(2, int(1.6 * f));
	for (int t = 0; t < th; t++)
	{
		dc.MoveTo(a.x, a.y + t);
		dc.LineTo(b.x, b.y + t, c);
		dc.MoveTo(b.x, b.y + t);
		dc.LineTo(c2.x + 1, c2.y + t - 1, c);
	}
}

void DrawArrowGlyph(CDC& dc, int cx, int cy, int size, int dir, COLORREF c) // 0 down 1 up 2 right 3 left
{
	for (int i = 0; i < size; i++)
	{
		const int half = size - 1 - i;
		switch (dir)
		{
			case 0:
				dc.FillSolidRect(CRect(cx - half, cy - size / 2 + i, cx + half + 1, cy - size / 2 + i + 1), c);
				break;
			case 1:
				dc.FillSolidRect(CRect(cx - half, cy + size / 2 - i, cx + half + 1, cy + size / 2 - i + 1), c);
				break;
			case 2:
				dc.FillSolidRect(CRect(cx - size / 2 + i, cy - half, cx - size / 2 + i + 1, cy + half + 1), c);
				break;
			default:
				dc.FillSolidRect(CRect(cx + size / 2 - i, cy - half, cx + size / 2 - i + 1, cy + half + 1), c);
				break;
		}
	}
}

// the Windows 10 chevron (combo boxes, menus' submenu arrow)
void DrawChevron(CDC& dc, int cx, int cy, int size, bool down, COLORREF c)
{
	const int th = std::max(1, size / 4);
	for (int i = 0; i <= size; i++)
		for (int t = 0; t < th; t++)
		{
			if (down)
			{
				dc.SetPixel(cx - size + i, cy - size / 2 + i + t, c);
				dc.SetPixel(cx + size - i, cy - size / 2 + i + t, c);
			}
			else
			{
				dc.SetPixel(cx - size / 2 + i + t, cy - size + i, c);
				dc.SetPixel(cx - size / 2 + i + t, cy + size - i, c);
			}
		}
}
} // namespace

void DrawThemedButton(CDC& dc, const CRect& r, bool hot, bool pressed, bool focused, bool is_default, bool enabled)
{
	COLORREF face = sys::ButtonFace, border = sys::ButtonBorder;
	if (!enabled)
	{
		face = sys::ButtonDisabledFace;
		border = sys::ButtonDisabledBorder;
	}
	else if (pressed)
	{
		face = sys::ButtonPushedFace;
		border = sys::ButtonPushedBorder;
	}
	else if (hot)
	{
		face = sys::ButtonHotFace;
		border = sys::ButtonHotBorder;
	}
	else if (is_default || focused)
		border = sys::ButtonHotBorder;
	dc.FillSolidRect(r, face);
	const int t = std::max(1, ScaleFloorX(1));
	for (int i = 0; i < t; i++)
	{
		CRect b = r;
		b.DeflateRect(i, i);
		dc.FrameRect(b, border);
	}
	if ((is_default || focused) && enabled && !pressed && !hot)
	{
		CRect b = r;
		b.DeflateRect(t, t);
		dc.FrameRect(b, sys::ButtonHotBorder);
	}
	if (focused && enabled)
	{
		CRect f = r;
		f.DeflateRect(ScaleX(3), ScaleX(3));
		dc.DrawFocusRect(f);
	}
}

void DrawCaptionButton(CDC& dc, const CRect& r, int glyph, bool hot, bool pressed, bool active)
{
	COLORREF bg = sys::Caption, fg = active ? RGB(0, 0, 0) : sys::CaptionInactiveText;
	if (glyph == 3 && (hot || pressed))
	{
		bg = pressed ? sys::ClosePushed : sys::CloseHot;
		fg = RGB(255, 255, 255);
	}
	else if (pressed)
		bg = sys::CaptionButtonPushed;
	else if (hot)
		bg = sys::CaptionButtonHot;
	dc.FillSolidRect(r, bg);
	const CPoint c = r.CenterPoint();
	const int s = ScaleX(10) / 2; // the 10x10 glyphs of Segoe MDL2 Assets
	const int th = std::max(1, ScaleFloorX(1));
	switch (glyph)
	{
		case 0: // minimize
			dc.FillSolidRect(CRect(c.x - s, c.y, c.x + s, c.y + th), fg);
			break;
		case 1: // maximize
		{
			CRect b(c.x - s, c.y - s, c.x + s, c.y + s);
			for (int i = 0; i < th; i++)
			{
				dc.FrameRect(b, fg);
				b.DeflateRect(1, 1);
			}
			break;
		}
		case 2: // restore: two squares
		{
			const int o = ScaleX(2);
			CRect back(c.x - s + o, c.y - s, c.x + s, c.y + s - o);
			CRect front(c.x - s, c.y - s + o, c.x + s - o, c.y + s);
			for (int i = 0; i < th; i++)
			{
				dc.HLine(back.left, back.right, back.top + i, fg);
				dc.VLine(back.right - 1 - i, back.top, back.bottom, fg);
			}
			dc.FillSolidRect(front, bg);
			CRect f = front;
			for (int i = 0; i < th; i++)
			{
				dc.FrameRect(f, fg);
				f.DeflateRect(1, 1);
			}
			break;
		}
		default: // close: an X
			for (int i = -s; i <= s; i++)
				for (int t = 0; t < th + 1; t++)
				{
					dc.SetPixel(c.x + i, c.y + i + t - th / 2, fg);
					dc.SetPixel(c.x + i, c.y - i + t - th / 2, fg);
				}
			break;
	}
}

void DrawScrollBar(CDC& dc, const CRect& r, int total, int visible, int top)
{
	// Windows 10's scroll bar: a light track, a grey thumb, small arrows at the ends
	dc.FillSolidRect(r, RGB(240, 240, 240));
	const int w = r.Width();
	const COLORREF arrow = RGB(96, 96, 96);
	DrawArrowGlyph(dc, r.CenterPoint().x, r.top + w / 2, std::max(3, w / 5), 1, arrow);
	DrawArrowGlyph(dc, r.CenterPoint().x, r.bottom - w / 2, std::max(3, w / 5), 0, arrow);
	if (total <= visible || total <= 0)
		return;
	const int track = r.Height() - 2 * w;
	const int th = std::max(ScaleY(17), track * visible / total);
	const int y = r.top + w + (track - th) * std::max(0, std::min(top, total - visible)) / std::max(1, total - visible);
	dc.FillSolidRect(CRect(r.left + ScaleX(2), y, r.right - ScaleX(2), y + th), RGB(205, 205, 205));
}

// ===========================================================================================================
// controls
void Static::Paint(CDC& dc, bool, bool)
{
	CRect r = rect;
	if (icon)
	{
		dc.AlphaBlit(r.left, r.top, icon->Bmp());
		return;
	}
	DrawText(dc, bold ? UiBoldFont() : UiFont(), text, r, flags, enabled ? color : sys::GrayText);
}

void Button::Paint(CDC& dc, bool focused, bool hot)
{
	DrawThemedButton(dc, rect, hot, pressed, focused, is_default, enabled);
	CRect r = rect;
	DrawText(dc, UiFont(), text, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX,
		enabled ? sys::WindowText : sys::GrayText);
}

void CheckBox::Paint(CDC& dc, bool focused, bool hot)
{
	const int bs = ScaleX(13);
	const bool multiline = rect.Height() > UiFont().Height() * 3 / 2;
	const int y = multiline ? rect.top + (UiFont().Height() - bs) / 2 : rect.top + (rect.Height() - bs) / 2;
	CRect box(rect.left, y, rect.left + bs, y + bs);
	const COLORREF border = !enabled ? RGB(204, 204, 204) : hot ? sys::ButtonHotBorder : RGB(51, 51, 51);
	const COLORREF mark = !enabled ? RGB(204, 204, 204) : hot ? sys::ButtonHotBorder : RGB(51, 51, 51);
	if (radio)
	{
		// a circle
		const double cx = box.left + bs / 2.0 - 0.5, cy = box.top + bs / 2.0 - 0.5, rr = bs / 2.0;
		for (int yy = box.top; yy < box.bottom; yy++)
			for (int xx = box.left; xx < box.right; xx++)
			{
				const double d = std::sqrt((xx - cx) * (xx - cx) + (yy - cy) * (yy - cy));
				if (d <= rr - 1.0)
					dc.SetPixel(xx, yy, RGB(255, 255, 255));
				else if (d <= rr)
					dc.SetPixel(xx, yy, border);
				if (checked && d <= rr * 0.42)
					dc.SetPixel(xx, yy, mark);
			}
	}
	else
	{
		dc.FillSolidRect(box, RGB(255, 255, 255));
		const int t = std::max(1, ScaleFloorX(1));
		CRect b = box;
		for (int i = 0; i < t; i++)
		{
			dc.FrameRect(b, border);
			b.DeflateRect(1, 1);
		}
		if (checked)
			DrawCheckMark(dc, box, mark);
	}
	CRect tr(rect.left + bs + ScaleX(5), rect.top, rect.right, rect.bottom);
	if (rect.Height() > UiFont().Height() * 3 / 2)
	{
		// BS_MULTILINE | BS_TOP: the text wraps, the box at the first line
		CRect wr = tr;
		DrawText(dc, UiFont(), text, wr, DT_LEFT | DT_WORDBREAK | DT_HIDEPREFIX, enabled ? sys::WindowText : sys::GrayText);
		return;
	}
	DrawText(dc, UiFont(), text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX,
		enabled ? sys::WindowText : sys::GrayText);
	if (focused)
	{
		CRect calc = tr;
		DrawText(dc, UiFont(), text, calc, DT_CALCRECT | DT_SINGLELINE, 0);
		CRect f(tr.left - 1, tr.top + (tr.Height() - calc.Height()) / 2, tr.left + calc.Width() + 2,
			tr.top + (tr.Height() + calc.Height()) / 2 + 1);
		dc.DrawFocusRect(f);
	}
}

void CheckBox::Activate()
{
	if (radio)
	{
		if (parent)
			parent->UncheckGroup(this);
		checked = true;
	}
	else
		checked = !checked;
	if (on_change)
		on_change();
}

void GroupBox::Paint(CDC& dc, bool, bool)
{
	const Font& f = UiFont();
	const int ty = rect.top + f.Height() / 2;
	const COLORREF line = RGB(220, 220, 220);
	int tw = text.empty() ? 0 : f.Measure(StripPrefix(text)) + ScaleX(4);
	int tx = rect.left + ScaleX(6);
	if (title_check)
	{
		// the box, the gap and the text as CheckBox::Paint draws them, with a little room on each side
		tx = title_check->rect.left - ScaleX(2);
		tw = std::min(int(title_check->rect.right) - tx,
			ScaleX(13) + ScaleX(5) + f.Measure(StripPrefix(title_check->text)) + ScaleX(4));
	}
	dc.HLine(rect.left, tx, ty, line);
	dc.HLine(tx + tw, rect.right, ty, line);
	dc.VLine(rect.left, ty, rect.bottom, line);
	dc.VLine(rect.right - 1, ty, rect.bottom, line);
	dc.HLine(rect.left, rect.right, rect.bottom - 1, line);
	if (!text.empty())
	{
		CRect r(tx + ScaleX(2), rect.top, tx + tw, rect.top + f.Height());
		DrawText(dc, f, text, r, DT_LEFT | DT_SINGLELINE | DT_HIDEPREFIX, RGB(0, 70, 213));
	}
}

void Edit::Paint(CDC& dc, bool focused, bool hot)
{
	const COLORREF border = !enabled ? RGB(204, 204, 204) : focused ? sys::EditBorderFocus : hot ? sys::EditBorderHot : sys::EditBorder;
	dc.FillSolidRect(rect, read_only || !enabled ? sys::BtnFace : RGB(255, 255, 255));
	dc.FrameRect(rect, border);
	CRect r = rect;
	r.DeflateRect(ScaleX(4), 0);
	std::string shown = password ? std::string(text.size(), '*') : text;
	if (shown.empty() && !cue.empty() && !focused)
		DrawText(dc, UiFont(), cue, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, sys::GrayText);
	else
	{
		// the end of a long text shows, as Windows scrolls an edit box to its caret
		const Font& f = UiFont();
		if (f.Measure(shown) > r.Width())
		{
			std::string s = shown;
			while (!s.empty() && f.Measure("..." + s) > r.Width())
			{
				size_t cut = 1;
				while (cut < s.size() && (s[cut] & 0xc0) == 0x80)
					cut++;
				s.erase(0, cut);
			}
			shown = "..." + s;
		}
		DrawText(dc, f, shown, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, enabled ? sys::WindowText : sys::GrayText);
		if (focused && !read_only)
		{
			const int cx = r.left + f.Measure(shown) + 1;
			if (int(NowSeconds() * 2) % 2 == 0)
				dc.VLine(cx, r.top + ScaleY(4), r.bottom - ScaleY(4), RGB(0, 0, 0));
		}
	}
}

void Edit::Activate()
{
	if (read_only || !enabled)
		return;
	OnScreenKeyboard(keyboard_title.empty() ? TC("keyboard", "Type") : keyboard_title, text, password,
		[this](bool ok, const std::string& t) {
			if (ok)
			{
				text = t;
				if (on_change)
					on_change();
			}
		});
}

void ComboBox::Paint(CDC& dc, bool focused, bool hot)
{
	const int bw = ScaleX(17);
	if (editable)
	{
		dc.FillSolidRect(rect, RGB(255, 255, 255));
		dc.FrameRect(rect, !enabled ? RGB(204, 204, 204) : focused ? sys::EditBorderFocus : hot ? sys::EditBorderHot : sys::EditBorder);
		CRect r(rect.left + ScaleX(4), rect.top, rect.right - bw, rect.bottom);
		DrawText(dc, UiFont(), text, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
			enabled ? sys::WindowText : sys::GrayText);
		CRect b(rect.right - bw, rect.top + 1, rect.right - 1, rect.bottom - 1);
		if (hot)
			dc.FillSolidRect(b, sys::ButtonHotFace);
		DrawChevron(dc, b.CenterPoint().x, b.CenterPoint().y, ScaleX(4), true, RGB(96, 96, 96));
		return;
	}
	DrawThemedButton(dc, rect, hot, false, false, false, enabled);
	CRect r(rect.left + ScaleX(4), rect.top, rect.right - bw, rect.bottom);
	const std::string t = Text();
	if (focused)
	{
		CRect f(rect.left + ScaleX(3), rect.top + ScaleY(3), rect.right - bw, rect.bottom - ScaleY(3));
		dc.FillSolidRect(f, sys::Highlight);
		DrawText(dc, UiFont(), t, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, sys::HighlightText);
	}
	else
		DrawText(dc, UiFont(), t, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
			enabled ? sys::WindowText : sys::GrayText);
	DrawChevron(dc, rect.right - bw / 2 - ScaleX(2), rect.CenterPoint().y, ScaleX(4), true, RGB(96, 96, 96));
}

void ComboBox::Activate()
{
	if (!enabled)
		return;
	if (editable && items.empty())
	{
		OnScreenKeyboard(TC("keyboard", "Type"), text, false, [this](bool ok, const std::string& t) {
			if (ok)
			{
				text = t;
				if (on_change)
					on_change();
			}
		});
		return;
	}
	std::vector<std::string> list = items;
	if (editable)
		list.insert(list.begin(), T("(Type with the on-screen keyboard...)"));
	DropDownList(rect, list, editable ? -1 : sel, [this](int i) {
		if (i < 0)
			return;
		if (editable)
		{
			if (i == 0)
			{
				OnScreenKeyboard(TC("keyboard", "Type"), text, false, [this](bool ok, const std::string& t) {
					if (ok)
					{
						text = t;
						if (on_change)
							on_change();
					}
				});
				return;
			}
			text = items[i - 1];
			sel = i - 1;
		}
		else
			sel = i;
		if (on_change)
			on_change();
	});
}

bool ComboBox::OnKey(Key k)
{
	if (editable)
		return false;
	if (k == Key::Left || k == Key::Right)
	{
		// a closed drop-down list: Left/Right change the choice (Up/Down in Windows; here they move between
		// the controls)
		if (k == Key::Left && sel > 0)
			sel--;
		else if (k == Key::Right && sel + 1 < int(items.size()))
			sel++;
		else
			return true;
		if (on_change)
			on_change();
		return true;
	}
	return false;
}

void Slider::Paint(CDC& dc, bool focused, bool hot)
{
	// Windows 10's trackbar: a thin track, a slim blue thumb
	const int cy = rect.CenterPoint().y;
	const int pad = ScaleX(6);
	CRect track(rect.left + pad, cy - ScaleY(2), rect.right - pad, cy + ScaleY(2));
	dc.FillSolidRect(track, RGB(231, 234, 234));
	dc.FrameRect(track, RGB(214, 214, 214));
	const int span = std::max(1, max - min);
	const int x = track.left + (track.Width() * (pos - min)) / span;
	const int tw = ScaleX(11) / 2, th = ScaleY(21) / 2;
	CRect thumb(x - tw / 1, cy - th, x + tw / 1 + 1, cy + th);
	thumb.left = x - ScaleX(5);
	thumb.right = x + ScaleX(6);
	dc.FillSolidRect(thumb, !enabled ? RGB(204, 204, 204) : hot ? RGB(23, 23, 23) : sys::Highlight);
	if (focused)
	{
		CRect f = rect;
		f.DeflateRect(1, 1);
		dc.DrawFocusRect(f);
	}
}

bool Slider::OnKey(Key k)
{
	int p = pos;
	if (k == Key::Left)
		p -= step;
	else if (k == Key::Right)
		p += step;
	else if (k == Key::PageDown)
		p += page;
	else if (k == Key::PageUp)
		p -= page;
	else
		return false;
	p = std::max(min, std::min(max, p));
	if (p != pos)
	{
		pos = p;
		if (on_change)
			on_change();
	}
	return true;
}

void Slider::OnClick(int x, int y)
{
	OnDrag(x, y);
}

void Slider::OnDrag(int x, int)
{
	const int pad = ScaleX(6);
	const int w = std::max(1, rect.Width() - 2 * pad);
	int p = min + ((x - rect.left - pad) * (max - min) + w / 2) / w;
	p = std::max(min, std::min(max, p));
	if (p != pos)
	{
		pos = p;
		if (on_change)
			on_change();
	}
}

int ListView::RowHeight() const
{
	return std::max(UiFont().Height() + ScaleY(4), ScaleY(20));
}

int ListView::VisibleRows() const
{
	const int hh = header ? ScaleY(24) : 0;
	return std::max(1, (rect.Height() - hh - 2) / RowHeight());
}

void ListView::EnsureVisible()
{
	const int vis = VisibleRows();
	if (sel < top)
		top = sel;
	if (sel >= top + vis)
		top = sel - vis + 1;
	top = std::max(0, std::min(top, std::max(0, int(rows.size()) - vis)));
}

void ListView::Paint(CDC& dc, bool focused, bool hot)
{
	dc.FillSolidRect(rect, RGB(255, 255, 255));
	dc.FrameRect(rect, RGB(130, 135, 144));
	const CRect old = dc.GetClip();
	CRect inner = rect;
	inner.DeflateRect(1, 1);
	CRect clip = inner;
	clip.Intersect(old);
	dc.SetClip(clip);
	const int sbw = ScaleX(17);
	const bool scroll = int(rows.size()) > VisibleRows();
	const int right = inner.right - (scroll ? sbw : 0);
	int y = inner.top;
	const Font& f = UiFont();
	if (header)
	{
		const int hh = ScaleY(24);
		int x = inner.left;
		for (const auto& c : columns)
		{
			CRect hr(x, y, std::min(x + c.width, right), y + hh);
			CRect tr = hr;
			tr.DeflateRect(ScaleX(6), 0);
			DrawText(dc, f, c.title, tr, (c.right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
				RGB(76, 96, 122));
			dc.VLine(hr.right - 1, hr.top, hr.bottom, RGB(229, 229, 229));
			x += c.width;
		}
		dc.HLine(inner.left, right, y + hh - 1, RGB(229, 229, 229));
		y += hh;
	}
	const int rh = RowHeight();
	const int vis = VisibleRows();
	for (int i = top; i < int(rows.size()) && i < top + vis + 1; i++)
	{
		CRect rr(inner.left, y, right, y + rh);
		if (i == sel)
		{
			dc.FillSolidRect(rr, focused ? sys::ListSelected : RGB(217, 217, 217));
			dc.FrameRect(rr, focused ? sys::ListSelectedBorder : RGB(217, 217, 217));
		}
		int x = inner.left;
		for (size_t c = 0; c < columns.size() && c < rows[i].size(); c++)
		{
			CRect cr(x + ScaleX(6), y, std::min(x + columns[c].width, right) - ScaleX(6), y + rh);
			if (c == 0 && i < int(icons.size()) && icons[i])
			{
				dc.AlphaBlit(cr.left, y + (rh - icons[i]->Height()) / 2, icons[i]->Bmp());
				cr.left += icons[i]->Width() + ScaleX(6);
			}
			DrawText(dc, f, rows[i][c], cr,
				(columns[c].right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
				c == 0 ? sys::WindowText : RGB(109, 109, 109));
			x += columns[c].width;
		}
		y += rh;
	}
	if (scroll)
		DrawScrollBar(dc, CRect(right, inner.top + (header ? ScaleY(24) : 0), inner.right, inner.bottom), int(rows.size()), vis, top);
	dc.SetClip(old);
	(void)hot;
}

bool ListView::OnKey(Key k)
{
	if (rows.empty())
		return k == Key::Up || k == Key::Down;
	const int old = sel;
	if (k == Key::Up && sel <= 0)
		return false; // the top: the focus moves on
	if (k == Key::Down && sel >= int(rows.size()) - 1)
		return false;
	if (k == Key::Up)
		sel = std::max(0, sel - 1);
	else if (k == Key::Down)
		sel = std::min(int(rows.size()) - 1, sel + 1);
	else if (k == Key::PageUp)
		sel = std::max(0, sel - VisibleRows());
	else if (k == Key::PageDown)
		sel = std::min(int(rows.size()) - 1, sel + VisibleRows());
	else
		return false;
	if (old < 0 && k == Key::Up)
		sel = 0;
	EnsureVisible();
	if (sel != old && on_select)
		on_select();
	return true;
}

void ListView::OnClick(int x, int y)
{
	const int hh = header ? ScaleY(24) : 0;
	const int sbw = ScaleX(17);
	if (int(rows.size()) > VisibleRows() && x >= rect.right - 1 - sbw)
	{
		// the scroll bar: page up or down
		OnKey(y < rect.CenterPoint().y ? Key::PageUp : Key::PageDown);
		return;
	}
	const int row = top + (y - rect.top - 1 - hh) / RowHeight();
	if (y < rect.top + 1 + hh || row < 0 || row >= int(rows.size()))
		return;
	const double now = NowSeconds();
	const bool dbl = row == last_click_row && now - last_click_t < 0.5;
	last_click_t = now;
	last_click_row = row;
	if (sel != row)
	{
		sel = row;
		if (on_select)
			on_select();
	}
	// a click on the selected row opens it (one click: the controller has no fast double click)
	if (dbl || (on_open && sel == row))
		Activate();
}

void ListView::OnWheel(int delta)
{
	top = std::max(0, std::min(top - delta * 3, std::max(0, int(rows.size()) - VisibleRows())));
}

int TreeView::RowHeight() const
{
	return std::max(UiFont().Height() + ScaleY(4), ScaleY(20));
}

void TreeView::Paint(CDC& dc, bool focused, bool)
{
	dc.FillSolidRect(rect, RGB(255, 255, 255));
	dc.FrameRect(rect, RGB(130, 135, 144));
	const CRect old = dc.GetClip();
	CRect inner = rect;
	inner.DeflateRect(1, 1);
	CRect clip = inner;
	clip.Intersect(old);
	dc.SetClip(clip);
	const int rh = RowHeight();
	int y = inner.top + ScaleY(2);
	for (int i = top; i < int(items.size()) && y < inner.bottom; i++)
	{
		const Item& it = items[i];
		CRect rr(inner.left + ScaleX(2), y, inner.right - ScaleX(2), y + rh);
		if (i == sel && !it.header)
		{
			dc.FillSolidRect(rr, focused ? sys::ListSelected : RGB(217, 217, 217));
			dc.FrameRect(rr, focused ? sys::ListSelectedBorder : RGB(217, 217, 217));
		}
		int x = rr.left + ScaleX(6) + it.level * ScaleX(16);
		if (it.icon)
		{
			dc.AlphaBlit(x, y + (rh - it.icon->Height()) / 2, it.icon->Bmp());
			x += it.icon->Width() + ScaleX(6);
		}
		CRect tr(x, y, rr.right, y + rh);
		DrawText(dc, it.header ? UiBoldFont() : UiFont(), it.text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
			it.header ? RGB(76, 96, 122) : sys::WindowText);
		y += rh;
	}
	dc.SetClip(old);
}

bool TreeView::OnKey(Key k)
{
	if (k != Key::Up && k != Key::Down)
		return false;
	if ((k == Key::Up && sel <= 0) || (k == Key::Down && sel >= int(items.size()) - 1))
		return false;
	int i = sel;
	const int dir = k == Key::Up ? -1 : 1;
	do
		i += dir;
	while (i >= 0 && i < int(items.size()) && items[i].header);
	if (i >= 0 && i < int(items.size()) && i != sel)
	{
		sel = i;
		const int vis = rect.Height() / RowHeight() - 1;
		if (sel < top)
			top = sel;
		if (sel >= top + vis)
			top = sel - vis + 1;
		if (on_select)
			on_select();
	}
	return true;
}

void TreeView::OnClick(int, int y)
{
	const int i = top + (y - rect.top - 1 - ScaleY(2)) / RowHeight();
	if (i >= 0 && i < int(items.size()) && !items[i].header)
	{
		sel = i;
		if (on_select)
			on_select();
	}
}

void TreeView::OnWheel(int delta)
{
	const int vis = rect.Height() / RowHeight() - 1;
	top = std::max(0, std::min(top - delta * 3, std::max(0, int(items.size()) - vis)));
}

CRect TabControl::TabRect(int i) const
{
	const Font& f = UiFont();
	int x = rect.left + ScaleX(2);
	for (int k = 0; k < i; k++)
		x += f.Measure(StripPrefix(tabs[k])) + ScaleX(14);
	const int w = f.Measure(StripPrefix(tabs[i])) + ScaleX(14);
	const int h = ScaleY(21);
	return CRect(x, rect.top, x + w, rect.top + h);
}

void TabControl::Paint(CDC& dc, bool focused, bool)
{
	const int h = ScaleY(21);
	CRect page(rect.left, rect.top + h - 1, rect.right, rect.bottom);
	dc.FillSolidRect(page, RGB(255, 255, 255));
	dc.FrameRect(page, RGB(217, 217, 217));
	for (int i = 0; i < int(tabs.size()); i++)
	{
		CRect t = TabRect(i);
		if (i == sel)
		{
			t.InflateRect(ScaleX(2), ScaleY(2), ScaleX(2), 0);
			dc.FillSolidRect(t, RGB(255, 255, 255));
			dc.HLine(t.left, t.right, t.top, RGB(217, 217, 217));
			dc.VLine(t.left, t.top, t.bottom, RGB(217, 217, 217));
			dc.VLine(t.right - 1, t.top, t.bottom, RGB(217, 217, 217));
		}
		else
		{
			dc.FillSolidRect(t, RGB(240, 240, 240));
			dc.FrameRect(t, RGB(217, 217, 217));
		}
		CRect tr = t;
		DrawText(dc, UiFont(), tabs[i], tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX, sys::WindowText);
		if (i == sel && focused)
		{
			CRect fr = t;
			fr.DeflateRect(ScaleX(3), ScaleY(3));
			dc.DrawFocusRect(fr);
		}
	}
}

bool TabControl::OnKey(Key k)
{
	if (k == Key::Left && sel > 0)
		sel--;
	else if (k == Key::Right && sel + 1 < int(tabs.size()))
		sel++;
	else
		return false;
	if (on_change)
		on_change();
	return true;
}

void TabControl::OnClick(int x, int y)
{
	for (int i = 0; i < int(tabs.size()); i++)
		if (TabRect(i).PtInRect(x, y))
		{
			if (sel != i)
			{
				sel = i;
				if (on_change)
					on_change();
			}
			return;
		}
}

int TextView::VisibleLines() const
{
	const Font& f = mono ? FontOfSize(ScaleY(12)) : UiFont();
	return std::max(1, (rect.Height() - ScaleY(6)) / f.Height());
}

void TextView::Paint(CDC& dc, bool focused, bool)
{
	dc.FillSolidRect(rect, RGB(255, 255, 255));
	dc.FrameRect(rect, focused ? sys::EditBorderFocus : sys::EditBorder);
	const Font& f = UiFont();
	const CRect old = dc.GetClip();
	CRect clip = rect;
	clip.DeflateRect(1, 1);
	clip.Intersect(old);
	dc.SetClip(clip);
	int y = rect.top + ScaleY(3);
	const int vis = VisibleLines();
	for (int i = top; i < int(lines.size()) && i < top + vis; i++)
	{
		f.Draw(dc, rect.left + ScaleX(6), y, lines[i], sys::WindowText);
		y += f.Height();
	}
	if (int(lines.size()) > vis)
		DrawScrollBar(dc, CRect(rect.right - 1 - ScaleX(17), rect.top + 1, rect.right - 1, rect.bottom - 1), int(lines.size()), vis, top);
	dc.SetClip(old);
}

bool TextView::OnKey(Key k)
{
	const int vis = VisibleLines();
	const int maxtop = std::max(0, int(lines.size()) - vis);
	if (k == Key::Up && top > 0)
		top = std::max(0, top - 1);
	else if (k == Key::Down && top < maxtop)
		top = std::min(maxtop, top + 1);
	else if (k == Key::PageUp)
		top = std::max(0, top - vis);
	else if (k == Key::PageDown)
		top = std::min(maxtop, top + vis);
	else
		return false;
	return true;
}

void TextView::OnWheel(int delta)
{
	const int maxtop = std::max(0, int(lines.size()) - VisibleLines());
	top = std::max(0, std::min(maxtop, top - delta * 3));
}

void NumberBox::Paint(CDC& dc, bool focused, bool hot)
{
	const int bw = ScaleX(17);
	dc.FillSolidRect(rect, enabled ? RGB(255, 255, 255) : sys::BtnFace);
	dc.FrameRect(rect, !enabled ? RGB(204, 204, 204) : focused ? sys::EditBorderFocus : hot ? sys::EditBorderHot : sys::EditBorder);
	CRect r(rect.left + ScaleX(4), rect.top, rect.right - bw - ScaleX(4), rect.bottom);
	DrawText(dc, UiFont(), std::to_string(value) + suffix, r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
		enabled ? sys::WindowText : sys::GrayText);
	// the up-down buddy
	CRect up(rect.right - bw, rect.top + 1, rect.right - 1, rect.CenterPoint().y);
	CRect dn(rect.right - bw, rect.CenterPoint().y, rect.right - 1, rect.bottom - 1);
	dc.FillSolidRect(up, sys::ButtonFace);
	dc.FillSolidRect(dn, sys::ButtonFace);
	dc.FrameRect(up, sys::ButtonBorder);
	dc.FrameRect(dn, sys::ButtonBorder);
	DrawArrowGlyph(dc, up.CenterPoint().x, up.CenterPoint().y, std::max(2, up.Height() / 4), 1, RGB(96, 96, 96));
	DrawArrowGlyph(dc, dn.CenterPoint().x, dn.CenterPoint().y, std::max(2, dn.Height() / 4), 0, RGB(96, 96, 96));
}

bool NumberBox::OnKey(Key k)
{
	int v = value;
	if (k == Key::Right)
		v += step;
	else if (k == Key::Left)
		v -= step;
	else
		return false;
	v = std::max(min, std::min(max, v));
	if (v != value)
	{
		value = v;
		if (on_change)
			on_change();
	}
	return true;
}

void NumberBox::OnClick(int x, int y)
{
	if (x >= rect.right - ScaleX(17))
		OnKey(y < rect.CenterPoint().y ? Key::Right : Key::Left);
}

void NumberBox::Activate()
{
	OnScreenKeyboard(T("Number"), std::to_string(value), false, [this](bool ok, const std::string& t) {
		if (!ok || t.empty())
			return;
		value = std::max(min, std::min(max, atoi(t.c_str())));
		if (on_change)
			on_change();
	});
}

// ===========================================================================================================
// dialogs
int Dialog::DuX(int du)
{
	return (du * 6 * g_dpi / 96 + 2) / 4;
}

int Dialog::DuY(int du)
{
	return (du * 13 * g_dpi / 96 + 4) / 8;
}

int Dialog::CaptionHeight() const
{
	return ScaleY(31);
}

CRect Dialog::Client() const
{
	return CRect(rect.left + 1, rect.top + CaptionHeight(), rect.right - 1, rect.bottom - 1);
}

CRect Dialog::DuRect(int x, int y, int w, int h) const
{
	const CRect c = Client();
	return CRect(c.left + DuX(x), c.top + DuY(y), c.left + DuX(x + w), c.top + DuY(y + h));
}

void Dialog::Center(const CRect& in)
{
	const int w = rect.Width(), h = rect.Height();
	rect.left = in.left + (in.Width() - w) / 2;
	rect.top = in.top + (in.Height() - h) / 2;
	rect.right = rect.left + w;
	rect.bottom = rect.top + h;
}

void Dialog::MoveBy(int dx, int dy)
{
	if (!dx && !dy)
		return;
	rect.OffsetRect(dx, dy);
	for (auto& c : controls)
		c->rect.OffsetRect(dx, dy);
	OnMove(dx, dy);
}

void Dialog::CancelPointer()
{
	if (moving_)
	{
		moving_ = false;
		if (moved_x_ || moved_y_)
		{
			OrbisLog("[ui] \"%s\" moved to %d,%d", title.c_str(), rect.left, rect.top);
			OnMoveDone();
		}
	}
	close_pressed_ = false;
	if (dragging_)
	{
		dragging_->OnRelease();
		dragging_ = nullptr;
	}
	for (auto& c : controls)
		if (Button* b = dynamic_cast<Button*>(c.get()))
			b->pressed = false;
}

void Dialog::Paint(CDC& dc)
{
	// Windows 10's shadow, then the frame
	for (int i = ScaleX(10); i >= 1; i--)
	{
		CRect s = rect;
		s.InflateRect(i, i);
		s.OffsetRect(0, i / 3);
		dc.BlendRect(CRect(s.left, s.top, s.right, rect.top), RGB(0, 0, 0), 4);
		dc.BlendRect(CRect(s.left, rect.bottom, s.right, s.bottom), RGB(0, 0, 0), 4);
		dc.BlendRect(CRect(s.left, rect.top, rect.left, rect.bottom), RGB(0, 0, 0), 4);
		dc.BlendRect(CRect(rect.right, rect.top, s.right, rect.bottom), RGB(0, 0, 0), 4);
	}
	dc.FillSolidRect(rect, sys::BtnFace);
	dc.FrameRect(rect, RGB(24, 131, 215));
	CRect cap(rect.left + 1, rect.top + 1, rect.right - 1, rect.top + CaptionHeight());
	dc.FillSolidRect(cap, sys::Caption);
	CRect tr(cap.left + ScaleX(10), cap.top, cap.right - ScaleX(46), cap.bottom);
	DrawText(dc, UiFont(), title, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, sys::CaptionText);
	if (has_close)
	{
		CRect cb(cap.right - ScaleX(46), cap.top, cap.right, cap.bottom);
		DrawCaptionButton(dc, cb, 3, close_hot_, close_pressed_, true);
	}
	PaintClient(dc);
	Control* f = Focused();
	for (auto& c : controls)
		if (c->visible)
			c->Paint(dc, c.get() == f, c.get() == hot_ && c->enabled);
}

void Dialog::SetFocus(Control* c)
{
	for (int i = 0; i < int(controls.size()); i++)
		if (controls[i].get() == c)
		{
			focus = i;
			return;
		}
}

Button* Dialog::DefaultButton() const
{
	for (auto& c : controls)
		if (Button* b = dynamic_cast<Button*>(c.get()))
			if (b->is_default && b->visible && b->enabled)
				return b;
	return nullptr;
}

void Dialog::UncheckGroup(CheckBox* except)
{
	for (auto& c : controls)
		if (CheckBox* cb = dynamic_cast<CheckBox*>(c.get()))
			if (cb != except && cb->radio && cb->group == except->group)
				cb->checked = false;
}

void Dialog::NextFocus(int dir)
{
	const int n = int(controls.size());
	if (!n)
		return;
	int i = focus < 0 ? (dir > 0 ? -1 : n) : focus;
	for (int k = 0; k < n; k++)
	{
		i = (i + dir + n) % n;
		if (controls[i]->Focusable())
		{
			focus = i;
			return;
		}
	}
}

void Dialog::MoveFocus(int dx, int dy)
{
	Control* cur = Focused();
	if (!cur)
	{
		NextFocus(1);
		return;
	}
	const CPoint c = cur->rect.CenterPoint();
	int best = -1;
	double best_d = 1e18;
	for (int i = 0; i < int(controls.size()); i++)
	{
		Control* o = controls[i].get();
		if (o == cur || !o->Focusable())
			continue;
		const CPoint p = o->rect.CenterPoint();
		const int ddx = p.x - c.x, ddy = p.y - c.y;
		// in that direction: the main axis forward, the cross axis weighted
		double main = dx ? ddx * dx : ddy * dy;
		// overlapping rows/columns count as "straight ahead"
		if (dx && o->rect.bottom > cur->rect.top && o->rect.top < cur->rect.bottom)
			main = dx > 0 ? o->rect.left - cur->rect.right + 1 : cur->rect.left - o->rect.right + 1;
		if (dy && o->rect.right > cur->rect.left && o->rect.left < cur->rect.right)
			main = dy > 0 ? o->rect.top - cur->rect.bottom + 1 : cur->rect.top - o->rect.bottom + 1;
		if (main <= 0)
			continue;
		const double cross = dx ? std::abs(ddy) : std::abs(ddx);
		const double d = main + cross * 2.5;
		if (d < best_d)
		{
			best_d = d;
			best = i;
		}
	}
	if (best >= 0)
		focus = best;
}

bool Dialog::OnEvent(const InputEvent& ev)
{
	const CRect cap(rect.left + 1, rect.top + 1, rect.right - 1, rect.top + CaptionHeight());
	const CRect closeb(cap.right - ScaleX(46), cap.top, cap.right, cap.bottom);
	switch (ev.type)
	{
		case InputEvent::MouseMove:
		{
			if (moving_)
			{
				// the title bar follows the pointer; the whole window stays on the screen
				const CRect& scr = WindowManager::Get().screen;
				const int w = rect.Width(), h = rect.Height();
				int x = ev.x - grab_x_, y = ev.y - grab_y_;
				x = std::max(scr.left, std::min(x, scr.right - w));
				y = std::max(scr.top, std::min(y, scr.bottom - h));
				const int dx = x - rect.left, dy = y - rect.top;
				MoveBy(dx, dy);
				moved_x_ += dx;
				moved_y_ += dy;
				return true;
			}
			close_hot_ = has_close && closeb.PtInRect(ev.x, ev.y);
			hot_ = nullptr;
			for (auto& c : controls)
				if (c->visible && c->rect.PtInRect(ev.x, ev.y))
					hot_ = c.get();
			if (dragging_)
				dragging_->OnDrag(ev.x, ev.y);
			return true;
		}
		case InputEvent::LButtonDown:
		{
			if (has_close && closeb.PtInRect(ev.x, ev.y))
			{
				close_pressed_ = true;
				return true;
			}
			if (cap.PtInRect(ev.x, ev.y))
			{
				// the title bar: held, it moves the window
				moving_ = true;
				grab_x_ = ev.x - rect.left;
				grab_y_ = ev.y - rect.top;
				moved_x_ = moved_y_ = 0;
				return true;
			}
			for (int i = int(controls.size()) - 1; i >= 0; i--)
			{
				Control* c = controls[i].get();
				if (c->visible && c->enabled && c->rect.PtInRect(ev.x, ev.y))
				{
					if (c->tab_stop)
						focus = i;
					if (Button* b = dynamic_cast<Button*>(c))
						b->pressed = true;
					else
					{
						dragging_ = c;
						c->OnClick(ev.x, ev.y);
					}
					return true;
				}
			}
			return true;
		}
		case InputEvent::LButtonUp:
		{
			if (moving_)
			{
				moving_ = false;
				if (moved_x_ || moved_y_)
				{
					OrbisLog("[ui] \"%s\" moved to %d,%d", title.c_str(), rect.left, rect.top);
					OnMoveDone();
				}
				return true;
			}
			if (close_pressed_)
			{
				close_pressed_ = false;
				if (closeb.PtInRect(ev.x, ev.y))
					OnCancel();
				return true;
			}
			if (dragging_)
			{
				dragging_->OnRelease();
				dragging_ = nullptr;
			}
			for (auto& c : controls)
				if (Button* b = dynamic_cast<Button*>(c.get()))
					if (b->pressed)
					{
						b->pressed = false;
						if (b->rect.PtInRect(ev.x, ev.y) && b->enabled)
							b->Activate();
					}
			return true;
		}
		case InputEvent::Wheel:
			for (auto& c : controls)
				if (c->visible && c->rect.PtInRect(ev.x, ev.y))
				{
					c->OnWheel(ev.wheel);
					return true;
				}
			if (Control* f = Focused())
				f->OnWheel(ev.wheel);
			return true;
		case InputEvent::KeyDown:
		{
			Control* f = Focused();
			switch (ev.key)
			{
				case Key::Escape:
					OnCancel();
					return true;
				case Key::Enter:
					if (f && f->enabled)
					{
						if (dynamic_cast<Button*>(f) || dynamic_cast<CheckBox*>(f) || dynamic_cast<Edit*>(f) ||
							dynamic_cast<ComboBox*>(f) || dynamic_cast<ListView*>(f) || dynamic_cast<NumberBox*>(f))
						{
							f->Activate();
							return true;
						}
					}
					if (Button* b = DefaultButton())
						b->Activate();
					return true;
				case Key::Menu:
					// Options: the default button (OK), as Enter on a keyboard
					if (Button* b = DefaultButton())
						b->Activate();
					return true;
				case Key::R1:
				case Key::Tab:
					NextFocus(1);
					return true;
				case Key::L1:
				case Key::ShiftTab:
					NextFocus(-1);
					return true;
				case Key::L2:
					if (f && f->OnKey(Key::PageUp))
						return true;
					return true;
				case Key::R2:
					if (f && f->OnKey(Key::PageDown))
						return true;
					return true;
				case Key::Left:
				case Key::Right:
				case Key::Up:
				case Key::Down:
					if (f && f->OnKey(ev.key))
						return true;
					MoveFocus(ev.key == Key::Left ? -1 : ev.key == Key::Right ? 1 : 0,
						ev.key == Key::Up ? -1 : ev.key == Key::Down ? 1 : 0);
					return true;
				default:
					if (f)
						f->OnKey(ev.key);
					return true;
			}
		}
		default:
			return true;
	}
}

// ===========================================================================================================
// the modal stack
WindowManager& WindowManager::Get()
{
	static WindowManager wm;
	return wm;
}

void WindowManager::Push(Dialog* d)
{
	if (d->focus < 0)
	{
		if (Button* b = d->DefaultButton())
			d->SetFocus(b);
		else
			d->NextFocus(1);
	}
	if (!dialogs_.empty())
		dialogs_.back()->CancelPointer();
	dialogs_.emplace_back(d);
}

void WindowManager::PushPopup(Popup* p)
{
	if (!dialogs_.empty())
		dialogs_.back()->CancelPointer();
	popups_.emplace_back(p);
}

bool WindowManager::Dispatch(const InputEvent& ev)
{
	// the right stick's left / right are the window's keys only: menus and dialogs don't see them
	if (ev.type == InputEvent::KeyDown && (ev.key == Key::StickLeft || ev.key == Key::StickRight))
		return !popups_.empty() || !dialogs_.empty();
	if (!popups_.empty())
	{
		Popup* p = popups_.back().get();
		const bool used = p->OnEvent(ev);
		if (used)
			return true;
		// a click outside a popup closes it and goes no further (as Windows' menus)
		return true;
	}
	if (!dialogs_.empty())
	{
		dialogs_.back()->OnEvent(ev);
		return true;
	}
	return false;
}

void WindowManager::Tick()
{
	for (auto& d : dialogs_)
		d->Tick();
	// close what asked to close (a closing dialog may open another: callbacks after the erase)
	for (size_t i = 0; i < popups_.size();)
		if (popups_[i]->closed)
			popups_.erase(popups_.begin() + long(i));
		else
			i++;
	for (size_t i = 0; i < dialogs_.size();)
		if (dialogs_[i]->closed)
		{
			std::unique_ptr<Dialog> d = std::move(dialogs_[i]);
			dialogs_.erase(dialogs_.begin() + long(i));
			if (d->on_close)
				d->on_close(d->result);
		}
		else
			i++;
}

void WindowManager::Paint(CDC& dc)
{
	for (auto& d : dialogs_)
		d->Paint(dc);
	for (auto& p : popups_)
		p->Paint(dc);
}

// ===========================================================================================================
// the drop-down list
namespace
{
class DropList : public Popup
{
public:
	CRect r;
	std::vector<std::string> items;
	int sel = -1, hot = -1, top = 0, vis = 0;
	std::function<void(int)> done;

	int RowH() const { return UiFont().Height() + ScaleY(4); }
	void Paint(CDC& dc) override
	{
		dc.FillSolidRect(r, RGB(255, 255, 255));
		dc.FrameRect(r, RGB(100, 100, 100));
		int y = r.top + 1;
		for (int i = top; i < int(items.size()) && i < top + vis; i++)
		{
			CRect rr(r.left + 1, y, r.right - 1 - (int(items.size()) > vis ? ScaleX(17) : 0), y + RowH());
			const bool h = i == hot;
			if (h)
				dc.FillSolidRect(rr, sys::Highlight);
			CRect tr = rr;
			tr.DeflateRect(ScaleX(4), 0);
			DrawText(dc, UiFont(), items[i], tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
				h ? sys::HighlightText : sys::WindowText);
			y += RowH();
		}
		if (int(items.size()) > vis)
			DrawScrollBar(dc, CRect(r.right - 1 - ScaleX(17), r.top + 1, r.right - 1, r.bottom - 1), int(items.size()), vis, top);
	}
	void Finish(int i)
	{
		closed = true;
		if (done)
			done(i);
	}
	void Ensure()
	{
		if (hot < top)
			top = hot;
		if (hot >= top + vis)
			top = hot - vis + 1;
	}
	bool OnEvent(const InputEvent& ev) override
	{
		switch (ev.type)
		{
			case InputEvent::MouseMove:
				if (r.PtInRect(ev.x, ev.y))
				{
					const int i = top + (ev.y - r.top - 1) / RowH();
					if (i >= 0 && i < int(items.size()))
						hot = i;
				}
				return true;
			case InputEvent::LButtonDown:
				if (r.PtInRect(ev.x, ev.y))
				{
					const int i = top + (ev.y - r.top - 1) / RowH();
					if (i >= 0 && i < int(items.size()))
						Finish(i);
				}
				else
					Finish(-1);
				return true;
			case InputEvent::Wheel:
				top = std::max(0, std::min(top - ev.wheel, std::max(0, int(items.size()) - vis)));
				return true;
			case InputEvent::KeyDown:
				if (ev.key == Key::Up && hot > 0)
					hot--, Ensure();
				else if (ev.key == Key::Down && hot + 1 < int(items.size()))
					hot++, Ensure();
				else if (ev.key == Key::Enter)
					Finish(hot);
				else if (ev.key == Key::Escape || ev.key == Key::Menu)
					Finish(-1);
				return true;
			default:
				return true;
		}
	}
};
} // namespace

void DropDownList(const CRect& under, const std::vector<std::string>& items, int sel, std::function<void(int)> done)
{
	DropList* p = new DropList;
	p->items = items;
	p->sel = sel;
	p->hot = std::max(0, sel);
	p->done = std::move(done);
	p->vis = std::min(int(items.size()), 12);
	const int h = p->vis * p->RowH() + 2;
	p->r = CRect(under.left, under.bottom, under.right, under.bottom + h);
	if (p->r.bottom > 1080)
		p->r = CRect(under.left, under.top - h, under.right, under.top);
	p->Ensure();
	WindowManager::Get().PushPopup(p);
}

// ===========================================================================================================
// message box
namespace
{
const Image& MessageIcon(int kind)
{
	// drawn once at the current scale: the circles of Windows 10's message icons
	static Image icons[5];
	static int made_dpi = 0;
	if (made_dpi != g_dpi)
	{
		made_dpi = g_dpi;
		const int s = ScaleX(32);
		for (int k = 1; k <= 4; k++)
		{
			std::vector<uint8_t> rgba(size_t(s) * s * 4, 0);
			const uint8_t col[5][3] = {{0, 0, 0}, {0, 120, 215}, {255, 185, 0}, {232, 17, 35}, {0, 120, 215}};
			const double c = s / 2.0, rr = s / 2.0 - 0.5;
			for (int y = 0; y < s; y++)
				for (int x = 0; x < s; x++)
				{
					double a;
					if (k == 2)
					{
						// a triangle
						const double tx = std::fabs(x + 0.5 - c), ty = y + 0.5;
						a = (ty >= s * 0.08 && ty <= s * 0.92 && tx <= (ty - s * 0.08) * 0.58) ? 1 : 0;
					}
					else
					{
						const double d = std::sqrt((x + 0.5 - c) * (x + 0.5 - c) + (y + 0.5 - c) * (y + 0.5 - c));
						a = std::max(0.0, std::min(1.0, rr - d + 0.5));
					}
					uint8_t* o = &rgba[(size_t(y) * s + x) * 4];
					o[0] = col[k][0];
					o[1] = col[k][1];
					o[2] = col[k][2];
					o[3] = uint8_t(a * 255);
				}
			icons[k].FromRGBA(rgba.data(), s, s);
		}
	}
	return icons[std::max(1, std::min(4, kind))];
}

class MsgBox : public Dialog
{
public:
	std::string text;
	int icon = 1;
	std::vector<std::string> lines;
	void PaintClient(CDC& dc) override
	{
		const CRect c = Client();
		const int strip = ScaleY(42);
		dc.FillSolidRect(CRect(c.left, c.top, c.right, c.bottom - strip), RGB(255, 255, 255));
		int x = c.left + ScaleX(26);
		if (icon)
		{
			const Image& ic = MessageIcon(icon);
			dc.AlphaBlit(x, c.top + ScaleY(26), ic.Bmp());
			const char* glyph = icon == 1 ? "i" : icon == 2 ? "!" : icon == 3 ? "x" : "?";
			Font& gf = FontOfSize(ScaleY(icon == 2 ? 18 : 20), true);
			CRect gr(x, c.top + ScaleY(26) + (icon == 2 ? ScaleY(5) : 0), x + ic.Width(), c.top + ScaleY(26) + ic.Height());
			DrawText(dc, gf, glyph, gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, icon == 2 ? RGB(0, 0, 0) : RGB(255, 255, 255));
			x += ic.Width() + ScaleX(12);
		}
		int y = c.top + ScaleY(26) + (icon ? (ScaleY(32) - UiFont().Height()) / 2 : 0);
		for (const auto& l : lines)
		{
			UiFont().Draw(dc, x, y, l, sys::WindowText);
			y += UiFont().Height();
		}
	}
};
} // namespace

void MessageBox(const std::string& text, const std::string& caption, int buttons, int icon, std::function<void(int)> done)
{
	OrbisLog("[ui] message: %s", text.c_str());
	MsgBox* d = new MsgBox;
	d->title = caption;
	d->text = text;
	d->icon = icon;
	const int maxw = ScaleX(560);
	d->lines = WrapText(UiFont(), text, maxw);
	int tw = 0;
	for (const auto& l : d->lines)
		tw = std::max(tw, UiFont().Measure(l));
	const int icon_w = icon ? ScaleX(32) + ScaleX(12) : 0;
	const int bw = ScaleX(88), bh = ScaleY(26);
	const int nb = buttons == MB_OK ? 1 : 2;
	const int w = std::max(ScaleX(26) * 2 + icon_w + tw + 2, nb * (bw + ScaleX(10)) + ScaleX(30));
	const int text_h = std::max(int(d->lines.size()) * UiFont().Height(), icon ? ScaleY(32) : 0);
	const int h = d->CaptionHeight() + ScaleY(26) * 2 + text_h + ScaleY(42) + 1;
	d->rect = CRect(0, 0, std::min(w, 1800), h);
	d->Center(WindowManager::Get().screen);
	const CRect c = d->Client();
	struct B
	{
		const char* text;
		int id;
	};
	std::vector<B> bs;
	if (buttons == MB_YESNO)
		bs = {{"&Yes", IDYES}, {"&No", IDNO}};
	else if (buttons == MB_OKCANCEL)
		bs = {{"OK", IDOK}, {"Cancel", IDCANCEL}};
	else
		bs = {{"OK", IDOK}};
	int x = c.right - ScaleX(10) - int(bs.size()) * (bw + ScaleX(10)) + ScaleX(10);
	for (size_t i = 0; i < bs.size(); i++)
	{
		Button* b = d->Add(new Button);
		b->text = T(bs[i].text);
		b->rect = CRect(x, c.bottom - ScaleY(42) + (ScaleY(42) - bh) / 2, x + bw, c.bottom - ScaleY(42) + (ScaleY(42) + bh) / 2);
		b->is_default = i == 0;
		const int id = bs[i].id;
		MsgBox* dd = d;
		b->on_click = [dd, id] { dd->EndDialog(id); };
		x += bw + ScaleX(10);
	}
	const int cancel = buttons == MB_YESNO ? IDNO : IDCANCEL;
	d->on_close = [done, cancel, buttons](int r) {
		if (done)
			done(r == 2 && buttons != MB_OK ? cancel : r);
	};
	WindowManager::Get().Push(d);
}

// ===========================================================================================================
// the on-screen keyboard (Windows 10's osk.exe: dark keys, the pressed one blue)
namespace
{
class Keyboard : public Dialog
{
public:
	std::string text;
	bool password = false, shift = false, caps = false;
	int row = 1, col = 0;
	std::function<void(bool, const std::string&)> done;
	bool finished = false;

	struct K
	{
		std::string label, shifted, action; // action: "" types the label
		int w; // in quarter keys
	};
	std::vector<std::vector<K>> rows;

	Keyboard()
	{
		auto R = [](const char* lo, const char* hi) {
			std::vector<K> r;
			for (size_t i = 0; lo[i]; i++)
				r.push_back({std::string(1, lo[i]), std::string(1, hi[i]), "", 4});
			return r;
		};
		rows.push_back({{"http://", "http://", "", 8}, {"https://", "https://", "", 8}, {"smb://", "smb://", "", 8},
			{"www.", "www.", "", 6}, {".com", ".com", "", 6}, {".br", ".br", "", 6}, {"/", "/", "", 4}, {":", ":", "", 4}});
		auto r1 = R("`1234567890-=", "~!@#$%^&*()_+");
		r1.push_back({"Bksp", "Bksp", "bksp", 8});
		rows.push_back(r1);
		auto r2 = R("qwertyuiop[]\\", "QWERTYUIOP{}|");
		r2.insert(r2.begin(), {"Tab", "Tab", "tab", 6});
		rows.push_back(r2);
		auto r3 = R("asdfghjkl;'", "ASDFGHJKL:\"");
		r3.insert(r3.begin(), {"Caps", "Caps", "caps", 7});
		r3.push_back({"Enter", "Enter", "ok", 9});
		rows.push_back(r3);
		auto r4 = R("zxcvbnm,./", "ZXCVBNM<>?");
		r4.insert(r4.begin(), {"Shift", "Shift", "shift", 9});
		r4.push_back({"Shift", "Shift", "shift", 11});
		rows.push_back(r4);
		rows.push_back({{"@", "@", "", 4}, {"Clear", "Clear", "clear", 8}, {"Space", "Space", "space", 28},
			{"Cancel", "Cancel", "cancel", 10}, {"Done", "Done", "ok", 10}});
		has_close = true;
	}

	int KeyUnit() const { return ScaleX(15); } // a quarter key
	int KeyH() const { return ScaleY(52); }
	CRect KeyRect(int r, int c) const
	{
		const CRect cl = Client();
		const int top = cl.top + ScaleY(70) + r * (KeyH() + ScaleY(6));
		int x = cl.left + ScaleX(14);
		for (int i = 0; i < c; i++)
			x += rows[r][i].w * KeyUnit() + ScaleX(6);
		return CRect(x, top, x + rows[r][c].w * KeyUnit(), top + KeyH());
	}
	bool Upper() const { return shift != caps; }

	void Press(const K& k)
	{
		if (k.action == "bksp")
		{
			if (!text.empty())
			{
				size_t n = text.size() - 1;
				while (n > 0 && (text[n] & 0xc0) == 0x80)
					n--;
				text.erase(n);
			}
		}
		else if (k.action == "tab")
			text += "\t";
		else if (k.action == "caps")
			caps = !caps;
		else if (k.action == "shift")
			shift = !shift;
		else if (k.action == "space")
			text += " ";
		else if (k.action == "clear")
			text.clear();
		else if (k.action == "ok")
			Finish(true);
		else if (k.action == "cancel")
			Finish(false);
		else
		{
			text += Upper() ? k.shifted : k.label;
			if (shift)
				shift = false;
		}
	}
	void Finish(bool ok)
	{
		if (finished)
			return;
		finished = true;
		if (done)
			done(ok, ok ? text : std::string());
		EndDialog(ok ? 1 : 2);
	}
	void OnCancel() override { Finish(false); }
	void OnOk() override { Finish(true); }

	void PaintClient(CDC& dc) override
	{
		const CRect c = Client();
		dc.FillSolidRect(c, RGB(31, 31, 31));
		// the text being typed, in an edit box above the keys
		CRect e(c.left + ScaleX(14), c.top + ScaleY(14), c.right - ScaleX(14), c.top + ScaleY(54));
		dc.FillSolidRect(e, RGB(255, 255, 255));
		dc.FrameRect(e, sys::EditBorderFocus);
		const Font& f = FontOfSize(ScaleY(16));
		std::string shown = password ? std::string(text.size(), '*') : text;
		for (char& ch : shown)
			if (ch == '\t')
				ch = ' ';
		CRect tr = e;
		tr.DeflateRect(ScaleX(8), 0);
		while (!shown.empty() && f.Measure(shown) > tr.Width() - ScaleX(4))
			shown.erase(0, 1);
		DrawText(dc, f, shown, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, RGB(0, 0, 0));
		if (int(NowSeconds() * 2) % 2 == 0)
		{
			const int cx = tr.left + f.Measure(shown) + 1;
			dc.VLine(cx, e.top + ScaleY(8), e.bottom - ScaleY(8), RGB(0, 0, 0));
		}
		const Font& kf = FontOfSize(ScaleY(15));
		for (int r = 0; r < int(rows.size()); r++)
			for (int k = 0; k < int(rows[r].size()); k++)
			{
				const K& key = rows[r][k];
				const CRect kr = KeyRect(r, k);
				const bool sel = r == row && k == col;
				const bool on = (key.action == "shift" && shift) || (key.action == "caps" && caps);
				dc.FillSolidRect(kr, sel ? RGB(0, 120, 215) : on ? RGB(70, 70, 70) : RGB(51, 51, 51));
				if (sel)
					dc.FrameRect(kr, RGB(255, 255, 255));
				CRect lr = kr;
				const std::string label = key.action.empty() ? (Upper() ? key.shifted : key.label) : T(key.label);
				DrawText(dc, kf, label, lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, RGB(255, 255, 255));
			}
		CRect hint(c.left + ScaleX(14), c.bottom - ScaleY(30), c.right - ScaleX(14), c.bottom - ScaleY(6));
		DrawText(dc, UiFont(), T("Cross: type   Square: backspace   Triangle: space   L2/R2: shift   Options: done   Circle: cancel"),
			hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, RGB(170, 170, 170));
	}

	bool OnEvent(const InputEvent& ev) override
	{
		if (ev.type == InputEvent::LButtonDown || ev.type == InputEvent::MouseMove)
		{
			for (int r = 0; r < int(rows.size()); r++)
				for (int k = 0; k < int(rows[r].size()); k++)
					if (KeyRect(r, k).PtInRect(ev.x, ev.y))
					{
						row = r;
						col = k;
						if (ev.type == InputEvent::LButtonDown)
							Press(rows[r][k]);
						return true;
					}
			return Dialog::OnEvent(ev);
		}
		if (ev.type != InputEvent::KeyDown)
			return Dialog::OnEvent(ev);
		switch (ev.key)
		{
			case Key::Left:
				col = col > 0 ? col - 1 : int(rows[row].size()) - 1;
				return true;
			case Key::Right:
				col = col + 1 < int(rows[row].size()) ? col + 1 : 0;
				return true;
			case Key::Up:
			case Key::Down:
			{
				// the key above/below whose middle is closest
				const int nr = ev.key == Key::Up ? (row > 0 ? row - 1 : int(rows.size()) - 1) : (row + 1) % int(rows.size());
				const int cx = KeyRect(row, col).CenterPoint().x;
				int best = 0, bd = 1 << 30;
				for (int k = 0; k < int(rows[nr].size()); k++)
				{
					const int d = std::abs(KeyRect(nr, k).CenterPoint().x - cx);
					if (d < bd)
					{
						bd = d;
						best = k;
					}
				}
				row = nr;
				col = best;
				return true;
			}
			case Key::Enter:
				Press(rows[row][col]);
				return true;
			case Key::Square:
				Press({"", "", "bksp", 0});
				return true;
			case Key::Triangle:
				Press({"", "", "space", 0});
				return true;
			case Key::L2:
			case Key::R2:
				shift = !shift;
				return true;
			case Key::Menu:
				Finish(true);
				return true;
			case Key::Escape:
				Finish(false);
				return true;
			default:
				return true;
		}
	}
};
} // namespace

void OnScreenKeyboard(const std::string& title, const std::string& text, bool password,
	std::function<void(bool, const std::string&)> done)
{
	Keyboard* k = new Keyboard;
	k->title = title.empty() ? T("On-Screen Keyboard") : title;
	k->text = text;
	k->password = password;
	k->done = std::move(done);
	int w = 0;
	for (size_t c = 0; c < k->rows[1].size(); c++)
		w += k->rows[1][c].w * k->KeyUnit() + ScaleX(6);
	const int h = k->CaptionHeight() + ScaleY(70) + int(k->rows.size()) * (k->KeyH() + ScaleY(6)) + ScaleY(40);
	k->rect = CRect(0, 0, w + ScaleX(22) + 2, h);
	CRect screen = WindowManager::Get().screen;
	k->Center(screen);
	k->rect.OffsetRect(0, screen.bottom - ScaleY(20) - k->rect.bottom); // at the bottom, as the touch keyboard
	if (k->rect.top < 0)
		k->rect.OffsetRect(0, -k->rect.top);
	WindowManager::Get().Push(k);
}
} // namespace mpc
