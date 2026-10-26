// Media Player Classic PS5: MPC-HC's dialog boxes (mpc_dialogs.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_dialogs.h"

#include "mpc_disc.h"
#include "mpc_i18n.h"
#include "mpc_player.h"
#include "mpc_settings.h"
#include "mpc_smb.h"
#include "mpc_ui.h"

#include "OrbisPaths.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#ifndef MPCPS5_PS5_VERSION
#define MPCPS5_PS5_VERSION "dev"
#endif

namespace mpc
{
// ===========================================================================================================
// files
namespace
{
std::string LowerExt(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	const size_t slash = name.find_last_of('/');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return "";
	std::string e = name.substr(dot + 1);
	for (char& c : e)
		c = char(tolower(static_cast<unsigned char>(c)));
	return e;
}

bool InList(const std::string& ext, const char* const* list)
{
	for (; *list; list++)
		if (ext == *list)
			return true;
	return false;
}

// CMediaFormats' extensions (video, audio, and the containers LAV Splitter opens)
const char* const kMedia[] = {"avi", "mkv", "mk3d", "webm", "mp4", "m4v", "mov", "qt", "3gp", "3g2", "wmv", "asf", "flv", "f4v",
	"ts", "tp", "trp", "m2ts", "mts", "m2t", "mpg", "mpeg", "mpe", "m1v", "m2v", "vob", "evo", "ogv", "ogm", "rm", "rmvb",
	"divx", "dv", "mxf", "nut", "y4m", "ivf", "hevc", "h264", "h265", "264", "265", "av1", "obu", "amv", "bik", "smk",
	"mp3", "mp2", "mpa", "m4a", "m4b", "aac", "ac3", "eac3", "ec3", "dts", "dtshd", "thd", "truehd", "mlp", "flac", "wav",
	"w64", "aif", "aiff", "ogg", "oga", "opus", "spx", "wma", "mka", "ape", "wv", "tta", "mpc", "alac", "caf", "au", "amr",
	"mid", "mod", "xm", "it", "s3m", "dsf", "dff", "m3u8", "m3u", "pls", "gif", nullptr};
const char* const kSubs[] = {"srt", "ass", "ssa", "sub", "vtt", "smi", "txt", "idx", "sup", nullptr};

// Explorer's order: case-insensitive, digit runs by value ("2" before "10")
bool NaturalLess(const std::string& a, const std::string& b)
{
	size_t i = 0, j = 0;
	while (i < a.size() && j < b.size())
	{
		if (isdigit(static_cast<unsigned char>(a[i])) && isdigit(static_cast<unsigned char>(b[j])))
		{
			size_t ie = i, je = j;
			while (ie < a.size() && isdigit(static_cast<unsigned char>(a[ie])))
				ie++;
			while (je < b.size() && isdigit(static_cast<unsigned char>(b[je])))
				je++;
			std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
			na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
			nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));
			if (na.size() != nb.size())
				return na.size() < nb.size();
			if (na != nb)
				return na < nb;
			i = ie;
			j = je;
			continue;
		}
		const int ca = tolower(static_cast<unsigned char>(a[i])), cb = tolower(static_cast<unsigned char>(b[j]));
		if (ca != cb)
			return ca < cb;
		i++;
		j++;
	}
	return a.size() - i < b.size() - j;
}

struct FsEntry
{
	std::string name;
	bool dir = false;
	uint64_t size = 0;
	int64_t mtime = 0;
	std::string target; // opens this instead (a disc's DVD-Video / Blu-ray)
};

bool ListDir(const std::string& path, std::vector<FsEntry>& out, std::string* err)
{
	if (path.compare(0, 7, "disc://") == 0)
	{
		std::vector<disc::Entry> e;
		if (!disc::List(path, e, err))
			return false;
		for (const auto& x : e)
			out.push_back({x.name, x.dir, x.size, x.mtime, ""});
		return true;
	}
	if (path.compare(0, 6, "smb://") == 0)
	{
		std::vector<smb::Entry> e;
		if (!smb::List(path, e, err))
			return false;
		for (const auto& x : e)
			out.push_back({x.name, x.dir, x.size, x.mtime, ""});
		return true;
	}
	DIR* d = opendir(path.c_str());
	if (!d)
	{
		if (err)
			*err = Fmt(T("Can't open the folder %s"), path.c_str());
		return false;
	}
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string full = path + "/" + e->d_name;
		struct stat st = {};
		if (stat(full.c_str(), &st) != 0)
			continue;
		out.push_back({e->d_name, S_ISDIR(st.st_mode), uint64_t(st.st_size), int64_t(st.st_mtime), ""});
	}
	closedir(d);
	return true;
}

void SortEntries(std::vector<FsEntry>& v)
{
	std::sort(v.begin(), v.end(), [](const FsEntry& a, const FsEntry& b) {
		if (a.dir != b.dir)
			return a.dir;
		return NaturalLess(a.name, b.name);
	});
}

std::string Join(const std::string& dir, const std::string& name)
{
	if (dir.empty())
		return name;
	return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string HideSecret(const std::string& url)
{
	const size_t at = url.find('@');
	if (url.compare(0, 6, "smb://") == 0 && at != std::string::npos)
	{
		const size_t colon = url.find(':', 6);
		if (colon != std::string::npos && colon < at)
			return url.substr(0, colon) + url.substr(at);
	}
	return url;
}
} // namespace

bool IsMediaFile(const std::string& name)
{
	return InList(LowerExt(name), kMedia);
}

bool IsSubtitleFile(const std::string& name)
{
	return InList(LowerExt(name), kSubs);
}

bool IsPlaylistFile(const std::string& name)
{
	const std::string e = LowerExt(name);
	return e == "m3u" || e == "m3u8" || e == "pls";
}

std::string FileName(const std::string& path)
{
	// a DVD-Video / Blu-ray title: the disc's name (and the title)
	if (disc::IsTitleUrl(path))
		return disc::DisplayName(path);
	std::string p = path;
	while (p.size() > 1 && p.back() == '/')
		p.pop_back();
	const size_t q = p.find('?');
	if (p.find("://") != std::string::npos && q != std::string::npos)
		p = p.substr(0, q);
	const size_t slash = p.find_last_of('/');
	std::string n = slash == std::string::npos ? p : p.substr(slash + 1);
	// %20 and friends in URLs
	std::string out;
	for (size_t i = 0; i < n.size(); i++)
		if (n[i] == '%' && i + 2 < n.size() && isxdigit(static_cast<unsigned char>(n[i + 1])) && isxdigit(static_cast<unsigned char>(n[i + 2])))
		{
			out += char(std::stoi(n.substr(i + 1, 2), nullptr, 16));
			i += 2;
		}
		else
			out += n[i];
	return out.empty() ? path : out;
}

std::string ParentFolder(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return "";
	if (slash == 0)
		return "/";
	return path.substr(0, slash);
}

std::string SizeText(uint64_t b)
{
	char s[64];
	if (b < 1024)
		snprintf(s, sizeof(s), T("%llu bytes"), (unsigned long long)b);
	else if (b < (1ull << 20))
		snprintf(s, sizeof(s), T("%llu KB"), (unsigned long long)((b + 1023) / 1024));
	else if (b < (1ull << 30))
		snprintf(s, sizeof(s), T("%.1f MB"), b / 1048576.0);
	else
		snprintf(s, sizeof(s), T("%.2f GB"), b / 1073741824.0);
	return s;
}

std::vector<std::string> ListFolderMedia(const std::string& folder)
{
	std::vector<FsEntry> e;
	std::vector<std::string> out;
	if (folder.empty() || !ListDir(folder, e, nullptr))
		return out;
	SortEntries(e);
	for (const auto& x : e)
		if (!x.dir && IsMediaFile(x.name) && !IsPlaylistFile(x.name))
			out.push_back(Join(folder, x.name));
	return out;
}

// ===========================================================================================================
// small pictures (Windows 10's shell icons, drawn at the window's scale)
namespace
{
enum class Ico
{
	Folder,
	Video,
	Audio,
	Text,
	File,
	Drive,
	Usb,
	Network,
	Server,
	Computer,
	Add,
	Disc
};

const Image* Icon(Ico which)
{
	static std::vector<Image> cache(20);
	static int made_dpi = 0;
	if (made_dpi != g_dpi)
	{
		made_dpi = g_dpi;
		for (auto& i : cache)
			i = Image();
	}
	Image& img = cache[int(which)];
	if (!img.IsNull())
		return &img;
	const int s = ScaleX(16);
	std::vector<uint8_t> rgba(size_t(s) * s * 4, 0);
	auto put = [&](double x0, double y0, double x1, double y1, uint8_t r, uint8_t g, uint8_t b) {
		for (int y = int(y0 * s); y < int(std::ceil(y1 * s)) && y < s; y++)
			for (int x = int(x0 * s); x < int(std::ceil(x1 * s)) && x < s; x++)
			{
				uint8_t* p = &rgba[(size_t(y) * s + x) * 4];
				p[0] = r;
				p[1] = g;
				p[2] = b;
				p[3] = 255;
			}
	};
	switch (which)
	{
		case Ico::Folder:
			put(0.06, 0.16, 0.44, 0.30, 220, 170, 50);
			put(0.06, 0.26, 0.94, 0.86, 245, 200, 80);
			put(0.06, 0.36, 0.94, 0.86, 252, 218, 115);
			break;
		case Ico::Video:
		case Ico::Audio:
		case Ico::Text:
		case Ico::File:
		{
			put(0.19, 0.06, 0.81, 0.94, 160, 160, 160);
			put(0.22, 0.09, 0.78, 0.91, 255, 255, 255);
			if (which == Ico::Video)
			{
				put(0.30, 0.38, 0.70, 0.70, 40, 40, 40);
				put(0.42, 0.46, 0.50, 0.62, 255, 255, 255);
				put(0.50, 0.50, 0.58, 0.58, 255, 255, 255);
			}
			else if (which == Ico::Audio)
			{
				put(0.44, 0.30, 0.52, 0.70, 0, 120, 215);
				put(0.34, 0.60, 0.52, 0.74, 0, 120, 215);
				put(0.52, 0.30, 0.64, 0.38, 0, 120, 215);
			}
			else if (which == Ico::Text)
				for (int k = 0; k < 4; k++)
					put(0.30, 0.30 + k * 0.14, 0.70, 0.36 + k * 0.14, 120, 120, 120);
			break;
		}
		case Ico::Drive:
		case Ico::Usb:
			put(0.06, 0.38, 0.94, 0.74, 120, 120, 120);
			put(0.09, 0.41, 0.91, 0.71, 210, 210, 210);
			put(0.72, 0.52, 0.84, 0.60, which == Ico::Usb ? 0 : 60, which == Ico::Usb ? 120 : 180, which == Ico::Usb ? 215 : 60);
			if (which == Ico::Usb)
				put(0.36, 0.20, 0.64, 0.38, 120, 120, 120);
			break;
		case Ico::Network:
		case Ico::Server:
			put(0.16, 0.12, 0.84, 0.62, 90, 90, 90);
			put(0.20, 0.16, 0.80, 0.58, which == Ico::Network ? 0 : 60, which == Ico::Network ? 120 : 140, which == Ico::Network ? 215 : 200);
			put(0.44, 0.62, 0.56, 0.78, 90, 90, 90);
			put(0.22, 0.78, 0.78, 0.86, 90, 90, 90);
			break;
		case Ico::Computer:
			put(0.10, 0.12, 0.90, 0.66, 70, 70, 70);
			put(0.14, 0.16, 0.86, 0.62, 30, 144, 255);
			put(0.40, 0.66, 0.60, 0.80, 70, 70, 70);
			put(0.26, 0.80, 0.74, 0.88, 70, 70, 70);
			break;
		case Ico::Add:
			put(0.44, 0.16, 0.56, 0.84, 0, 120, 215);
			put(0.16, 0.44, 0.84, 0.56, 0, 120, 215);
			break;
		case Ico::Disc:
		{
			// the drive, and a disc on it
			put(0.06, 0.62, 0.94, 0.90, 120, 120, 120);
			put(0.09, 0.65, 0.91, 0.87, 210, 210, 210);
			const double cx = 0.5 * s, cy = 0.36 * s, r = 0.32 * s;
			for (int y = 0; y < s; y++)
				for (int x = 0; x < s; x++)
				{
					const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
					if (d > r)
						continue;
					uint8_t* p = &rgba[(size_t(y) * s + x) * 4];
					const bool hole = d < r * 0.22, edge = d > r - 1.2 || (d >= r * 0.22 && d < r * 0.22 + 1.2);
					if (hole)
						continue;
					p[0] = edge ? 110 : uint8_t(190 + 40 * (x + y) / (2 * s));
					p[1] = edge ? 110 : uint8_t(200 + 30 * x / s);
					p[2] = edge ? 120 : 230;
					p[3] = 255;
				}
			break;
		}
	}
	img.FromRGBA(rgba.data(), s, s);
	return &img;
}

const Image* FileIcon(const std::string& name)
{
	const std::string e = LowerExt(name);
	static const char* const audio[] = {"mp3", "mp2", "mpa", "m4a", "m4b", "aac", "ac3", "eac3", "ec3", "dts", "flac", "wav", "ogg",
		"oga", "opus", "wma", "mka", "ape", "wv", "tta", "mpc", "alac", "amr", "dsf", "dff", "mid", nullptr};
	if (InList(e, audio))
		return Icon(Ico::Audio);
	if (IsSubtitleFile(name) || IsPlaylistFile(name))
		return Icon(Ico::Text);
	if (IsMediaFile(name))
		return Icon(Ico::Video);
	return Icon(Ico::File);
}

std::string TypeName(const std::string& name, bool dir)
{
	if (dir)
		return T("File folder");
	std::string e = LowerExt(name);
	if (e.empty())
		return T("File");
	for (char& c : e)
		c = char(toupper(static_cast<unsigned char>(c)));
	if (IsSubtitleFile(name))
		return Fmt(T("%s Subtitle File"), e.c_str());
	if (IsPlaylistFile(name))
		return Fmt(T("%s Playlist"), e.c_str());
	if (IsMediaFile(name))
		return "MPC-HC." + e;
	return Fmt(T("%s File"), e.c_str());
}

std::string DateText(int64_t t)
{
	if (t <= 0)
		return "";
	const time_t tt = time_t(t);
	struct tm tmv;
	localtime_r(&tt, &tmv);
	char b[64];
	strftime(b, sizeof(b), "%m/%d/%Y %I:%M %p", &tmv);
	return b;
}
} // namespace

// ===========================================================================================================
// the "Open" common dialog
namespace
{
struct Place
{
	std::string name, path;
	Ico icon;
	bool add_network = false;
};

class FileBrowser : public Dialog
{
public:
	BrowseFilter filter = BrowseFilter::Media;
	std::function<void(const std::string&)> done;
	std::string dir; // "" = This PS5
	std::vector<std::string> back, forward;
	std::vector<FsEntry> entries;
	std::vector<Place> places;
	TreeView* nav = nullptr;
	ListView* list = nullptr;
	Edit* address = nullptr;
	Edit* name = nullptr;
	ComboBox* types = nullptr;
	Button *ok = nullptr, *cancel = nullptr, *bback = nullptr, *bfwd = nullptr, *bup = nullptr;
	std::string error;
	bool finished = false;

	void Finish(const std::string& r)
	{
		if (finished)
			return;
		finished = true;
		if (done)
			done(r);
		EndDialog(r.empty() ? 2 : 1);
	}
	void OnCancel() override { Finish(""); }

	void BuildPlaces()
	{
		places.clear();
		nav->items.clear();
		nav->items.push_back({T("This PS5"), 0, Icon(Ico::Computer), false});
		places.push_back({T("This PS5"), "", Ico::Computer});
		auto add = [&](const std::string& n, const std::string& p, Ico ic, int level = 1) {
			nav->items.push_back({n, level, Icon(ic), false});
			places.push_back({n, p, ic});
		};
		add(Fmt(T("Media (%s)"), OrbisDir("media").c_str()), OrbisDir("media"), Ico::Folder);
		add(T("Internal storage (/data)"), "/data", Ico::Drive);
		for (int i = 0; i < 8; i++)
		{
			const std::string p = "/mnt/usb" + std::to_string(i);
			if (OrbisIsDir(p))
				add(Fmt(T("USB Drive %d (%s)"), i, p.c_str()), p, Ico::Usb);
		}
		for (int i = 0; i < 2; i++)
		{
			const std::string p = "/mnt/ext" + std::to_string(i);
			if (OrbisIsDir(p))
				add(Fmt(T("Extended Storage %d (%s)"), i, p.c_str()), p, Ico::Usb);
		}
		// the disc drives (mpc_disc.h): the console's own and USB drives, with the disc's name
		drives_seen = DrivesKey();
		for (const disc::DriveState& d : disc::Drives())
		{
			add(disc::DriveName(d), "disc://" + d.id, Ico::Disc);
			OrbisLog("[ui] file browser: %s (disc://%s)", disc::DriveName(d).c_str(), d.id.c_str());
		}
		nav->items.push_back({T("Network"), 0, Icon(Ico::Network), false});
		places.push_back({T("Network"), "smb://", Ico::Network});
		for (const NetServer& s : AppSettings().servers)
			add(s.name, s.url, Ico::Server);
		nav->items.push_back({T("Add a network location..."), 1, Icon(Ico::Add), false});
		Place ap;
		ap.name = T("Add a network location...");
		ap.icon = Ico::Add;
		ap.add_network = true;
		places.push_back(ap);
	}

	// what the disc drives look like now (the pane is made again when it changes)
	std::string drives_seen;
	static std::string DrivesKey()
	{
		std::string k;
		for (const disc::DriveState& d : disc::Drives())
			k += d.id + ":" + std::to_string(int(d.kind)) + ":" + std::to_string(d.generation) + ";";
		return k;
	}
	void Tick() override
	{
		const std::string k = DrivesKey();
		if (k == drives_seen)
			return;
		// a disc put in or taken out: the pane, and the list if it shows a drive
		const int sel = nav->sel;
		const std::string sel_path = sel >= 0 && sel < int(places.size()) ? places[sel].path : "";
		BuildPlaces();
		for (size_t i = 0; i < places.size(); i++)
			if (places[i].path == sel_path)
				nav->sel = int(i);
		if (dir.empty() || dir.compare(0, 7, "disc://") == 0)
		{
			const int keep = list->sel;
			std::string to = dir;
			if (!to.empty() && !disc::IsDir(to))
				to = "disc://" + disc::DriveOf(to); // the disc changed: its root
			Navigate(to, false);
			if (keep >= 0 && keep < int(list->rows.size()))
				list->sel = keep;
		}
	}

	// a DVD-Video / Blu-ray structure's file: it plays the disc (or folder) as MPC-HC does
	static bool IsTitleFile(const std::string& name)
	{
		std::string n = name;
		for (char& c : n)
			c = char(tolower(static_cast<unsigned char>(c)));
		return n == "video_ts.ifo" || n == "index.bdmv";
	}

	bool Shown(const FsEntry& e) const
	{
		if (e.dir)
			return true;
		if (!e.target.empty())
			return filter == BrowseFilter::Media;
		if (filter == BrowseFilter::Media && IsTitleFile(e.name))
			return true;
		if (filter == BrowseFilter::Folder)
			return false;
		const int t = types ? types->sel : 0;
		if (t == 1)
			return true; // All files (*.*)
		return filter == BrowseFilter::Subtitles ? IsSubtitleFile(e.name) : IsMediaFile(e.name);
	}

	void Navigate(const std::string& to, bool record = true)
	{
		if (record && to != dir)
		{
			back.push_back(dir);
			forward.clear();
		}
		dir = to;
		entries.clear();
		error.clear();
		list->rows.clear();
		list->icons.clear();
		list->sel = -1;
		list->top = 0;
		if (dir.empty())
		{
			// This PS5: the places as "drives"
			for (size_t i = 1; i < places.size(); i++)
				if (!places[i].path.empty() && places[i].path != "smb://" && !places[i].add_network)
				{
					FsEntry e;
					e.name = places[i].name;
					e.dir = true;
					entries.push_back(e);
					list->rows.push_back({places[i].name, "", T(places[i].path.compare(0, 6, "smb://") == 0 ? "Network location"
						: places[i].path.compare(0, 7, "disc://") == 0 ? "Disc drive" : "Drive"), ""});
					list->icons.push_back(Icon(places[i].icon));
				}
		}
		else if (dir == "smb://")
		{
			for (const NetServer& s : AppSettings().servers)
			{
				FsEntry e;
				e.name = s.name;
				e.dir = true;
				entries.push_back(e);
				list->rows.push_back({s.name, "", T("Network location"), HideSecret(s.url)});
				list->icons.push_back(Icon(Ico::Server));
			}
		}
		else
		{
			std::vector<FsEntry> all;
			// a server without a share: its shares
			const bool server_only = dir.compare(0, 6, "smb://") == 0 && dir.find('/', 6) == std::string::npos;
			if (server_only)
			{
				std::vector<std::string> shares;
				if (smb::Shares(dir, shares, &error))
					for (const auto& sh : shares)
						all.push_back({sh, true, 0, 0});
			}
			else
				ListDir(dir, all, &error);
			SortEntries(all);
			// the root of a DVD-Video / Blu-ray disc: playing it comes first
			if (dir.compare(0, 7, "disc://") == 0 && dir.find('/', 7) == std::string::npos && error.empty())
			{
				disc::DriveState ds;
				if (disc::GetDrive(disc::DriveOf(dir), &ds) && !disc::PlayUrl(ds).empty())
				{
					FsEntry play;
					play.name = ds.kind == disc::Kind::BluRay ? T("Play the Blu-ray") : T("Play the DVD-Video");
					play.target = disc::PlayUrl(ds);
					all.insert(all.begin(), play);
				}
			}
			for (const auto& e : all)
				if (Shown(e))
				{
					entries.push_back(e);
					if (!e.target.empty())
						list->rows.push_back({e.name, "", T(e.target.compare(0, 9, "bluray://") == 0 ? "Blu-ray" : "DVD-Video"), ""});
					else
						list->rows.push_back({e.name, DateText(e.mtime), TypeName(e.name, e.dir), e.dir ? "" : SizeText(e.size)});
					list->icons.push_back(e.dir ? Icon(Ico::Folder) : !e.target.empty() ? Icon(Ico::Disc) : FileIcon(e.name));
				}
		}
		if (!error.empty())
		{
			list->rows.push_back({error, "", "", ""});
			list->icons.push_back(nullptr);
		}
		if (dir.compare(0, 7, "disc://") == 0)
		{
			std::string names;
			for (const auto& e : entries)
				names += (names.empty() ? "" : ", ") + e.name;
			OrbisLog("[ui] file browser at %s: %s", dir.c_str(), error.empty() ? names.c_str() : error.c_str());
		}
		if (!entries.empty())
			list->sel = 0;
		address->text = dir.empty() ? T("This PS5") : dir == "smb://" ? T("Network") : HideSecret(dir);
		name->text = filter == BrowseFilter::Folder ? (dir.empty() || dir == "smb://" ? "" : FileName(dir)) : "";
		// the navigation pane follows
		for (size_t i = 0; i < places.size(); i++)
			if (places[i].path == dir)
				nav->sel = int(i);
		bback->enabled = !back.empty();
		bfwd->enabled = !forward.empty();
		bup->enabled = !dir.empty();
		AppSettings().last_dir = dir;
	}

	std::string EntryPath(int i) const
	{
		if (i < 0 || i >= int(entries.size()))
			return "";
		const FsEntry& e = entries[i];
		if (!e.target.empty())
			return e.target;
		if (dir.empty())
		{
			for (const auto& p : places)
				if (p.name == e.name)
					return p.path;
			return "";
		}
		if (dir == "smb://")
		{
			for (const auto& s : AppSettings().servers)
				if (s.name == e.name)
					return s.url;
			return "";
		}
		return Join(dir, e.name);
	}

	void OpenSelected()
	{
		const int i = list->sel;
		if (i < 0 || i >= int(entries.size()))
		{
			if (filter == BrowseFilter::Folder && !dir.empty() && dir != "smb://")
				Finish(dir);
			return;
		}
		const std::string p = EntryPath(i);
		if (entries[i].dir)
			Navigate(p);
		else if (filter == BrowseFilter::Media && entries[i].target.empty() && IsTitleFile(entries[i].name))
		{
			// VIDEO_TS.IFO / index.bdmv: the DVD-Video / Blu-ray it belongs to
			const std::string u = disc::TitleUrlForFolder(dir);
			if (u.empty())
				MessageBox(T("This folder has no DVD-Video (VIDEO_TS) or Blu-ray (BDMV) to play."), title, MB_OK, 2);
			else
				Finish(u);
		}
		else
			Finish(p);
	}

	void Up()
	{
		if (dir.empty())
			return;
		if (dir == "smb://")
		{
			Navigate("");
			return;
		}
		if (dir.compare(0, 6, "smb://") == 0)
		{
			// smb://server/share/a -> smb://server/share -> smb://server -> Network
			const size_t slash = dir.find_last_of('/');
			if (slash <= 5)
				Navigate("smb://");
			else
				Navigate(dir.substr(0, slash));
			return;
		}
		for (const auto& p : places)
			if (p.path == dir)
			{
				Navigate("");
				return;
			}
		const std::string up = ParentFolder(dir);
		Navigate(up.empty() || up == "/" || up == "/mnt" ? "" : up);
	}

	void PaintClient(CDC& dc) override
	{
		const CRect c = Client();
		// the command bar band under the address bar, the bottom panel
		dc.FillSolidRect(CRect(c.left, c.top, c.right, list->rect.top - DuY(3)), RGB(255, 255, 255));
		dc.FillSolidRect(CRect(c.left, nav->rect.top, c.right, list->rect.bottom), RGB(255, 255, 255));
		dc.HLine(c.left, c.right, list->rect.bottom, RGB(230, 230, 230));
		CRect hint(c.left + DuX(7), c.bottom - DuY(10), c.right - DuX(7), c.bottom - DuY(2));
		DrawText(dc, UiFont(), T("Cross: open   Circle: cancel   L1/R1: next control   Triangle: up one level"), hint,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, sys::GrayText);
	}

	bool OnEvent(const InputEvent& ev) override
	{
		if (ev.type == InputEvent::KeyDown && ev.key == Key::Triangle)
		{
			Up();
			return true;
		}
		return Dialog::OnEvent(ev);
	}
};
} // namespace

void ShowFileBrowser(const std::string& title, BrowseFilter filter, std::function<void(const std::string&)> done)
{
	FileBrowser* d = new FileBrowser;
	d->title = title;
	d->filter = filter;
	d->done = std::move(done);
	const int W = Dialog::DuX(560), H = Dialog::DuY(330);
	d->rect = CRect(0, 0, W, H + d->CaptionHeight());
	d->Center(WindowManager::Get().screen);
	const CRect c = d->Client();
	auto R = [&](int x, int y, int w, int h) { return d->DuRect(x, y, w, h); };
	const int cw = (c.Width() * 4 + Dialog::DuX(4) / 2) / Dialog::DuX(4); // client width in dialog units
	// the address row: Back, Forward, Up, the address
	d->bback = d->Add(new Button);
	d->bback->text = "<";
	d->bback->rect = R(4, 4, 16, 14);
	d->bfwd = d->Add(new Button);
	d->bfwd->text = ">";
	d->bfwd->rect = R(22, 4, 16, 14);
	d->bup = d->Add(new Button);
	d->bup->text = "^";
	d->bup->rect = R(40, 4, 16, 14);
	d->address = d->Add(new Edit);
	d->address->rect = R(60, 4, cw - 64, 14);
	d->address->keyboard_title = T("Go to an address (a folder, or smb://server/share)");
	// the panes
	d->nav = d->Add(new TreeView);
	d->nav->rect = R(4, 22, 140, 250);
	d->list = d->Add(new ListView);
	d->list->rect = R(148, 22, cw - 152, 250);
	const int lw = d->list->rect.Width() - ScaleX(20);
	d->list->columns = {{T("Name"), lw * 46 / 100}, {T("Date modified"), lw * 20 / 100}, {T("Type"), lw * 18 / 100},
		{T("Size"), lw * 16 / 100, true}};
	// the bottom panel: File name, the types, Open / Cancel
	Static* fl = d->Add(new Static);
	fl->text = T(filter == BrowseFilter::Folder ? "Folder:" : "File &name:");
	fl->flags = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
	fl->rect = R(4, 280, 64, 14);
	d->name = d->Add(new Edit);
	d->name->rect = R(72, 280, cw - 72 - 160, 14);
	d->name->read_only = true;
	d->types = d->Add(new ComboBox);
	d->types->rect = R(cw - 156, 280, 152, 14);
	if (filter == BrowseFilter::Subtitles)
		d->types->items = {T("Subtitle files (*.srt;*.ass;*.ssa;*.sub;*.vtt;*.smi)"), T("All files (*.*)")};
	else if (filter == BrowseFilter::Folder)
		d->types->items = {T("Folders")};
	else
		d->types->items = {T("Media files (all types)"), T("All files (*.*)")};
	d->types->sel = 0;
	d->ok = d->Add(new Button);
	d->ok->text = T(filter == BrowseFilter::Folder ? "Select Folder" : "&Open");
	d->ok->rect = R(cw - 156, 298, 74, 15);
	d->ok->is_default = true;
	d->cancel = d->Add(new Button);
	d->cancel->text = T("Cancel");
	d->cancel->rect = R(cw - 78, 298, 74, 15);

	FileBrowser* fb = d;
	d->bback->on_click = [fb] {
		if (fb->back.empty())
			return;
		fb->forward.push_back(fb->dir);
		const std::string to = fb->back.back();
		fb->back.pop_back();
		fb->Navigate(to, false);
	};
	d->bfwd->on_click = [fb] {
		if (fb->forward.empty())
			return;
		fb->back.push_back(fb->dir);
		const std::string to = fb->forward.back();
		fb->forward.pop_back();
		fb->Navigate(to, false);
	};
	d->bup->on_click = [fb] { fb->Up(); };
	d->address->on_change = [fb] {
		std::string t = fb->address->text;
		while (t.size() > 1 && t.back() == '/')
			t.pop_back();
		if (t.compare(0, 6, "smb://") == 0 || OrbisIsDir(t) || disc::IsDir(t))
			fb->Navigate(t);
		else if (OrbisIsFile(t))
			fb->Finish(t);
		else
			MessageBox(Fmt(T("Can't find '%s'. Check the spelling and try again."), t.c_str()), fb->title, MB_OK, 3);
	};
	d->types->on_change = [fb] { fb->Navigate(fb->dir, false); };
	d->list->on_open = [fb] { fb->OpenSelected(); };
	d->list->on_select = [fb] {
		const int i = fb->list->sel;
		if (i >= 0 && i < int(fb->entries.size()) && (!fb->entries[i].dir || fb->filter == BrowseFilter::Folder))
			fb->name->text = fb->entries[i].name;
	};
	d->nav->on_select = [fb] {
		const int i = fb->nav->sel;
		if (i < 0 || i >= int(fb->places.size()))
			return;
		if (fb->places[i].add_network)
		{
			ShowAddNetworkDialog([fb](const std::string& name, const std::string& url) {
				AppSettings().servers.push_back({name, url});
				AppSettings().Save();
				fb->BuildPlaces();
				fb->Navigate(url);
			});
			return;
		}
		fb->Navigate(fb->places[i].path);
	};
	d->ok->on_click = [fb] { fb->OpenSelected(); };
	d->cancel->on_click = [fb] { fb->Finish(""); };
	d->BuildPlaces();
	std::string start = AppSettings().last_dir;
	disc::DriveState start_drive;
	if (!start.empty() && start.compare(0, 6, "smb://") != 0 && !OrbisIsDir(start) && !disc::IsDir(start) &&
		!(start == "disc://" + disc::DriveOf(start) && disc::GetDrive(disc::DriveOf(start), &start_drive)))
		start.clear();
	d->Navigate(start.empty() ? OrbisDir("media") : start, false);
	d->SetFocus(d->list);
	WindowManager::Get().Push(d);
}

// ===========================================================================================================
// Open (IDD_OPEN_DLG, 255 x 113)
void ShowOpenDialog(std::function<void(const std::string&, const std::string&, bool)> done)
{
	struct OpenDlg : public Dialog
	{
		ComboBox *open = nullptr, *dub = nullptr;
		CheckBox* add = nullptr;
		std::function<void(const std::string&, const std::string&, bool)> done;
		void OnOk() override
		{
			const std::string url = open->Text();
			if (url.empty())
				return;
			auto cb = done;
			const std::string d = dub->Text();
			const bool a = add->checked;
			EndDialog(1);
			if (cb)
				cb(url, d, a);
		}
		void PaintClient(CDC& dc) override
		{
			const CRect r = DuRect(5, 5, 21, 20);
			dc.AlphaBlit(r.left, r.top, res::AppIcon(ScaleX(32)).Bmp());
		}
	};
	OpenDlg* d = new OpenDlg;
	d->title = T("Open");
	d->done = std::move(done);
	d->rect = CRect(0, 0, Dialog::DuX(255) + 2, Dialog::DuY(113) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	Static* t = d->Add(new Static);
	t->text = T("Type the address of a movie or audio file (on the Internet or your computer) and the player will open it for you.");
	t->rect = d->DuRect(35, 5, 213, 25);
	Static* l1 = d->Add(new Static);
	l1->text = T("Open:");
	l1->rect = d->DuRect(10, 32, 245, 8);
	d->open = d->Add(new ComboBox);
	d->open->editable = true;
	d->open->rect = d->DuRect(10, 42, 188, 14);
	d->open->items = AppSettings().recent;
	if (!d->open->items.empty())
		d->open->text = d->open->items.front();
	Button* b1 = d->Add(new Button);
	b1->text = T("Browse...");
	b1->rect = d->DuRect(200, 41, 50, 14);
	Static* l2 = d->Add(new Static);
	l2->text = T("Dub:");
	l2->rect = d->DuRect(10, 58, 245, 8);
	d->dub = d->Add(new ComboBox);
	d->dub->editable = true;
	d->dub->rect = d->DuRect(10, 68, 188, 14);
	d->dub->enabled = false; // a separate audio file is not supported yet
	Button* b2 = d->Add(new Button);
	b2->text = T("Browse...");
	b2->rect = d->DuRect(200, 67, 50, 14);
	b2->enabled = false;
	d->add = d->Add(new CheckBox);
	d->add->text = T("Add to playlist without opening");
	d->add->rect = d->DuRect(10, 84, 235, 8);
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(148, 94, 50, 14);
	Button* cancel = d->Add(new Button);
	cancel->text = T("Cancel");
	cancel->rect = d->DuRect(200, 94, 50, 14);
	OpenDlg* dd = d;
	b1->on_click = [dd] {
		ShowFileBrowser(T("Open"), BrowseFilter::Media, [dd](const std::string& p) {
			if (!p.empty())
				dd->open->text = p;
		});
	};
	ok->on_click = [dd] { dd->OnOk(); };
	cancel->on_click = [dd] { dd->OnCancel(); };
	WindowManager::Get().Push(d);
	d->SetFocus(d->open);
}

// ===========================================================================================================
// Go To (IDD_GOTO_DLG, 186 x 120)
void ShowGoToDialog(double pos, double fps, std::function<void(double)> done)
{
	struct GoTo : public Dialog
	{
		Edit *time = nullptr, *frame = nullptr;
		std::function<void(double)> done;
		double fps = 25;
		static double ParseTime(const std::string& s)
		{
			// [hh:]mm:ss.ms, or the digits alone (separators not needed)
			std::vector<double> parts;
			std::string cur;
			for (char c : s)
			{
				if (c == ':')
				{
					parts.push_back(atof(cur.c_str()));
					cur.clear();
				}
				else if (isdigit(static_cast<unsigned char>(c)) || c == '.')
					cur += c;
			}
			parts.push_back(atof(cur.c_str()));
			double t = 0;
			for (double p : parts)
				t = t * 60 + p;
			if (parts.size() == 1 && s.find('.') == std::string::npos && s.size() > 2)
			{
				// "012345" = 01:23:45 as the masked edit reads it
				std::string d;
				for (char c : s)
					if (isdigit(static_cast<unsigned char>(c)))
						d += c;
				while (d.size() < 6)
					d = "0" + d;
				t = atoi(d.substr(0, d.size() - 4).c_str()) * 3600 + atoi(d.substr(d.size() - 4, 2).c_str()) * 60 +
					atoi(d.substr(d.size() - 2).c_str());
			}
			return t;
		}
		void GoTime()
		{
			const double t = ParseTime(time->text);
			auto cb = done;
			EndDialog(1);
			if (cb)
				cb(t);
		}
		void GoFrame()
		{
			int f = 0;
			double r = fps;
			sscanf(frame->text.c_str(), "%d%*[ ,]%lf", &f, &r);
			auto cb = done;
			EndDialog(1);
			if (cb && r > 0)
				cb(f / r);
		}
	};
	GoTo* d = new GoTo;
	d->title = T("Go To...");
	d->done = std::move(done);
	d->fps = fps > 0 ? fps : 25;
	d->rect = CRect(0, 0, Dialog::DuX(186) + 2, Dialog::DuY(120) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	Static* t1 = d->Add(new Static);
	t1->text = T("Enter a timecode using the format [hh:]mm:ss.ms to jump to a specified time. You do not need to enter the separators explicitly.");
	t1->rect = d->DuRect(5, 5, 173, 27);
	Static* l1 = d->Add(new Static);
	l1->text = T("Time");
	l1->rect = d->DuRect(10, 42, 35, 8);
	d->time = d->Add(new Edit);
	d->time->rect = d->DuRect(47, 39, 80, 14);
	char b[64];
	const long ms = long(pos * 1000);
	snprintf(b, sizeof(b), "%02ld:%02ld:%02ld.%03ld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000);
	d->time->text = b;
	d->time->keyboard_title = T("Time ([hh:]mm:ss.ms)");
	Button* g1 = d->Add(new Button);
	g1->text = T("Go!");
	g1->is_default = true;
	g1->rect = d->DuRect(131, 39, 50, 14);
	Static* t2 = d->Add(new Static);
	t2->text = T("Enter two numbers to jump to a specified frame, the first is the frame number, the second is the frame rate.");
	t2->rect = d->DuRect(5, 64, 173, 26);
	Static* l2 = d->Add(new Static);
	l2->text = T("Frame");
	l2->rect = d->DuRect(10, 99, 35, 8);
	d->frame = d->Add(new Edit);
	d->frame->rect = d->DuRect(47, 96, 81, 14);
	snprintf(b, sizeof(b), "%ld, %.3f", long(std::lround(pos * d->fps)), d->fps);
	d->frame->text = b;
	d->frame->keyboard_title = T("Frame, frame rate");
	Button* g2 = d->Add(new Button);
	g2->text = T("Go!");
	g2->rect = d->DuRect(131, 96, 50, 14);
	GoTo* dd = d;
	g1->on_click = [dd] { dd->GoTime(); };
	g2->on_click = [dd] { dd->GoFrame(); };
	d->time->on_change = [dd] { dd->GoTime(); };
	WindowManager::Get().Push(d);
	d->SetFocus(d->time);
}

// ===========================================================================================================
// Properties (CPPageFileInfoSheet: Details, MediaInfo)
namespace
{
// a Details() key on the screen: "Duration", "Video #0" (the kind translated, the number kept); the file's own
// tags come back as they are
std::string KeyText(const std::string& key)
{
	const size_t hash = key.find(" #");
	if (hash != std::string::npos)
		return T(key.substr(0, hash)) + key.substr(hash);
	return T(key);
}
} // namespace

void ShowPropertiesDialog(Player& player, const std::string& url)
{
	struct Props : public Dialog
	{
		TabControl* tabs = nullptr;
		std::vector<Control*> page[2];
		void Show(int p)
		{
			for (int i = 0; i < 2; i++)
				for (Control* c : page[i])
					c->visible = i == p;
		}
		void PaintClient(CDC& dc) override
		{
			if (tabs->sel == 0)
			{
				const CRect ic = DuRect(12, 26, 21, 20);
				dc.AlphaBlit(ic.left, ic.top, res::AppIcon(ScaleX(32)).Bmp());
				const CRect l1 = DuRect(14, 51, 214, 1), l2 = DuRect(14, 132, 214, 1);
				dc.HLine(l1.left, l1.right, l1.top, sys::Shadow);
				dc.HLine(l1.left, l1.right, l1.top + 1, sys::Hilight);
				dc.HLine(l2.left, l2.right, l2.top, sys::Shadow);
				dc.HLine(l2.left, l2.right, l2.top + 1, sys::Hilight);
			}
		}
	};
	Props* d = new Props;
	d->title = Fmt(T("%s Properties"), FileName(url).c_str());
	d->rect = CRect(0, 0, Dialog::DuX(248) + 2, Dialog::DuY(250) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	d->tabs = d->Add(new TabControl);
	d->tabs->tabs = {T("Details"), T("MediaInfo")};
	d->tabs->rect = d->DuRect(7, 7, 234, 216);
	Props* dd = d;
	d->tabs->on_change = [dd] { dd->Show(dd->tabs->sel); };
	// Details (IDD_FILEPROPDETAILS), 14 du in from the tab
	auto add_static = [&](const std::string& t, int x, int y, int w, int h, int p, bool value = false) {
		Static* s = d->Add(new Static);
		s->text = t;
		s->rect = d->DuRect(7 + x, 21 + y, w, h);
		s->flags = DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER;
		(void)value;
		d->page[p].push_back(s);
		return s;
	};
	add_static(FileName(url), 32, 13, 186, 13, 0, true);
	const std::vector<std::pair<std::string, std::string>> det = player.Details();
	std::string type = "?", length = "", vsize = "", created = "", size = "";
	for (const auto& kv : det)
	{
		if (kv.first == "Format")
			type = kv.second;
		else if (kv.first == "Duration")
			length = kv.second;
		else if (kv.first == "File size")
			size = kv.second;
		else if (kv.first == "creation_time")
			created = kv.second;
	}
	if (player.HasVideo())
	{
		char b[96];
		snprintf(b, sizeof(b), "%d x %d", player.VideoWidth(), player.VideoHeight());
		vsize = b;
	}
	const char* labels[] = {T("Type:"), T("Size:"), T("Media length:"), T("Video size:"), T("Created:")};
	const std::string values[] = {type, size.empty() ? T("Not known") : size, length.empty() ? T("Not known") : length,
		vsize.empty() ? T("Not applicable") : vsize, created.empty() ? T("Not known") : created};
	for (int i = 0; i < 5; i++)
	{
		add_static(labels[i], 10, 36 + i * 15, 60, 8, 0);
		add_static(values[i], 72, 36 + i * 15, 151, 13, 0, true);
	}
	TextView* tags = d->Add(new TextView);
	tags->rect = d->DuRect(7 + 10, 21 + 115, 213, 77);
	for (const auto& kv : det)
		if (kv.first.find('#') == std::string::npos && kv.first != "Format" && kv.first != "Duration" && kv.first != "File size")
			tags->lines.push_back(KeyText(kv.first) + ": " + kv.second);
	d->page[0].push_back(tags);
	// MediaInfo (IDD_FILEMEDIAINFO): the whole report
	TextView* mi = d->Add(new TextView);
	mi->rect = d->DuRect(9, 23, 230, 198);
	mi->lines.push_back(T("General"));
	mi->lines.push_back(T("Complete name") + std::string(" : ") + HideSecret(url));
	for (const auto& kv : det)
	{
		if (kv.first.find('#') != std::string::npos)
		{
			mi->lines.push_back("");
			mi->lines.push_back(KeyText(kv.first));
			// "codec (profile), WxH, fps, ..." one per line
			std::string v = kv.second;
			size_t p = 0;
			while (p != std::string::npos)
			{
				const size_t c = v.find(", ", p);
				mi->lines.push_back("  " + v.substr(p, c == std::string::npos ? std::string::npos : c - p));
				p = c == std::string::npos ? c : c + 2;
			}
		}
		else
			mi->lines.push_back(KeyText(kv.first) + " : " + kv.second);
	}
	d->page[1].push_back(mi);
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(85, 229, 50, 14);
	Button* cancel = d->Add(new Button);
	cancel->text = T("Cancel");
	cancel->rect = d->DuRect(139, 229, 50, 14);
	Button* apply = d->Add(new Button);
	apply->text = T("&Apply");
	apply->rect = d->DuRect(193, 229, 50, 14);
	apply->enabled = false;
	ok->on_click = [dd] { dd->EndDialog(1); };
	cancel->on_click = [dd] { dd->EndDialog(2); };
	d->Show(0);
	WindowManager::Get().Push(d);
	d->SetFocus(ok);
}

// ===========================================================================================================
// About (IDD_ABOUTBOX, 260 x 206)
void ShowAboutDialog()
{
	struct About : public Dialog
	{
		void PaintClient(CDC& dc) override
		{
			const CRect r = DuRect(14, 10, 32, 32);
			const Image& ic = res::AppIcon(ScaleX(48));
			dc.AlphaBlit(r.left, r.top, ic.Bmp());
		}
	};
	About* d = new About;
	d->title = T("About");
	d->rect = CRect(0, 0, Dialog::DuX(260) + 2, Dialog::DuY(206) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	auto S = [&](const std::string& t, int x, int y, int w, int h, COLORREF c = sys::WindowText) {
		Static* s = d->Add(new Static);
		s->text = t;
		s->rect = d->DuRect(x, y, w, h);
		s->color = c;
		return s;
	};
	S("MPC-HC 1.7.13 (PS5 port " MPCPS5_PS5_VERSION ")", 60, 8, 200, 8);
	S(T("Copyright \xC2\xA9 2002-2017 see Authors.txt"), 60, 20, 200, 8, sys::HotTrack);
	S("https://mpc-hc.org/  -  PS5: github.com/MisterTemaki", 60, 32, 200, 8, sys::HotTrack);
	S(T("This program is freeware and released under the GNU General Public License."), 7, 46, 246, 18);
	S(T("English translation made by MPC-HC Team. PS5 port: MisterTemaki, with PS5SX2's PS5 layer."), 7, 66, 246, 8);
	GroupBox* g1 = d->Add(new GroupBox);
	g1->text = T("Build information");
	g1->rect = d->DuRect(7, 80, 246, 60);
	S(T("Version:"), 13, 92, 85, 8);
	S("1.7.13 / PS5 " MPCPS5_PS5_VERSION, 95, 92, 155, 8);
	S(T("Compiler:"), 13, 104, 85, 8);
#if defined(__clang__)
	S(std::string("Clang ") + __clang_version__, 95, 104, 155, 8);
#else
	S("GCC " __VERSION__, 95, 104, 155, 8);
#endif
	S(T("FFmpeg:"), 13, 116, 85, 8);
	S(Fmt(T("%s (LAV Filters' engine)"), FFmpegVersion().c_str()), 95, 116, 155, 8);
	S(T("Build date:"), 13, 128, 85, 8);
	S(__DATE__ " " __TIME__, 95, 128, 155, 8);
	GroupBox* g2 = d->Add(new GroupBox);
	g2->text = T("Operating system");
	g2->rect = d->DuRect(7, 146, 246, 36);
	S(T("Name:"), 13, 158, 85, 8);
	S("PlayStation 5", 95, 158, 155, 8);
	S(T("Version:"), 13, 170, 85, 8);
	S(T("native title (ps5-payload-dev SDK)"), 95, 170, 155, 8);
	Button* copy = d->Add(new Button);
	copy->text = T("Copy to clipboard");
	copy->rect = d->DuRect(7, 186, 110, 14);
	copy->enabled = false;
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(204, 186, 50, 14);
	About* dd = d;
	ok->on_click = [dd] { dd->EndDialog(1); };
	WindowManager::Get().Push(d);
}

// ===========================================================================================================
// Options (CPPageSheet: the page tree, a page, OK / Cancel / Apply)
namespace
{
class Options : public Dialog
{
public:
	TreeView* tree = nullptr;
	std::vector<std::vector<Control*>> pages;
	std::vector<std::string> names;
	std::vector<std::function<void()>> apply_fns;
	std::function<void()> applied;
	int page = 0;
	CRect page_rect;

	void ShowPage(int p)
	{
		page = p;
		for (int i = 0; i < int(pages.size()); i++)
			for (Control* c : pages[i])
				c->visible = i == p;
	}
	void Apply()
	{
		for (auto& f : apply_fns)
			f();
		if (applied)
			applied();
	}
	void PaintClient(CDC& dc) override
	{
		// CTreePropSheet's caption over the page: the page's name on a light gradient band
		const CRect cap(page_rect.left, page_rect.top - DuY(16), page_rect.right, page_rect.top - DuY(3));
		for (int x = cap.left; x < cap.right; x++)
		{
			const int t = (x - cap.left) * 255 / std::max(1, cap.Width());
			const COLORREF c = RGB(255 - t * 20 / 255, 255 - t * 20 / 255, 255 - t * 20 / 255);
			dc.VLine(x, cap.top, cap.bottom, c);
		}
		dc.FrameRect(cap, RGB(200, 200, 200));
		CRect tr = cap;
		tr.DeflateRect(ScaleX(6), 0);
		DrawText(dc, UiBoldFont(), page >= 0 && page < int(names.size()) ? names[page] : "", tr,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, sys::WindowText);
	}
	// controls of a page, at page coordinates in dialog units (the templates are 296 x 241)
	template <typename T>
	T* P(int p, T* c, int x, int y, int w, int h)
	{
		Add(c);
		c->rect = DuRect(136 + x, 23 + y, w, h); // the page's place (page_rect)
		pages[p].push_back(c);
		return c;
	}
	Static* Text(int p, const std::string& t, int x, int y, int w, int h, unsigned flags = DT_LEFT | DT_WORDBREAK)
	{
		Static* s = P(p, new Static, x, y, w, h);
		s->text = T(t);
		s->flags = flags;
		return s;
	}
	CheckBox* Check(int p, const std::string& t, int x, int y, int w, int h, bool v, bool en = true)
	{
		CheckBox* c = P(p, new CheckBox, x, y, w, h);
		c->text = T(t);
		c->checked = v;
		c->enabled = en;
		return c;
	}
	CheckBox* Radio(int p, const std::string& t, int x, int y, int w, int h, bool v, int group)
	{
		CheckBox* c = Check(p, t, x, y, w, h, v);
		c->radio = true;
		c->group = group;
		return c;
	}
	GroupBox* Group(int p, const std::string& t, int x, int y, int w, int h)
	{
		GroupBox* g = P(p, new GroupBox, x, y, w, h);
		g->text = T(t);
		return g;
	}
	NumberBox* Number(int p, int x, int y, int w, int h, int v, int lo, int hi, int step = 1, const std::string& suffix = "")
	{
		NumberBox* n = P(p, new NumberBox, x, y, w, h);
		n->value = v;
		n->min = lo;
		n->max = hi;
		n->step = step;
		n->suffix = suffix;
		return n;
	}
	ComboBox* Combo(int p, int x, int y, int w, int h, const std::vector<std::string>& items, int sel)
	{
		ComboBox* c = P(p, new ComboBox, x, y, w, h);
		for (const std::string& i : items)
			c->items.push_back(T(i));
		c->sel = sel;
		return c;
	}
	int NewPage(const std::string& name, int level = 0)
	{
		pages.emplace_back();
		names.push_back(T(name));
		tree->items.push_back({T(name), level, nullptr, false});
		return int(pages.size()) - 1;
	}
};
} // namespace

void ShowOptionsDialog(std::function<void()> applied)
{
	Settings& s = AppSettings();
	Options* d = new Options;
	d->title = T("Options");
	d->applied = std::move(applied);
	const int W = Dialog::DuX(436), H = Dialog::DuY(290);
	d->rect = CRect(0, 0, W + 2, H + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	d->tree = d->Add(new TreeView);
	d->tree->rect = d->DuRect(7, 7, 125, 254);
	d->page_rect = d->DuRect(136, 23, 296, 241);
	Options* o = d;

	// ---- Player (IDD_PPAGEPLAYER) ----
	int p = d->NewPage("Player");
	d->Group(p, "Open options", 5, 5, 115, 120);
	d->Radio(p, "Use the same player for each media file", 10, 17, 105, 27, true, 1);
	d->Radio(p, "Open a new player for each media file played", 10, 72, 105, 27, false, 1)->enabled = false;
	d->Group(p, "Language", 5, 129, 115, 30);
	ComboBox* lang = d->Combo(p, 10, 141, 105, 13, {}, s.language); // the languages by their own names
	for (int i = 0; i < int(Language::Count); i++)
		lang->items.push_back(LanguageName(i));
	d->Group(p, "Title bar", 5, 164, 115, 68);
	CheckBox* t0 = d->Radio(p, "Display full path", 10, 176, 105, 8, s.title_bar == 0, 2);
	CheckBox* t1 = d->Radio(p, "File name only", 10, 188, 105, 8, s.title_bar == 1, 2);
	CheckBox* t2 = d->Radio(p, "Don't prefix anything", 10, 200, 105, 8, s.title_bar == 2, 2);
	d->Group(p, "Other", 124, 5, 167, 120);
	d->Check(p, "Tray icon", 129, 17, 160, 8, false, false);
	CheckBox* osd = d->Check(p, "Show OSD", 129, 30, 160, 8, s.show_osd);
	d->Check(p, "Limit window proportions on resize", 129, 43, 160, 8, false, false);
	d->Check(p, "Snap to desktop edges", 129, 56, 160, 8, false, false);
	d->Check(p, "Store settings in .ini file", 129, 69, 160, 8, true, false);
	d->Check(p, "Disable \"Open Disc\" menu", 129, 82, 160, 8, true, false);
	d->Check(p, "Process priority above normal", 129, 95, 160, 8, false, false);
	d->Check(p, "Enable cover-art support", 129, 108, 160, 8, true, false);
	d->Group(p, "History", 124, 129, 167, 103);
	CheckBox* hist = d->Check(p, "Keep history of recently opened files", 129, 141, 160, 8, s.keep_history);
	d->Check(p, "Remember last playlist", 129, 154, 160, 8, false, false);
	CheckBox* fpos = d->Check(p, "Remember File position", 129, 167, 160, 8, s.remember_file_pos);
	d->Check(p, "Remember DVD position", 129, 180, 160, 8, false, false);
	d->Check(p, "Remember last window position", 129, 193, 160, 8, true, false);
	d->Check(p, "Remember last window size", 129, 206, 160, 8, true, false);
	d->Check(p, "Remember last Pan-n-Scan Zoom", 129, 219, 160, 8, false, false);
	d->apply_fns.push_back([&s, t0, t1, t2, osd, hist, fpos, lang] {
		s.language = std::max(0, std::min(int(Language::Count) - 1, lang->sel));
		s.title_bar = t0->checked ? 0 : t1->checked ? 1 : 2;
		(void)t2;
		s.show_osd = osd->checked;
		s.keep_history = hist->checked;
		s.remember_file_pos = fpos->checked;
	});

	// ---- Player > Keys (IDD_PPAGEACCELTBL): the controller's buttons in the window ----
	p = d->NewPage("Keys", 1);
	d->Text(p, "The controller's buttons in the window, without the pointer. Cross on a button: choose its action.", 5, 5, 286, 16);
	ListView* keys = d->P(p, new ListView, 5, 23, 286, 150);
	keys->columns = {{T("Button"), keys->rect.Width() * 38 / 100}, {T("Action"), keys->rect.Width() * 58 / 100}};
	auto pad = std::make_shared<std::vector<PadAction>>(s.pad, s.pad + int(PadButton::Count));
	auto fill_keys = [keys, pad] {
		keys->rows.clear();
		for (int i = 0; i < int(PadButton::Count); i++)
			keys->rows.push_back({T(PadButtonName(PadButton(i))), PadActionText((*pad)[i])});
	};
	fill_keys();
	keys->sel = 0;
	d->Text(p, "Action:", 5, 180, 40, 8);
	ComboBox* act = d->Combo(p, 45, 178, 170, 13, {}, int((*pad)[0]));
	for (int a = 0; a < int(PadAction::Count); a++)
		act->items.push_back(PadActionText(PadAction(a)));
	Button* kdef = d->P(p, new Button, 225, 177, 66, 14);
	kdef->text = T("Default");
	keys->on_select = [keys, act, pad] {
		if (keys->sel >= 0 && keys->sel < int(pad->size()))
			act->sel = int((*pad)[keys->sel]);
	};
	keys->on_open = [act] { act->Activate(); };
	act->on_change = [keys, act, pad, fill_keys] {
		if (keys->sel >= 0 && keys->sel < int(pad->size()) && act->sel >= 0)
		{
			(*pad)[keys->sel] = PadAction(act->sel);
			fill_keys();
		}
	};
	kdef->on_click = [keys, act, pad, fill_keys] {
		for (int i = 0; i < int(pad->size()); i++)
			(*pad)[i] = DefaultPadAction(PadButton(i));
		fill_keys();
		if (keys->sel >= 0)
			act->sel = int((*pad)[keys->sel]);
	};
	d->Text(p, "Options always opens the menu bar; the pointer's Cross / Square clicks and the buttons in menus and dialogs "
			   "stay as they are. While L2, R2 and R3 keep their default actions, L2 or R2 held with Left / Right jumps "
			   "20 s / 1 s, and R3 held with Square shows the statistics.",
		5, 197, 286, 40);
	d->apply_fns.push_back([&s, pad] {
		for (int i = 0; i < int(PadButton::Count); i++)
			s.pad[i] = (*pad)[i];
	});

	// ---- Playback (IDD_PPAGEPLAYBACK) ----
	p = d->NewPage("Playback");
	d->Group(p, "Audio", 5, 5, 286, 47);
	d->Text(p, "Volume", 45, 15, 55, 8, DT_CENTER);
	d->Text(p, "Min", 10, 31, 20, 8, DT_RIGHT);
	Slider* vol = d->P(p, new Slider, 30, 23, 85, 24);
	vol->pos = s.volume;
	vol->step = 1;
	d->Text(p, "Max", 116, 31, 23, 8);
	d->Text(p, "Balance", 190, 15, 55, 8, DT_CENTER);
	d->Text(p, "L", 145, 31, 30, 8, DT_RIGHT);
	Slider* bal = d->P(p, new Slider, 175, 23, 85, 24);
	bal->min = -100;
	bal->max = 100;
	bal->pos = 0;
	bal->enabled = false;
	d->Text(p, "R", 261, 31, 27, 8);
	d->Group(p, "Control", 5, 55, 126, 46);
	d->Text(p, "Volume step:", 11, 67, 68, 8);
	d->Number(p, 81, 64, 46, 14, 5, 1, 100, 1, " %")->enabled = false;
	d->Group(p, "Default zoom", 137, 55, 154, 46);
	d->Check(p, "Auto-zoom:", 142, 67, 75, 8, true, false);
	d->Group(p, "Playback", 5, 104, 126, 54);
	CheckBox* rplay = d->Radio(p, "Play", 10, 117, 55, 8, s.repeat == 0, 3);
	CheckBox* rforever = d->Radio(p, "Repeat forever", 10, 129, 116, 8, s.repeat != 0, 3);
	d->Text(p, "Repeat mode:", 10, 143, 48, 8);
	ComboBox* rmode = d->Combo(p, 65, 141, 60, 13, {"File", "Playlist"}, s.repeat == 2 ? 1 : 0);
	d->Group(p, "After Playback", 5, 160, 126, 27);
	ComboBox* after = d->Combo(p, 10, 170, 116, 13, {"Do nothing", "Play next file in the folder", "Exit"}, int(s.after_playback));
	d->Group(p, "Default track preference", 137, 104, 154, 83);
	d->Text(p, "Subtitles:", 142, 117, 44, 8);
	d->Text(p, "the default or forced track; an .srt next to the video wins", 190, 115, 98, 16);
	d->Text(p, "Audio:", 142, 134, 44, 8);
	d->Text(p, "the stream marked default", 190, 134, 98, 8);
	d->Group(p, "Open settings", 5, 190, 286, 46);
	d->Check(p, "Use worker thread to construct the filter graph", 10, 200, 276, 8, true, false);
	d->Check(p, "Report pins which fail to render", 10, 212, 276, 8, false, false);
	CheckBox* autosub = d->Check(p, "Auto-load subtitle files next to the video", 10, 224, 276, 8, s.auto_load_subtitles);
	d->apply_fns.push_back([&s, vol, rplay, rforever, rmode, after, autosub] {
		s.volume = vol->pos;
		s.repeat = rplay->checked ? 0 : (rmode->sel == 1 ? 2 : 1);
		(void)rforever;
		s.after_playback = AfterPlayback(after->sel);
		s.auto_load_subtitles = autosub->checked;
	});

	// ---- Output (what this player draws with) ----
	p = d->NewPage("Output");
	d->Group(p, "DirectShow Video", 5, 5, 285, 43);
	d->Combo(p, 10, 16, 276, 13, {"Software renderer (FFmpeg / libswscale, bicubic)"}, 0)->enabled = false;
	CheckBox* hwdec = d->Check(p, "Hardware decoding (H.264 / HEVC on the PS5's video decoder)", 10, 33, 276, 8,
		s.hw_decoding);
	d->Group(p, "HDR (HDR10 / HLG on the SDR picture)", 5, 50, 285, 36);
	ComboBox* tm = d->Combo(p, 10, 63, 150, 13, {"Off (as it comes)", "Tone mapping: BT.2390", "Tone mapping: Hable", "Tone mapping: Reinhard"},
		s.hdr_tonemap);
	d->Text(p, "White:", 170, 66, 30, 8);
	NumberBox* white = d->Number(p, 200, 63, 60, 13, s.hdr_peak, 100, 400, 10, " nits");
	d->Group(p, "Audio Renderer", 5, 90, 285, 30);
	ComboBox* aout = d->Combo(p, 10, 102, 276, 13,
		{"PlayStation 5 audio out: stereo (5.1/7.1 mixed down)", "PlayStation 5 audio out: surround (5.1/7.1, 8 channels)"},
		s.audio_output);
	d->Group(p, "Video frame", 5, 125, 285, 60);
	ComboBox* vf = d->Combo(p, 10, 137, 180, 13,
		{"Half Size", "Normal Size", "Double Size", "Stretch To Window", "Touch Window From Inside", "Zoom 1", "Zoom 2",
			"Touch Window From Outside"},
		int(s.video_frame));
	ComboBox* ar = d->Combo(p, 10, 156, 180, 13, {"Default (DAR)", "4:3", "5:4", "16:9", "235:100", "185:100", "Assume square pixels (SAR)"},
		s.aspect_ratio);
	d->apply_fns.push_back([&s, vf, ar, hwdec, tm, white, aout] {
		s.video_frame = VideoFrame(vf->sel);
		s.aspect_ratio = ar->sel;
		s.hw_decoding = hwdec->checked;
		s.hdr_tonemap = tm->sel;
		s.hdr_peak = white->value;
		s.audio_output = aout->sel;
	});

	// ---- Fullscreen (IDD_PPAGEFULLSCREEN) ----
	p = d->NewPage("Fullscreen");
	d->Group(p, "Fullscreen monitor", 4, 2, 288, 46);
	d->Combo(p, 8, 11, 280, 13, {"Current (TV)"}, 0)->enabled = false;
	CheckBox* launchfs = d->Check(p, "Launch files in fullscreen", 8, 26, 280, 8, s.fullscreen);
	d->Check(p, "Exit fullscreen at the end of playback", 8, 37, 280, 8, false, false);
	CheckBox* hide = d->Check(p, "Hide controls in fullscreen (shown when the pointer moves)", 8, 54, 280, 8, s.hide_controls_fullscreen);
	d->apply_fns.push_back([&s, launchfs, hide] {
		s.fullscreen = launchfs->checked;
		s.hide_controls_fullscreen = hide->checked;
	});

	// ---- Subtitles > Default Style (IDD_PPAGESUBTITLES, IDD_PPAGESUBSTYLE) ----
	p = d->NewPage("Subtitles");
	d->Group(p, "Delay", 5, 5, 286, 46);
	d->Text(p, "Delay interval:", 10, 18, 92, 8);
	NumberBox* sstep = d->Number(p, 104, 15, 58, 14, s.sub_delay_step, 10, 10000, 10, " ms");
	d->Text(p, "the step of Play > Subtitle Track's delay and of the keys", 167, 15, 121, 24);
	d->Text(p, "Current delay:", 10, 34, 92, 8);
	NumberBox* sdel = d->Number(p, 104, 31, 58, 14, s.sub_delay_ms, -60000, 60000, 100, " ms");
	d->Group(p, "Position", 5, 56, 286, 40);
	CheckBox* pvid = d->Radio(p, "Position subtitles relative to the video frame", 10, 68, 270, 8, s.sub_position == 0, 4);
	d->Radio(p, "Position subtitles relative to the window", 10, 80, 270, 8, s.sub_position == 1, 4);
	int sp = d->NewPage("Default Style", 1);
	d->Group(sp, "Font", 5, 5, 105, 64);
	// the fonts: Selawik, Roboto and the fonts folder's, read now (a font copied in while the app runs is listed)
	const std::vector<FontChoice> fonts = SubtitleFontChoices(OrbisDir("fonts"));
	ComboBox* font = d->Combo(sp, 10, 17, 95, 13, {}, -1);
	for (const FontChoice& fc : fonts)
	{
		font->items.push_back(fc.name);
		if (fc.file == s.sub_font)
			font->sel = int(font->items.size()) - 1;
	}
	if (font->sel < 0)
	{
		// the saved font's file isn't in the folder (drawn as Selawik): shown by its name and kept unless another is chosen
		font->items.push_back(s.sub_font);
		font->sel = int(font->items.size()) - 1;
	}
	d->Text(sp, "Size", 10, 36, 48, 8);
	NumberBox* fsize = d->Number(sp, 60, 33, 45, 13, s.sub_font_size, 8, 72);
	CheckBox* bold = d->Check(sp, "Bold", 10, 52, 95, 8, s.sub_bold);
	d->Group(sp, "Border Style", 5, 73, 105, 100);
	CheckBox* boutline = d->Radio(sp, "Outline", 10, 85, 95, 8, s.sub_border == 0, 5);
	CheckBox* bbox = d->Radio(sp, "Opaque box", 10, 97, 95, 8, s.sub_border == 1, 5);
	d->Text(sp, "Width", 10, 112, 48, 8);
	NumberBox* ow = d->Number(sp, 60, 110, 45, 13, s.sub_outline, 0, 8);
	d->Text(sp, "Shadow", 10, 130, 48, 8);
	NumberBox* shadow = d->Number(sp, 60, 128, 45, 13, s.sub_shadow, 0, 8);
	d->Text(sp, "Box opacity", 10, 148, 48, 16);
	NumberBox* bop = d->Number(sp, 60, 146, 45, 13, s.sub_box_opacity, 10, 100, 5, " %");
	d->Group(sp, "Screen Alignment && Margins", 114, 5, 177, 30);
	d->Text(sp, "Bottom", 119, 18, 40, 8);
	NumberBox* margin = d->Number(sp, 160, 15, 50, 14, s.sub_margin, 0, 200);
	// MPC-HC's colour buttons: a palette here
	static const struct
	{
		const char* name;
		uint32_t rgb;
	} kPalette[] = {{"White", 0xffffff}, {"Yellow", 0xffff00}, {"Light grey", 0xc0c0c0}, {"Grey", 0x808080}, {"Black", 0x000000},
		{"Red", 0xff0000}, {"Green", 0x00ff00}, {"Blue", 0x0000ff}, {"Cyan", 0x00ffff}, {"Magenta", 0xff00ff}};
	auto palette = std::make_shared<std::vector<uint32_t>>();
	std::vector<std::string> cnames;
	for (const auto& c : kPalette)
	{
		palette->push_back(c.rgb);
		cnames.push_back(c.name);
	}
	auto color_combo = [&](int y, uint32_t v) {
		ComboBox* c = d->Combo(sp, 175, y, 110, 13, cnames, -1);
		for (int i = 0; i < int(palette->size()); i++)
			if ((*palette)[i] == (v & 0xffffff))
				c->sel = i;
		if (c->sel < 0)
		{
			// a colour from mpc-hc.ini that isn't in the palette: kept as it is
			char b[16];
			snprintf(b, sizeof(b), "#%06X", unsigned(v & 0xffffff));
			c->items.push_back(b);
			c->sel = int(c->items.size()) - 1;
		}
		return c;
	};
	d->Group(sp, "Colors", 114, 39, 177, 46);
	d->Text(sp, "Text:", 119, 52, 54, 8);
	ComboBox* ctext = color_combo(50, s.sub_color);
	d->Text(sp, "Outline / box:", 119, 68, 54, 8);
	ComboBox* cout = color_combo(66, s.sub_outline_color);
	auto color_of = [palette](const ComboBox* c, uint32_t old) {
		return c->sel >= 0 && c->sel < int(palette->size()) ? (*palette)[c->sel] : old;
	};
	auto fonts_p = std::make_shared<std::vector<FontChoice>>(fonts);
	const std::string old_font = s.sub_font;
	auto font_of = [fonts_p, old_font](const ComboBox* c) {
		return c->sel >= 0 && c->sel < int(fonts_p->size()) ? (*fonts_p)[c->sel].file : old_font;
	};
	// the preview: the style as it would be drawn over a picture twice the preview's height
	struct Preview : public Control
	{
		std::function<SubtitleStyle(int ref_h)> style;
		Image img;
		std::string key;
		Preview() { tab_stop = false; }
		void Paint(CDC& dc, bool, bool) override
		{
			// a picture: a sky over a field, so the outline, the shadow and a see-through box show
			for (int y = rect.top; y < rect.bottom; y++)
			{
				const int t = (y - rect.top) * 255 / std::max(1, rect.Height());
				dc.HLine(rect.left, rect.right, y, t < 150 ? RGB(70 + t / 3, 120 + t / 4, 190 - t / 5) : RGB(60 + t / 8, 140 - t / 6, 50));
			}
			dc.FrameRect(rect, RGB(130, 135, 144));
			const SubtitleStyle st = style(rect.Height() * 2);
			char k[200];
			snprintf(k, sizeof(k), "%d|%d|%d|%d|%x|%x|%d|%d|%s|%d", st.em, st.outline, st.shadow, int(st.bold), unsigned(st.color),
				unsigned(st.outline_color), int(st.box), st.box_alpha, st.font.c_str(), rect.Width());
			if (key != k)
			{
				key = k;
				RenderSubtitleText(st, {T("The subtitles look like this."), T("Second line")}, {false, true}, rect.Width() - 8, img);
			}
			const CRect old = dc.GetClip();
			CRect clip = rect;
			clip.DeflateRect(1, 1);
			clip.Intersect(old);
			dc.SetClip(clip);
			dc.AlphaBlit(rect.left + (rect.Width() - img.Width()) / 2, rect.bottom - img.Height() - rect.Height() / 12, img.Bmp());
			dc.SetClip(old);
		}
	};
	Preview* pv = d->P(sp, new Preview, 114, 90, 177, 72);
	const uint32_t old_color = s.sub_color, old_ocolor = s.sub_outline_color;
	pv->style = [=](int ref_h) {
		SubtitleStyle st;
		st.em = std::max(8, fsize->value * ref_h / 288);
		st.bold = bold->checked;
		st.outline = ow->value > 0 ? std::max(1, ow->value * ref_h / 288) : 0;
		st.shadow = shadow->value > 0 ? std::max(1, shadow->value * ref_h / 288 / 2) : 0;
		const uint32_t c = color_of(ctext, old_color), o = color_of(cout, old_ocolor);
		st.color = RGB(c >> 16, c >> 8, c);
		st.outline_color = RGB(o >> 16, o >> 8, o);
		st.box = bbox->checked;
		st.box_alpha = bop->value * 255 / 100;
		st.own_font = true;
		st.font = font_of(font);
		return st;
	};
	d->Text(sp, "", 114, 166, 177, 72)->text =
		Fmt(T("For SRT, WebVTT and the other text formats; ASS/SSA keep their own styles. The fonts are the .ttf/.otf/.ttc "
			  "files in %s, read when this page opens (ASS/SSA: when the player starts)."),
			OrbisDir("fonts").c_str());
	d->apply_fns.push_back([&s, sstep, sdel, pvid, fsize, bold, ow, margin, font, font_of, bbox, shadow, bop, ctext, cout,
							   color_of, boutline] {
		s.sub_delay_step = sstep->value;
		s.sub_delay_ms = sdel->value;
		s.sub_position = pvid->checked ? 0 : 1;
		s.sub_font = font_of(font);
		s.sub_font_size = fsize->value;
		s.sub_bold = bold->checked;
		s.sub_border = bbox->checked ? 1 : 0;
		(void)boutline;
		s.sub_outline = ow->value;
		s.sub_shadow = shadow->value;
		s.sub_box_opacity = bop->value;
		s.sub_color = color_of(ctext, s.sub_color);
		s.sub_outline_color = color_of(cout, s.sub_outline_color);
		s.sub_margin = margin->value;
	});

	// ---- Audio Switcher (IDD_PPAGEAUDIOSWITCHER) ----
	p = d->NewPage("Audio Switcher");
	d->Check(p, "Enable built-in audio switcher filter", 5, 5, 286, 8, true, false);
	// Normalize, Max amplification, Regain volume: MPC-HC's group (the last two only with Normalize on)
	GroupBox* ngroup = d->Group(p, "", 5, 20, 286, 25);
	CheckBox* norm = d->Check(p, "Normalize", 15, 21, 120, 8, s.audio_normalize); // 68 in MPC-HC: German is longer
	ngroup->title_check = norm;
	Static* nmax_label = d->Text(p, "Max amplification:", 12, 31, 110, 8);
	NumberBox* nmax = d->Number(p, 124, 29, 50, 13, s.audio_max_norm, 100, 1000, 10, " %");
	CheckBox* regain = d->Check(p, "Regain volume", 177, 31, 108, 8, s.audio_norm_regain);
	norm->on_change = [norm, nmax_label, nmax, regain] {
		nmax_label->enabled = nmax->enabled = regain->enabled = norm->checked;
	};
	norm->on_change();
	d->Text(p, "Boost:", 5, 49, 55, 8);
	Slider* boost = d->P(p, new Slider, 60, 46, 110, 15);
	boost->max = 300;
	boost->pos = s.audio_boost;
	boost->step = 10;
	d->Text(p, "Down-mix: 5.1/7.1 tracks are mixed down to stereo, or go to the PS5's 8-channel output when Output > Audio "
			   "Renderer is set to surround.", 5, 65, 286, 16);
	d->Text(p, "Audio time shift (ms):", 5, 86, 115, 8);
	NumberBox* ashift = d->Number(p, 124, 83, 60, 13, s.audio_delay_ms, -10000, 10000, 10);
	d->apply_fns.push_back([&s, boost, ashift, norm, nmax, regain] {
		s.audio_normalize = norm->checked;
		s.audio_max_norm = std::max(100, std::min(1000, nmax->value));
		s.audio_norm_regain = regain->checked;
		s.audio_boost = boost->pos;
		s.audio_delay_ms = ashift->value;
	});

	// ---- Miscellaneous (PS5: the interface scale, the pointer, the shares) ----
	p = d->NewPage("Miscellaneous");
	d->Group(p, "Interface (PlayStation 5)", 5, 5, 286, 74);
	d->Text(p, "Size of the window's text and controls:", 10, 18, 160, 8);
	ComboBox* scale = d->Combo(p, 175, 16, 110, 13, {"100%", "125%", "150% (recommended)", "175%", "200%"},
		s.dpi == 96 ? 0 : s.dpi == 120 ? 1 : s.dpi == 168 ? 3 : s.dpi == 192 ? 4 : 2);
	d->Text(p, "Pointer speed (left stick):", 10, 36, 160, 8);
	NumberBox* speed = d->Number(p, 175, 33, 50, 14, s.cursor_speed, 1, 5);
	// the emulator ports' Settings > Debug logs: logs/boot.log in the data folder, on or off at once (off by default)
	CheckBox* logs = d->Check(p, "Debug logs", 10, 50, 276, 8, s.debug_logs);
	d->Text(p, "logs/boot.log in the data folder; on or off at once", 22, 62, 264, 8);
	d->Group(p, "Network locations", 5, 83, 286, 107);
	ListView* servers = d->P(p, new ListView, 10, 95, 220, 89);
	servers->columns = {{T("Name"), servers->rect.Width() * 40 / 100}, {T("Address"), servers->rect.Width() * 58 / 100}};
	auto fill_servers = [&s, servers] {
		servers->rows.clear();
		for (const auto& v : s.servers)
			servers->rows.push_back({v.name, HideSecret(v.url)});
		if (servers->sel >= int(servers->rows.size()))
			servers->sel = int(servers->rows.size()) - 1;
	};
	fill_servers();
	Button* addsrv = d->P(p, new Button, 235, 95, 50, 14);
	addsrv->text = T("Add...");
	Button* delsrv = d->P(p, new Button, 235, 113, 50, 14);
	delsrv->text = T("Delete");
	addsrv->on_click = [&s, fill_servers] {
		ShowAddNetworkDialog([&s, fill_servers](const std::string& n, const std::string& u) {
			s.servers.push_back({n, u});
			fill_servers();
		});
	};
	delsrv->on_click = [&s, servers, fill_servers] {
		if (servers->sel >= 0 && servers->sel < int(s.servers.size()))
			s.servers.erase(s.servers.begin() + servers->sel);
		fill_servers();
	};
	d->Group(p, "Settings management", 5, 194, 286, 28);
	Button* reset = d->P(p, new Button, 10, 204, 86, 14);
	reset->text = T("Reset");
	reset->on_click = [o] {
		MessageBox(T("Restore all the settings to their defaults? (Recent files, favorites and network locations are kept.)"),
			T("Options"), MB_YESNO, 4, [o](int r) {
				if (r != IDYES)
					return;
				Settings& st = AppSettings();
				Settings fresh;
				fresh.recent = st.recent;
				fresh.favorites = st.favorites;
				fresh.servers = st.servers;
				fresh.positions = st.positions;
				st = fresh;
				o->EndDialog(1);
				if (o->applied)
					o->applied();
			});
	};
	d->apply_fns.push_back([&s, scale, speed, logs] {
		static const int dpis[] = {96, 120, 144, 168, 192};
		s.dpi = dpis[std::max(0, std::min(4, scale->sel))];
		s.cursor_speed = speed->value;
		s.debug_logs = logs->checked;
	});

	// the buttons
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(270, 270, 50, 14);
	Button* cancel = d->Add(new Button);
	cancel->text = T("Cancel");
	cancel->rect = d->DuRect(326, 270, 50, 14);
	Button* apply = d->Add(new Button);
	apply->text = T("&Apply");
	apply->rect = d->DuRect(382, 270, 50, 14);
	ok->on_click = [o] {
		o->Apply();
		o->EndDialog(1);
	};
	cancel->on_click = [o] { o->EndDialog(2); };
	apply->on_click = [o] { o->Apply(); };
	d->tree->sel = 0;
	d->tree->on_select = [o] { o->ShowPage(o->tree->sel); };
	d->ShowPage(0);
	WindowManager::Get().Push(d);
	d->SetFocus(d->tree);
}

// ===========================================================================================================
// Favorites
void ShowAddFavoriteDialog(const std::string& name, std::function<void(const std::string&, bool)> done)
{
	struct Fav : public Dialog
	{
		ComboBox* name = nullptr;
		CheckBox* pos = nullptr;
		std::function<void(const std::string&, bool)> done;
	};
	Fav* d = new Fav;
	d->title = T("Add Favorite");
	d->done = std::move(done);
	d->rect = CRect(0, 0, Dialog::DuX(252) + 2, Dialog::DuY(84) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	GroupBox* g = d->Add(new GroupBox);
	g->text = T("Choose a name for your shortcut:");
	g->rect = d->DuRect(5, 5, 242, 29);
	d->name = d->Add(new ComboBox);
	d->name->editable = true;
	d->name->text = name;
	d->name->rect = d->DuRect(10, 17, 232, 13);
	d->pos = d->Add(new CheckBox);
	d->pos->text = T("Remember position");
	d->pos->checked = true;
	d->pos->rect = d->DuRect(10, 38, 232, 8);
	CheckBox* rel = d->Add(new CheckBox);
	rel->text = T("Relative drive");
	rel->enabled = false;
	rel->rect = d->DuRect(10, 50, 232, 10);
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(143, 62, 50, 14);
	Button* cancel = d->Add(new Button);
	cancel->text = T("Cancel");
	cancel->rect = d->DuRect(197, 62, 50, 14);
	Fav* dd = d;
	ok->on_click = [dd] {
		auto cb = dd->done;
		const std::string n = dd->name->Text();
		const bool p = dd->pos->checked;
		dd->EndDialog(1);
		if (cb && !n.empty())
			cb(n, p);
	};
	cancel->on_click = [dd] { dd->EndDialog(2); };
	WindowManager::Get().Push(d);
}

void ShowOrganizeFavoritesDialog()
{
	struct Org : public Dialog
	{
		ListView* list = nullptr;
		void Fill()
		{
			list->rows.clear();
			for (const auto& f : AppSettings().favorites)
				list->rows.push_back({f.name, f.pos > 0 ? FormatTime(f.pos) : ""});
			if (list->sel >= int(list->rows.size()))
				list->sel = int(list->rows.size()) - 1;
			if (list->sel < 0 && !list->rows.empty())
				list->sel = 0;
		}
	};
	Org* d = new Org;
	d->title = T("Organize Favorites");
	d->rect = CRect(0, 0, Dialog::DuX(276) + 2, Dialog::DuY(170) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	TabControl* tabs = d->Add(new TabControl);
	tabs->tabs = {T("Files"), T("DVDs"), T("Devices")};
	tabs->rect = d->DuRect(5, 5, 205, 160);
	tabs->tab_stop = false;
	d->list = d->Add(new ListView);
	d->list->header = false;
	d->list->rect = d->DuRect(8, 21, 199, 141);
	d->list->columns = {{"", d->list->rect.Width() * 75 / 100}, {"", d->list->rect.Width() * 23 / 100, true}};
	Button* ren = d->Add(new Button);
	ren->text = T("Rename");
	ren->rect = d->DuRect(214, 17, 58, 14);
	Button* up = d->Add(new Button);
	up->text = T("Move Up");
	up->rect = d->DuRect(214, 43, 58, 14);
	Button* dn = d->Add(new Button);
	dn->text = T("Move Down");
	dn->rect = d->DuRect(214, 58, 58, 14);
	Button* del = d->Add(new Button);
	del->text = T("Delete");
	del->rect = d->DuRect(214, 84, 58, 14);
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(214, 151, 58, 14);
	Org* dd = d;
	auto& fav = AppSettings().favorites;
	ren->on_click = [dd, &fav] {
		const int i = dd->list->sel;
		if (i < 0 || i >= int(fav.size()))
			return;
		OnScreenKeyboard(T("Rename"), fav[i].name, false, [dd, i, &fav](bool okk, const std::string& t) {
			if (okk && !t.empty() && i < int(fav.size()))
				fav[i].name = t;
			dd->Fill();
		});
	};
	up->on_click = [dd, &fav] {
		const int i = dd->list->sel;
		if (i > 0 && i < int(fav.size()))
		{
			std::swap(fav[i], fav[i - 1]);
			dd->list->sel--;
			dd->Fill();
		}
	};
	dn->on_click = [dd, &fav] {
		const int i = dd->list->sel;
		if (i >= 0 && i + 1 < int(fav.size()))
		{
			std::swap(fav[i], fav[i + 1]);
			dd->list->sel++;
			dd->Fill();
		}
	};
	del->on_click = [dd, &fav] {
		const int i = dd->list->sel;
		if (i >= 0 && i < int(fav.size()))
		{
			fav.erase(fav.begin() + i);
			dd->Fill();
		}
	};
	ok->on_click = [dd] {
		AppSettings().Save();
		dd->EndDialog(1);
	};
	d->Fill();
	WindowManager::Get().Push(d);
}

// ===========================================================================================================
// Add a network location (Windows' wizard, in one page)
void ShowAddNetworkDialog(std::function<void(const std::string&, const std::string&)> done)
{
	struct Net : public Dialog
	{
		Edit *host = nullptr, *share = nullptr, *user = nullptr, *pass = nullptr, *name = nullptr;
		std::function<void(const std::string&, const std::string&)> done;
	};
	Net* d = new Net;
	d->title = T("Add Network Location");
	d->done = std::move(done);
	d->rect = CRect(0, 0, Dialog::DuX(280) + 2, Dialog::DuY(176) + d->CaptionHeight() + 1);
	d->Center(WindowManager::Get().screen);
	Static* intro = d->Add(new Static);
	intro->text = T("Specify the location of your network share (a shared folder on a Windows PC, a NAS, Samba). "
					"The share and the user are optional: without a share the server's shares are listed.");
	intro->rect = d->DuRect(7, 7, 266, 26);
	auto row = [&](const char* label, int y, Edit*& e, const char* kb) {
		Static* l = d->Add(new Static);
		l->text = T(label);
		l->flags = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
		l->rect = d->DuRect(7, y, 80, 14);
		e = d->Add(new Edit);
		e->rect = d->DuRect(92, y, 181, 14);
		e->keyboard_title = T(kb);
	};
	row("Server (name or IP):", 40, d->host, "Server name or IP address (e.g. 192.168.0.10)");
	row("Share (optional):", 60, d->share, "Share name (e.g. Videos)");
	row("User name:", 80, d->user, "User name (empty: guest)");
	row("Password:", 100, d->pass, "Password");
	d->pass->password = true;
	row("Name for this location:", 120, d->name, "Name shown in the browser");
	d->host->cue = "192.168.0.10";
	d->share->cue = "Videos";
	d->user->cue = "guest";
	Button* ok = d->Add(new Button);
	ok->text = T("OK");
	ok->is_default = true;
	ok->rect = d->DuRect(169, 154, 50, 14);
	Button* cancel = d->Add(new Button);
	cancel->text = T("Cancel");
	cancel->rect = d->DuRect(223, 154, 50, 14);
	Net* dd = d;
	ok->on_click = [dd] {
		std::string host = dd->host->text;
		if (host.compare(0, 6, "smb://") == 0)
			host = host.substr(6);
		while (!host.empty() && (host.back() == '/' || host.back() == '\\'))
			host.pop_back();
		for (char& c : host)
			if (c == '\\')
				c = '/';
		if (host.empty())
		{
			MessageBox(T("Type the server's name or IP address."), dd->title, MB_OK, 2);
			return;
		}
		auto esc = [](const std::string& s) {
			std::string o;
			for (char c : s)
			{
				if (c == '@' || c == ':' || c == '/' || c == '%' || c == ';')
				{
					char b[4];
					snprintf(b, sizeof(b), "%%%02X", static_cast<unsigned char>(c));
					o += b;
				}
				else
					o += c;
			}
			return o;
		};
		std::string url = "smb://";
		if (!dd->user->text.empty())
		{
			url += esc(dd->user->text);
			if (!dd->pass->text.empty())
				url += ":" + esc(dd->pass->text);
			url += "@";
		}
		url += host;
		if (!dd->share->text.empty())
			url += "/" + dd->share->text;
		std::string n = dd->name->text;
		if (n.empty())
			n = dd->share->text.empty() ? host : dd->share->text + " (" + host + ")";
		auto cb = dd->done;
		dd->EndDialog(1);
		if (cb)
			cb(n, url);
	};
	cancel->on_click = [dd] { dd->EndDialog(2); };
	WindowManager::Get().Push(d);
	d->SetFocus(d->host);
}
} // namespace mpc
