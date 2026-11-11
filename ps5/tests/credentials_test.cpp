// Encrypted password round trips, private local keys, failure preservation and session-only use.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using mpc::credentials::Store;
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
	std::string error, clean;
	Store store(root);
	check(store.Load(error) && !fs::exists(store.Path()) && !fs::exists(store.KeyPath()), "empty installation creates no key or password file");
	check(mpc::credentials::WithoutPassword(full) == bare, "display URL removes encoded password and preserves user, host, port and path");
	check(mpc::credentials::WithoutPassword("smb://user:secret@[::1]:445/share") == "smb://user@[::1]:445/share", "IPv6 authority is redacted without removing the port");
	check(store.SaveUrl(full, false, clean, error) && clean == bare && store.Resolve(bare) == full && !fs::exists(store.Path()), "unchecked password works in this session without being written to disk");
	store.ClearSession();
	check(store.Load(error) && store.Resolve(bare) == bare, "session-only credentials disappear after closing the app");
	store.ImportLegacy({{account, "p%40ss%3Aword"}});
	for (const char* failure : {"entropy", "key-write", "key-rename"})
	{
		setenv("MPCPS5_HOST_CREDENTIALS_FAIL", failure, 1);
		check(!store.Commit(error) && store.Pending() && !fs::exists(store.Path()) && !fs::exists(store.KeyPath()), "failed key creation preserves pending migration without a plaintext fallback");
	}
	unsetenv("MPCPS5_HOST_CREDENTIALS_FAIL");
	check(store.Commit(error) && !store.Pending() && store.Resolve(bare) == full, "legacy passwords are encrypted and verified automatically without a master password");
	const fs::path path = store.Path(), key_path = store.KeyPath();
	const std::string encrypted = read(path), key = read(key_path);
	check(encrypted.find("p%40ss%3Aword") == std::string::npos && encrypted.find(account) == std::string::npos && key.find("p%40ss%3Aword") == std::string::npos, "neither saved file contains the plaintext network password");
	struct stat info = {};
	stat(path.c_str(), &info); check((info.st_mode & 0777) == 0600, "encrypted passwords have owner-only permissions");
	stat(key_path.c_str(), &info); check((info.st_mode & 0777) == 0600, "local random key has owner-only permissions");
	stat(key_path.parent_path().c_str(), &info); check((info.st_mode & 0777) == 0700, "local key directory has owner-only permissions");
	store.ClearSession();
	check(store.Load(error) && store.Resolve(bare) == full, "a new app session restores the password automatically");
	store.Stage({{account, "new%40secret"}});
	for (const char* failure : {"write", "fsync", "rename", "entropy"})
	{
		setenv("MPCPS5_HOST_CREDENTIALS_FAIL", failure, 1);
		check(!store.Commit(error) && read(path) == encrypted && store.Pending() && !fs::exists(path.string()+".tmp"), "failed encrypted write keeps the previous file and pending password");
	}
	unsetenv("MPCPS5_HOST_CREDENTIALS_FAIL");
	check(store.Commit(error) && read(path) != encrypted && read(key_path) == key, "retry saves with a fresh nonce and retains the local key");
	check(store.SaveUrl(full, false, clean, error) && store.Resolve(bare) == full, "unchecking replaces the saved credential with a session-only password");
	store.ImportLegacy({{account, "old-plaintext"}});
	check(store.Commit(error), "legacy data does not reintroduce an explicitly unsaved account");
	store.ClearSession();
	check(store.Load(error) && store.Resolve(bare) == bare, "unchecked account is absent on the next launch");
	check(store.SaveUrl(full, true, clean, error), "checking saves the password again");
	store.ClearSession();
	const std::string valid = read(path);
	std::string bad = valid; bad.back() ^= 1; write(path, bad);
	check(!store.Load(error) && read(path) == bad, "modified ciphertext is rejected and preserved");
	write(path, valid); bad = valid; bad[40] ^= 1; write(path, bad);
	check(!store.Load(error), "authenticated header modification is rejected");
	write(path, valid.substr(0, 63)); check(!store.Load(error), "truncated file is rejected");
	write(path, valid); chmod(key_path.c_str(), 0644);
	check(!store.Load(error), "readable-by-others local key is rejected");
	chmod(key_path.c_str(), 0600); chmod(key_path.parent_path().c_str(), 0755);
	check(!store.Load(error), "non-private key directory is rejected");
	chmod(key_path.parent_path().c_str(), 0700);
	fs::rename(key_path, key_path.string()+".backup");
	check(!store.Load(error) && !fs::exists(key_path) && read(path) == valid, "missing key does not generate a replacement or overwrite encrypted credentials");
	fs::create_symlink(key_path.string()+".backup", key_path);
	check(!store.Load(error), "symlink key is rejected");
	fs::remove(key_path); fs::rename(key_path.string()+".backup", key_path);
	bad = key; bad.back() ^= 1; write(key_path, bad); chmod(key_path.c_str(), 0600);
	check(!store.Load(error), "wrong local key cannot decrypt saved credentials");
	write(key_path, key); chmod(key_path.c_str(), 0600);
	check(store.Load(error) && store.Resolve(bare) == full, "restoring valid files restores access");
	fs::create_directory(fs::path(root)/"second"); Store other(root+"/second");
	check(other.SaveUrl(full, true, clean, error) && read(other.KeyPath()) != key && read(other.Path()) != valid, "different installations use independent random keys and ciphertext");
	setenv("MPCPS5_HOST_CREDENTIALS_FAIL", "write", 1);
	check(!store.Retain({}, error) && read(path) == valid, "failed password cleanup preserves the last encrypted file");
	unsetenv("MPCPS5_HOST_CREDENTIALS_FAIL");
	check(store.Retain({}, error) && store.Resolve(bare) == full, "removing saved references keeps credentials available only for the current session");
	store.ClearSession();
	check(store.Load(error) && store.Resolve(bare) == bare, "unreferenced passwords are absent from the next session");
	std::cout << "passed " << passed << ", failed " << failed << std::endl;
	return failed != 0;
}
