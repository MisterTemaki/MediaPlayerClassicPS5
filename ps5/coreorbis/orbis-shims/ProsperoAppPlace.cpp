// Media Player Classic PS5 1.5: where the app the console runs really is (ProsperoAppPlace.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoAppPlace.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__PROSPERO__) || defined(__FreeBSD__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
struct MountInfo
{
	bool ok = false;
	std::string on, type, from;
	bool ro = false;
};

bool StartsWith(const std::string& s, const char* p)
{
	return s.compare(0, strlen(p), p) == 0;
}

// the mount a path is on: statfs(2) on the console; on the host the tests' table (MPCPS5_HOST_MOUNTS), else "/"
MountInfo MountOf(const std::string& path)
{
	MountInfo m;
#if defined(__PROSPERO__) || defined(__FreeBSD__)
	struct statfs sf;
	memset(&sf, 0, sizeof(sf));
	if (statfs(path.c_str(), &sf) != 0)
		return m;
	m.ok = true;
	m.on = sf.f_mntonname;
	m.type = sf.f_fstypename;
	m.from = sf.f_mntfromname;
	m.ro = (sf.f_flags & MNT_RDONLY) != 0;
#else
	const char* env = getenv("MPCPS5_HOST_MOUNTS");
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return m;
	m.ok = true;
	m.on = "/";
	m.type = "host";
	if (!env)
		return m;
	const std::string table = env;
	size_t at = 0;
	size_t best = 0;
	while (at < table.size())
	{
		size_t end = table.find(';', at);
		if (end == std::string::npos)
			end = table.size();
		const std::string row = table.substr(at, end - at);
		at = end + 1;
		std::vector<std::string> f;
		size_t p = 0;
		for (;;)
		{
			const size_t bar = row.find('|', p);
			f.push_back(row.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
			if (bar == std::string::npos)
				break;
			p = bar + 1;
		}
		if (f.size() < 4 || f[0].empty())
			continue;
		const std::string& on = f[0];
		const bool inside = path == on || (path.size() > on.size() && path.compare(0, on.size(), on) == 0 && path[on.size()] == '/');
		if (inside && on.size() > best)
		{
			best = on.size();
			m.on = on;
			m.type = f[1];
			m.from = f[2];
			m.ro = f[3] == "ro";
		}
	}
#endif
	return m;
}

// a file can be made (and removed) there
bool CanWrite(const std::string& dir)
{
	const std::string probe = dir + "/.mpcps5-write-test";
	const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	const bool wrote = write(fd, "x", 1) == 1;
	close(fd);
	unlink(probe.c_str());
	return wrote;
}

// ShadowMount+'s images (.ffpkg ufs, .exfat, .ffpfs pfs) are mounted under /mnt/shadowmnt from LVD / MD devices
bool IsImage(const MountInfo& u, const std::string& source)
{
	return StartsWith(u.on, "/mnt/shadowmnt") || StartsWith(source, "/mnt/shadowmnt") || StartsWith(u.from, "/dev/lvd") ||
		   StartsWith(u.from, "/dev/md") || u.type == "pfs";
}
} // namespace

AppPlace FindAppPlace(const std::string& title_id, const std::string& default_dir)
{
	AppPlace a;
	a.source = a.dir = default_dir;
	const char* sx = getenv("MPCPS5_PS5_SYSTEM_EX");
	const std::string mount = std::string(sx && *sx ? sx : "/system_ex") + "/app/" + title_id;
	const MountInfo m = MountOf(mount);
	if (!m.ok || m.on != mount)
	{
		a.log = mount + " isn't a mount (ShadowMount+ hasn't registered the app, or it runs another way): files go to " +
				default_dir;
		return a;
	}
	a.mount = mount;
	a.source = m.type == "nullfs" && !m.from.empty() ? m.from : mount;
	// the file system the source is on (through nullfs layers, if any)
	MountInfo u = m.type == "nullfs" ? MountOf(a.source) : m;
	std::string at = a.source;
	for (int depth = 0; u.ok && u.type == "nullfs" && u.on == at && !u.from.empty() && depth < 4; depth++)
	{
		at = u.from;
		u = MountOf(at);
	}
	a.fs = u.ok ? u.type : "";
	a.image = IsImage(u, a.source);
	char line[512];
	snprintf(line, sizeof(line), "%s is a %s mount of %s (on %s, %s, from %s%s)", mount.c_str(), m.type.c_str(),
		a.source.c_str(), u.on.c_str(), a.fs.c_str(), u.from.c_str(), m.ro || u.ro ? ", read-only" : "");
	a.log = line;
	if (a.image)
	{
		a.writable = false;
		a.reason = "Media Player Classic PS5 runs from an image that ShadowMount+ mounted (" + a.source +
				   "): an update can't be written into it. Update the image the way you made it, with the new version's "
				   "folder.";
		a.log += ": an image, not written";
		return a;
	}
	if (m.ro || u.ro)
	{
		a.writable = false;
		a.reason = "Media Player Classic PS5 runs from " + a.source +
				   ", which is read-only: an update can't be written there. Update that folder the way you put it there.";
		a.log += ": read-only, not written";
		return a;
	}
	// through the mount first (what the console runs sees the new files at once), else the folder itself
	for (const std::string& d : {a.mount, a.source})
		if (CanWrite(d))
		{
			a.dir = d;
			a.log += ": files go through " + d;
			return a;
		}
	a.writable = false;
	a.reason = "Media Player Classic PS5 runs from " + a.source + ", where it can't write a file: an update can't be "
			   "installed there. Update that folder the way you put it there.";
	a.log += ": neither " + a.mount + " nor " + a.source + " takes a file";
	return a;
}
