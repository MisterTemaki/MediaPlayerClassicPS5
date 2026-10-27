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
// statfs(2)'s f_mntonname / f_mntfromname hold MNAMELEN (88) bytes with the NUL: a longer name comes back cut
constexpr size_t kMountName = 88;

struct MountInfo
{
	bool ok = false;
	std::string on, type, from;
	bool ro = false;
	bool from_cut = false; // f_mntfromname was full: the source's name may be cut short
};

bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool Inside(const std::string& path, const std::string& on)
{
	return on == "/" || path == on || (path.size() > on.size() && path.compare(0, on.size(), on) == 0 && path[on.size()] == '/');
}

// the longest mount point a path is in; of the mounts on one point the last one (the top)
MountInfo Lookup(const std::vector<MountInfo>& table, const std::string& path)
{
	MountInfo best;
	for (const MountInfo& m : table)
		if (Inside(path, m.on) && (!best.ok || m.on.size() >= best.on.size()))
			best = m;
	return best;
}

#if defined(__PROSPERO__) || defined(__FreeBSD__)
MountInfo FromStatfs(const struct statfs& sf)
{
	MountInfo m;
	m.ok = true;
	m.on.assign(sf.f_mntonname, strnlen(sf.f_mntonname, sizeof(sf.f_mntonname)));
	m.type.assign(sf.f_fstypename, strnlen(sf.f_fstypename, sizeof(sf.f_fstypename)));
	m.from.assign(sf.f_mntfromname, strnlen(sf.f_mntfromname, sizeof(sf.f_mntfromname)));
	m.from_cut = m.from.size() + 1 >= kMountName;
	m.ro = (sf.f_flags & MNT_RDONLY) != 0;
	return m;
}

bool StatfsOf(const std::string& path, MountInfo& m)
{
	struct statfs sf;
	memset(&sf, 0, sizeof(sf));
	if (statfs(path.c_str(), &sf) != 0)
		return false;
	m = FromStatfs(sf);
	return true;
}

// every mount, in the order they were made (getfsstat(2), as getmntinfo(3) does)
bool MountTable(std::vector<MountInfo>& out)
{
	for (int tries = 0; tries < 3; tries++)
	{
		const int n = getfsstat(nullptr, 0, MNT_NOWAIT);
		if (n < 0)
			return false;
		std::vector<struct statfs> v(size_t(n) + 8);
		const int got = getfsstat(v.data(), long(v.size() * sizeof(struct statfs)), MNT_NOWAIT);
		if (got < 0)
			return false;
		if (size_t(got) >= v.size())
			continue; // more mounts meanwhile
		out.clear();
		for (int i = 0; i < got; i++)
			out.push_back(FromStatfs(v[size_t(i)]));
		return true;
	}
	return false;
}
#else
// The host tests: MPCPS5_HOST_MOUNTS is the table ("<on>|<type>|<from>|ro or rw;...", in mount order, names cut at
// 87 bytes as the console's), "/" under it all; MPCPS5_HOST_NO_STATFS: statfs(2) fails; MPCPS5_HOST_NO_GETFSSTAT:
// getfsstat(2) fails
void HostTable(std::vector<MountInfo>& out)
{
	out.clear();
	MountInfo root;
	root.ok = true;
	root.on = "/";
	root.type = "host";
	out.push_back(root);
	const char* env = getenv("MPCPS5_HOST_MOUNTS");
	if (!env)
		return;
	const std::string table = env;
	size_t at = 0;
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
		MountInfo m;
		m.ok = true;
		m.on = f[0].substr(0, kMountName - 1);
		m.type = f[1];
		m.from = f[2].substr(0, kMountName - 1);
		m.from_cut = m.from.size() + 1 >= kMountName;
		m.ro = f[3] == "ro";
		out.push_back(m);
	}
}

bool MountTable(std::vector<MountInfo>& out)
{
	if (getenv("MPCPS5_HOST_NO_GETFSSTAT"))
		return false;
	HostTable(out);
	return true;
}

bool StatfsOf(const std::string& path, MountInfo& m)
{
	if (getenv("MPCPS5_HOST_NO_STATFS") || !Exists(path))
		return false;
	std::vector<MountInfo> table;
	HostTable(table);
	m = Lookup(table, path);
	return m.ok;
}
#endif

// the mount a path is on: statfs(2), else the mount table
MountInfo MountOf(const std::string& path)
{
	MountInfo m;
	if (StatfsOf(path, m))
		return m;
	std::vector<MountInfo> table;
	if (Exists(path) && MountTable(table))
		return Lookup(table, path);
	return MountInfo();
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

// the folder's sce_sys/param.json says it is this app ("titleId": "<TITLE_ID>")
bool NamesTitle(const std::string& dir, const std::string& title_id)
{
	FILE* f = fopen((dir + "/sce_sys/param.json").c_str(), "rb");
	if (!f)
		return false;
	std::string s(65536, '\0');
	s.resize(fread(&s[0], 1, s.size(), f));
	fclose(f);
	const size_t k = s.find("\"titleId\"");
	if (k == std::string::npos)
		return false;
	const size_t colon = s.find(':', k);
	const size_t a = colon == std::string::npos ? colon : s.find('"', colon + 1);
	const size_t b = a == std::string::npos ? a : s.find('"', a + 1);
	return b != std::string::npos && s.compare(a + 1, b - a - 1, title_id) == 0;
}

// the source's eboot.bin is the one the mount shows (nullfs passes the inode number through)
bool SameEboot(const std::string& source, const std::string& mount)
{
	struct stat a, b;
	if (stat((mount + "/eboot.bin").c_str(), &b) != 0)
		return true; // nothing to compare with
	return stat((source + "/eboot.bin").c_str(), &a) == 0 && a.st_ino == b.st_ino && a.st_size == b.st_size;
}

// ShadowMount+'s images (.ffpkg ufs, .exfat, .ffpfs pfs) are mounted under /mnt/shadowmnt. Anything else is judged by
// its read-only flag and whether a file can be written (extended storage may be on an LVD device and writable)
bool IsImage(const MountInfo& u, const std::string& source)
{
	const char* env = getenv("MPCPS5_PS5_SHADOWMNT"); // the host tests' stand-in
	const std::string images = env && *env ? env : "/mnt/shadowmnt";
	return Inside(u.on, images) || Inside(source, images);
}

std::string Line(const char* fmt, const std::string& a, const std::string& b = "", const std::string& c = "")
{
	char line[1024];
	snprintf(line, sizeof(line), fmt, a.c_str(), b.c_str(), c.c_str());
	return line;
}

void Unsure(AppPlace& a, const std::string& what, const std::string& log)
{
	a.writable = false;
	a.unsure = true;
	a.reason = "Media Player Classic PS5: " + what + " Nothing is written (no second copy is made either).";
	a.log += log;
}

// a.source found: what it is on, and where its files can be written (candidates: through the mount first)
void Finish(AppPlace& a, const std::string& title_id, bool mount_ro, const std::vector<std::string>& candidates)
{
	// the file system the source is on (through nullfs layers, if any)
	MountInfo u = MountOf(a.source);
	std::string at = a.source;
	for (int depth = 0; u.ok && u.type == "nullfs" && u.on == at && !u.from.empty() && !u.from_cut && depth < 4; depth++)
	{
		at = u.from;
		u = MountOf(at);
	}
	a.fs = u.ok ? u.type : "";
	a.image = IsImage(u, a.source);
	a.log += Line(" (on %s, %s, from %s", u.on, a.fs, u.from) + (mount_ro || u.ro ? ", read-only)" : ")");
	a.dir = a.source;
	if (a.image)
	{
		a.writable = false;
		a.reason = "Media Player Classic PS5 runs from an image that ShadowMount+ mounted (" + a.source +
				   "): an update can't be written into it. Update the image the way you made it, with the new version's "
				   "folder.";
		a.log += ": an image, not written";
		return;
	}
	if (mount_ro || u.ro)
	{
		a.writable = false;
		a.reason = "Media Player Classic PS5 runs from " + a.source +
				   ", which is read-only: an update can't be written there. Update that folder the way you put it there.";
		a.log += ": read-only, not written";
		return;
	}
	std::string tried;
	for (const std::string& d : candidates)
	{
		tried += (tried.empty() ? "" : " nor ") + d;
		if (NamesTitle(d, title_id) && CanWrite(d))
		{
			a.dir = d;
			a.log += ": files go through " + d;
			return;
		}
	}
	a.writable = false;
	a.reason = "Media Player Classic PS5 runs from " + a.source + ", where it can't write a file: an update can't be "
			   "installed there. Update that folder the way you put it there.";
	a.log += ": neither " + tried + " takes a file";
}

// No mount on /system_ex/app/<TITLE_ID> (yet: a payload autoloaded before ShadowMount+ has mounted the app): the
// folder ShadowMount+ last mounted there, from its link /user/app/<TITLE_ID>/mount.lnk
void NoMount(AppPlace& a, const std::string& title_id, const std::string& default_dir, const std::string& mount)
{
	const char* ua = getenv("MPCPS5_PS5_USER_APP");
	const std::string lnk = std::string(ua && *ua ? ua : "/user/app") + "/" + title_id + "/mount.lnk";
	std::string target;
	if (FILE* f = fopen(lnk.c_str(), "r"))
	{
		char buf[1024] = "";
		if (fgets(buf, sizeof(buf), f))
		{
			buf[strcspn(buf, "\r\n")] = 0;
			target = buf;
		}
		fclose(f);
	}
	while (target.size() > 1 && target.back() == '/')
		target.pop_back();
	a.log = mount + " isn't a mount";
	if (target.empty() || target == default_dir)
	{
		a.log += target.empty() ? " and ShadowMount+ has no link for the app: files go to " + default_dir
								: " (ShadowMount+'s link: " + target + "): files go to " + default_dir;
		return;
	}
	if (!Exists(target))
	{
		a.source = a.dir = target;
		Unsure(a,
			"ShadowMount+ runs the app from " + target +
				", which isn't there now (a drive not connected yet?). Send the payload again once it is.",
			" and ShadowMount+'s link names " + target + ", which isn't there: not written");
		return;
	}
	if (!NamesTitle(target, title_id))
	{
		a.log += " and ShadowMount+'s link names " + target + ", which doesn't hold " + title_id + ": files go to " +
				 default_dir;
		return;
	}
	a.source = target;
	a.via_link = true;
	a.log += "; ShadowMount+ runs the app from " + target + " (" + lnk + ")";
	Finish(a, title_id, false, {target});
}
} // namespace

AppPlace FindAppPlace(const std::string& title_id, const std::string& default_dir)
{
	AppPlace a;
	a.source = a.dir = default_dir;
	const char* sx = getenv("MPCPS5_PS5_SYSTEM_EX");
	const std::string mount = std::string(sx && *sx ? sx : "/system_ex") + "/app/" + title_id;
	std::vector<MountInfo> table;
	const bool have_table = MountTable(table);
	MountInfo top;
	if (!StatfsOf(mount, top) && Exists(mount))
	{
		// statfs(2) failed: the mount table says what is there
		if (!have_table)
		{
			a.log = mount + ": ";
			Unsure(a, "what is mounted on " + mount + " can't be read, so the folder the console runs isn't known.",
				"neither statfs nor getfsstat answers: not written");
			return a;
		}
		top = Lookup(table, mount);
	}
	if (!top.ok || top.on != mount)
	{
		NoMount(a, title_id, default_dir, mount);
		return a;
	}
	a.mount = mount;
	// ShadowMount+ mounts the app's folder with nullfs. Something on top of it (a backport's unionfs) isn't
	// written into: the nullfs layer beneath gives the folder, written directly
	const MountInfo* layer = nullptr;
	bool through_mount = false;
	if (top.type == "nullfs")
	{
		layer = &top;
		through_mount = true;
	}
	else
		for (auto it = table.rbegin(); it != table.rend(); ++it)
			if (it->on == mount && it->type == "nullfs")
			{
				layer = &*it;
				break;
			}
	a.log = mount + " is a " + top.type + " mount";
	if (!layer)
	{
		Unsure(a, mount + " is a " + top.type + " mount, not ShadowMount+'s folder mount: the folder the console runs isn't known.",
			", no nullfs mount of a folder under it: not written");
		return a;
	}
	if (!through_mount)
		a.log += " over a nullfs mount";
	// the folder: named by the mount (in full) and the app's own, with the eboot the console sees
	const std::string from = layer->from;
	const bool known = !layer->from_cut && !from.empty() && NamesTitle(from, title_id) &&
					   (!through_mount || SameEboot(from, mount));
	if (known)
	{
		a.source = from;
		a.log += " of " + from;
		std::vector<std::string> cand;
		if (through_mount)
			cand.push_back(mount);
		cand.push_back(from);
		Finish(a, title_id, through_mount && top.ro, cand);
		return a;
	}
	const std::string why = layer->from_cut ? " (its name cut short by the system: \"" + from + "\")"
											: " of " + from + ", which isn't the folder it shows";
	if (through_mount && NamesTitle(mount, title_id))
	{
		// only through the mount, which is the app whatever its folder's name
		a.source = mount;
		a.log += why + "; written only through the mount";
		Finish(a, title_id, top.ro, {mount});
		return a;
	}
	Unsure(a, "the folder mounted on " + mount + " can't be identified for sure.", why + ": not written");
	return a;
}
