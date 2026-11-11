// Credential-vault prompts; expensive password derivation runs outside the UI thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include "mpc_dialogs.h"
#include "mpc_i18n.h"
#include "mpc_ui.h"
#include <atomic>
#include <memory>
#include <thread>

namespace mpc
{
namespace
{
struct Secret
{
	std::string text;
	~Secret() { credentials::Wipe(text); }
};
class VaultDialog : public Dialog
{
public:
	Edit *password = nullptr, *confirm = nullptr;
	Static* status = nullptr;
	Button *accept = nullptr, *cancel = nullptr;
	CheckBox* remember = nullptr;
	bool create = false, existing_unlocked = false, busy = false, retry_migration = false;
	struct Work { std::atomic<bool> done{false}; bool ok = false; std::string error; };
	std::shared_ptr<Work> work;
	std::thread worker;
	~VaultDialog() override { if (worker.joinable()) worker.join(); }
	void OnCancel() override { if (!busy) EndDialog(IDCANCEL); }
	void OnOk() override { Start(); }
	void FinishMigration()
	{
		std::string error;
		if (!credentials::Get().Commit(error) || !credentials::Get().SetRemembered(remember->checked, error) || !AppSettings().Save())
		{
			status->text = error.empty() ? T("The vault is saved, but settings migration could not finish. Try again.") : T(error);
			retry_migration = true;
			accept->text = T("Retry");
			return;
		}
		EndDialog(IDOK);
	}
	void Start()
	{
		if (busy) return;
		if (retry_migration) { FinishMigration(); return; }
		if (existing_unlocked && password->text.empty() && confirm->text.empty()) { FinishMigration(); return; }
		if (create && password->text != confirm->text)
		{
			status->text = T("The master passwords do not match.");
			return;
		}
		auto secret = std::make_shared<Secret>();
		secret->text.swap(password->text);
		credentials::Wipe(password->text);
		credentials::Wipe(confirm->text);
		busy = true;
		has_close = false;
		password->enabled = confirm->enabled = accept->enabled = cancel->enabled = false;
		remember->enabled = false;
		status->text = T("Unlocking credential vault...");
		work = std::make_shared<Work>();
		const auto result = work;
		const bool make = create;
		try
		{
			worker = std::thread([secret, result, make] {
				try
				{
					result->ok = make ? credentials::Get().CreateOrChange(secret->text, result->error)
						: credentials::Get().Unlock(secret->text, result->error);
				}
				catch (const std::exception&) { result->error = "The credential vault operation failed."; }
				credentials::Wipe(secret->text);
				result->done.store(true, std::memory_order_release);
			});
		}
		catch (const std::exception&)
		{
			work->error = "The credential vault operation failed.";
			work->done.store(true, std::memory_order_release);
		}
	}
	void Tick() override
	{
		if (!busy || !work->done.load(std::memory_order_acquire)) return;
		if (worker.joinable()) worker.join();
		busy = false;
		has_close = true;
		password->enabled = confirm->enabled = accept->enabled = cancel->enabled = true;
		remember->enabled = true;
		if (work->ok) FinishMigration();
		else status->text = T(work->error);
	}
};
} // namespace

void ShowCredentialVaultDialog(std::function<void(bool)> done)
{
	auto* dialog = new VaultDialog;
	const bool exists = credentials::Get().Exists(), unlocked = credentials::Get().Unlocked();
	dialog->create = !exists || unlocked;
	dialog->existing_unlocked = unlocked;
	dialog->title = T(!exists ? "Create credential vault" : unlocked ? "Credential vault" : "Unlock credential vault");
	dialog->rect = CRect(0, 0, Dialog::DuX(310) + 2, Dialog::DuY(207) + dialog->CaptionHeight() + 1);
	dialog->Center(WindowManager::Get().screen);
	auto* intro = dialog->Add(new Static);
	intro->rect = dialog->DuRect(9, 8, 292, 32);
	intro->text = T(unlocked ? "The vault is unlocked. Leave the password fields empty to keep your master password."
		: dialog->create ? "Choose a master password with at least 8 characters. It is required once per app session and cannot be recovered."
		: "Enter your master password to use saved network passwords for this app session.");
	auto row = [&](const char* label, int y) {
		auto* text = dialog->Add(new Static);
		text->text = T(label); text->rect = dialog->DuRect(9, y, 110, 14);
		auto* edit = dialog->Add(new Edit);
		edit->rect = dialog->DuRect(122, y, 179, 14); edit->password = true;
		edit->keyboard_title = T(label);
		return edit;
	};
	dialog->password = row("Master password:", 45);
	dialog->confirm = row("Confirm password:", 65);
	dialog->confirm->visible = dialog->create;
	dialog->status = dialog->Add(new Static);
	dialog->remember = dialog->Add(new CheckBox);
	dialog->remember->text = T("Remember on this PS5 (automatic unlock)");
	dialog->remember->rect = dialog->DuRect(9, 88, 292, 12);
	dialog->remember->checked = credentials::Get().Remembered();
	auto* warning = dialog->Add(new Static);
	warning->rect = dialog->DuRect(9, 103, 292, 29);
	warning->text = T("Saves a local unlock key. Anyone who can read both the key and the vault can access saved passwords.");
	dialog->status->rect = dialog->DuRect(9, 138, 292, 36);
	if (credentials::Get().Pending())
		dialog->status->text = T("Existing plaintext passwords will be migrated only after the encrypted vault is saved and verified.");
	dialog->accept = dialog->Add(new Button);
	dialog->accept->text = T(dialog->create ? "Save" : "Unlock");
	dialog->accept->is_default = true;
	dialog->accept->rect = dialog->DuRect(165, 183, 76, 14);
	dialog->accept->on_click = [dialog] { dialog->Start(); };
	dialog->cancel = dialog->Add(new Button);
	dialog->cancel->text = T("Cancel");
	dialog->cancel->rect = dialog->DuRect(247, 183, 54, 14);
	dialog->cancel->on_click = [dialog] { dialog->OnCancel(); };
	dialog->on_close = [done](int result) { if (done) done(result == IDOK); };
	WindowManager::Get().Push(dialog);
	dialog->SetFocus(dialog->password);
}

bool NeedsCredentialPrompt(const std::string& url)
{
	std::string key, password, bare;
	const bool secret = credentials::Split(url, key, password, bare) && !password.empty();
	credentials::Wipe(password);
	return secret || (credentials::HasUser(url) && !credentials::Get().Unlocked() &&
		(credentials::Get().Exists() || credentials::Get().Pending()));
}
void RequestCredentials(std::string url, std::function<void(const std::string&)> ready)
{
	std::string key, bare;
	auto secret = std::make_shared<Secret>();
	const bool inline_password = credentials::Split(url, key, secret->text, bare);
	if (!inline_password) bare = url;
	credentials::Wipe(url);
	const auto finish = [secret, key, bare, ready, inline_password](bool ok) {
		if (!ok) return;
		if (inline_password && (!secret->text.empty() || credentials::Get().Unlocked()))
		{
			credentials::Get().Stage({{key, secret->text}});
			std::string error;
			if (!credentials::Get().Commit(error))
			{
				MessageBox(T(error), T("Credential vault"), MB_OK, 3);
				return;
			}
		}
		credentials::Wipe(secret->text);
		if (ready) ready(bare);
	};
	if (!credentials::Get().Unlocked() && (!secret->text.empty() ||
		(credentials::HasUser(bare) && (credentials::Get().Exists() || credentials::Get().Pending()))))
		ShowCredentialVaultDialog(finish);
	else finish(true);
}
} // namespace mpc
