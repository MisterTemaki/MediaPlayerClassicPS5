// Media Player Classic PS5: the settings backed up to a USB drive and imported back (mpc_backup.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_backup.h"

#include "mpc_i18n.h"
#include "mpc_settings.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// the operating system's entropy source (third_party/mbedtls/mpc_entropy.c), as the credential store uses
extern "C" int mbedtls_hardware_poll(void*, unsigned char*, size_t, size_t*);

namespace mpc
{
namespace backup
{
namespace
{
// a settings file is a few KB (20 recent files, positions, favorites...): anything bigger isn't one
constexpr long kMaxBytes = 4 << 20;

std::mutex g_status_mutex;
Status g_status;
std::mutex g_write_mutex; // one writer at a time: the backup thread, Back up now

// the backup thread's work: the newest text handed over, written when the thread gets to it
std::mutex g_job_mutex;
std::condition_variable g_job_cv;
bool g_thread_started = false, g_job_pending = false, g_busy = false;
std::string g_job_drive, g_job_id, g_job_text;

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

// a settings text's BackupId= ("" when it has none)
std::string IdIn(const std::string& text)
{
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.compare(0, 9, "BackupId=") == 0)
			return line.substr(9);
		at = end + 1;
	}
	return "";
}

void Note(const std::string& drive, Result result, const std::string& error)
{
	std::lock_guard<std::mutex> lock(g_status_mutex);
	g_status.drive = drive;
	g_status.when = time(nullptr);
	g_status.result = result;
	g_status.error = error;
}

// text to the drive's backup (the write lock held)
Result Write(const std::string& drive, const std::string& id, const std::string& text, std::string& error, bool force)
{
	error.clear();
	if (!ValidDrive(drive))
		error = T("no drive is chosen");
	else if (!OrbisIsDir(drive))
		error = Fmt(T("%s isn't connected"), drive.c_str());
	else if (text.empty())
		error = T("the settings file can't be read");
	if (!error.empty())
	{
		OrbisLog("[backup] not written: %s", error.c_str());
		Note(drive, Result::Failed, error);
		return Result::Failed;
	}
	const std::string folder = FolderOn(drive), path = FileOn(drive), tmp = path + ".tmp";
	std::string old;
	if (ReadAll(path, old))
	{
		if (old == text)
		{
			Note(drive, Result::Unchanged, "");
			return Result::Unchanged; // the backup already has these settings
		}
		if (!force && (id.empty() || IdIn(old) != id))
		{
			OrbisLog("[backup] not written: %s is another installation's backup (BackupId %s, this one's %s)",
				path.c_str(), IdIn(old).empty() ? "none" : IdIn(old).c_str(), id.c_str());
			Note(drive, Result::Foreign, "");
			return Result::Foreign;
		}
	}
	if (!OrbisMkdirs(folder))
		error = Fmt(T("can't create %s (%s)"), folder.c_str(), strerror(errno));
	else
	{
		FILE* f = fopen(tmp.c_str(), "wb");
		if (!f)
			error = Fmt(T("can't write %s (%s)"), tmp.c_str(), strerror(errno));
		else
		{
			bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
			ok = fflush(f) == 0 && ok;
			// on the disk before the rename; a drive's file system that can't (fsync refused) still gets the file
			if (ok && fsync(fileno(f)) != 0)
				OrbisLog("[backup] fsync of %s: %s (written without it)", tmp.c_str(), strerror(errno));
			ok = fclose(f) == 0 && ok;
			if (!ok)
			{
				error = Fmt(T("can't write %s (%s)"), tmp.c_str(), strerror(errno));
				unlink(tmp.c_str());
			}
			else if (rename(tmp.c_str(), path.c_str()) != 0)
			{
				// a file system that won't rename over a file: the old backup goes first. If the rename still
				// fails, the new backup stays as mpc-hc.ini.tmp (said), never deleted
				if (unlink(path.c_str()) != 0 && errno != ENOENT)
				{
					error = Fmt(T("can't replace %s (%s)"), path.c_str(), strerror(errno));
					unlink(tmp.c_str());
				}
				else if (rename(tmp.c_str(), path.c_str()) != 0)
					error = Fmt(T("can't replace %s (%s); the new backup is in %s"), path.c_str(), strerror(errno),
						tmp.c_str());
			}
		}
	}
	if (!error.empty())
	{
		OrbisLog("[backup] not written: %s", error.c_str());
		Note(drive, Result::Failed, error);
		return Result::Failed;
	}
	const int fd = open(folder.c_str(), O_RDONLY);
	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
	OrbisLog("[backup] the settings (%zu bytes) backed up to %s%s", text.size(), path.c_str(),
		force ? " (replacing another installation's backup)" : "");
	Note(drive, Result::Written, "");
	return Result::Written;
}

void Thread()
{
	std::unique_lock<std::mutex> lock(g_job_mutex);
	for (;;)
	{
		g_job_cv.wait(lock, [] { return g_job_pending; });
		const std::string drive = g_job_drive, id = g_job_id, text = g_job_text;
		g_job_pending = false;
		g_busy = true;
		lock.unlock();
		{
			std::lock_guard<std::mutex> write(g_write_mutex);
			std::string error;
			Write(drive, id, text, error, false);
		}
		lock.lock();
		g_busy = false;
		g_job_cv.notify_all();
	}
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

std::string NewId()
{
	unsigned char b[16] = {};
	size_t done = 0;
	if (mbedtls_hardware_poll(nullptr, b, sizeof(b), &done) != 0 || done != sizeof(b))
	{
		// no entropy: the time and the process (an id only tells installations apart, it guards nothing)
		const uint64_t t = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ uint64_t(time(nullptr));
		for (int i = 0; i < 16; i++)
			b[i] = static_cast<unsigned char>((t >> ((i % 8) * 8)) ^ (unsigned(getpid()) >> (i % 4)) ^ (i * 37));
	}
	static const char hex[] = "0123456789abcdef";
	std::string id;
	for (unsigned char c : b)
	{
		id += hex[c >> 4];
		id += hex[c & 15];
	}
	return id;
}

bool ValidId(const std::string& id)
{
	return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}

Info InfoOn(const std::string& drive, const std::string& id)
{
	Info info;
	struct stat st = {};
	if (!ValidDrive(drive) || stat(FileOn(drive).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return info;
	info.exists = true;
	info.when = st.st_mtime;
	std::string text;
	info.ours = ReadAll(FileOn(drive), text) && !id.empty() && IdIn(text) == id;
	return info;
}

Result WriteTo(const std::string& drive, const std::string& id, std::string& error, bool force)
{
	std::string text;
	if (!ReadAll(SettingsPath(), text))
		text.clear();
	std::lock_guard<std::mutex> write(g_write_mutex);
	return Write(drive, id, text, error, force);
}

void AfterSave(const Settings& s)
{
	if (s.backup_drive.empty())
		return;
	std::string text;
	if (!ReadAll(SettingsPath(), text))
		text.clear(); // the thread says why
	std::lock_guard<std::mutex> lock(g_job_mutex);
	g_job_drive = s.backup_drive;
	g_job_id = s.backup_id;
	g_job_text = std::move(text);
	g_job_pending = true;
	if (!g_thread_started)
	{
		g_thread_started = true;
		std::thread(Thread).detach(); // waits on g_job_cv for the rest of the process
	}
	g_job_cv.notify_all();
}

void Flush(int ms)
{
	std::unique_lock<std::mutex> lock(g_job_mutex);
	if (!g_job_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return !g_job_pending && !g_busy; }))
		OrbisLog("[backup] the backup thread didn't finish within %d ms", ms);
}

Status LastStatus()
{
	std::lock_guard<std::mutex> lock(g_status_mutex);
	return g_status;
}

std::vector<Found> FindBackups()
{
	std::vector<Found> found;
	for (const std::string& d : ConnectedDrives())
	{
		const Info info = InfoOn(d, "");
		if (info.exists)
			found.push_back({d, FileOn(d), info.when});
	}
	std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });
	return found;
}

bool ReadBackup(const std::string& path, Settings& out, std::string& error)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		error = Fmt(T("there's no backup at %s"), path.c_str());
		return false;
	}
	out = Settings();
	if (st.st_size <= 0 || st.st_size > kMaxBytes || !out.LoadFile(path))
	{
		out = Settings();
		error = Fmt(T("%s isn't a settings backup"), path.c_str());
		return false;
	}
	OrbisLog("[backup] read %s: %zu recent files, %zu favorites, %zu network locations", path.c_str(), out.recent.size(),
		out.favorites.size(), out.servers.size());
	return true;
}
} // namespace backup
} // namespace mpc
