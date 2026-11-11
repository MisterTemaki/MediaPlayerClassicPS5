// Automatic encrypted migration and the real SMB form with masked passwords.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include "mpc_dialogs.h"
#include "mpc_settings.h"
#include "mpc_ui.h"
#include "mpc_i18n.h"
#include "OrbisPaths.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace mpc;
namespace fs = std::filesystem;
static int passed = 0, failed = 0;
void check(bool value, const char* message)
{
	std::cout << (value ? "ok: " : "FAIL: ") << message << std::endl;
	(value ? passed : failed)++;
}
std::string read(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}
void write(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
std::vector<Edit*> edits(Dialog* dialog)
{
	std::vector<Edit*> result;
	for (const auto& control : dialog->controls)
		if (auto* edit = dynamic_cast<Edit*>(control.get())) result.push_back(edit);
	return result;
}
Button* button(Dialog* dialog, const std::string& text)
{
	for (const auto& control : dialog->controls)
		if (auto* item = dynamic_cast<Button*>(control.get())) if (item->text == text) return item;
	return nullptr;
}
CheckBox* remember(Dialog* dialog)
{
	for (const auto& control : dialog->controls)
		if (auto* item = dynamic_cast<CheckBox*>(control.get())) return item;
	return nullptr;
}
void snapshot(const char* name)
{
	const char* folder = getenv("CREDENTIALS_TEST_DUMP_DIR");
	if (!folder) return;
	Image image(1920, 1080); CDC dc(image.Bmp());
	dc.FillSolidRect(CRect(0, 0, 1920, 1080), sys::BtnFace);
	WindowManager::Get().Paint(dc);
	std::ofstream file(std::string(folder) + "/" + name + ".ppm", std::ios::binary);
	file << "P6\n1920 1080\n255\n";
	for (int i = 0; i < 1920 * 1080; ++i)
	{
		const uint32_t pixel = image.Data()[i];
		const char rgb[] = {char(pixel), char(pixel >> 8), char(pixel >> 16)};
		file.write(rgb, 3);
	}
}
int main()
{
	if (!getenv("MPCPS5_PS5_ROOT") || !OrbisPathsInit()) return 2;
	const fs::path root = OrbisRoot(), ini = root / "mpc-hc.ini", legacy = root / "credentials.ini";
	const std::string transport = "smb://bob:fixture%3Asecret@server/Movies/movie.mp4";
	const std::string bare = "smb://bob@server/Movies/movie.mp4";
	const std::string original = "[Settings]\nVolume=73\n[Recent File List]\nFile1=" + transport +
		"\n[Network]\nServer0=Home|smb://bob:fixture%3Asecret@server/Movies\n[Credentials]\nGeneration=2\n";
	write(ini, original); write(legacy, "; generation 2\nsmb://bob@server\tfixture%3Asecret\n");
	Settings& settings = AppSettings(); settings.Load();
	check(settings.volume == 73 && settings.recent.front() == bare && settings.servers.front().url == "smb://bob@server/Movies", "legacy URLs are stripped immediately while preserving other preferences");
	setenv("MPCPS5_HOST_CREDENTIALS_FAIL", "entropy", 1);
	check(!settings.Save() && read(ini) == original && fs::exists(legacy), "failed encryption preserves original settings and legacy passwords");
	unsetenv("MPCPS5_HOST_CREDENTIALS_FAIL");
	check(settings.Save() && !fs::exists(legacy) && read(ini).find("fixture") == std::string::npos && read(root/"credentials.enc").find("fixture") == std::string::npos, "automatic verified migration removes plaintext without requiring a master password");
	std::string error;
	credentials::Get().ClearSession();
	check(credentials::Get().Load(error) && credentials::Get().Resolve(bare) == transport, "network credentials return automatically after reopening the app");
	auto& wm = WindowManager::Get();
	bool added = false;
	ShowAddNetworkDialog([&](const std::string& name, const std::string& url) {
		added = name == "Home videos" && url == "smb://bob@server/Movies";
		settings.servers = {{name, url}}; settings.Save();
	});
	Dialog* dialog = wm.Top();
	auto fields = edits(dialog);
	check(fields.size() == 5 && fields[3]->password && remember(dialog)->checked, "SMB form masks the password and offers one save-password checkbox");
	fields[0]->text = "server"; fields[1]->text = "Movies"; fields[2]->text = "bob";
	fields[3]->text = "a?b#c@d:e/f%g"; fields[4]->text = "Home videos";
	for (bool dark : {false, true})
	{
		sys::SetDarkTheme(dark);
		Image actual(900, 60), expected(900, 60); CDC a(actual.Bmp()), b(expected.Bmp());
		const CRect original_rect = fields[3]->rect;
		fields[3]->rect = CRect(0, 0, 900, 60);
		Edit safe; safe.rect = fields[3]->rect; safe.text = std::string(fields[3]->text.size(), '*');
		fields[3]->Paint(a, false, false); safe.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data()+900*60, expected.Data()), "SMB password field paints only masked characters in both themes");
		fields[3]->rect = original_rect;
		snapshot(dark ? "smb-save-dark" : "smb-save-light");
	}
	setenv("MPCPS5_HOST_CREDENTIALS_FAIL", "write", 1);
	button(dialog, "OK")->Activate();
	check(!added && wm.Top() != dialog, "failed encrypted save shows an error without accepting the network form");
	wm.Top()->OnCancel(); wm.Tick();
	check(wm.Top() == dialog && fields[3]->text == "a?b#c@d:e/f%g", "dismissing the error preserves entered data for retry");
	unsetenv("MPCPS5_HOST_CREDENTIALS_FAIL");
	button(dialog, "OK")->Activate(); wm.Tick();
	check(added && !wm.HasModal(), "saving the SMB form completes directly without an extra credential dialog");
	check(credentials::Get().Resolve("smb://bob@server/Movies") == "smb://bob:a%3Fb%23c%40d%3Ae%2Ff%25g@server/Movies", "reserved password characters are safely encoded for the SMB transport");
	credentials::Get().ClearSession();
	check(credentials::Get().Load(error) && credentials::Get().Resolve("smb://bob@server/Movies").find("a%3Fb") != std::string::npos, "checked password is restored on the next launch");
	ShowAddNetworkDialog([&](const std::string&, const std::string& url) { settings.servers = {{"Home", url}}; settings.Save(); });
	dialog = wm.Top(); fields = edits(dialog);
	fields[0]->text = "server"; fields[1]->text = "Movies"; fields[2]->text = "bob"; fields[3]->text = "session-only";
	remember(dialog)->checked = false;
	button(dialog, "OK")->Activate(); wm.Tick();
	check(!wm.HasModal() && credentials::Get().Resolve("smb://bob@server/Movies").find("session-only") != std::string::npos, "unchecked SMB password remains usable for the current session");
	credentials::Get().ClearSession();
	check(credentials::Get().Load(error) && credentials::Get().Resolve("smb://bob@server/Movies") == "smb://bob@server/Movies", "unchecked password is absent after restart, including previously saved accounts");
	ShowFileBrowser("Open media", BrowseFilter::Media, nullptr); dialog = wm.Top();
	Edit* address = edits(dialog).front();
	address->text = "smb://bob:fixture%3Asecret@127.0.0.1:1/Movies"; address->Activate();
	InputEvent finish{}; finish.type = InputEvent::KeyDown; finish.key = Key::Menu;
	wm.Dispatch(finish); wm.Tick();
	check(wm.Top() == dialog && address->text == "smb://bob@127.0.0.1:1/Movies" && settings.last_dir == address->text, "URL keyboard strips the transport password before navigation");
	dialog->OnCancel(); wm.Tick();
	check(FileName("smb://bob:part?secret@server/Movies/film.mp4") == "film.mp4", "legacy password query characters do not leak through filename formatting");
	for (bool dark : {false, true})
	{
		sys::SetDarkTheme(dark);
		Image actual(900, 60), expected(900, 60); CDC a(actual.Bmp()), b(expected.Bmp());
		Edit input, safe; input.rect = safe.rect = CRect(0, 0, 900, 60);
		input.text = transport; input.hide_url_password = true; safe.text = bare;
		input.Paint(a, false, false); safe.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data()+900*60, expected.Data()), "URL edit renders only the password-free address in both themes");
		ComboBox combo, safe_combo; combo.rect = safe_combo.rect = input.rect; combo.editable = safe_combo.editable = true;
		combo.text = transport; combo.hide_url_password = true; safe_combo.text = bare;
		combo.Paint(a, false, false); safe_combo.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data()+900*60, expected.Data()), "URL combo never paints the transport password");
		input.text = "smb://bob:partiallytyped"; safe.text = "smb://bob:***";
		input.Paint(a, false, false); safe.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data()+900*60, expected.Data()), "partial URL passwords are hidden before typing the at-sign");
	}
	for (int language=0; language<int(Language::Count); ++language)
	{
		settings.language = language; ShowAddNetworkDialog(nullptr); dialog = wm.Top();
		check(UiFont().Measure(remember(dialog)->text) <= remember(dialog)->rect.Width()-ScaleX(20), "save-password checkbox fits in every supported language");
		dialog->OnCancel(); wm.Tick();
	}
	settings.language = 0;
	settings.recent.clear(); settings.servers.clear(); settings.favorites.clear(); settings.positions.clear();
	settings.last_dir = (root/"media").string();
	check(settings.Save(), "removing the last saved references commits password cleanup");
	credentials::Get().ClearSession();
	const std::string removed = "smb://bob@127.0.0.1:1/Movies";
	check(credentials::Get().Load(error) && credentials::Get().Resolve(removed) == removed, "deleted saved references do not retain a password across app sessions");
	const std::string committed = read(root/"credentials.enc");
	credentials::Get().ClearSession();
	const std::string malformed("broken\0legacy", 13);
	write(legacy, malformed); settings.Load();
	check(!settings.Save() && read(legacy) == malformed && read(root/"credentials.enc") == committed, "malformed legacy data is preserved without overwriting encrypted credentials");
	fs::remove(legacy); settings.Load(); check(settings.Save(), "settings can be saved after explicitly removing an invalid legacy file");
	wm.Clear(); credentials::Get().ClearSession();
	std::cout << "passed " << passed << ", failed " << failed << std::endl;
	return failed != 0;
}
