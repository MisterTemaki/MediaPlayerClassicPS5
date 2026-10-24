// Media Player Classic PS5: the window system -- the input (the DualSense as mouse and keyboard), Windows 10's
// common controls drawn as uxtheme draws them (push buttons, check boxes, radio buttons, edit boxes, combo
// boxes, list views, tree views, tabs, group boxes, scroll bars, sliders), dialog boxes with Windows'
// keyboard behaviour, message boxes, and an on-screen keyboard (Windows' osk.exe, for typing an address or a
// password with the controller).
//
// Everything is redrawn every frame into the 1920x1080 surface; the dialogs stack above the main window and the
// top one gets the input (modal, as MPC-HC's are).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_font.h"
#include "mpc_gdi.h"
#include "mpc_res.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mpc
{
// ---- input ----
enum class Key
{
	None,
	Left,
	Right,
	Up,
	Down,
	Enter, // Cross (no cursor) / Return
	Escape, // Circle
	Menu, // Options: Alt (the menu bar)
	Tab, // R1 in dialogs: next control
	ShiftTab, // L1 in dialogs
	Square,
	Triangle,
	L1,
	R1,
	L2,
	R2,
	L3,
	R3,
	PageUp, // L2 in lists
	PageDown, // R2 in lists
	Backspace, // the on-screen keyboard
};

struct InputEvent
{
	enum Type
	{
		MouseMove,
		LButtonDown,
		LButtonUp,
		RButtonDown,
		RButtonUp,
		Wheel,
		KeyDown,
		Char, // on-screen keyboard: text
	} type;
	int x = 0, y = 0;
	int wheel = 0; // +1 up, -1 down
	Key key = Key::None;
	bool repeat = false;
	std::string text;
};

// The pad as a mouse and a keyboard: the left stick moves the pointer, Cross clicks while the pointer is
// shown (it hides after a few seconds still), Square right-clicks; the buttons are keys otherwise.
class InputMapper
{
public:
	void Poll(std::vector<InputEvent>& out);
	int CursorX() const { return int(cx_); }
	int CursorY() const { return int(cy_); }
	bool CursorVisible() const;
	void ShowCursorAt(int x, int y);
	void HideCursor();
	double IdleSeconds() const; // since the last input
	bool Held(Key k) const;
	void SetSpeed(int s) { speed_ = s; }

private:
	double cx_ = 960, cy_ = 540;
	double cursor_t_ = -100; // when the pointer last moved
	double input_t_ = 0;
	uint32_t prev_ = 0;
	double repeat_at_[32] = {};
	bool cross_click_ = false, square_click_ = false;
	int speed_ = 3;
	double wheel_acc_ = 0;
	int wheel_dir_ = 0; // the right stick: -1 / 0 / 1 as it is pushed now
	bool wheel_sent_ = false; // a wheel step already sent for this push (the next ones are repeats)
};

void DrawCursor(CDC& dc, int x, int y);
double NowSeconds();

// ---- controls ----
class Dialog;

class Control
{
public:
	virtual ~Control() = default;
	CRect rect;
	int id = 0;
	bool enabled = true;
	bool visible = true;
	bool tab_stop = true;
	Dialog* parent = nullptr;

	virtual void Paint(CDC& dc, bool focused, bool hot) = 0;
	// returns true when it used the event
	virtual bool OnKey(Key k) { return false; }
	virtual void OnClick(int x, int y) { Activate(); }
	virtual void OnWheel(int delta) {}
	virtual void OnDrag(int x, int y) {}
	virtual void OnRelease() {}
	virtual void Activate() {} // Cross / Enter / Space on it
	virtual bool WantsArrows() const { return false; } // lists, sliders, combo boxes take Up/Down
	virtual bool Focusable() const { return visible && enabled && tab_stop; }
};

class Static : public Control
{
public:
	std::string text;
	unsigned flags = DT_LEFT | DT_WORDBREAK;
	COLORREF color = sys::WindowText;
	bool bold = false;
	const Image* icon = nullptr;
	Static() { tab_stop = false; }
	void Paint(CDC& dc, bool focused, bool hot) override;
};

class Button : public Control
{
public:
	std::string text;
	bool is_default = false;
	std::function<void()> on_click;
	void Paint(CDC& dc, bool focused, bool hot) override;
	void Activate() override
	{
		if (on_click)
			on_click();
	}
	bool pressed = false;
};

class CheckBox : public Control
{
public:
	std::string text;
	bool checked = false;
	bool radio = false;
	int group = 0; // radio buttons of one group
	std::function<void()> on_change;
	void Paint(CDC& dc, bool focused, bool hot) override;
	void Activate() override;
};

class GroupBox : public Control
{
public:
	std::string text;
	GroupBox() { tab_stop = false; }
	void Paint(CDC& dc, bool focused, bool hot) override;
};

// A one-line edit box: Cross opens the on-screen keyboard on it.
class Edit : public Control
{
public:
	std::string text;
	bool password = false;
	bool read_only = false;
	std::string cue; // the grey hint while empty
	std::string keyboard_title;
	std::function<void()> on_change;
	void Paint(CDC& dc, bool focused, bool hot) override;
	void Activate() override;
};

class ComboBox : public Control
{
public:
	std::vector<std::string> items;
	int sel = -1;
	bool editable = false; // a CBS_DROPDOWN: the text can be typed (on-screen keyboard)
	std::string text; // the editable one's text
	std::function<void()> on_change;
	void Paint(CDC& dc, bool focused, bool hot) override;
	void Activate() override; // opens the list
	bool OnKey(Key k) override;
	bool WantsArrows() const override { return true; }
	std::string Text() const { return editable ? text : (sel >= 0 && sel < int(items.size()) ? items[sel] : ""); }
};

class Slider : public Control
{
public:
	int min = 0, max = 100, pos = 0, step = 1, page = 10;
	std::function<void()> on_change;
	void Paint(CDC& dc, bool focused, bool hot) override;
	bool OnKey(Key k) override;
	void OnClick(int x, int y) override;
	void OnDrag(int x, int y) override;
	bool WantsArrows() const override { return true; }
};

// An edit box with an up-down control (UDS_SETBUDDYINT): Left/Right change it, Cross types a number.
class NumberBox : public Control
{
public:
	int value = 0, min = 0, max = 100, step = 1;
	std::string suffix;
	std::function<void()> on_change;
	void Paint(CDC& dc, bool focused, bool hot) override;
	bool OnKey(Key k) override;
	void OnClick(int x, int y) override;
	void Activate() override;
};

// A report-style list view (columns, one selected row); Cross opens the row (on_open).
class ListView : public Control
{
public:
	struct Column
	{
		std::string title;
		int width;
		bool right = false;
	};
	std::vector<Column> columns;
	std::vector<std::vector<std::string>> rows;
	std::vector<const Image*> icons; // per row (optional)
	int sel = -1;
	int top = 0;
	bool header = true;
	std::function<void()> on_open, on_select;
	void Paint(CDC& dc, bool focused, bool hot) override;
	bool OnKey(Key k) override;
	void OnClick(int x, int y) override;
	void OnWheel(int delta) override;
	void Activate() override
	{
		if (on_open && sel >= 0)
			on_open();
	}
	bool WantsArrows() const override { return true; }
	int RowHeight() const;
	int VisibleRows() const;
	void EnsureVisible();
	double last_click_t = 0;
	int last_click_row = -1;
};

// A tree view of one level of folders (the Options' page tree, the browser's navigation pane).
class TreeView : public Control
{
public:
	struct Item
	{
		std::string text;
		int level = 0;
		const Image* icon = nullptr;
		bool header = false; // a group heading, not selectable
	};
	std::vector<Item> items;
	int sel = -1;
	int top = 0;
	std::function<void()> on_select;
	void Paint(CDC& dc, bool focused, bool hot) override;
	bool OnKey(Key k) override;
	void OnClick(int x, int y) override;
	void OnWheel(int delta) override;
	bool WantsArrows() const override { return true; }
	int RowHeight() const;
};

class TabControl : public Control
{
public:
	std::vector<std::string> tabs;
	int sel = 0;
	std::function<void()> on_change;
	void Paint(CDC& dc, bool focused, bool hot) override;
	bool OnKey(Key k) override;
	void OnClick(int x, int y) override;
	bool WantsArrows() const override { return false; }
	CRect TabRect(int i) const;
};

// A multi-line read-only text with a scroll bar (Properties' MediaInfo page)
class TextView : public Control
{
public:
	std::vector<std::string> lines;
	int top = 0;
	bool mono = false;
	void Paint(CDC& dc, bool focused, bool hot) override;
	bool OnKey(Key k) override;
	void OnWheel(int delta) override;
	bool WantsArrows() const override { return true; }
	int VisibleLines() const;
};

void DrawScrollBar(CDC& dc, const CRect& r, int total, int visible, int top);
void DrawThemedButton(CDC& dc, const CRect& r, bool hot, bool pressed, bool focused, bool is_default, bool enabled);
void DrawCaptionButton(CDC& dc, const CRect& r, int glyph, bool hot, bool pressed, bool active); // 0 min 1 max 2 restore 3 close

// ---- dialogs ----
class Dialog
{
public:
	virtual ~Dialog() = default;
	std::string title;
	CRect rect; // the whole window, title bar included
	std::vector<std::unique_ptr<Control>> controls;
	int focus = -1;
	bool closed = false;
	int result = 0; // IDOK 1 / IDCANCEL 2 ...
	bool has_close = true;

	template <typename T>
	T* Add(T* c)
	{
		c->parent = this;
		controls.emplace_back(c);
		return c;
	}
	CRect Client() const; // the client area (screen coordinates)
	int CaptionHeight() const;
	// dialog units (MS Shell Dlg 8: 4 horizontal units per average character, 8 vertical per line)
	static int DuX(int du);
	static int DuY(int du);
	CRect DuRect(int x, int y, int w, int h) const; // in the client area
	void Center(const CRect& in);

	virtual void Paint(CDC& dc);
	virtual void PaintClient(CDC& dc) {}
	virtual bool OnEvent(const InputEvent& ev);
	virtual void OnOk() { EndDialog(1); }
	virtual void OnCancel() { EndDialog(2); }
	virtual void Tick() {} // once per frame
	void EndDialog(int r)
	{
		result = r;
		closed = true;
	}
	void SetFocus(Control* c);
	Control* Focused() const { return focus >= 0 && focus < int(controls.size()) ? controls[focus].get() : nullptr; }
	void MoveFocus(int dx, int dy); // spatial: the nearest control that way
	void NextFocus(int dir);
	Button* DefaultButton() const;
	void UncheckGroup(CheckBox* except);
	std::function<void(int)> on_close; // called with the result when it closes

protected:
	Control* hot_ = nullptr;
	Control* dragging_ = nullptr;
	bool close_hot_ = false, close_pressed_ = false;
};

// The modal stack (the top one gets the input) and the drop-downs that float above everything.
class Popup
{
public:
	virtual ~Popup() = default;
	virtual void Paint(CDC& dc) = 0;
	virtual bool OnEvent(const InputEvent& ev) = 0; // false: closed, give the event on
	bool closed = false;
};

class WindowManager
{
public:
	static WindowManager& Get();
	void Push(Dialog* d); // takes it
	void PushPopup(Popup* p);
	bool HasModal() const { return !dialogs_.empty() || !popups_.empty(); }
	bool Dispatch(const InputEvent& ev); // true when a dialog or a popup took it
	void Paint(CDC& dc);
	void Tick();
	Dialog* Top() const { return dialogs_.empty() ? nullptr : dialogs_.back().get(); }
	CRect screen = CRect(0, 0, 1920, 1080);

private:
	std::vector<std::unique_ptr<Dialog>> dialogs_;
	std::vector<std::unique_ptr<Popup>> popups_;
};

// MessageBox (MB_OK / MB_YESNO / MB_OKCANCEL), icon 0 none 1 information 2 warning 3 error 4 question
void MessageBox(const std::string& text, const std::string& caption, int buttons = 0, int icon = 1,
	std::function<void(int)> done = nullptr);
enum
{
	MB_OK = 0,
	MB_OKCANCEL = 1,
	MB_YESNO = 4,
	IDOK = 1,
	IDCANCEL = 2,
	IDYES = 6,
	IDNO = 7
};

// The on-screen keyboard: edits text, then calls done(true, text) (or done(false, "") when cancelled).
void OnScreenKeyboard(const std::string& title, const std::string& text, bool password,
	std::function<void(bool, const std::string&)> done);

// A drop-down list under r (combo boxes): done(index) or -1
void DropDownList(const CRect& under, const std::vector<std::string>& items, int sel, std::function<void(int)> done);
} // namespace mpc
