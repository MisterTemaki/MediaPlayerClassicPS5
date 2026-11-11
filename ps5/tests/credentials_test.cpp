// Real encrypted-vault round trips, failure preservation and optional automatic unlock.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using mpc::credentials::Vault;
static int passed = 0, failed = 0;
static std::string root;
const std::string& OrbisRoot() { return root; }
void OrbisLog(const char*, ...) {}
void check(bool value, const char* message)
{
	std::cout << (value ? "ok: " : "FAIL: ") << message << std::endl;
	(value ? passed : failed)++;
}
std::string read(const fs::path& file)
{
	std::ifstream input(file, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), {}};
}
void write(const fs::path& file, const std::string& value)
{
	std::ofstream output(file, std::ios::binary);
	output.write(value.data(), value.size());
	if (!output) std::abort();
}
int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	root = argv[1];
	if (!fs::create_directory(root)) return 2;
	const std::string bare = "smb://DOMAIN;bob@server:445/Movies/movie.mp4";
	const std::string full = "smb://DOMAIN;bob:p%40ss%3Aword@server:445/Movies/movie.mp4";
	const std::string account = "smb://DOMAIN;bob@server:445";
	const std::string master = "a synthetic master passphrase", second = "a different master passphrase";
	std::string error;
	Vault vault(root);
	check(!vault.Exists() && !vault.Unlocked() && !vault.Remembered(), "new vault is locked and automatic unlock is off");
	check(mpc::credentials::WithoutPassword(full) == bare, "display URL removes percent-encoded password and preserves user, host, port and path");
	check(mpc::credentials::WithoutPassword("smb://user:secret@[::1]:445/share") == "smb://user@[::1]:445/share",
		"IPv6 authority is redacted without removing the port");
	vault.ImportLegacy({{account, "p%40ss%3Aword"}});
	check(vault.Pending() && vault.Resolve(bare) == bare, "legacy credentials remain unavailable while locked");
	check(!vault.Commit(error) && !vault.Exists(), "locked vault cannot write a plaintext fallback");
	check(!vault.CreateOrChange("short", error) && !vault.Exists(), "short master password is rejected");
	setenv("MPCPS5_HOST_VAULT_FAIL", "entropy", 1);
	check(!vault.CreateOrChange(master, error) && !vault.Exists(), "unavailable OS randomness fails closed");
	unsetenv("MPCPS5_HOST_VAULT_FAIL");
	check(vault.CreateOrChange(master, error) && vault.Unlocked() && !vault.Pending(), "master password creates and verifies the encrypted vault");
	const fs::path path = fs::path(root) / "credentials.vault";
	const std::string encrypted = read(path);
	check(encrypted.find("p%40ss%3Aword") == std::string::npos && encrypted.find(master) == std::string::npos &&
		encrypted.find(account) == std::string::npos, "vault stores neither plaintext credentials nor the master password");
	struct stat info = {};
	stat(path.c_str(), &info);
	check((info.st_mode & 0777) == 0600, "vault file has owner-only permissions");
	check(vault.Resolve(bare) == full, "unlocked vault restores the actual transport password");
	check(!fs::exists(path.string() + ".tmp") && !vault.Remembered(), "normal save leaves no temporary file or automatic unlock key");
	vault.Lock();
	check(!vault.Unlocked() && vault.Resolve(bare) == bare && !vault.Unlock("incorrect password", error), "locking clears usable secrets and an incorrect master cannot unlock");
	check(read(path) == encrypted, "failed authentication leaves the vault unchanged");
	check(vault.Unlock(master, error) && vault.Resolve(bare) == full, "master password unlocks a fresh session");
	vault.Stage({{account, "new%40secret"}});
	for (const char* failure : {"write", "fsync", "rename", "entropy"})
	{
		setenv("MPCPS5_HOST_VAULT_FAIL", failure, 1);
		check(!vault.Commit(error) && read(path) == encrypted && vault.Pending() && !fs::exists(path.string() + ".tmp"),
			"failed save preserves the previous vault and pending password without a plaintext temporary file");
	}
	unsetenv("MPCPS5_HOST_VAULT_FAIL");
	check(vault.Commit(error) && read(path) != encrypted, "retry commits the pending password using a fresh nonce");
	const std::string updated = read(path);
	check(vault.SetRemembered(true, error) && vault.Remembered(), "explicit checkbox opt-in creates an automatic unlock key");
	const fs::path auto_key = fs::path(root) / ".credentials-device/unlock.key";
	stat(auto_key.c_str(), &info);
	check((info.st_mode & 0777) == 0600, "automatic unlock key has owner-only permissions");
	stat(auto_key.parent_path().c_str(), &info);
	check((info.st_mode & 0777) == 0700, "automatic unlock directory has owner-only permissions");
	const std::string remembered = read(auto_key);
	check(remembered.find(master) == std::string::npos && remembered.find("new%40secret") == std::string::npos,
		"remembered state contains neither master password nor SMB password");
	vault.Lock();
	check(vault.UnlockRemembered(error) && vault.Resolve(bare).find("new%40secret") != std::string::npos,
		"opted-in automatic unlock works after closing and reopening a vault session");
	write(auto_key.string() + ".tmp", remembered);
	check(vault.SetRemembered(false, error) && !fs::exists(auto_key) && !fs::exists(auto_key.string() + ".tmp"),
		"unchecking the option removes the local key and a temporary key left by an interrupted save");
	vault.Lock();
	check(!vault.UnlockRemembered(error) && !vault.Unlocked(), "unchecking restores master-password authentication next session");
	check(vault.Unlock(master, error), "manual master authentication remains available");
	check(vault.SetRemembered(true, error) && vault.CreateOrChange(second, error), "master password can be changed without losing credentials");
	vault.Lock();
	check(!vault.UnlockRemembered(error), "a remembered key for an old vault salt cannot unlock the changed vault");
	check(!vault.Unlock(master, error) && vault.Unlock(second, error), "old master is rejected and new master works");
	check(vault.Resolve(bare).find("new%40secret") != std::string::npos, "master change preserves saved credentials");
	check(vault.SetRemembered(true, error), "remembered unlock can be refreshed after changing the master password");
	chmod(auto_key.c_str(), 0644);
	vault.Lock();
	check(!vault.UnlockRemembered(error), "automatic unlock rejects an unlock-key file readable by other users");
	chmod(auto_key.c_str(), 0600);
	const std::string original = read(path);
	std::string corrupt = original; corrupt.back() ^= 1; write(path, corrupt);
	check(!vault.Unlock(second, error) && !vault.UnlockRemembered(error), "authenticated encryption rejects modified ciphertext");
	write(path, original);
	corrupt = original; corrupt[8] = static_cast<char>(0xff); write(path, corrupt);
	check(!vault.Unlock(second, error), "invalid KDF parameters are rejected before allocating attacker-selected memory");
	write(path, original.substr(0, 79));
	check(!vault.Unlock(second, error), "truncated vault is rejected without changing data");
	write(path, original);
	check(vault.Unlock(second, error), "restoring a valid vault restores access");
	check(vault.SetRemembered(false, error) && read(path) == original, "disabling automatic unlock does not alter the encrypted vault");
	check(updated != original, "changing the master password rotates the salt and ciphertext");
	std::cout << "passed " << passed << ", failed " << failed << std::endl;
	return failed != 0;
}
