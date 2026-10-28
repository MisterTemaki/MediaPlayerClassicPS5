// Media Player Classic PS5: installs the dashboard app from files built into the payload (ProsperoInstall.h).
//
// Like PS5SX2's installer: a file is only rewritten when its bytes differ, each one is written to
// <name>.part, flushed and then renamed over the old one (a power cut leaves either the old or the new file),
// and eboot.bin goes last, so a half-done update never has a new eboot with old metadata. Nothing outside
// the app folder is touched.
//
// SPDX-License-Identifier: MIT

#include "ProsperoInstall.h"

#include <zlib.h>

#include "OrbisPaths.h"
#include "ProsperoAppPlace.h"
#include "ProsperoUpdate.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <vector>

// Overridden by install_data.cpp in the build that carries the app.
__attribute__((weak)) const EmbeddedAppFile* EmbeddedAppFiles()
{
	return nullptr;
}

namespace
{
bool SameContent(const std::string& path, const unsigned char* data, size_t size)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || size_t(st.st_size) != size)
		return false;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<unsigned char> buf(1 << 16);
	size_t off = 0;
	bool same = true;
	while (same && off < size)
	{
		const size_t n = fread(buf.data(), 1, buf.size(), f);
		if (n == 0)
			break;
		same = memcmp(buf.data(), data + off, n) == 0;
		off += n;
	}
	fclose(f);
	return same && off == size;
}

bool WriteAtomically(const std::string& path, const unsigned char* data, size_t size)
{
	const std::string part = path + ".part";
	const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
	{
		OrbisLog("[install] can't create %s", part.c_str());
		return false;
	}
	size_t off = 0;
	while (off < size)
	{
		const ssize_t n = write(fd, data + off, size - off);
		if (n <= 0)
		{
			close(fd);
			unlink(part.c_str());
			OrbisLog("[install] write failed: %s (disk full?)", part.c_str());
			return false;
		}
		off += size_t(n);
	}
	const bool synced = fsync(fd) == 0;
	const bool closed = close(fd) == 0;
	if (!synced || !closed || rename(part.c_str(), path.c_str()) != 0)
	{
		unlink(part.c_str());
		OrbisLog("[install] can't finish %s", part.c_str());
		return false;
	}
	return true;
}
} // namespace

const AppPlace& AppInstallPlace()
{
	static const AppPlace place = [] {
		const char* env = getenv("MPCPS5_PS5_HOMEBREW");
		const AppPlace p = FindAppPlace(MPCPS5_TITLE_ID, std::string(env && *env ? env : "/data/homebrew") + "/" MPCPS5_TITLE_ID);
		OrbisLog("[install] %s", p.log.c_str());
		return p;
	}();
	return place;
}

std::string AppInstallDir()
{
	return AppInstallPlace().dir;
}

namespace
{
std::string ContentVersion(const std::string& param)
{
	const size_t k = param.find("\"contentVersion\"");
	if (k == std::string::npos)
		return "";
	const size_t a = param.find('"', param.find(':', k) + 1);
	const size_t b = a == std::string::npos ? a : param.find('"', a + 1);
	return b == std::string::npos ? "" : param.substr(a + 1, b - a - 1);
}
} // namespace

std::string InstalledVersion()
{
	// the app the console runs: through the mount when there is one, else its folder
	const AppPlace& place = AppInstallPlace();
	const std::string dir = !place.mount.empty() ? place.mount : place.source;
	FILE* f = fopen((dir + "/sce_sys/param.json").c_str(), "rb");
	if (!f)
		return "";
	std::string s(65536, '\0');
	s.resize(fread(&s[0], 1, s.size(), f));
	fclose(f);
	return ContentVersion(s);
}

// After a repair: the half-done update's journal, its old copies (<name>.old.<transaction>) and new files
// (<name>.new) in the app folder and its subfolders. The journal goes last.
void RemoveUpdateLeftovers(const std::string& dir)
{
	std::vector<std::string> dirs = {dir};
	for (size_t i = 0; i < dirs.size(); i++)
	{
		DIR* d = opendir(dirs[i].c_str());
		if (!d)
			continue;
		while (dirent* e = readdir(d))
		{
			const std::string name = e->d_name;
			if (name == "." || name == "..")
				continue;
			const std::string path = dirs[i] + "/" + name;
			struct stat st;
			if (lstat(path.c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode))
				dirs.push_back(path);
			else if (name.find(".old.") != std::string::npos ||
					 (name.size() > 4 && name.compare(name.size() - 4, 4, ".new") == 0) || name == "update.journal.tmp")
			{
				unlink(path.c_str());
				OrbisLog("[install] removed %s (left by the unfinished update)", path.c_str());
			}
		}
		closedir(d);
	}
	unlink((dir + "/update.journal").c_str());
	OrbisLog("[install] the unfinished update's journal removed: the app is this payload's version again");
}

InstallResult InstallApp()
{
	const EmbeddedAppFile* files = EmbeddedAppFiles();
	if (!files)
		return InstallResult::NothingEmbedded;

	const AppPlace& place = AppInstallPlace();
	if (!place.writable && place.image_pending)
	{
		// autoloaded before ShadowMount+ has mounted the image the app runs from: nothing to write, nothing to say
		OrbisLog("[install] %s: left to the image", place.log.c_str());
		return InstallResult::UpToDate;
	}
	if (!place.writable)
	{
		// an autoloaded payload and an image (or a read-only folder) already holding this version or a newer one:
		// nothing to say at every start; only a newer payload is something the user can't put there
		std::string mine;
		for (const EmbeddedAppFile* f = files; f->rel; f++)
		{
			std::string param;
			if (strcmp(f->rel, "sce_sys/param.json") == 0 && EmbeddedContents(f, param))
				mine = ContentVersion(param);
		}
		const std::string there = place.unsure ? "" : InstalledVersion();
		if (!mine.empty() && !there.empty())
		{
			const int c = update::CompareVersions(there, mine);
			if (c > 0)
			{
				OrbisLog("[install] %s holds version %s, newer than this payload's %s: left as it is (%s)",
					place.source.c_str(), there.c_str(), mine.c_str(), place.log.c_str());
				return InstallResult::NewerInstalled;
			}
			if (c == 0)
			{
				OrbisLog("[install] %s holds this version (%s): nothing to write (%s)", place.source.c_str(),
					there.c_str(), place.log.c_str());
				return InstallResult::UpToDate;
			}
		}
		OrbisLog("[install] not installed: %s", place.reason.c_str());
		return InstallResult::CantWrite;
	}
	if (!place.mount.empty())
		OrbisLog("[install] the console runs the app from %s: that copy is the one brought up to date (through %s)",
			place.source.c_str(), place.dir.c_str());
	const std::string dir = AppInstallDir();
	// an in-app update left half done (its journal still there, see ProsperoUpdate.cpp): whatever version the files
	// say, the payload repairs the app: every file written again from its own copy, the update's leftovers removed
	const bool repair = OrbisIsFile(dir + "/update.journal");
	if (repair)
		OrbisLog("[install] %s/update.journal: an update wasn't finished; repairing the app from this payload", dir.c_str());
	// a newer version installed (by the app's own update): this older payload doesn't put itself over it
	for (const EmbeddedAppFile* f = files; f->rel && !repair; f++)
	{
		std::string param;
		if (strcmp(f->rel, "sce_sys/param.json") == 0 && EmbeddedContents(f, param))
		{
			const std::string mine = ContentVersion(param), there = InstalledVersion();
			if (!mine.empty() && !there.empty() && update::CompareVersions(there, mine) > 0)
			{
				OrbisLog("[install] %s holds version %s, newer than this payload's %s: left as it is", dir.c_str(),
					there.c_str(), mine.c_str());
				return InstallResult::NewerInstalled;
			}
		}
	}
	const bool existed = OrbisIsFile(dir + "/eboot.bin");
	if (!OrbisMkdirs(dir + "/sce_sys"))
	{
		OrbisLog("[install] can't create %s", dir.c_str());
		return InstallResult::Failed;
	}

	// eboot.bin last
	std::vector<const EmbeddedAppFile*> order;
	const EmbeddedAppFile* eboot = nullptr;
	for (const EmbeddedAppFile* f = files; f->rel; f++)
	{
		if (strcmp(f->rel, "eboot.bin") == 0)
			eboot = f;
		else
			order.push_back(f);
	}
	if (eboot)
		order.push_back(eboot);

	int written = 0;
	for (const EmbeddedAppFile* f : order)
	{
		const std::string path = dir + "/" + f->rel;
		std::string bytes;
		if (!EmbeddedContents(f, bytes))
		{
			OrbisLog("[install] %s doesn't inflate", f->rel);
			return InstallResult::Failed;
		}
		const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
		const size_t size = bytes.size();
		if (SameContent(path, data, size))
			continue;
		const size_t slash = path.rfind('/');
		if (slash != std::string::npos && !OrbisMkdirs(path.substr(0, slash)))
		{
			OrbisLog("[install] can't create the folder of %s", path.c_str());
			return InstallResult::Failed;
		}
		if (!WriteAtomically(path, data, size))
			return InstallResult::Failed;
		OrbisLog("[install] wrote %s (%zu bytes)", path.c_str(), size);
		written++;
	}
	if (repair)
	{
		RemoveUpdateLeftovers(dir);
		return InstallResult::Updated;
	}
	if (written == 0)
	{
		OrbisLog("[install] %s is up to date", dir.c_str());
		return InstallResult::UpToDate;
	}
	return existed ? InstallResult::Updated : InstallResult::Installed;
}

bool EmbeddedContents(const EmbeddedAppFile* f, std::string& out)
{
	const size_t n = size_t(f->end - f->begin);
	if (f->raw_size == 0)
	{
		out.assign(reinterpret_cast<const char*>(f->begin), n);
		return true;
	}
	out.resize(f->raw_size);
	uLongf len = uLongf(f->raw_size);
	const int r = uncompress(reinterpret_cast<Bytef*>(&out[0]), &len, f->begin, uLong(n));
	return r == Z_OK && len == f->raw_size;
}

std::string AppMetaDir()
{
	const char* env = getenv("MPCPS5_PS5_APPMETA");
	return std::string(env && *env ? env : "/user/appmeta") + "/" MPCPS5_TITLE_ID;
}

int SyncAppMeta()
{
	// ShadowMountPlus copies sce_sys's art (icon0.png, pic0/pic1, param.json) to /user/appmeta/<title> only when
	// it first registers a title (or when that folder is gone); the home screen reads it from there. So an update
	// of the art never reached the screen (1.8: the new pic0.dds stayed in sce_sys). Keep our own title's copy
	// current. Nothing else in /user/appmeta is touched.
	const std::string dir = AppMetaDir();
	if (!OrbisIsDir(dir))
	{
		OrbisLog("[appmeta] %s not there yet (ShadowMountPlus registers the title first)", dir.c_str());
		return 0;
	}
	const EmbeddedAppFile* files = EmbeddedAppFiles();
	if (!files)
		return 0;
	int changed = 0;
	for (const EmbeddedAppFile* f = files; f->rel; f++)
	{
		if (strncmp(f->rel, "sce_sys/", 8) != 0)
			continue;
		const std::string name = f->rel + 8;
		const std::string path = dir + "/" + name;
		std::string bytes;
		if (!EmbeddedContents(f, bytes))
			continue;
		const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
		const size_t size = bytes.size();
		if (SameContent(path, data, size))
			continue;
		if (!WriteAtomically(path, data, size))
		{
			OrbisLog("[appmeta] can't write %s", path.c_str());
			continue;
		}
		OrbisLog("[appmeta] wrote %s (%zu bytes)", path.c_str(), size);
		changed++;
	}
	// The PNG backgrounds of 1.0-1.7 would hide the DDS ones: remove them (ours, in our title's folders only).
	for (const char* old : {"pic0.png", "pic1.png"})
		for (const std::string& d : {dir, AppInstallDir() + "/sce_sys"})
		{
			if (d != dir && !AppInstallPlace().writable)
				continue; // the app's folder can't be written (an image...): only our /user/appmeta copy
			const std::string path = d + "/" + old;
			if (OrbisIsFile(path) && unlink(path.c_str()) == 0)
			{
				OrbisLog("[appmeta] removed the old %s", path.c_str());
				changed++;
			}
		}
	OrbisLog("[appmeta] %s: %d change(s)", dir.c_str(), changed);
	return changed;
}
