// Credential migration and the real create/unlock/remember dialogs.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include "mpc_dialogs.h"
#include "mpc_settings.h"
#include "mpc_ui.h"
#include "mpc_i18n.h"
#include "OrbisPaths.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

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
void wait(Dialog* dialog, Button* accept)
{
	auto& wm = WindowManager::Get();
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (wm.Top() == dialog && !accept->enabled && std::chrono::steady_clock::now() < until)
	{
		wm.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	wm.Tick();
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
	write(ini, original);
	write(legacy, "; generation 2\nsmb://bob@server\tfixture%3Asecret\n");
	Settings& settings = AppSettings(); settings.Load();
	check(settings.volume == 73 && settings.recent.front() == bare && settings.servers.front().url == "smb://bob@server/Movies",
		"legacy settings preserve preferences while removing passwords from runtime model URLs");
	check(credentials::Get().Pending() && !credentials::Get().Unlocked(), "legacy migration waits for master-password setup");
	check(!settings.Save() && read(ini) == original && fs::exists(legacy), "cancelled or locked migration preserves the original credentials and settings");
	auto& wm = WindowManager::Get();
	bool completed = false;
	ShowCredentialVaultDialog([&](bool ok) { completed = ok; });
	Dialog* dialog = wm.Top();
	check(!remember(dialog)->checked, "remember checkbox is off by default");
	check(edits(dialog)[0]->password && edits(dialog)[1]->password, "master and confirmation fields are masked");
	dialog->OnCancel(); wm.Tick();
	check(!completed && !credentials::Get().Exists() && read(ini) == original, "Cancel creates no vault and loses no legacy password");
	ShowCredentialVaultDialog([&](bool ok) { completed = ok; }); dialog = wm.Top();
	const std::string master = "fixture master passphrase";
	edits(dialog)[0]->text = master; edits(dialog)[1]->text = "different";
	button(dialog, "Save")->Activate();
	check(!credentials::Get().Exists() && wm.Top() == dialog, "mismatched master confirmation does not create a vault");
	edits(dialog)[1]->text = master;
	sys::SetDarkTheme(true); snapshot("vault-create-dark");
	Button* accept = button(dialog, "Save");
	setenv("MPCPS5_HOST_VAULT_FAIL", "write", 1);
	accept->Activate(); wait(dialog, accept);
	check(wm.Top() == dialog && !credentials::Get().Exists() && read(ini) == original && fs::exists(legacy),
		"failed initial vault write keeps the real migration dialog open and all original plaintext files intact");
	unsetenv("MPCPS5_HOST_VAULT_FAIL");
	edits(dialog)[0]->text = master; edits(dialog)[1]->text = master;
	accept->Activate();
	check(!accept->enabled && !dialog->has_close, "KDF runs in background while duplicate submissions are disabled");
	wait(dialog, accept);
	check(completed && !wm.HasModal() && credentials::Get().Unlocked(), "real create dialog commits and closes after successful authentication");
	check(!fs::exists(legacy) && !fs::exists(root / "credentials.ini.new") && read(ini).find("fixture%3Asecret") == std::string::npos,
		"verified migration removes plaintext legacy files and writes password-free settings");
	check(read(root / "credentials.vault").find("fixture%3Asecret") == std::string::npos &&
		read(root / "credentials.vault").find(master) == std::string::npos, "encrypted file contains neither network password nor master password");
	check(WithStoredPassword(bare) == transport && !credentials::Get().Remembered(), "migrated network password works without enabling automatic unlock");
	ShowCredentialVaultDialog(); dialog = wm.Top();
	remember(dialog)->Activate();
	snapshot("vault-remember-dark");
	button(dialog, "Save")->Activate(); wm.Tick();
	check(!wm.HasModal() && credentials::Get().Remembered(), "real checkbox saves automatic unlock without changing the master password");
	credentials::Get().Lock(); settings.Load();
	std::string error;
	check(credentials::Get().UnlockRemembered(error), "remembered state unlocks a new app session");
	ShowCredentialVaultDialog(); dialog = wm.Top();
	check(remember(dialog)->checked, "saved remember state is shown with a tick");
	remember(dialog)->Activate(); button(dialog, "Save")->Activate(); wm.Tick();
	credentials::Get().Lock();
	check(!credentials::Get().Remembered() && !credentials::Get().UnlockRemembered(error), "unchecking removes automatic unlock for the next session");
	ShowCredentialVaultDialog(); dialog = wm.Top();
	edits(dialog)[0]->text = "incorrect master";
	accept = button(dialog, "Unlock"); accept->Activate(); wait(dialog, accept);
	check(wm.Top() == dialog && !credentials::Get().Unlocked(), "incorrect master leaves the real dialog open and the vault locked");
	edits(dialog)[0]->text = master;
	accept->Activate(); wait(dialog, accept);
	check(!wm.HasModal() && credentials::Get().Unlocked(), "correct master unlocks after a failed attempt");
	bool network_added = false;
	ShowAddNetworkDialog([&](const std::string& name, const std::string& url) {
		network_added = name.find("fixture") == std::string::npos && url == "smb://bob@server/Movies";
	});
	dialog = wm.Top();
	edits(dialog)[0]->text = "smb://bob:fixture%3Asecret@server/Movies";
	button(dialog, "OK")->Activate(); wm.Tick();
	check(network_added, "a pasted authenticated server URL yields a password-free location name and URL");
	check(FileName("smb://bob:secret?part@server/Movies/movie.mp4") == "movie.mp4",
		"legacy passwords containing a question mark cannot leak through filename generation");
	bool escaped = false;
	ShowAddNetworkDialog([&](const std::string& name, const std::string& url) {
		escaped = name == "Movies" && url == "smb://bob@server/Movies" &&
			WithStoredPassword(url) == "smb://bob:p%40ss%3Fword%2342@server/Movies";
	});
	dialog = wm.Top();
	edits(dialog)[0]->text = "server"; edits(dialog)[1]->text = "Movies";
	edits(dialog)[2]->text = "bob"; edits(dialog)[3]->text = "p@ss?word#42";
	button(dialog, "OK")->Activate(); wm.Tick();
	check(escaped, "network dialog percent-encodes password delimiters before storing them in the vault");
	settings.last_dir.clear();
	ShowFileBrowser("Open media", BrowseFilter::Media, nullptr); dialog = wm.Top();
	Edit* address = edits(dialog).front();
	address->text = "smb://bob:fixture%3Asecret@127.0.0.1:1/Movies";
	address->Activate();
	InputEvent finish{}; finish.type = InputEvent::KeyDown; finish.key = Key::Menu;
	wm.Dispatch(finish); wm.Tick();
	check(wm.Top() == dialog && address->text == "smb://bob@127.0.0.1:1/Movies" && settings.last_dir == address->text,
		"finishing the URL keyboard navigates with a password-free address while the vault is already unlocked");
	dialog->OnCancel(); wm.Tick();
	for (bool dark : {false, true})
	{
		sys::SetDarkTheme(dark);
		Image actual(900, 60), expected(900, 60); CDC a(actual.Bmp()), b(expected.Bmp());
		Edit input, safe; input.rect = safe.rect = CRect(0, 0, 900, 60);
		input.text = transport; input.hide_url_password = true; safe.text = bare;
		input.Paint(a, false, false); safe.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data() + 900 * 60, expected.Data()), "URL edit renders exactly the password-free address in both themes");
		ComboBox combo, safe_combo;
		combo.rect = safe_combo.rect = input.rect; combo.editable = safe_combo.editable = true;
		combo.text = transport; combo.hide_url_password = true; safe_combo.text = bare;
		combo.Paint(a, false, false); safe_combo.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data() + 900 * 60, expected.Data()), "editable URL combo never paints the transport password");
		input.text = "smb://bob:partiallytyped"; safe.text = "smb://bob:***";
		input.Paint(a, false, false); safe.Paint(b, false, false);
		check(std::equal(actual.Data(), actual.Data() + 900 * 60, expected.Data()), "partial URL password is hidden before the at-sign has been typed");
	}
	for (int language = 0; language < int(Language::Count); ++language)
	{
		settings.language = language;
		ShowCredentialVaultDialog(); dialog = wm.Top();
		bool fits = UiFont().Measure(remember(dialog)->text) <= remember(dialog)->rect.Width() - ScaleX(20);
		for (const auto& control : dialog->controls)
			if (auto* label = dynamic_cast<Static*>(control.get()))
				fits &= int(WrapText(UiFont(), label->text, label->rect.Width()).size()) * UiFont().Height() <= label->rect.Height();
		accept = button(dialog, T("Save"));
		fits &= UiFont().Measure(T("Retry")) <= accept->rect.Width() - ScaleX(8);
		check(fits, "vault checkbox, explanations and retry button fit in every supported language");
		dialog->OnCancel(); wm.Tick();
	}
	settings.language = 0;
	credentials::Get().Lock();
	bool nested_added = false;
	ShowAddNetworkDialog([&](const std::string&, const std::string& url) { nested_added = url == "smb://bob@server/Movies"; });
	Dialog* parent = wm.Top();
	edits(parent)[0]->text = "smb://bob:fixture%3Asecret@server/Movies";
	button(parent, "OK")->Activate();
	check(wm.Top() != parent && !nested_added, "adding a protected location waits for the nested vault dialog");
	wm.Top()->OnCancel(); wm.Tick();
	check(wm.Top() == parent && !nested_added, "cancelling the nested vault prompt preserves the network form without adding a location");
	button(parent, "OK")->Activate(); dialog = wm.Top();
	edits(dialog)[0]->text = master;
	accept = button(dialog, "Unlock"); accept->Activate(); wait(dialog, accept); wm.Tick();
	check(!wm.HasModal() && nested_added, "unlocking the nested vault completes the original network request");
	const std::string committed = read(root / "credentials.vault");
	credentials::Get().Lock();
	const std::string malformed("broken\0legacy", 13);
	write(legacy, malformed); settings.Load();
	check(credentials::Get().Unlock(master, error) && !settings.Save() && read(legacy) == malformed &&
		read(root / "credentials.vault") == committed, "malformed legacy data is preserved and never silently removed during migration");
	fs::remove(legacy); settings.Load();
	check(settings.Save(), "settings can be saved after an invalid legacy file is explicitly removed");
	wm.Clear(); credentials::Get().Lock();
	std::cout << "passed " << passed << ", failed " << failed << std::endl;
	return failed != 0;
}
