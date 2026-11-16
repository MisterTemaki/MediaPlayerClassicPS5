// Media Player Classic PS5: the settings backed up to a USB drive and imported back (mpc_backup.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_backup.h"

#include "mpc_settings.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace mpc
{
namespace backup
{
namespace
{
// a settings file is a few KB (20 recent files, positions, favorites...): anything bigger isn't one
constexpr long kMaxBytes = 4 << 20;

std::mutex g_mutex;
Status g_status;

std::vector<std::string> AllDrives()
{
	std::vector<std::string> all;
#ifndef __PROSPERO__
	// host tests: the drives are folders named here (MPCPS5_HOST_DRIVES="/tmp/x/usb0:/tmp/x/usb1"), in that order
	if (const char* env = getenv("MPCPS5_HOST_DRIVES"))
	{
		std::string list = env;
		size_t at = 0;
		while (at <= list.size())
		{
			const size_t end = std::min(list.find(':', at), list.size());
			if (end > at)
				all.push_back(list.substr(at, end - at));
			at = end + 1;
		}
		return all;
	}
#endif
	for (int i = 0; i < 8; i++)
		all.push_back("/mnt/usb" + std::to_string(i));
	for (int i = 0; i < 2; i++)
		all.push_back("/mnt/ext" + std::to_string(i));
	return all;
}

bool ReadAll(const std::string& path, std::string& out)
{
	out.clear();
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[16384];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		out.append(buf, n);
		if (long(out.size()) > kMaxBytes)
		{
			ok = false;
			break;
		}
	}
	ok = ok && !ferror(f);
	fclose(f);
	return ok;
}

void Note(const std::string& drive, const std::string& error)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_status.drive = drive;
	g_status.when = time(nullptr);
	g_status.error = error;
}
} // namespace

std::vector<std::string> ConnectedDrives()
{
	std::vector<std::string> out;
	for (const std::string& d : AllDrives())
		if (OrbisIsDir(d))
			out.push_back(d);
	return out;
}

bool ValidDrive(const std::string& drive)
{
	const std::vector<std::string> all = AllDrives();
	return !drive.empty() && std::find(all.begin(), all.end(), drive) != all.end();
}

std::string FolderOn(const std::string& drive)
{
	return drive + "/mpcps5/backup";
}

std::string FileOn(const std::string& drive)
{
	return FolderOn(drive) + "/mpc-hc.ini";
}

time_t BackupTime(const std::string& drive)
{
	struct stat st = {};
	if (!ValidDrive(drive) || stat(FileOn(drive).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return 0;
	return st.st_mtime;
}

bool WriteTo(const std::string& drive, std::string& error, bool* written)
{
	if (written)
		*written = false;
	error.clear();
	std::string text, old;
	if (!ValidDrive(drive))
		error = "no drive is chosen";
	else if (!OrbisIsDir(drive))
		error = drive + " isn't connected";
	else if (!ReadAll(SettingsPath(), text) || text.empty())
		error = "the settings file can't be read";
	if (!error.empty())
	{
		Note(drive, error);
		OrbisLog("[backup] not written: %s", error.c_str());
		return false;
	}
	const std::string folder = FolderOn(drive), path = FileOn(drive), tmp = path + ".tmp";
	if (ReadAll(path, old) && old == text)
	{
		Note(drive, "");
		return true; // the backup already has these settings
	}
	if (!OrbisMkdirs(folder))
		error = "can't create " + folder + " (" + strerror(errno) + ")";
	else
	{
		FILE* f = fopen(tmp.c_str(), "wb");
		if (!f)
			error = "can't write " + tmp + " (" + strerror(errno) + ")";
		else
		{
			bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
			ok = fflush(f) == 0 && ok;
			// on the disk before the rename; a drive's file system that can't (fsync refused) still gets the file
			if (ok && fsync(fileno(f)) != 0)
				OrbisLog("[backup] fsync of %s: %s (written without it)", tmp.c_str(), strerror(errno));
			ok = fclose(f) == 0 && ok;
			if (!ok)
				error = "can't write " + tmp + " (" + strerror(errno) + ")";
			else if (rename(tmp.c_str(), path.c_str()) != 0)
			{
				// a file system that won't rename over a file: the old backup goes first
				if (unlink(path.c_str()) != 0 || rename(tmp.c_str(), path.c_str()) != 0)
					error = "can't replace " + path + " (" + strerror(errno) + ")";
			}
		}
		if (!error.empty())
			unlink(tmp.c_str());
	}
	if (error.empty())
	{
		const int fd = open(folder.c_str(), O_RDONLY);
		if (fd >= 0)
		{
			fsync(fd);
			close(fd);
		}
		if (written)
			*written = true;
		OrbisLog("[backup] the settings (%zu bytes) backed up to %s", text.size(), path.c_str());
	}
	else
		OrbisLog("[backup] not written: %s", error.c_str());
	Note(drive, error);
	return error.empty();
}

void AfterSave(const Settings& s)
{
	if (s.backup_drive.empty())
		return;
	std::string error;
	WriteTo(s.backup_drive, error);
}

Status LastStatus()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_status;
}

std::vector<Found> FindBackups()
{
	std::vector<Found> found;
	for (const std::string& d : ConnectedDrives())
		if (const time_t t = BackupTime(d))
			found.push_back({d, FileOn(d), t});
	std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });
	return found;
}

bool ReadBackup(const std::string& path, Settings& out, std::string& error)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		error = "there's no backup at " + path;
		return false;
	}
	if (st.st_size <= 0 || st.st_size > kMaxBytes)
	{
		error = path + " isn't a settings backup (its size)";
		return false;
	}
	out = Settings();
	if (!out.LoadFile(path))
	{
		out = Settings();
		error = path + " isn't a settings backup";
		return false;
	}
	OrbisLog("[backup] read %s: %zu recent files, %zu favorites, %zu network locations", path.c_str(), out.recent.size(),
		out.favorites.size(), out.servers.size());
	return true;
}
} // namespace backup
} // namespace mpc
