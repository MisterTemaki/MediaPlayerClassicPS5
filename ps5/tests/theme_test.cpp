// Theme changes through the real Options dialog, settings and resource cache.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_dialogs.h"
#include "mpc_font.h"
#include "mpc_i18n.h"
#include "mpc_res.h"
#include "mpc_ui.h"
#include "OrbisPaths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace mpc;
namespace
{
int passed = 0, failed = 0;
void Check(bool value, const char* message)
{
	printf("%s: %s\n", value ? "ok" : "FAIL", message);
	(value ? passed : failed)++;
}
std::vector<uint32_t> Pixels(const Image& image)
{
	const Bitmap b = image.Bmp();
	return {b.px, b.px + size_t(b.w) * b.h};
}
ComboBox* Theme(Dialog* dialog)
{
	for (const auto& c : dialog->controls)
		if (auto* combo = dynamic_cast<ComboBox*>(c.get()))
			if (combo->items == std::vector<std::string>{"Light", "Dark"})
				return combo;
	return nullptr;
}
Button* FindButton(Dialog* dialog, const char* name)
{
	for (const auto& c : dialog->controls)
		if (auto* button = dynamic_cast<Button*>(c.get()))
			if (button->text == name)
				return button;
	return nullptr;
}
double Luminance(COLORREF color)
{
	auto linear = [](int channel) {
		const double x = channel / 255.0;
		return x <= .04045 ? x / 12.92 : pow((x + .055) / 1.055, 2.4);
	};
	return .2126 * linear(GetRValue(color)) + .7152 * linear(GetGValue(color)) + .0722 * linear(GetBValue(color));
}
void Contrast(COLORREF text, COLORREF background)
{
	const double a = Luminance(text), b = Luminance(background);
	Check((std::max(a, b) + .05) / (std::min(a, b) + .05) >= 4.5, "normal text contrast is at least 4.5:1");
}
void Snapshot(const char* name)
{
	const char* dir = getenv("THEME_TEST_DUMP_DIR");
	if (!dir)
		return;
	Image image(1920, 1080);
	CDC dc(image.Bmp());
	dc.FillSolidRect(CRect(0, 0, 1920, 1080), sys::BtnFace);
	WindowManager::Get().Paint(dc);
	const std::string path = std::string(dir) + "/" + name + ".ppm";
	FILE* file = fopen(path.c_str(), "wb");
	Check(file != nullptr, "screenshot file opened");
	if (!file)
		return;
	fprintf(file, "P6\n1920 1080\n255\n");
	for (uint32_t px : Pixels(image))
	{
		const unsigned char rgb[] = {static_cast<unsigned char>(px), static_cast<unsigned char>(px >> 8),
			static_cast<unsigned char>(px >> 16)};
		fwrite(rgb, 1, 3, file);
	}
	fclose(file);
}
} // namespace

int main()
{
	// The runner supplies an isolated root; never overwrite a user's configuration.
	if (!getenv("MPCPS5_PS5_ROOT") || !OrbisPathsInit())
		return 2;
	const std::string ini = OrbisRoot() + "/mpc-hc.ini";
	for (const char* value : {"", "DarkMode=0\n", "DarkMode=2\n", "DarkMode=-1\n", "DarkMode=1junk\n"})
	{
		std::ofstream(ini) << "[Settings]\nVolume=73\n" << value;
		Settings settings;
		settings.Load();
		Check(!settings.dark_mode && settings.volume == 73, "old or invalid theme settings keep Light and existing preferences");
	}
	Settings& settings = AppSettings();
	settings = Settings();
	for (int language = 0; language < int(Language::Count); ++language)
	{
		settings.language = language;
		const int width = UiFont().Measure(T("Retry GPU next start")), available = Dialog::DuX(108) - ScaleX(8);
		if (width > available)
			printf("language %d: GPU retry button needs %d pixels, has %d\n", language, width, available);
		Check(width <= available,
			"translated GPU retry button fits its control");
		const auto lines = WrapText(UiFont(), T("After a GPU failure, software rendering is kept until you request a retry."),
			Dialog::DuX(161));
		Check(int(lines.size()) * UiFont().Height() <= Dialog::DuY(18), "translated GPU retry hint fits its control");
	}
	settings.language = 0;
	sys::SetDarkTheme(false);
	const auto light = Pixels(res::Toolbar(32));
	const auto light_disabled = Pixels(res::ToolbarDisabled(32));
	auto& wm = WindowManager::Get();
	ShowOptionsDialog([&] { settings.Save(); }, OptionsPage::Player);
	Dialog* dialog = wm.Top();
	ComboBox* theme = Theme(dialog);
	Button* apply = FindButton(dialog, "&Apply");
	Button* ok = FindButton(dialog, "OK");
	Check(theme && apply && ok, "Player options exposes the theme selector, Apply and OK");
	if (!theme || !apply || !ok)
		return 1;
	Snapshot("options-light");
	theme->sel = 1;
	Check(!settings.dark_mode && !sys::IsDarkTheme(), "selecting Dark waits for Apply or OK");
	apply->Activate();
	Check(settings.dark_mode && sys::IsDarkTheme() && !dialog->closed, "Apply changes an already open dialog immediately");
	Settings loaded;
	loaded.Load();
	Check(loaded.dark_mode, "Dark survives a settings save and reload");
	Snapshot("options-dark");
	Contrast(sys::WindowText, sys::Window);
	Contrast(sys::WindowText, sys::BtnFace);
	Contrast(sys::WindowText, sys::MenuItemHot);
	Contrast(sys::HighlightText, sys::Highlight);
	const auto dark = Pixels(res::Toolbar(32));
	const auto disabled = Pixels(res::ToolbarDisabled(32));
	bool alpha = dark.size() == light.size(), colored = true;
	int changed = 0;
	for (size_t i = 0; i < dark.size() && alpha; ++i)
	{
		const unsigned a = dark[i] >> 24;
		alpha = a == (light[i] >> 24) && (dark[i] & 255) <= a && ((dark[i] >> 8) & 255) <= a &&
			((dark[i] >> 16) & 255) <= a && (disabled[i] >> 24) == a;
		const int r = light[i] & 255, g = (light[i] >> 8) & 255, b = (light[i] >> 16) & 255;
		if (std::max({r, g, b}) - std::min({r, g, b}) > 20)
			colored &= dark[i] == light[i];
		changed += dark[i] != light[i];
	}
	Check(alpha && colored && changed > 0, "toolbar lifts dark glyphs, preserves colored icons and premultiplied alpha");
	// Paint every real Options page with the selected theme, including subtitle color previews.
	for (const auto& c : dialog->controls)
		if (auto* tree = dynamic_cast<TreeView*>(c.get()))
			for (int page = 0; page < int(tree->items.size()); ++page)
			{
				tree->sel = page;
				tree->on_select();
				Snapshot(("options-dark-" + std::to_string(page)).c_str());
			}
	dialog->OnCancel();
	wm.Tick();
	MessageBox("Dark mode keeps messages readable.", "Dark mode", MB_OK, 1);
	Snapshot("message-dark");
	wm.Top()->OnCancel();
	wm.Tick();
	OnScreenKeyboard("Dark mode keyboard", "Readable text", false, nullptr);
	Snapshot("keyboard-dark");
	wm.Top()->OnCancel();
	wm.Tick();
	ShowFileBrowser("Open media", BrowseFilter::Media, nullptr);
	Snapshot("browser-dark");
	wm.Top()->OnCancel();
	wm.Tick();
	ShowOptionsDialog([&] { settings.Save(); }, OptionsPage::Player);
	dialog = wm.Top();
	Theme(dialog)->sel = 0;
	dialog->OnCancel();
	wm.Tick();
	loaded.Load();
	Check(settings.dark_mode && sys::IsDarkTheme() && loaded.dark_mode, "Cancel discards an unapplied selection");
	ShowOptionsDialog([&] { settings.Save(); }, OptionsPage::Player);
	dialog = wm.Top();
	Theme(dialog)->sel = 0;
	FindButton(dialog, "OK")->Activate();
	wm.Tick();
	loaded.Load();
	Check(!settings.dark_mode && !sys::IsDarkTheme() && !loaded.dark_mode, "OK applies Light, saves it and closes Options");
	Check(Pixels(res::Toolbar(32)) == light && Pixels(res::ToolbarDisabled(32)) == light_disabled,
		"switching back restores the exact original light toolbar pixels");
	printf("passed %d, failed %d\n", passed, failed);
	return failed != 0;
}
