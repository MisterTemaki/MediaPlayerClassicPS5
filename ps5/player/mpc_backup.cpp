// Media Player Classic PS5: the settings backed up to a USB drive and imported back (mpc_backup.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_backup.h"

#include "mpc_i18n.h"
#include "mpc_settings.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/time.h>
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

// 1.6.4 (audit T1): an errno's text, safe on any thread. strerror() isn't required to be (FreeBSD's fills one static
// buffer), and the backup and disc threads use it beside the UI thread. strerror_r has two forms (XSI: int, GNU:
// char*); both are taken.
[[maybe_unused]] inline const char* ErrnoTextOf(int r, const char* buf) { return r == 0 || buf[0] ? buf : "unknown error"; }
[[maybe_unused]] inline const char* ErrnoTextOf(const char* r, const char*) { return r ? r : "unknown error"; }
std::string ErrnoText(int e)
{
	char buf[128] = "";
	return ErrnoTextOf(strerror_r(e, buf, sizeof(buf)), buf);
}

// made once and never destroyed: the backup thread waits on the condition variable until the process ends, and
// destroying one with a waiter at exit() blocks for ever on glibc (the app itself ends with _exit)
struct Shared
{
	std::mutex status_mutex;
	Status status;
	unsigned generation = 0;
	std::mutex write_mutex; // one writer of the settings' backup at a time: the backup thread, Back up now
	// (1.7.1 review I2) the fonts' copies have their own: Back up now doesn't wait for a long font copy
	std::mutex fonts_mutex; // SyncFonts (the backup thread), RestoreFonts (an import)
	// the backup thread's work: the newest text handed over, written when the thread gets to it
	std::mutex job_mutex;
	std::condition_variable job_cv;
	bool thread_started = false, thread_failed = false, job_pending = false, busy = false;
	std::string job_drive, job_id, job_text;
	// 1.7.1 (#12): the fonts to copy to a drive (Back up now hands them over: the screen doesn't wait for them)
	bool fonts_pending = false;
	std::string fonts_drive;
};
Shared& G()
{
	static Shared* g = new Shared;
	return *g;
}

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

Status Note(const std::string& drive, Result result, Error error = Error::None, const std::string& path = "",
	const std::string& reason = "", const std::string& kept = "")
{
	Status s;
	s.drive = drive;
	s.when = time(nullptr);
	s.result = result;
	s.error = error;
	s.path = path;
	s.reason = reason;
	s.kept = kept;
	if (error != Error::None)
		OrbisLog("[backup] not written: %s", ErrorText(s, false).c_str());
	std::lock_guard<std::mutex> lock(G().status_mutex);
	G().status = s;
	G().generation++;
	return s;
}

// text to the drive's backup (the write lock held). Never translates (the backup thread calls it).
Status Write(const std::string& drive, const std::string& id, const std::string& text, bool force)
{
	if (!ValidDrive(drive))
		return Note(drive, Result::Failed, Error::NoDrive);
	if (!OrbisIsDir(drive))
		return Note(drive, Result::Failed, Error::NotConnected, drive);
	if (text.empty())
		return Note(drive, Result::Failed, Error::CantRead);
	const std::string folder = FolderOn(drive), path = FileOn(drive), tmp = path + ".tmp";
	struct stat st = {};
	if (stat(path.c_str(), &st) == 0)
	{
		std::string old;
		const bool read = ReadAll(path, old);
		if (read && old == text)
			return Note(drive, Result::Unchanged); // the backup already has these settings
		// someone else's, or one that can't be read (another file system's, a folder, too big): kept
		if (!force && (!read || id.empty() || IdIn(old) != id))
		{
			OrbisLog("[backup] not written: %s is another installation's backup or can't be read (BackupId %s, this "
					 "one's %s)",
				path.c_str(), !read ? "unreadable" : IdIn(old).empty() ? "none" : IdIn(old).c_str(), id.c_str());
			return Note(drive, Result::Foreign);
		}
	}
	if (!OrbisMkdirs(folder))
		return Note(drive, Result::Failed, Error::CantCreate, folder, ErrnoText(errno));
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return Note(drive, Result::Failed, Error::CantWrite, tmp, ErrnoText(errno));
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	ok = fflush(f) == 0 && ok;
	// on the disk before the rename; a drive's file system that can't (fsync refused) still gets the file
	if (ok && fsync(fileno(f)) != 0)
		OrbisLog("[backup] fsync of %s: %s (written without it)", tmp.c_str(), ErrnoText(errno).c_str());
	ok = fclose(f) == 0 && ok;
	if (!ok)
	{
		const std::string reason = ErrnoText(errno);
		unlink(tmp.c_str());
		return Note(drive, Result::Failed, Error::CantWrite, tmp, reason);
	}
	if (rename(tmp.c_str(), path.c_str()) != 0)
	{
		// a file system that won't rename over a file: the old backup goes first. If the rename still fails, the
		// new backup stays as mpc-hc.ini.tmp (said), never deleted
		if (unlink(path.c_str()) != 0 && errno != ENOENT)
		{
			const std::string reason = ErrnoText(errno);
			unlink(tmp.c_str());
			return Note(drive, Result::Failed, Error::CantReplace, path, reason);
		}
		if (rename(tmp.c_str(), path.c_str()) != 0)
			return Note(drive, Result::Failed, Error::CantReplaceKept, path, ErrnoText(errno), tmp);
	}
	const int fd = open(folder.c_str(), O_RDONLY);
	if (fd >= 0)
	{
		fsync(fd);
		close(fd);
	}
	OrbisLog("[backup] the settings (%zu bytes) backed up to %s%s", text.size(), path.c_str(),
		force ? " (replacing another installation's backup)" : "");
	return Note(drive, Result::Written);
}

// ---- 1.7.1 (Sulfurico-One64, #12): the fonts folder ----------------------------------------------------------
// a font file's name (as the fonts folder lists them: .ttf, .otf, .ttc, .otc); not a hidden or half-copied one
constexpr long long kMaxFontBytes = 256ll << 20;

bool FontFileName(const std::string& n)
{
	if (n.empty() || n[0] == '.' || n.size() < 5)
		return false;
	std::string ext = n.substr(n.size() - 4);
	for (char& c : ext)
		c = char(tolower(static_cast<unsigned char>(c)));
	return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

struct FontFile
{
	std::string name;
	long long size = 0;
	time_t mtime = 0;
};

std::vector<FontFile> FontsIn(const std::string& dir)
{
	std::vector<FontFile> out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	while (dirent* e = readdir(d))
	{
		const std::string n = e->d_name;
		struct stat st = {};
		if (FontFileName(n) && stat((dir + "/" + n).c_str(), &st) == 0 && S_ISREG(st.st_mode))
			out.push_back({n, (long long)st.st_size, st.st_mtime});
	}
	closedir(d);
	return out;
}

// one file copied: beside its place (.part), synced, then renamed over it, with the source's time (so an unchanged
// font isn't copied again: review I4); why: the reason on a failure
bool CopyFile(const std::string& from, const std::string& to, time_t mtime, std::string& why)
{
	const std::string part = to + ".part";
	FILE* in = fopen(from.c_str(), "rb");
	if (!in)
	{
		why = ErrnoText(errno);
		return false;
	}
	FILE* out = fopen(part.c_str(), "wb");
	if (!out)
	{
		why = ErrnoText(errno);
		fclose(in);
		return false;
	}
	char buf[65536];
	size_t n;
	bool ok = true;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	ok = ok && !ferror(in);
	ok = fflush(out) == 0 && ok;
	if (ok && fsync(fileno(out)) != 0)
		OrbisLog("[backup] fsync of %s: %s (written without it)", part.c_str(), ErrnoText(errno).c_str());
	if (!ok)
		why = ErrnoText(errno);
	ok = fclose(out) == 0 && ok;
	fclose(in);
	if (ok && rename(part.c_str(), to.c_str()) != 0)
	{
		// (a file system that won't rename over a file: the old copy goes first; if the rename still fails, the new
		// one stays as .part, as the settings' .tmp does: review I6)
		unlink(to.c_str());
		if (rename(part.c_str(), to.c_str()) != 0)
		{
			why = ErrnoText(errno) + " (the copy is in " + part + ")";
			return false;
		}
	}
	if (!ok)
	{
		if (why.empty())
			why = "write error";
		unlink(part.c_str());
		return false;
	}
	struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
	utimes(to.c_str(), tv); // (best effort: a copy without it is only copied again)
	return true;
}

// a copy that is the file's: the same size, and its time the same (2 s: FAT's precision) or later
bool SameCopy(const struct stat& copy, const FontFile& f)
{
	return S_ISREG(copy.st_mode) && (long long)copy.st_size == f.size && copy.st_mtime + 2 >= f.mtime;
}

// the fonts folder's fonts to a drive's backup: those not there yet, of another size, or changed since their copy
// was made (the source newer than it). Nothing is deleted there. (The backup thread; it stops between two fonts when
// the settings are to be backed up: review I2.)
bool SettingsWaiting();
FontsResult SyncFonts(const std::string& drive)
{
	FontsResult r;
	std::lock_guard<std::mutex> fonts_lock(G().fonts_mutex);
	const std::vector<FontFile> local = FontsIn(OrbisDir("fonts"));
	if (local.empty() || !OrbisIsDir(drive))
		return r;
	const std::string dest = FontsOn(drive);
	if (!OrbisMkdirs(dest))
	{
		OrbisLog("[backup] fonts: can't create %s (%s)", dest.c_str(), ErrnoText(errno).c_str());
		r.failed = int(local.size());
		return r;
	}
	for (const FontFile& f : local)
	{
		struct stat st = {};
		const std::string to = dest + "/" + f.name;
		if (stat(to.c_str(), &st) == 0 && SameCopy(st, f))
		{
			r.same++;
			continue;
		}
		if (SettingsWaiting())
		{
			OrbisLog("[backup] fonts: the settings first (the rest of the fonts after them)");
			break;
		}
		if (f.size > kMaxFontBytes)
		{
			OrbisLog("[backup] fonts: %s left out (%lld MB)", f.name.c_str(), f.size >> 20);
			r.failed++;
			continue;
		}
		std::string why;
		if (CopyFile(OrbisDir("fonts") + "/" + f.name, to, f.mtime, why))
			r.copied++;
		else
		{
			OrbisLog("[backup] fonts: %s not copied to %s (%s)", f.name.c_str(), dest.c_str(), why.c_str());
			r.failed++;
		}
	}
	if (r.copied || r.failed)
		OrbisLog("[backup] fonts: %d copied to %s, %d there already, %d not copied", r.copied, dest.c_str(), r.same,
			r.failed);
	return r;
}

void Thread()
{
	Shared& g = G();
	std::unique_lock<std::mutex> lock(g.job_mutex);
	for (;;)
	{
		g.job_cv.wait(lock, [&g] { return g.job_pending || g.fonts_pending; });
		const bool ini = g.job_pending;
		const std::string drive = g.job_drive, id = g.job_id, text = g.job_text;
		std::string fonts = g.fonts_pending ? g.fonts_drive : "";
		g.job_pending = g.fonts_pending = false;
		g.busy = true;
		lock.unlock();
		if (ini)
		{
			std::lock_guard<std::mutex> write(g.write_mutex);
			const Status s = Write(drive, id, text, false);
			// 1.7.1 (#12): the fonts follow a backup that is this installation's
			if (s.result == Result::Written || s.result == Result::Unchanged)
				fonts = drive;
		}
		if (!fonts.empty())
			SyncFonts(fonts); // (its own lock: review I2)
		lock.lock();
		g.busy = false;
		g.job_cv.notify_all();
	}
}

bool SettingsWaiting()
{
	std::lock_guard<std::mutex> lock(G().job_mutex);
	return G().job_pending;
}

// the thread started once (false: none could be)
bool StartThread(Shared& g)
{
	if (!g.thread_started && !g.thread_failed)
	{
		try
		{
			std::thread(Thread).detach(); // waits on job_cv for the rest of the process
			g.thread_started = true;
		}
		catch (...)
		{
			g.thread_failed = true;
			OrbisLog("[backup] no backup thread: the backups are written as the settings are saved");
		}
	}
	return g.thread_started;
}
} // namespace

std::string ErrorText(const Status& s, bool translated)
{
	auto tr = [translated](const char* english) { return translated ? T(english) : english; };
	switch (s.error)
	{
	case Error::None: return "";
	case Error::NoDrive: return tr("no drive is chosen");
	case Error::NotConnected: return Fmt(tr("%s isn't connected"), s.path.c_str());
	case Error::CantRead: return tr("the settings file can't be read");
	case Error::CantCreate: return Fmt(tr("can't create %s (%s)"), s.path.c_str(), s.reason.c_str());
	case Error::CantWrite: return Fmt(tr("can't write %s (%s)"), s.path.c_str(), s.reason.c_str());
	case Error::CantReplace: return Fmt(tr("can't replace %s (%s)"), s.path.c_str(), s.reason.c_str());
	case Error::CantReplaceKept:
		return Fmt(tr("can't replace %s (%s); the new backup is in %s"), s.path.c_str(), s.reason.c_str(), s.kept.c_str());
	}
	return "";
}

unsigned Generation()
{
	std::lock_guard<std::mutex> lock(G().status_mutex);
	return G().generation;
}

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

std::string FontsOn(const std::string& drive)
{
	return FolderOn(drive) + "/fonts";
}

FontsResult RestoreFonts(const std::string& drive)
{
	FontsResult r;
	const std::vector<FontFile> saved = FontsIn(FontsOn(drive));
	if (saved.empty())
		return r;
	const std::string dir = OrbisDir("fonts");
	if (!OrbisMkdirs(dir))
	{
		OrbisLog("[backup] fonts: can't create %s (%s)", dir.c_str(), ErrnoText(errno).c_str());
		r.failed = int(saved.size());
		return r;
	}
	std::lock_guard<std::mutex> fonts_lock(G().fonts_mutex); // (not while the thread copies to the same drive)
	for (const FontFile& f : saved)
	{
		// (review I5) a font of that name already in the fonts folder stays as it is: only the missing ones come back
		struct stat st = {};
		const std::string to = dir + "/" + f.name;
		if (lstat(to.c_str(), &st) == 0)
		{
			r.same++;
			continue;
		}
		std::string why;
		if (f.size <= kMaxFontBytes && CopyFile(FontsOn(drive) + "/" + f.name, to, f.mtime, why))
			r.copied++;
		else
		{
			OrbisLog("[backup] fonts: %s not copied back (%s)", f.name.c_str(), why.empty() ? "too big" : why.c_str());
			r.failed++;
		}
	}
	OrbisLog("[backup] fonts: %d copied back from %s, %d there already, %d not copied", r.copied, FontsOn(drive).c_str(),
		r.same, r.failed);
	return r;
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
	Status s;
	{
		std::lock_guard<std::mutex> write(G().write_mutex);
		s = Write(drive, id, text, force);
	}
	error = ErrorText(s, true);
	// 1.7.1 (#12): the fonts, by the backup thread (the screen doesn't wait for them)
	if (s.result == Result::Written || s.result == Result::Unchanged)
	{
		Shared& g = G();
		std::unique_lock<std::mutex> lock(g.job_mutex);
		if (StartThread(g))
		{
			g.fonts_drive = drive;
			g.fonts_pending = true;
			g.job_cv.notify_all();
		}
		else
		{
			lock.unlock();
			SyncFonts(drive);
		}
	}
	return s.result;
}

void AfterSave(const Settings& s)
{
	if (s.backup_drive.empty())
		return;
	std::string text;
	if (!ReadAll(SettingsPath(), text))
		text.clear(); // the thread says why
	Shared& g = G();
	std::unique_lock<std::mutex> lock(g.job_mutex);
	if (!StartThread(g))
	{
		// no thread: written here (the screen waits for the drive)
		lock.unlock();
		Status st;
		{
			std::lock_guard<std::mutex> write(g.write_mutex);
			st = Write(s.backup_drive, s.backup_id, text, false);
		}
		if (st.result == Result::Written || st.result == Result::Unchanged)
			SyncFonts(s.backup_drive);
		return;
	}
	g.job_drive = s.backup_drive;
	g.job_id = s.backup_id;
	g.job_text = std::move(text);
	g.job_pending = true;
	g.job_cv.notify_all();
}

void Flush(int ms)
{
	Shared& g = G();
	std::unique_lock<std::mutex> lock(g.job_mutex);
	if (!g.job_cv.wait_for(lock, std::chrono::milliseconds(ms), [&g] { return !g.job_pending && !g.fonts_pending && !g.busy; }))
		OrbisLog("[backup] the backup thread didn't finish within %d ms", ms);
}

Status LastStatus()
{
	std::lock_guard<std::mutex> lock(G().status_mutex);
	return G().status;
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
