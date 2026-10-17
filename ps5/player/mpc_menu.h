// Media Player Classic PS5: menus -- MPC-HC's main menu and context menu (mpc-hc.rc's IDR_MAINFRAME and
// IDR_POPUP), drawn as Windows 10 draws themed menus, tracked with the pointer or the D-pad as Windows tracks
// them with the mouse or the keyboard (Up/Down, Right opens a submenu, Left closes it, Enter runs, Esc closes).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_i18n.h"
#include "mpc_ui.h"

#include <functional>
#include <string>
#include <vector>

namespace mpc
{
struct MenuItem
{
	std::string text; // "&Open File..."; "" with id 0: a separator
	int id = 0;
	std::vector<MenuItem> sub;
	bool popup = false;
	bool enabled = true;
	bool checked = false;
	bool radio = false; // a bullet instead of a check
	std::string accel; // the shortcut, right-aligned (here: the controller's button)

	// the text and the shortcut in the interface's language (mpc_i18n.h); a name (a file, a track) comes back as it is

	static MenuItem Sep() { return MenuItem(); }
	static MenuItem Cmd(const std::string& t, int id, const std::string& accel = "")
	{
		MenuItem m;
		m.text = T(t);
		m.id = id;
		m.accel = accel.empty() ? accel : T(accel);
		return m;
	}
	static MenuItem Popup(const std::string& t, std::vector<MenuItem> items)
	{
		MenuItem m;
		m.text = T(t);
		m.popup = true;
		m.sub = std::move(items);
		return m;
	}
	MenuItem& Check(bool c)
	{
		checked = c;
		return *this;
	}
	MenuItem& Radio(bool c)
	{
		checked = c;
		radio = true;
		return *this;
	}
	MenuItem& Enable(bool e)
	{
		enabled = e;
		return *this;
	}
	bool IsSeparator() const { return id == 0 && !popup && text.empty(); }
};

// Opens a popup menu at (x, y) (the context menu, or a menu bar item's menu under it).
// done(id) runs with the chosen command (0: none). bar_switch(dir) (menu bar menus): Left/Right at the
// top level moves to the neighbouring menu bar item.
void TrackPopupMenu(const std::vector<MenuItem>& items, int x, int y, std::function<void(int)> done,
	std::function<void(int)> bar_switch = nullptr, bool keyboard = false, int exclude_top = -1);
bool MenuOpen();
void CloseMenus();

// menu metrics (themed menus at the current scale)
int MenuBarHeight();
void DrawMenuBarItem(CDC& dc, const CRect& r, const std::string& text, bool hot, bool open, bool show_prefix);
} // namespace mpc
