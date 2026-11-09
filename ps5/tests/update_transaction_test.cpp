// SPDX-License-Identifier: MIT
// Standalone regressions for the real updater, without FFmpeg or a PS5 SDK.
#include "ProsperoUpdate.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
static std::string blocked_journal;
static bool fail_commit_sync = false;
static bool journal_removed = false;
static int failures = 0;

void OrbisLog(const char*, ...) {}
extern "C" int __real_unlink(const char*);
extern "C" int __real_fsync(int);
extern "C" int __wrap_unlink(const char* path)
{
	if (!blocked_journal.empty() && blocked_journal == path)
	{
		errno = EIO;
		return -1;
	}
	const int rc = __real_unlink(path);
	if (rc == 0 && std::string(path).find("/update.journal") != std::string::npos)
		journal_removed = true;
	return rc;
}
extern "C" int __wrap_fsync(int fd)
{
	if (fail_commit_sync && journal_removed)
	{
		fail_commit_sync = false; // the commit's directory sync fails once
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static void check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += !ok;
}
static void write(const fs::path& path, const std::string& bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream f(path, std::ios::binary);
	f << bytes;
	if (!f) std::abort();
}
static std::string read(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}
static std::string sha(const std::string& s)
{
	return update::Sha256Hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
static void seed(const fs::path& app)
{
	write(app / "eboot.bin", "old eboot");
	write(app / "sce_sys/param.json", "old param");
}

int main(int argc, char** argv)
{
	if (argc != 3) return 2;
	const fs::path root = argv[1], fixtures = argv[2];
	fs::create_directories(root);
	update::Config c;
	c.title_id = "PPSA99015";
	auto install = [&](const char* name, const char* fixture = "valid.zip") {
		c.app_dir = (root / name).string();
		const std::string bytes = read(fixtures / fixture);
		return update::InstallZip({bytes.begin(), bytes.end()}, sha(bytes), c);
	};
	const fs::path lost = root / "lost";
	write(lost / "eboot.bin", "new eboot without backup");
	write(lost / "update.journal", "mpcps5-update-journal 1 100-1\n1\t" + sha("old eboot") + "\teboot.bin\nend\n");
	check(update::RecoverInterrupted(lost.string()) == 0, "lost old file is not counted as restored");
	check(fs::exists(lost / "update.journal"), "lost old file keeps the recovery journal");
	check(!install("lost").empty(), "a missing backup blocks a new install");
	check(read(lost / "eboot.bin") == "new eboot without backup", "failed recovery leaves the unrecognized file alone");
	const fs::path missing = root / "missing-empty";
	write(missing / "update.journal", "mpcps5-update-journal 1 100-3\n1\t" + sha("") + "\teboot.bin\nend\n");
	check(update::RecoverInterrupted(missing.string()) == 0 && fs::exists(missing / "update.journal"),
		"a missing file cannot masquerade as a restored empty file");

	const fs::path already = root / "already";
	seed(already);
	write(already / "update.journal", "mpcps5-update-journal 1 100-4\n1\t" + sha("old eboot") + "\teboot.bin\nend\n");
	check(update::RecoverInterrupted(already.string()) == 1 && !fs::exists(already / "update.journal"),
		"an already-restored file is recognized by its full hash");
	const std::string large(2u << 20, 'a');
	write(already / "eboot.bin", large);
	write(already / "update.journal", "mpcps5-update-journal 1 100-5\n1\t" + sha(large.substr(0, 1u << 20)) +
		"\teboot.bin\nend\n");
	check(update::RecoverInterrupted(already.string()) == 1 && !fs::exists(already / "update.journal"),
		"legacy first-MiB journal hashes remain readable");

	const fs::path retry = root / "retry";
	seed(retry);
	write(retry / "eboot.bin.old.100-2", "old eboot");
	write(retry / "eboot.bin", "new eboot");
	write(retry / "update.journal", "mpcps5-update-journal 1 100-2\n1\t" + sha("old eboot") + "\teboot.bin\nend\n");
	setenv("MPCPS5_HOST_UPDATE_FAIL_RESTORE", "eboot.bin", 1);
	check(update::RecoverInterrupted(retry.string()) == 0 && fs::exists(retry / "update.journal"),
		"failed restore retains the journal and backup");
	unsetenv("MPCPS5_HOST_UPDATE_FAIL_RESTORE");
	check(update::RecoverInterrupted(retry.string()) == 1 && !fs::exists(retry / "update.journal") &&
		read(retry / "eboot.bin") == "old eboot", "restore succeeds on retry");

	seed(root / "unlink");
	blocked_journal = (root / "unlink/update.journal").string();
	const std::string unlink_error = install("unlink");
	blocked_journal.clear();
	check(!unlink_error.empty(), "failure to remove the journal is not reported as success");
	check(read(root / "unlink/eboot.bin") == "old eboot" && read(root / "unlink/sce_sys/param.json") == "old param",
		"commit unlink failure restores the original files");
	update::RecoverInterrupted((root / "unlink").string());
	check(!fs::exists(root / "unlink/update.journal") && read(root / "unlink/eboot.bin") == "old eboot",
		"recovery after a failed commit remains possible");

	seed(root / "sync");
	journal_removed = false;
	fail_commit_sync = true;
	check(!install("sync").empty(), "failure to sync the journal removal is not reported as success");
	check(read(root / "sync/eboot.bin") == "old eboot", "commit sync failure restores the original eboot");

	seed(root / "rename");
	setenv("MPCPS5_HOST_UPDATE_FAIL_RENAME", "eboot.bin", 1);
	check(!install("rename").empty() && read(root / "rename/eboot.bin") == "old eboot" &&
		read(root / "rename/sce_sys/param.json") == "old param", "rename failure rolls all files back");
	unsetenv("MPCPS5_HOST_UPDATE_FAIL_RENAME");

	seed(root / "valid");
	check(install("valid").empty() && read(root / "valid/eboot.bin") == "new eboot" &&
		!fs::exists(root / "valid/update.journal"), "valid release installs and commits");
	fs::create_directories(root / "unreadable/eboot.bin");
	write(root / "unreadable/sce_sys/param.json", "old param");
	check(!install("unreadable").empty() && fs::is_directory(root / "unreadable/eboot.bin") &&
		!fs::exists(root / "unreadable/update.journal"), "failure to read an old file stops the install before replacement");
	seed(root / "auth");
	c.app_dir = (root / "auth").string();
	const std::string bytes = read(fixtures / "valid.zip");
	const std::vector<uint8_t> zip(bytes.begin(), bytes.end());
	check(!update::InstallZip(zip, std::string(64, '0'), c).empty() && read(root / "auth/eboot.bin") == "old eboot",
		"wrong download hash leaves the app unchanged");
	c.release_key = std::string(64, '0');
	check(!update::InstallZip(zip, sha(bytes), c).empty() && read(root / "auth/eboot.bin") == "old eboot",
		"a configured release key still requires a signature");
	c.release_key.clear();
	for (const char* name : {"reserved.zip", "newline.zip", "nul.zip", "traversal.zip", "duplicate.zip",
		"staging.zip", "backup.zip", "unlisted.zip", "self-manifest.zip"})
	{
		const std::string dir = std::string("bad-") + name;
		seed(root / dir);
		check(!install(dir.c_str(), name).empty() && read(root / dir / "eboot.bin") == "old eboot" &&
			!fs::exists(root / dir / "update.journal"), name);
	}
	std::printf("update_transaction_test: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
