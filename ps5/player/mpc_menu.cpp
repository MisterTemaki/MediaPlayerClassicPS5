// Media Player Classic PS5: menus (mpc_menu.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_menu.h"

#include <algorithm>
#include <cmath>

namespace mpc
{
namespace
{
int ItemH()
{
	return std::max(UiFont().Height() + ScaleY(6), ScaleY(22));
}
int SepH()
{
	return ScaleY(9);
}
int Gutter()
{
	return ScaleX(30);
}

struct Level
{
	std::vector<MenuItem> items;
	CRect r;
	int hot = -1;
	int open_sub = -1; // the item whose submenu is open (the next level)
	double hot_t = 0;
};

class MenuTracker;
MenuTracker* g_active = nullptr;

int Width(const std::vector<MenuItem>& items)
{
	const Font& f = UiFont();
	int tw = 0, aw = 0;
	for (const auto& it : items)
	{
		if (it.IsSeparator())
			continue;
		tw = std::max(tw, f.Measure(StripPrefix(it.text)));
		if (!it.accel.empty())
			aw = std::max(aw, f.Measure(it.accel));
	}
	return Gutter() + ScaleX(4) + tw + (aw ? ScaleX(28) + aw : 0) + ScaleX(36);
}

int Height(const std::vector<MenuItem>& items)
{
	int h = ScaleY(3) * 2;
	for (const auto& it : items)
		h += it.IsSeparator() ? SepH() : ItemH();
	return h;
}

CRect ItemRect(const Level& l, int i)
{
	int y = l.r.top + ScaleY(3);
	for (int k = 0; k < i; k++)
		y += l.items[k].IsSeparator() ? SepH() : ItemH();
	return CRect(l.r.left + ScaleX(2), y, l.r.right - ScaleX(2), y + (l.items[i].IsSeparator() ? SepH() : ItemH()));
}

void DrawGlyphCheck(CDC& dc, const CRect& cell, bool radio, COLORREF c)
{
	const CPoint m = cell.CenterPoint();
	if (radio)
	{
		const double r = ScaleX(3) + 0.5;
		for (int y = int(m.y - r - 1); y <= int(m.y + r + 1); y++)
			for (int x = int(m.x - r - 1); x <= int(m.x + r + 1); x++)
			{
				const double d = std::sqrt((x - m.x) * (x - m.x) + (y - m.y) * (y - m.y));
				if (d <= r)
					dc.SetPixel(x, y, c);
			}
		return;
	}
	const double f = ScaleFactor();
	const int th = std::max(1, int(1.4 * f));
	const CPoint a{m.x - int(4 * f), m.y}, b{m.x - int(1 * f), m.y + int(3 * f)}, e{m.x + int(5 * f), m.y - int(3 * f)};
	for (int t = 0; t < th; t++)
	{
		dc.MoveTo(a.x, a.y + t);
		dc.LineTo(b.x, b.y + t, c);
		dc.MoveTo(b.x, b.y + t);
		dc.LineTo(e.x + 1, e.y + t - 1, c);
	}
}

void DrawLevel(CDC& dc, const Level& l)
{
	// the drop shadow, the border, the items
	for (int i = 1; i <= ScaleX(5); i++)
	{
		CRect s = l.r;
		s.OffsetRect(i / 2 + 1, i / 2 + 1);
		dc.BlendRect(CRect(l.r.right, s.top, s.right, s.bottom), RGB(0, 0, 0), 10);
		dc.BlendRect(CRect(s.left, l.r.bottom, l.r.right, s.bottom), RGB(0, 0, 0), 10);
	}
	dc.FillSolidRect(l.r, sys::MenuPopup);
	dc.FrameRect(l.r, sys::MenuPopupBorder);
	const Font& f = UiFont();
	for (int i = 0; i < int(l.items.size()); i++)
	{
		const MenuItem& it = l.items[i];
		const CRect r = ItemRect(l, i);
		if (it.IsSeparator())
		{
			dc.HLine(r.left + Gutter(), r.right - ScaleX(1), r.CenterPoint().y, sys::MenuSeparator);
			continue;
		}
		const bool hot = i == l.hot || i == l.open_sub;
		if (hot)
			dc.FillSolidRect(r, it.enabled ? sys::MenuItemHot : RGB(230, 230, 230));
		const COLORREF tc = it.enabled ? sys::WindowText : sys::MenuDisabled;
		if (it.checked)
			DrawGlyphCheck(dc, CRect(r.left, r.top, r.left + Gutter(), r.bottom), it.radio, tc);
		CRect tr(r.left + Gutter() + ScaleX(4), r.top, r.right - ScaleX(20), r.bottom);
		DrawText(dc, f, it.text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX, tc);
		if (!it.accel.empty())
		{
			CRect ar(tr.left, r.top, r.right - ScaleX(22), r.bottom);
			DrawText(dc, f, it.accel, ar, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, tc);
		}
		if (it.popup)
		{
			// the submenu arrow
			const int cx = r.right - ScaleX(12), cy = r.CenterPoint().y, s = ScaleX(4);
			for (int k = 0; k <= s; k++)
				dc.FillSolidRect(CRect(cx - s / 2 + k, cy - (s - k), cx - s / 2 + k + 1, cy + (s - k) + 1), tc);
		}
	}
}

class MenuTracker : public Popup
{
public:
	std::vector<Level> levels;
	std::function<void(int)> done, bar_switch;
	bool keyboard = false;
	CRect exclude; // the menu bar item that opened it (a click there closes the menu)

	explicit MenuTracker(bool kb) : keyboard(kb) { g_active = this; }
	~MenuTracker() override
	{
		if (g_active == this)
			g_active = nullptr;
	}

	void OpenLevel(const std::vector<MenuItem>& items, int x, int y, const CRect* beside)
	{
		Level l;
		l.items = items;
		const int w = Width(items), h = Height(items);
		const CRect screen = WindowManager::Get().screen;
		if (beside)
		{
			x = beside->right - ScaleX(3);
			y = beside->top - ScaleY(3);
			if (x + w > screen.right)
				x = beside->left - w + ScaleX(3);
		}
		if (x + w > screen.right)
			x = screen.right - w;
		if (y + h > screen.bottom)
			y = std::max(0, screen.bottom - h);
		l.r = CRect(std::max(0, x), y, std::max(0, x) + w, y + h);
		if (keyboard)
			l.hot = NextEnabled(l, -1, 1);
		levels.push_back(l);
	}

	static int NextEnabled(const Level& l, int from, int dir)
	{
		const int n = int(l.items.size());
		int i = from;
		for (int k = 0; k < n; k++)
		{
			i = (i + dir + n) % n;
			if (!l.items[i].IsSeparator())
				return i;
		}
		return -1;
	}

	void Finish(int id)
	{
		closed = true;
		g_active = nullptr;
		if (done)
			done(id);
	}

	void Paint(CDC& dc) override
	{
		// a submenu opens when the pointer rests on its item
		if (!levels.empty())
		{
			Level& l = levels.back();
			if (!keyboard && l.hot >= 0 && l.items[l.hot].popup && l.items[l.hot].enabled && l.open_sub != l.hot &&
				NowSeconds() - l.hot_t > 0.35)
				OpenSub(int(levels.size()) - 1);
		}
		for (const auto& l : levels)
			DrawLevel(dc, l);
	}

	void OpenSub(int li)
	{
		Level& l = levels[li];
		levels.resize(li + 1);
		l.open_sub = l.hot;
		const CRect ir = ItemRect(l, l.hot);
		const std::vector<MenuItem> sub = l.items[l.hot].sub;
		OpenLevel(sub, 0, 0, &ir);
	}

	void Activate(int li, int i)
	{
		Level& l = levels[li];
		const MenuItem& it = l.items[i];
		if (!it.enabled || it.IsSeparator())
			return;
		if (it.popup)
		{
			l.hot = i;
			if (l.open_sub != i)
				OpenSub(li);
			if (keyboard && levels.size() > size_t(li + 1))
			{
				Level& s = levels.back();
				s.hot = NextEnabled(s, -1, 1);
			}
			return;
		}
		Finish(it.id);
	}

	bool OnEvent(const InputEvent& ev) override
	{
		switch (ev.type)
		{
			case InputEvent::MouseMove:
			{
				keyboard = false;
				for (int li = int(levels.size()) - 1; li >= 0; li--)
				{
					Level& l = levels[li];
					if (!l.r.PtInRect(ev.x, ev.y))
						continue;
					for (int i = 0; i < int(l.items.size()); i++)
						if (ItemRect(l, i).PtInRect(ev.x, ev.y))
						{
							if (l.hot != i)
							{
								l.hot = l.items[i].IsSeparator() ? -1 : i;
								l.hot_t = NowSeconds();
							}
							// pointing back at a parent closes the deeper levels after the delay (Windows does)
							if (li + 1 < int(levels.size()) && l.open_sub != i && NowSeconds() - l.hot_t > 0.35)
							{
								levels.resize(li + 1);
								l.open_sub = -1;
							}
						}
					return true;
				}
				return true;
			}
			case InputEvent::LButtonDown:
			case InputEvent::RButtonDown:
				for (int li = int(levels.size()) - 1; li >= 0; li--)
				{
					Level& l = levels[li];
					if (!l.r.PtInRect(ev.x, ev.y))
						continue;
					for (int i = 0; i < int(l.items.size()); i++)
						if (ItemRect(l, i).PtInRect(ev.x, ev.y))
						{
							Activate(li, i);
							return true;
						}
					return true;
				}
				// outside: closes (a click on the menu bar item that opened it too)
				Finish(0);
				return true;
			case InputEvent::KeyDown:
			{
				keyboard = true;
				Level& l = levels.back();
				switch (ev.key)
				{
					case Key::Up:
						l.hot = NextEnabled(l, l.hot < 0 ? 0 : l.hot, -1);
						return true;
					case Key::Down:
						l.hot = NextEnabled(l, l.hot, 1);
						return true;
					case Key::Right:
						if (l.hot >= 0 && l.items[l.hot].popup && l.items[l.hot].enabled)
							Activate(int(levels.size()) - 1, l.hot);
						else if (bar_switch)
						{
							closed = true;
							g_active = nullptr;
							bar_switch(1);
						}
						return true;
					case Key::Left:
						if (levels.size() > 1)
						{
							levels.pop_back();
							levels.back().open_sub = -1;
						}
						else if (bar_switch)
						{
							closed = true;
							g_active = nullptr;
							bar_switch(-1);
						}
						return true;
					case Key::Enter:
						if (l.hot >= 0)
							Activate(int(levels.size()) - 1, l.hot);
						return true;
					case Key::Escape:
						if (levels.size() > 1)
						{
							levels.pop_back();
							levels.back().open_sub = -1;
						}
						else
							Finish(0);
						return true;
					case Key::Menu:
					case Key::Square:
						Finish(0);
						return true;
					default:
						return true;
				}
			}
			default:
				return true;
		}
	}
};
} // namespace

void TrackPopupMenu(const std::vector<MenuItem>& items, int x, int y, std::function<void(int)> done,
	std::function<void(int)> bar_switch, bool keyboard, int exclude_top)
{
	(void)exclude_top;
	MenuTracker* t = new MenuTracker(keyboard);
	t->done = std::move(done);
	t->bar_switch = std::move(bar_switch);
	t->OpenLevel(items, x, y, nullptr);
	WindowManager::Get().PushPopup(t);
}

bool MenuOpen()
{
	return g_active != nullptr;
}

void CloseMenus()
{
	if (g_active)
		g_active->Finish(0);
}

int MenuBarHeight()
{
	return ScaleY(20);
}

void DrawMenuBarItem(CDC& dc, const CRect& r, const std::string& text, bool hot, bool open, bool show_prefix)
{
	if (open)
	{
		dc.FillSolidRect(r, sys::MenuBarPushed);
		dc.FrameRect(r, sys::MenuBarPushedBorder);
	}
	else if (hot)
	{
		dc.FillSolidRect(r, sys::MenuBarHot);
		dc.FrameRect(r, sys::MenuBarHotBorder);
	}
	CRect tr = r;
	DrawText(dc, UiFont(), text, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | (show_prefix ? 0u : unsigned(DT_HIDEPREFIX)), sys::WindowText);
}
} // namespace mpc
