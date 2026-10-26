// Media Player Classic PS5 1.4: disc drives (mpc_disc.h).
//
// Disc reading after 5S2CD by The7mirrors, a fork of PS5SX2, GPL-3.0 (ProsperoDisc.cpp: /dev/cdN, 2048-byte pread()s,
// DIOCGMEDIASIZE, DVDIOCREADSTRUCTURE, CDIOREADTOCHEADER, a disc being there when its sector 16 reads; its 64-sector
// read-ahead).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_disc.h"

#include "mpc_i18n.h"

#include "OrbisPaths.h"

extern "C" {
#include <dvdread/dvd_filesystem.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libbluray/bluray.h>
#include <libbluray/log_control.h>
#include <udfread/blockinput.h>
#include <udfread/udfread.h>
}

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <sys/cdio.h>
#include <sys/disk.h>
#include <sys/dvdio.h>
#include <sys/ioctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace mpc
{
namespace disc
{
namespace
{
constexpr uint32_t kSector = 2048;
constexpr uint32_t kChunk = 64; // sectors in a cache chunk: 128 KiB, read in one go (5S2CD reads 64 ahead too)
constexpr size_t kChunks = 48; // 6 MiB of chunks per disc
constexpr int kAhead = 2; // chunks read ahead while reading goes on in order
constexpr size_t kMaxNodes = 100000; // a file system bigger than this is listed in part

double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(static_cast<unsigned char>(c)));
	return s;
}

bool StartsWith(const std::string& s, const char* p)
{
	return s.compare(0, strlen(p), p) == 0;
}

// UCS-2 big-endian (Joliet) -> UTF-8
std::string Ucs2ToUtf8(const uint8_t* p, size_t n)
{
	std::string out;
	for (size_t i = 0; i + 1 < n; i += 2)
	{
		uint32_t c = (uint32_t(p[i]) << 8) | p[i + 1];
		if (c >= 0xD800 && c <= 0xDBFF && i + 3 < n)
		{
			const uint32_t lo = (uint32_t(p[i + 2]) << 8) | p[i + 3];
			if (lo >= 0xDC00 && lo <= 0xDFFF)
			{
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (c < 0x80)
			out += char(c);
		else if (c < 0x800)
		{
			out += char(0xC0 | (c >> 6));
			out += char(0x80 | (c & 0x3F));
		}
		else if (c < 0x10000)
		{
			out += char(0xE0 | (c >> 12));
			out += char(0x80 | ((c >> 6) & 0x3F));
			out += char(0x80 | (c & 0x3F));
		}
		else
		{
			out += char(0xF0 | (c >> 18));
			out += char(0x80 | ((c >> 12) & 0x3F));
			out += char(0x80 | ((c >> 6) & 0x3F));
			out += char(0x80 | (c & 0x3F));
		}
	}
	return out;
}

std::string TrimRight(std::string s)
{
	while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
		s.pop_back();
	return s;
}

// ---------------------------------------------------------------------------------------------------------------
// a file's place on the disc
struct Extent
{
	uint64_t file_off = 0; // where in the file it starts
	uint64_t lba = 0; // its first sector
	uint64_t bytes = 0;
};

// one entry of the file system, read when the disc went in
struct Node
{
	std::string name;
	bool dir = false;
	uint64_t size = 0;
	int64_t mtime = 0;
	std::vector<Extent> extents; // ISO 9660 (UDF files are mapped through libudfread when opened)
	std::vector<int> kids;
};

#ifndef __PROSPERO__
// host tests: the drives' images and the "eject" switch
std::mutex g_host_mu;
std::map<std::string, bool> g_host_out;
bool HostOut(const std::string& id)
{
	std::lock_guard<std::mutex> l(g_host_mu);
	auto it = g_host_out.find(id);
	return it != g_host_out.end() && it->second;
}
#endif

void Poke();

// ---------------------------------------------------------------------------------------------------------------
// One disc in one drive: the device, a cache of chunks with read-ahead, the file system.
class Media
{
public:
	std::string id, dev;
	int fd = -1;
	uint64_t sectors = 0;
	uint64_t gen = 0;
	std::atomic<bool> gone{false};
	std::atomic<double> last_io{0};
	// the file system
	std::mutex udf_mu; // libudfread isn't for several threads at once
	udfread* udf = nullptr;
	std::vector<Node> nodes; // [0]: the root
	std::string fs, label;
	bool cut = false; // more entries than kMaxNodes

	Media(std::string i, std::string d, int f, uint64_t g) : id(std::move(i)), dev(std::move(d)), fd(f), gen(g)
	{
		chunks_.resize(kChunks);
		for (auto& c : chunks_)
			c.data.resize(size_t(kChunk) * kSector);
	}
	~Media()
	{
		{
			std::lock_guard<std::mutex> l(cmu_);
			stop_ = true;
			ccv_.notify_all();
		}
		if (ahead_.joinable())
			ahead_.join();
		if (udf)
			udfread_close(udf);
		if (fd >= 0)
			close(fd);
	}

	// sectors [lba, lba + count) into out; false when the disc can't be read there (or is gone)
	bool Read(uint64_t lba, uint32_t count, uint8_t* out)
	{
		while (count > 0)
		{
			if (gone)
				return false;
			const uint64_t idx = lba / kChunk;
			const uint32_t in = uint32_t(lba % kChunk);
			const uint32_t n = std::min<uint32_t>(count, kChunk - in);
			if (!ReadFromChunk(idx, in, n, out))
			{
				// the chunk couldn't be read whole (the disc's end, a bad sector): exactly what was asked for
				if (!DeviceRead(lba, n, out))
					return false;
			}
			lba += n;
			count -= n;
			out += size_t(n) * kSector;
		}
		return true;
	}

	// the watcher's "still there?": a readable disc by its sector 16 straight from the drive, one without a file system
	// (an audio CD) by its table of contents
	bool Probe()
	{
#ifdef __PROSPERO__
		if (fs.empty())
		{
			ioc_toc_header th = {};
			return ioctl(fd, CDIOREADTOCHEADER, &th) == 0 && th.ending_track >= th.starting_track && th.starting_track > 0;
		}
#endif
		uint8_t s[kSector];
		return DeviceRead(16, 1, s, false);
	}

private:
	struct Chunk
	{
		uint64_t index = UINT64_MAX;
		std::vector<uint8_t> data;
		uint32_t count = 0; // sectors in it
		bool loading = false;
		uint64_t stamp = 0;
	};
	std::mutex cmu_;
	std::condition_variable ccv_;
	std::vector<Chunk> chunks_;
	std::deque<uint64_t> want_;
	std::thread ahead_;
	bool stop_ = false;
	uint64_t clock_ = 0;
	uint64_t last_idx_ = UINT64_MAX;

	bool DeviceRead(uint64_t lba, uint32_t count, uint8_t* out, bool check = true)
	{
#ifndef __PROSPERO__
		if (HostOut(id))
		{
			gone = true;
			return false;
		}
#endif
		last_io = Now();
		const size_t want = size_t(count) * kSector;
		size_t got = 0;
		int tries = 0;
		while (got < want)
		{
			const ssize_t r = pread(fd, out + got, want - got, off_t(lba * kSector + got));
			if (r > 0)
			{
				got += size_t(r);
				continue;
			}
			if (r < 0 && errno == EINTR)
				continue;
			const int e = r < 0 ? errno : 0;
			if (++tries < 2 && r < 0)
				continue; // once more (a drive spinning up)
			if (check)
			{
				OrbisLog("[disc] %s: read of sectors %llu-%llu failed: %s", id.c_str(), (unsigned long long)lba,
					(unsigned long long)(lba + count - 1), e ? strerror(e) : "end of the disc");
				// is the disc still there? (sector 16 always reads on a readable disc)
				uint8_t s[kSector];
				if (!DeviceRead(16, 1, s, false))
				{
					gone = true;
					Poke();
				}
			}
			return false;
		}
		return true;
	}

	Chunk* Find(uint64_t idx)
	{
		for (auto& c : chunks_)
			if (c.index == idx)
				return &c;
		return nullptr;
	}

	Chunk* Victim()
	{
		Chunk* v = nullptr;
		for (auto& c : chunks_)
			if (!c.loading && (!v || c.stamp < v->stamp))
				v = &c;
		return v;
	}

	// loads chunk idx into a free slot (the lock held on entry and on return; the drive is read without it)
	Chunk* Load(std::unique_lock<std::mutex>& l, uint64_t idx)
	{
		Chunk* c = Victim();
		if (!c)
			return nullptr;
		c->index = idx;
		c->loading = true;
		c->stamp = ++clock_;
		const uint64_t first = idx * kChunk;
		const uint32_t n = uint32_t(std::min<uint64_t>(kChunk, sectors > first ? sectors - first : 0));
		l.unlock();
		const bool ok = n > 0 && DeviceRead(first, n, c->data.data());
		l.lock();
		c->loading = false;
		c->count = ok ? n : 0;
		if (!ok)
			c->index = UINT64_MAX;
		ccv_.notify_all();
		return ok ? c : nullptr;
	}

	bool ReadFromChunk(uint64_t idx, uint32_t in, uint32_t n, uint8_t* out)
	{
		std::unique_lock<std::mutex> l(cmu_);
		Chunk* c = Find(idx);
		while (c && c->loading)
		{
			ccv_.wait(l);
			c = Find(idx);
		}
		if (!c)
			c = Load(l, idx);
		if (!c || in + n > c->count)
			return false;
		memcpy(out, c->data.data() + size_t(in) * kSector, size_t(n) * kSector);
		c->stamp = ++clock_;
		// in order (a film playing): the next chunks in the background
		if (idx == last_idx_ + 1 || idx == last_idx_)
		{
			for (int k = 1; k <= kAhead; k++)
			{
				const uint64_t a = idx + uint64_t(k);
				if (a * kChunk < sectors && !Find(a) && std::find(want_.begin(), want_.end(), a) == want_.end())
					want_.push_back(a);
			}
			if (!want_.empty())
			{
				if (!ahead_.joinable())
					ahead_ = std::thread([this] { AheadThread(); });
				ccv_.notify_all();
			}
		}
		last_idx_ = idx;
		return true;
	}

	void AheadThread()
	{
		std::unique_lock<std::mutex> l(cmu_);
		while (!stop_)
		{
			if (want_.empty() || gone)
			{
				ccv_.wait(l);
				continue;
			}
			const uint64_t idx = want_.front();
			want_.pop_front();
			if (Find(idx))
				continue;
			Load(l, idx);
		}
	}
};

// libudfread's view of a disc
struct UdfInput
{
	udfread_block_input in;
	Media* m;
};

int UdfRead(udfread_block_input* in, uint32_t lba, void* buf, uint32_t n, int)
{
	Media* m = reinterpret_cast<UdfInput*>(in)->m;
	return m->Read(lba, n, static_cast<uint8_t*>(buf)) ? int(n) : -1;
}

uint32_t UdfSize(udfread_block_input* in)
{
	return uint32_t(reinterpret_cast<UdfInput*>(in)->m->sectors);
}

int UdfClose(udfread_block_input* in)
{
	delete reinterpret_cast<UdfInput*>(in);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// reading the file system into Media::nodes
int AddNode(Media& m, int parent, Node n)
{
	if (m.nodes.size() >= kMaxNodes)
	{
		m.cut = true;
		return -1;
	}
	m.nodes.push_back(std::move(n));
	const int i = int(m.nodes.size()) - 1;
	if (parent >= 0)
		m.nodes[parent].kids.push_back(i);
	return i;
}

void UdfWalk(Media& m, const std::string& path, int node, int depth)
{
	if (depth > 32)
		return;
	UDFDIR* d = udfread_opendir(m.udf, path.empty() ? "/" : path.c_str());
	if (!d)
		return;
	std::vector<std::pair<std::string, bool>> list;
	udfread_dirent e;
	while (udfread_readdir(d, &e))
	{
		if (!e.d_name || !strcmp(e.d_name, ".") || !strcmp(e.d_name, ".."))
			continue;
		list.push_back({e.d_name, e.d_type == UDF_DT_DIR});
	}
	for (const auto& x : list)
	{
		Node n;
		n.name = x.first;
		n.dir = x.second;
		const std::string p = path + "/" + x.first;
		if (!n.dir)
		{
			if (UDFFILE* f = udfread_file_openat(d, x.first.c_str()))
			{
				n.size = uint64_t(std::max<int64_t>(0, udfread_file_size(f)));
				udfread_file_close(f);
			}
		}
		const int i = AddNode(m, node, std::move(n));
		if (i < 0)
			break;
		if (x.second)
			UdfWalk(m, p, i, depth + 1);
	}
	udfread_closedir(d);
}

int64_t IsoTime(const uint8_t* t)
{
	struct tm tmv = {};
	tmv.tm_year = t[0];
	tmv.tm_mon = t[1] - 1;
	tmv.tm_mday = t[2];
	tmv.tm_hour = t[3];
	tmv.tm_min = t[4];
	tmv.tm_sec = t[5];
	if (tmv.tm_mon < 0 || tmv.tm_mday <= 0)
		return 0;
	return int64_t(timegm(&tmv)) - int64_t(int8_t(t[6])) * 15 * 60;
}

void IsoWalk(Media& m, uint64_t lba, uint64_t bytes, int node, bool joliet, int depth)
{
	if (depth > 32 || bytes > (64u << 20))
		return;
	const uint32_t n = uint32_t((bytes + kSector - 1) / kSector);
	std::vector<uint8_t> buf(size_t(n) * kSector);
	if (!m.Read(lba, n, buf.data()))
		return;
	size_t p = 0;
	std::string last_name;
	int last = -1;
	while (p < buf.size())
	{
		const uint8_t len = buf[p];
		if (len == 0)
		{
			p = (p / kSector + 1) * kSector; // records don't cross sectors
			continue;
		}
		if (len < 34 || p + len > buf.size())
			break;
		const uint8_t* r = &buf[p];
		p += len;
		const uint8_t nl = r[32];
		if (33u + nl > len)
			break;
		if (nl == 1 && (r[33] == 0 || r[33] == 1))
			continue; // . and ..
		uint32_t at, size;
		memcpy(&at, r + 2, 4);
		memcpy(&size, r + 10, 4);
		const uint8_t flags = r[25];
		std::string name = joliet ? Ucs2ToUtf8(r + 33, nl) : std::string(reinterpret_cast<const char*>(r + 33), nl);
		const size_t semi = name.find(';');
		if (semi != std::string::npos)
			name.resize(semi);
		if (!joliet && !name.empty() && name.back() == '.')
			name.pop_back();
		const bool dir = (flags & 2) != 0;
		// a file in several extents (multi-extent: the same name again)
		if (!dir && last >= 0 && name == last_name && !m.nodes[last].dir)
		{
			Node& prev = m.nodes[last];
			prev.extents.push_back({prev.size, at, size});
			prev.size += size;
			continue;
		}
		Node nd;
		nd.name = name;
		nd.dir = dir;
		nd.size = dir ? 0 : size;
		nd.mtime = IsoTime(r + 18);
		if (!dir)
			nd.extents.push_back({0, at, size});
		const int i = AddNode(m, node, std::move(nd));
		if (i < 0)
			break;
		last = i;
		last_name = name;
		if (dir)
			IsoWalk(m, at, size, i, joliet, depth + 1);
	}
}

int FindNode(const Media& m, const std::string& path)
{
	if (m.nodes.empty())
		return -1;
	int cur = 0;
	size_t p = 0;
	while (p < path.size())
	{
		size_t e = path.find('/', p);
		if (e == std::string::npos)
			e = path.size();
		const std::string part = path.substr(p, e - p);
		p = e + 1;
		if (part.empty())
			continue;
		int found = -1;
		for (int k : m.nodes[cur].kids)
			if (m.nodes[k].name == part)
			{
				found = k;
				break;
			}
		if (found < 0)
		{
			const std::string lp = Lower(part);
			for (int k : m.nodes[cur].kids)
				if (Lower(m.nodes[k].name) == lp)
				{
					found = k;
					break;
				}
		}
		if (found < 0)
			return -1;
		cur = found;
	}
	return cur;
}

// a file's extents: ISO 9660 from its records, UDF through libudfread (block by block, a run at a time)
bool FileExtents(Media& m, const std::string& path, std::vector<Extent>& out, uint64_t* size)
{
	const int n = FindNode(m, path);
	if (n < 0 || m.nodes[n].dir)
		return false;
	*size = m.nodes[n].size;
	if (!m.udf)
	{
		out = m.nodes[n].extents;
		return true;
	}
	std::lock_guard<std::mutex> l(m.udf_mu);
	UDFFILE* f = udfread_file_open(m.udf, ("/" + path).c_str());
	if (!f)
		return false;
	const int64_t sz = udfread_file_size(f);
	*size = uint64_t(std::max<int64_t>(0, sz));
	const uint32_t blocks = uint32_t((*size + kSector - 1) / kSector);
	uint32_t b = 0;
	bool ok = true;
	while (b < blocks)
	{
		const uint32_t lba = udfread_file_lba(f, b);
		if (lba == 0)
		{
			ok = false; // data inside the file entry (a tiny file), or unrecorded: read through libudfread
			break;
		}
		// how far the run goes: double the step while it holds, then halve it
		uint32_t run = 1, step = 1;
		while (b + run < blocks)
		{
			const uint32_t s = std::min(step, blocks - b - run);
			if (udfread_file_lba(f, b + run + s - 1) == lba + run + s - 1 &&
				(s == 1 || udfread_file_lba(f, b + run) == lba + run))
			{
				run += s;
				step *= 2;
			}
			else if (step > 1)
				step /= 2;
			else
				break;
		}
		const uint64_t off = uint64_t(b) * kSector;
		out.push_back({off, lba, std::min<uint64_t>(uint64_t(run) * kSector, *size - off)});
		b += run;
	}
	udfread_file_close(f);
	return ok;
}

// the whole of a small file, through libudfread
bool UdfReadAll(Media& m, const std::string& path, std::vector<uint8_t>& out, size_t max_bytes)
{
	std::lock_guard<std::mutex> l(m.udf_mu);
	UDFFILE* f = udfread_file_open(m.udf, ("/" + path).c_str());
	if (!f)
		return false;
	const int64_t sz = udfread_file_size(f);
	bool ok = sz >= 0 && size_t(sz) <= max_bytes;
	if (ok)
	{
		out.resize(size_t(sz));
		ok = sz == 0 || udfread_file_read(f, out.data(), size_t(sz)) == sz;
	}
	udfread_file_close(f);
	return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// the drives
struct Slot
{
	std::string id, dev;
	bool usb = false;
	std::shared_ptr<Media> media;
	DriveState state;
	double next_probe = 0;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<Slot> g_slots;
std::thread g_thread;
bool g_stop = false, g_poke = false, g_started = false;
uint64_t g_gen = 0;

void Poke()
{
	std::lock_guard<std::mutex> l(g_mu);
	g_poke = true;
	g_cv.notify_all();
}

std::shared_ptr<Media> MediaOf(const std::string& id)
{
	std::lock_guard<std::mutex> l(g_mu);
	for (auto& s : g_slots)
		if (s.id == id)
			return s.media && !s.media->gone ? s.media : nullptr;
	return nullptr;
}

// the PES scrambling bits CSS sets, in a run of sectors (a pack: 00 00 01 BA; the first PES after its header)
bool Scrambled(const uint8_t* s, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++, s += kSector)
	{
		if (s[0] || s[1] || s[2] != 1 || s[3] != 0xBA)
			continue;
		const uint32_t p = 14 + (s[13] & 7);
		if (p + 7 > kSector || s[p] || s[p + 1] || s[p + 2] != 1)
			continue;
		const uint8_t sid = s[p + 3];
		if ((sid >= 0xE0 && sid <= 0xEF) || (sid >= 0xC0 && sid <= 0xDF) || sid == 0xBD)
			if (s[p + 6] & 0x30)
				return true;
	}
	return false;
}

// a few runs of sectors across a VOB file: any marked scrambled?
template <typename ReadAt>
bool VobScrambled(uint64_t sectors, ReadAt read)
{
	if (sectors == 0)
		return false;
	std::vector<uint8_t> buf(16 * kSector);
	const uint64_t at[] = {0, std::min<uint64_t>(64, sectors - 1), sectors / 4, sectors / 2, sectors * 3 / 4};
	for (uint64_t a : at)
	{
		const uint32_t n = uint32_t(std::min<uint64_t>(16, sectors - a));
		if (n && read(a, n, buf.data()) && Scrambled(buf.data(), n))
			return true;
	}
	return false;
}

void Classify(Media& m, DriveState& st)
{
	auto has = [&](const char* p) { return FindNode(m, p) >= 0; };
	if (has("VIDEO_TS/VIDEO_TS.IFO"))
	{
		st.kind = Kind::DvdVideo;
#ifdef __PROSPERO__
		// the drive's copyright information (lead-in): CSS / CPPM, CPRM
		dvd_struct ds = {};
		ds.format = DVD_STRUCT_COPYRIGHT;
		if (ioctl(m.fd, DVDIOCREADSTRUCTURE, &ds) == 0 && ds.cpst != 0)
		{
			OrbisLog("[disc] %s: copyright protection system %u (the drive says so)", m.id.c_str(), ds.cpst);
			st.protection = ds.cpst == 2 ? "CPRM" : "CSS";
		}
#endif
		// the title VOBs' packs, sampled
		const int vt = FindNode(m, "VIDEO_TS");
		if (st.protection.empty() && vt >= 0)
			for (int k : m.nodes[vt].kids)
			{
				const std::string n = Lower(m.nodes[k].name);
				if (n.size() != 12 || n.compare(0, 4, "vts_") || n.compare(6, 6, "_1.vob"))
					continue;
				std::vector<Extent> ext;
				uint64_t size = 0;
				if (!FileExtents(m, "VIDEO_TS/" + m.nodes[k].name, ext, &size))
					continue;
				int refused = 0;
				auto read = [&](uint64_t s, uint32_t cnt, uint8_t* out) {
					const uint64_t off = s * kSector;
					for (const Extent& e : ext)
						if (off >= e.file_off && off + uint64_t(cnt) * kSector <= e.file_off + ((e.bytes + kSector - 1) / kSector) * kSector)
						{
							const bool ok = m.Read(e.lba + (off - e.file_off) / kSector, cnt, out);
							refused += !ok && !m.gone;
							return ok;
						}
					return false;
				};
				if (VobScrambled(size / kSector, read))
				{
					OrbisLog("[disc] %s: %s has scrambled packs", m.id.c_str(), m.nodes[k].name.c_str());
					st.protection = "CSS";
					break;
				}
				// a drive refuses to read CSS-scrambled sectors before the (never done) authentication: a title VOB
				// that can't be read at all while the rest of the disc can
				if (refused >= 3)
				{
					OrbisLog("[disc] %s: the drive won't read %s (scrambled sectors, CSS)", m.id.c_str(), m.nodes[k].name.c_str());
					st.protection = "CSS";
					break;
				}
			}
		if (!st.protection.empty())
			st.kind = Kind::Encrypted;
	}
	else if (has("BDMV/index.bdmv") || has("BDMV/MovieObject.bdmv"))
	{
		st.kind = Kind::BluRay;
		if (has("AACS/Unit_Key_RO.inf") || has("AACS/MKB_RO.inf"))
			st.protection = "AACS";
		else if (has("BDSVM/00000.svm"))
			st.protection = "BD+";
		if (!st.protection.empty())
			st.kind = Kind::Encrypted;
	}
	else
		st.kind = Kind::Data;
}

// reads a disc that was put in: its size, its file system, what it is
void Mount(Media& m, DriveState& st)
{
	uint64_t bytes = 0;
#ifdef __PROSPERO__
	off_t mb = 0;
	if (ioctl(m.fd, DIOCGMEDIASIZE, &mb) == 0 && mb > 0)
		bytes = uint64_t(mb);
#else
	struct stat sb = {};
	if (fstat(m.fd, &sb) == 0)
		bytes = uint64_t(sb.st_size);
#endif
	m.sectors = bytes / kSector;
	// the volume descriptors from sector 16: ISO 9660 (+ Joliet), and the UDF recognition sequence
	uint8_t s[kSector];
	uint64_t iso_root = 0, iso_root_len = 0, jol_root = 0, jol_root_len = 0;
	std::string iso_label, jol_label;
	bool udf = false;
	uint64_t vol_sectors = 0;
	for (uint64_t sec = 16; sec < 16 + 64; sec++)
	{
		if (!m.Read(sec, 1, s))
			break;
		if (!memcmp(s + 1, "CD001", 5))
		{
			if (s[0] == 1 || (s[0] == 2 && s[88] == '%' && s[89] == '/' && (s[90] == '@' || s[90] == 'C' || s[90] == 'E')))
			{
				uint32_t at, len, vs;
				memcpy(&at, s + 156 + 2, 4);
				memcpy(&len, s + 156 + 10, 4);
				memcpy(&vs, s + 80, 4);
				vol_sectors = std::max<uint64_t>(vol_sectors, vs);
				if (s[0] == 1)
				{
					iso_root = at;
					iso_root_len = len;
					iso_label = TrimRight(std::string(reinterpret_cast<char*>(s + 40), 32));
				}
				else
				{
					jol_root = at;
					jol_root_len = len;
					jol_label = TrimRight(Ucs2ToUtf8(s + 40, 32));
				}
			}
			continue;
		}
		if (!memcmp(s + 1, "NSR02", 5) || !memcmp(s + 1, "NSR03", 5))
			udf = true;
		if (!memcmp(s + 1, "BEA01", 5) || !memcmp(s + 1, "NSR0", 4) || !memcmp(s + 1, "TEA01", 5) || !memcmp(s + 1, "CD001", 5))
			continue;
		if (s[0] == 0 && !iso_root && !udf)
			continue; // the gap before the descriptors on some discs
		break;
	}
	if (!m.sectors)
		m.sectors = vol_sectors;
	m.nodes.clear();
	m.nodes.push_back(Node{"", true, 0, 0, {}, {}});
	if (udf)
	{
		UdfInput* in = new UdfInput;
		in->in.close = UdfClose;
		in->in.read = UdfRead;
		in->in.size = m.sectors ? UdfSize : nullptr;
		in->m = &m;
		m.udf = udfread_init();
		if (m.udf && udfread_open_input(m.udf, &in->in) >= 0)
		{
			m.fs = "UDF";
			const char* v = udfread_get_volume_id(m.udf);
			m.label = v ? TrimRight(v) : "";
			UdfWalk(m, "", 0, 0);
		}
		else
		{
			// (a failed open leaves the input to its caller)
			if (m.udf)
				udfread_close(m.udf);
			delete in;
			m.udf = nullptr;
		}
	}
	if (!m.udf && (jol_root || iso_root))
	{
		const bool j = jol_root != 0;
		m.fs = j ? "ISO 9660 + Joliet" : "ISO 9660";
		m.label = j && !jol_label.empty() ? jol_label : iso_label;
		IsoWalk(m, j ? jol_root : iso_root, j ? jol_root_len : iso_root_len, 0, j, 0);
	}
	st.fs = m.fs;
	st.label = m.label;
	st.sectors = m.sectors;
	if (m.fs.empty())
	{
		st.kind = Kind::Unreadable;
		OrbisLog("[disc] %s: no file system this player reads (%llu sectors)", m.id.c_str(), (unsigned long long)m.sectors);
		return;
	}
	Classify(m, st);
	static const char* const kKinds[] = {"no disc", "reading", "data", "DVD-Video", "Blu-ray", "encrypted", "unreadable"};
	OrbisLog("[disc] %s: \"%s\", %s, %llu sectors, %zu entries%s: %s%s%s", m.id.c_str(), m.label.c_str(), m.fs.c_str(),
		(unsigned long long)m.sectors, m.nodes.size() - 1, m.cut ? " (cut)" : "", kKinds[int(st.kind)],
		st.protection.empty() ? "" : " - ", st.protection.c_str());
}

// is there a readable disc in the drive? (an fd open on it, or -1)
int OpenDisc(const Slot& s)
{
#ifndef __PROSPERO__
	if (HostOut(s.id))
		return -1;
#endif
	const int fd = open(s.dev.c_str(), O_RDONLY);
	if (fd < 0)
		return -1;
	uint8_t b[kSector];
	if (pread(fd, b, kSector, 16 * kSector) != ssize_t(kSector))
	{
#ifdef __PROSPERO__
		// a disc that has no sector 16 to read: an audio CD (a table of contents), or a CD in the PS5's own drive
		ioc_toc_header th = {};
		if (ioctl(fd, CDIOREADTOCHEADER, &th) == 0 && th.ending_track >= th.starting_track && th.starting_track > 0)
			return fd; // Mount finds nothing: Unreadable
#endif
		close(fd);
		return -1;
	}
	return fd;
}

void FindDrives()
{
	std::vector<Slot> found;
#ifdef __PROSPERO__
	for (int i = 0; i < 8; i++)
	{
		const std::string dev = "/dev/cd" + std::to_string(i);
		struct stat st;
		if (stat(dev.c_str(), &st) == 0)
		{
			Slot s;
			s.id = "cd" + std::to_string(i);
			s.dev = dev;
			s.usb = i > 0; // 5S2CD: the console's own drive is cd0, a USB drive cd1
			found.push_back(s);
		}
	}
#else
	if (const char* e = getenv("MPCPS5_HOST_DISC"))
	{
		std::string list = e;
		size_t p = 0;
		int i = 0;
		while (p <= list.size() && !list.empty())
		{
			size_t c = list.find(',', p);
			if (c == std::string::npos)
				c = list.size();
			Slot s;
			s.id = "cd" + std::to_string(i++);
			s.dev = list.substr(p, c - p);
			s.usb = i > 1;
			found.push_back(s);
			p = c + 1;
		}
	}
#endif
	std::lock_guard<std::mutex> l(g_mu);
	// keep what is known about the drives still there
	std::vector<Slot> next;
	for (auto& f : found)
	{
		bool kept = false;
		for (auto& s : g_slots)
			if (s.id == f.id)
			{
				next.push_back(std::move(s));
				kept = true;
				break;
			}
		if (!kept)
		{
			f.state.id = f.id;
			f.state.usb = f.usb;
			OrbisLog("[disc] drive %s (%s): %s", f.id.c_str(), f.dev.c_str(), f.usb ? "USB" : "the console's own");
			next.push_back(std::move(f));
		}
	}
	for (auto& s : g_slots)
		if (s.media && std::none_of(next.begin(), next.end(), [&](const Slot& n) { return n.id == s.id; }))
			s.media->gone = true; // a USB drive unplugged
	g_slots = std::move(next);
}

void WatchOnce()
{
	FindDrives();
	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> l(g_mu);
		for (auto& s : g_slots)
			ids.push_back(s.id);
	}
	for (const std::string& id : ids)
	{
		Slot copy;
		{
			std::lock_guard<std::mutex> l(g_mu);
			auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.id == id; });
			if (it == g_slots.end())
				continue;
			copy.id = it->id;
			copy.dev = it->dev;
			copy.usb = it->usb;
			copy.media = it->media;
			copy.state = it->state;
		}
		if (copy.media)
		{
			bool there = !copy.media->gone;
#ifndef __PROSPERO__
			struct stat st;
			if (there && (HostOut(id) || stat(copy.dev.c_str(), &st) != 0))
				there = false;
#else
			// a disc being read (a film playing) is there; otherwise ask the drive now and then
			if (there && Now() - copy.media->last_io > 5 && !copy.media->Probe())
				there = false;
#endif
			if (!there)
			{
				copy.media->gone = true;
				std::lock_guard<std::mutex> l(g_mu);
				for (auto& s : g_slots)
					if (s.id == id && s.media == copy.media)
					{
						s.media.reset();
						s.state.kind = Kind::NoDisc;
						s.state.label.clear();
						s.state.fs.clear();
						s.state.protection.clear();
						s.state.generation = 0;
						s.state.sectors = 0;
					}
				OrbisLog("[disc] %s: the disc was taken out", id.c_str());
			}
			continue;
		}
		const int fd = OpenDisc(copy);
		if (fd < 0)
			continue;
		uint64_t gen;
		{
			std::lock_guard<std::mutex> l(g_mu);
			gen = ++g_gen;
			for (auto& s : g_slots)
				if (s.id == id)
				{
					s.state.kind = Kind::Reading;
					s.state.generation = gen;
				}
		}
		auto m = std::make_shared<Media>(id, copy.dev, fd, gen);
		DriveState st = copy.state;
		st.generation = gen;
		Mount(*m, st);
		std::lock_guard<std::mutex> l(g_mu);
		for (auto& s : g_slots)
			if (s.id == id)
			{
				s.media = m;
				s.state = st;
				s.state.id = s.id;
				s.state.usb = s.usb;
			}
	}
}

void WatchThread()
{
	OrbisLog("[disc] watching the disc drives");
	std::unique_lock<std::mutex> l(g_mu);
	while (!g_stop)
	{
		l.unlock();
		WatchOnce();
		l.lock();
#ifdef __PROSPERO__
		const auto wait = std::chrono::milliseconds(2000);
#else
		const auto wait = std::chrono::milliseconds(250);
#endif
		g_cv.wait_for(l, wait, [] { return g_stop || g_poke; });
		g_poke = false;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// URLs
// disc://cd0/a/b -> ("cd0", "a/b")
bool SplitDiscUrl(const std::string& url, std::string& id, std::string& path)
{
	if (!StartsWith(url, "disc://"))
		return false;
	const std::string rest = url.substr(7);
	const size_t slash = rest.find('/');
	id = rest.substr(0, slash);
	path = slash == std::string::npos ? "" : rest.substr(slash + 1);
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	return !id.empty();
}

// ---------------------------------------------------------------------------------------------------------------
// files on a data disc, to FFmpeg
struct FileReader
{
	std::shared_ptr<Media> m;
	std::vector<Extent> ext;
	std::vector<uint8_t> mem; // a file read whole (UDF data inside its entry)
	bool in_mem = false;
	uint64_t size = 0, pos = 0;
	std::vector<uint8_t> tmp;
};

int FileRead(void* opaque, uint8_t* buf, int len)
{
	FileReader* f = static_cast<FileReader*>(opaque);
	if (f->pos >= f->size)
		return AVERROR_EOF;
	if (f->m->gone)
		return AVERROR(EIO);
	size_t want = size_t(std::min<uint64_t>(uint64_t(len), f->size - f->pos));
	if (f->in_mem)
	{
		memcpy(buf, f->mem.data() + f->pos, want);
		f->pos += want;
		return int(want);
	}
	const Extent* e = nullptr;
	for (const Extent& x : f->ext)
		if (f->pos >= x.file_off && f->pos < x.file_off + x.bytes)
		{
			e = &x;
			break;
		}
	if (!e)
		return AVERROR(EIO);
	const uint64_t in = f->pos - e->file_off;
	want = size_t(std::min<uint64_t>(want, e->bytes - in));
	const uint64_t first = e->lba + in / kSector;
	const uint32_t skip = uint32_t(in % kSector);
	const uint32_t count = uint32_t(std::min<uint64_t>((skip + want + kSector - 1) / kSector, 128));
	want = std::min<size_t>(want, size_t(count) * kSector - skip);
	f->tmp.resize(size_t(count) * kSector);
	if (!f->m->Read(first, count, f->tmp.data()))
		return AVERROR(EIO);
	memcpy(buf, f->tmp.data() + skip, want);
	f->pos += want;
	return int(want);
}

int64_t FileSeek(void* opaque, int64_t offset, int whence)
{
	FileReader* f = static_cast<FileReader*>(opaque);
	if (whence == AVSEEK_SIZE)
		return int64_t(f->size);
	whence &= ~AVSEEK_FORCE;
	const int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? int64_t(f->pos) + offset : int64_t(f->size) + offset;
	if (p < 0)
		return AVERROR(EINVAL);
	f->pos = uint64_t(p);
	return p;
}

bool OpenFileReader(const std::string& url, FileReader& f, std::string* error)
{
	std::string id, path;
	if (!SplitDiscUrl(url, id, path))
		return false;
	f.m = MediaOf(id);
	if (!f.m)
	{
		if (error)
			*error = "No disc";
		return false;
	}
	if (FileExtents(*f.m, path, f.ext, &f.size))
		return true;
	if (f.m->udf)
	{
		const int n = FindNode(*f.m, path);
		if (n >= 0 && !f.m->nodes[n].dir && UdfReadAll(*f.m, path, f.mem, 16u << 20))
		{
			f.in_mem = true;
			f.size = f.mem.size();
			return true;
		}
	}
	if (error)
		*error = "The file can't be opened from the disc.";
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// libdvdread reads "mpcdisc:cd0" through the sector reader; anything else (a VIDEO_TS folder on a drive) as files
struct DvdHandle
{
	std::shared_ptr<Media> m;
	uint64_t pos = 0;
	int fd = -1;
};

std::shared_ptr<Media> DvdMedia(const char* path)
{
	if (strncmp(path, "mpcdisc:", 8) != 0)
		return nullptr;
	return MediaOf(path + 8);
}

int DvdStat(dvd_reader_filesystem_h*, const char* path, dvdstat_t* st)
{
	if (strncmp(path, "mpcdisc:", 8) == 0)
	{
		auto m = DvdMedia(path);
		if (!m)
			return -1;
		st->size = m->sectors * kSector;
		st->st_mode = DVD_S_IFBLK;
		return 0;
	}
	struct stat sb;
	if (stat(path, &sb) != 0)
		return -1;
	st->size = uint64_t(sb.st_size);
	st->st_mode = S_ISDIR(sb.st_mode) ? DVD_S_IFDIR : S_ISREG(sb.st_mode) ? DVD_S_IFREG : unsigned(sb.st_mode);
	return 0;
}

void* DvdDirOpen(dvd_reader_filesystem_h*, const char* dirname)
{
	return opendir(dirname);
}

int DvdDirRead(void* dir, dvd_dirent_t* entry)
{
	errno = 0;
	dirent* e = readdir(static_cast<DIR*>(dir));
	if (!e)
		return errno ? -errno : 1;
	strncpy(entry->d_name, e->d_name, sizeof(entry->d_name) - 1);
	entry->d_name[sizeof(entry->d_name) - 1] = 0;
	return 0;
}

void DvdDirClose(void* dir)
{
	if (dir)
		closedir(static_cast<DIR*>(dir));
}

void* DvdFileOpen(dvd_reader_filesystem_h*, const char* name)
{
	DvdHandle* h = new DvdHandle;
	if (strncmp(name, "mpcdisc:", 8) == 0)
	{
		h->m = DvdMedia(name);
		if (h->m)
			return h;
	}
	else
	{
		h->fd = open(name, O_RDONLY);
		if (h->fd >= 0)
			return h;
	}
	delete h;
	return nullptr;
}

ssize_t DvdFileRead(void* file, char* buf, size_t size)
{
	DvdHandle* h = static_cast<DvdHandle*>(file);
	if (h->fd >= 0)
		return read(h->fd, buf, size);
	const uint64_t end = h->m->sectors * kSector;
	if (h->pos >= end || size == 0)
		return 0;
	size = size_t(std::min<uint64_t>(size, end - h->pos));
	size_t done = 0;
	std::vector<uint8_t> tmp;
	while (done < size)
	{
		const uint64_t at = h->pos + done;
		const uint32_t skip = uint32_t(at % kSector);
		if (skip == 0 && size - done >= kSector)
		{
			// whole sectors straight into the buffer
			const uint32_t n = uint32_t(std::min<size_t>((size - done) / kSector, 512));
			if (!h->m->Read(at / kSector, n, reinterpret_cast<uint8_t*>(buf) + done))
				return done ? ssize_t(done) : -1;
			done += size_t(n) * kSector;
			continue;
		}
		tmp.resize(kSector);
		if (!h->m->Read(at / kSector, 1, tmp.data()))
			return done ? ssize_t(done) : -1;
		const size_t n = std::min<size_t>(kSector - skip, size - done);
		memcpy(buf + done, tmp.data() + skip, n);
		done += n;
	}
	h->pos += done;
	return ssize_t(done);
}

off64_t DvdFileSeek(void* file, off64_t offset, int whence)
{
	DvdHandle* h = static_cast<DvdHandle*>(file);
	if (h->fd >= 0)
		return lseek(h->fd, offset, whence);
	const int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? int64_t(h->pos) + offset : int64_t(h->m->sectors * kSector) + offset;
	if (p < 0)
		return -1;
	h->pos = uint64_t(p);
	return p;
}

int DvdFileClose(void* file)
{
	DvdHandle* h = static_cast<DvdHandle*>(file);
	if (h->fd >= 0)
		close(h->fd);
	delete h;
	return 0;
}

void DvdFsClose(dvd_reader_filesystem_h* fs)
{
	delete fs;
}

dvd_reader_filesystem_h* MakeDvdFs()
{
	dvd_reader_filesystem_h* fs = new dvd_reader_filesystem_h;
	fs->internal = nullptr;
	fs->close = DvdFsClose;
	fs->stat = DvdStat;
	fs->dir_open = DvdDirOpen;
	fs->dir_read = DvdDirRead;
	fs->dir_close = DvdDirClose;
	fs->file_open = DvdFileOpen;
	fs->file_read = DvdFileRead;
	fs->file_seek = DvdFileSeek;
	fs->file_close = DvdFileClose;
	return fs;
}

void DvdQuietLog(void*, dvd_logger_level_t level, const char* fmt, va_list va)
{
	if (level > DVD_LOGGER_LEVEL_WARN)
		return;
	char b[512];
	vsnprintf(b, sizeof(b), fmt, va);
	OrbisLog("[disc] libdvdread: %s", b);
}

double DvdSeconds(const dvd_time_t& t)
{
	auto bcd = [](uint8_t v) { return (v >> 4) * 10 + (v & 15); };
	const int fps = (t.frame_u >> 6) == 1 ? 25 : 30;
	return bcd(t.hour) * 3600.0 + bcd(t.minute) * 60.0 + bcd(t.second) + bcd(t.frame_u & 0x3F) / double(fps);
}

// ---------------------------------------------------------------------------------------------------------------
// Blu-ray: libbluray reading the disc through the sector reader; the title's MPEG-TS to FFmpeg
struct BdTitle
{
	BLURAY* bd = nullptr;
	std::shared_ptr<Media> m;
};

int BdReadBlocks(void* handle, void* buf, int lba, int n)
{
	Media* m = static_cast<Media*>(handle);
	return lba >= 0 && n > 0 && m->Read(uint64_t(lba), uint32_t(n), static_cast<uint8_t*>(buf)) ? n : -1;
}

int BdRead(void* opaque, uint8_t* buf, int len)
{
	BdTitle* t = static_cast<BdTitle*>(opaque);
	if (t->m && t->m->gone)
		return AVERROR(EIO);
	const int r = bd_read(t->bd, buf, len);
	if (r == 0)
		return AVERROR_EOF;
	return r < 0 ? AVERROR(EIO) : r;
}

int64_t BdSeek(void* opaque, int64_t offset, int whence)
{
	BdTitle* t = static_cast<BdTitle*>(opaque);
	const int64_t size = int64_t(bd_get_title_size(t->bd));
	if (whence == AVSEEK_SIZE)
		return size;
	whence &= ~AVSEEK_FORCE;
	const int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? int64_t(bd_tell(t->bd)) + offset : size + offset;
	if (p < 0 || p > size)
		return AVERROR(EINVAL);
	return bd_seek(t->bd, uint64_t(p));
}

std::string Lang(const uint8_t* l)
{
	std::string s;
	for (int i = 0; i < 3 && l[i]; i++)
		s += char(tolower(l[i]));
	return s.size() == 3 ? s : "";
}

bool OpenBluray(const std::string& where, const std::shared_ptr<Media>& m, int want, TitleOpen& out)
{
	BdTitle* t = new BdTitle;
	t->m = m;
	t->bd = bd_init();
	const bool opened = t->bd && (m ? bd_open_stream(t->bd, m.get(), BdReadBlocks) : bd_open_disc(t->bd, where.c_str(), nullptr));
	const BLURAY_DISC_INFO* info = opened ? bd_get_disc_info(t->bd) : nullptr;
	auto fail = [&](const std::string& e) {
		if (t->bd)
			bd_close(t->bd);
		delete t;
		out.error = e;
		return false;
	};
	if (!info || !info->bluray_detected)
		return fail("This is not a Blu-ray this player can read.");
	if (info->aacs_detected || info->bdplus_detected)
	{
		OrbisLog("[disc] %s: Blu-ray with %s: refused", where.c_str(), info->aacs_detected ? "AACS" : "BD+");
		out.encrypted = true;
		return fail(EncryptedText());
	}
	const uint32_t n = bd_get_titles(t->bd, TITLES_RELEVANT, 0);
	if (n == 0)
		return fail("This Blu-ray has no title to play.");
	const int main_title = bd_get_main_title(t->bd);
	for (uint32_t i = 0; i < n; i++)
		if (BLURAY_TITLE_INFO* ti = bd_get_title_info(t->bd, i, 0))
		{
			out.titles.push_back({int(i) + 1, ti->duration / 90000.0, int(ti->chapter_count)});
			bd_free_title_info(ti);
		}
	const int idx = want > 0 && want <= int(n) ? want - 1 : std::max(0, main_title);
	if (!bd_select_title(t->bd, uint32_t(idx)))
		return fail("This Blu-ray's title can't be played.");
	out.title = idx + 1;
	if (BLURAY_TITLE_INFO* ti = bd_get_title_info(t->bd, uint32_t(idx), 0))
	{
		out.duration = ti->duration / 90000.0;
		for (uint32_t c = 0; c < ti->chapter_count; c++)
			out.chapters.push_back({ti->chapters[c].start / 90000.0, ti->chapters[c].chapter_name ? ti->chapters[c].chapter_name : ""});
		if (ti->clip_count > 0)
		{
			const BLURAY_CLIP_INFO& c = ti->clips[0];
			auto add = [&](const BLURAY_STREAM_INFO* s, int k) {
				for (int i = 0; i < k; i++)
				{
					const std::string l = Lang(s[i].lang);
					if (!l.empty())
						out.languages.push_back({s[i].pid, l});
				}
			};
			add(c.audio_streams, c.audio_stream_count);
			add(c.pg_streams, c.pg_stream_count);
		}
		OrbisLog("[disc] Blu-ray title %d of %u: playlist %05u.mpls, %.1f s, %u chapters, %u clips", idx + 1, n, ti->playlist,
			out.duration, ti->chapter_count, ti->clip_count);
		bd_free_title_info(ti);
	}
	const int bufsize = 192 * 1024;
	uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufsize));
	out.io = buf ? avio_alloc_context(buf, bufsize, 0, t, BdRead, nullptr, BdSeek) : nullptr;
	if (!out.io)
	{
		av_free(buf);
		return fail("out of memory");
	}
	out.owner = t;
	out.format = av_find_input_format("mpegts");
	return true;
}

std::string FolderOf(const std::string& url, const char* scheme)
{
	// dvd:///mnt/usb0/MOVIE -> /mnt/usb0/MOVIE
	std::string p = url.substr(strlen(scheme));
	const size_t q = p.find('?');
	if (q != std::string::npos)
		p.resize(q);
	return p;
}

void BdLog(const char* msg)
{
	std::string m = msg;
	while (!m.empty() && (m.back() == '\n' || m.back() == '\r'))
		m.pop_back();
	OrbisLog("[disc] libbluray: %s", m.c_str());
}
} // namespace

void Start()
{
	{
		std::lock_guard<std::mutex> l(g_mu);
		if (g_started)
			return;
		g_started = true;
		g_stop = false;
	}
	DVDSetFilesystemFactory(MakeDvdFs);
	// libbluray's messages to the log, its serious ones only
	bd_set_debug_handler(BdLog);
	bd_set_debug_mask(DBG_CRIT);
	g_thread = std::thread(WatchThread);
}

void Stop()
{
	{
		std::lock_guard<std::mutex> l(g_mu);
		if (!g_started)
			return;
		g_stop = true;
		g_cv.notify_all();
	}
	if (g_thread.joinable())
		g_thread.join();
	std::lock_guard<std::mutex> l(g_mu);
	g_slots.clear();
	g_started = false;
}

std::vector<DriveState> Drives()
{
	std::lock_guard<std::mutex> l(g_mu);
	std::vector<DriveState> v;
	for (const auto& s : g_slots)
		v.push_back(s.state);
	return v;
}

bool GetDrive(const std::string& id, DriveState* out)
{
	std::lock_guard<std::mutex> l(g_mu);
	for (const auto& s : g_slots)
		if (s.id == id)
		{
			*out = s.state;
			return true;
		}
	return false;
}

std::string DriveName(const DriveState& d)
{
	std::string what;
	switch (d.kind)
	{
		case Kind::NoDisc:
			what = T("No disc");
			break;
		case Kind::Reading:
			what = T("reading...");
			break;
		case Kind::Unreadable:
			what = T("unreadable disc");
			break;
		default:
			what = d.label.empty() ? std::string(T("no name")) : d.label;
			break;
	}
	return Fmt(d.usb ? T("USB disc drive (%s)") : T("Disc drive (%s)"), what.c_str());
}

const char* EncryptedText()
{
	return "This disc is encrypted (a commercial film) and can't be played here: Media Player Classic PS5 doesn't "
		   "decrypt discs. On a PS5 with a disc drive, the console's own player plays it.";
}

std::string EncryptedMessage()
{
	return T(EncryptedText());
}

std::string DriveMessage(const DriveState& d)
{
	switch (d.kind)
	{
		case Kind::NoDisc:
			return T("No disc");
		case Kind::Reading:
			return T("Reading the disc...");
		case Kind::Encrypted:
			return EncryptedMessage();
		case Kind::Unreadable:
			return d.usb ? T("This disc can't be read: an audio CD (audio CDs can't be played here), or a blank or damaged disc.")
						 : T("This disc can't be read: the PS5's own drive doesn't read CDs (a USB drive does; audio CDs "
							 "can't be played here), or the disc is blank or damaged.");
		default:
			return "";
	}
}

bool IsDiscUrl(const std::string& url)
{
	return StartsWith(url, "disc://") || StartsWith(url, "dvd://") || StartsWith(url, "bluray://");
}

bool IsTitleUrl(const std::string& url)
{
	return StartsWith(url, "dvd://") || StartsWith(url, "bluray://");
}

std::string DriveOf(const std::string& url)
{
	std::string rest;
	if (StartsWith(url, "disc://"))
		rest = url.substr(7);
	else if (StartsWith(url, "dvd://"))
		rest = url.substr(6);
	else if (StartsWith(url, "bluray://"))
		rest = url.substr(9);
	else
		return "";
	if (rest.empty() || rest[0] == '/')
		return ""; // a folder
	const size_t e = rest.find_first_of("/?");
	return rest.substr(0, e);
}

uint64_t Generation(const std::string& url)
{
	const std::string id = DriveOf(url);
	if (id.empty())
		return 0;
	std::lock_guard<std::mutex> l(g_mu);
	for (const auto& s : g_slots)
		if (s.id == id)
			return s.media && !s.media->gone ? s.media->gen : 0;
	return 0;
}

std::string TitleBase(const std::string& url, int* title)
{
	const size_t q = url.find("?title=");
	if (title)
		*title = q == std::string::npos ? 0 : atoi(url.c_str() + q + 7);
	return q == std::string::npos ? url : url.substr(0, q);
}

std::string DisplayName(const std::string& url)
{
	if (!IsTitleUrl(url))
		return "";
	int title = 0;
	const std::string base = TitleBase(url, &title);
	std::string name;
	const std::string id = DriveOf(url);
	DriveState d;
	if (!id.empty() && GetDrive(id, &d) && !d.label.empty())
		name = d.label;
	else if (!id.empty())
		name = id;
	else
	{
		std::string f = FolderOf(base, StartsWith(base, "dvd://") ? "dvd://" : "bluray://");
		while (f.size() > 1 && f.back() == '/')
			f.pop_back();
		name = f.substr(f.find_last_of('/') + 1);
	}
	if (title > 0)
		name += " - " + Fmt(T("Title %d"), title);
	return name;
}

bool List(const std::string& url, std::vector<Entry>& out, std::string* error)
{
	std::string id, path;
	if (!SplitDiscUrl(url, id, path))
		return false;
	DriveState d;
	if (!GetDrive(id, &d))
	{
		if (error)
			*error = T("No disc");
		return false;
	}
	const std::string msg = DriveMessage(d);
	if (!msg.empty())
	{
		if (error)
			*error = msg;
		return false;
	}
	auto m = MediaOf(id);
	const int n = m ? FindNode(*m, path) : -1;
	if (n < 0 || !m->nodes[n].dir)
	{
		if (error)
			*error = Fmt(T("Can't open the folder %s"), url.c_str());
		return false;
	}
	for (int k : m->nodes[n].kids)
	{
		const Node& x = m->nodes[k];
		out.push_back({x.name, x.dir, x.size, x.mtime});
	}
	return true;
}

bool Exists(const std::string& url)
{
	std::string id, path;
	if (!SplitDiscUrl(url, id, path))
		return false;
	auto m = MediaOf(id);
	return m && FindNode(*m, path) >= 0;
}

bool IsDir(const std::string& url)
{
	std::string id, path;
	if (!SplitDiscUrl(url, id, path))
		return false;
	auto m = MediaOf(id);
	const int n = m ? FindNode(*m, path) : -1;
	return n >= 0 && m->nodes[n].dir;
}

bool ReadFile(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes)
{
	FileReader f;
	if (!OpenFileReader(url, f, nullptr) || f.size > max_bytes)
		return false;
	out.resize(size_t(f.size));
	size_t done = 0;
	while (done < out.size())
	{
		const int r = FileRead(&f, out.data() + done, int(std::min<size_t>(out.size() - done, 1u << 20)));
		if (r <= 0)
			return false;
		done += size_t(r);
	}
	return true;
}

AVIOContext* OpenAvio(const std::string& url, void** owner, std::string* error)
{
	FileReader* f = new FileReader;
	if (!OpenFileReader(url, *f, error))
	{
		delete f;
		return nullptr;
	}
	const int bufsize = 256 * 1024;
	uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufsize));
	AVIOContext* io = buf ? avio_alloc_context(buf, bufsize, 0, f, FileRead, nullptr, FileSeek) : nullptr;
	if (!io)
	{
		av_free(buf);
		delete f;
		if (error)
			*error = "out of memory";
		return nullptr;
	}
	*owner = f;
	OrbisLog("[disc] opened %s (%llu bytes, %zu extent(s)%s)", url.c_str(), (unsigned long long)f->size, f->ext.size(),
		f->in_mem ? ", read whole" : "");
	return io;
}

void CloseAvio(AVIOContext* io, void* owner)
{
	if (io)
	{
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	delete static_cast<FileReader*>(owner);
}

bool OpenTitle(const std::string& url, TitleOpen& out)
{
	int want = 0;
	const std::string base = TitleBase(url, &want);
	const bool dvd = StartsWith(base, "dvd://");
	const std::string id = DriveOf(base);
	std::shared_ptr<Media> m;
	std::string folder;
	if (!id.empty())
	{
		DriveState d;
		if (!GetDrive(id, &d) || d.kind == Kind::NoDisc)
		{
			out.error = "No disc";
			return false;
		}
		if (d.kind == Kind::Reading)
		{
			out.error = "Reading the disc...";
			return false;
		}
		if (d.kind == Kind::Encrypted)
		{
			OrbisLog("[disc] %s: encrypted (%s): refused", id.c_str(), d.protection.c_str());
			out.encrypted = true;
			out.error = EncryptedText();
			return false;
		}
		if (d.kind != (dvd ? Kind::DvdVideo : Kind::BluRay))
		{
			out.error = dvd ? "This disc is not a DVD-Video." : "This disc is not a Blu-ray.";
			return false;
		}
		m = MediaOf(id);
		if (!m)
		{
			out.error = "No disc";
			return false;
		}
	}
	else
	{
		folder = FolderOf(base, dvd ? "dvd://" : "bluray://");
		if (folder.empty() || !OrbisIsDir(folder))
		{
			out.error = "The folder can't be opened.";
			return false;
		}
	}
	if (!dvd)
		return OpenBluray(m ? id : folder, m, want, out);

	// DVD-Video: the titles from the IFOs (libdvdread), the main one the longest
	const std::string path = m ? "mpcdisc:" + id : folder;
	dvd_logger_cb log = {DvdQuietLog};
	dvd_reader_t* dvdr = DVDOpen2(nullptr, &log, path.c_str());
	ifo_handle_t* vmg = dvdr ? ifoOpen(dvdr, 0) : nullptr;
	if (!vmg || !vmg->tt_srpt || vmg->tt_srpt->nr_of_srpts == 0)
	{
		if (vmg)
			ifoClose(vmg);
		if (dvdr)
			DVDClose(dvdr);
		out.error = "This DVD-Video's structure (VIDEO_TS) can't be read.";
		return false;
	}
	std::map<int, ifo_handle_t*> vts;
	int main_title = 1;
	double longest = -1;
	bool scrambled = false;
	for (int t = 0; t < vmg->tt_srpt->nr_of_srpts; t++)
	{
		const title_info_t& ti = vmg->tt_srpt->title[t];
		Title title;
		title.number = t + 1;
		title.chapters = ti.nr_of_ptts;
		ifo_handle_t*& v = vts[ti.title_set_nr];
		if (!v)
		{
			v = ifoOpen(dvdr, ti.title_set_nr);
			// folders weren't looked at when the disc went in: the title VOBs' packs, sampled
			if (v && m == nullptr && !scrambled)
				if (dvd_file_t* f = DVDOpenFile(dvdr, ti.title_set_nr, DVD_READ_TITLE_VOBS))
				{
					const uint64_t secs = uint64_t(std::max<ssize_t>(0, DVDFileSize(f)));
					scrambled = VobScrambled(secs, [&](uint64_t s, uint32_t n, uint8_t* b) {
						return DVDReadBlocks(f, int(s), n, b) == ssize_t(n);
					});
					DVDCloseFile(f);
				}
		}
		if (v && v->vts_ptt_srpt && v->vts_pgcit && ti.vts_ttn >= 1 && ti.vts_ttn <= v->vts_ptt_srpt->nr_of_srpts)
		{
			const ttu_t& ttu = v->vts_ptt_srpt->title[ti.vts_ttn - 1];
			if (ttu.nr_of_ptts > 0)
			{
				const int pgcn = ttu.ptt[0].pgcn;
				if (pgcn >= 1 && pgcn <= v->vts_pgcit->nr_of_pgci_srp && v->vts_pgcit->pgci_srp[pgcn - 1].pgc)
					title.seconds = DvdSeconds(v->vts_pgcit->pgci_srp[pgcn - 1].pgc->playback_time);
			}
		}
		if (title.seconds > longest)
		{
			longest = title.seconds;
			main_title = title.number;
		}
		out.titles.push_back(title);
	}
	for (auto& v : vts)
		if (v.second)
			ifoClose(v.second);
	ifoClose(vmg);
	DVDClose(dvdr);
	if (scrambled)
	{
		OrbisLog("[disc] %s: the DVD's packs are marked scrambled (CSS): refused", path.c_str());
		out.encrypted = true;
		out.error = EncryptedText();
		return false;
	}
	out.title = want > 0 && want <= int(out.titles.size()) ? want : main_title;
	out.format = av_find_input_format("dvdvideo");
	if (!out.format)
	{
		out.error = "FFmpeg was built without its DVD-Video demuxer";
		return false;
	}
	out.ffmpeg_url = path;
	av_dict_set_int(&out.options, "title", out.title, 0);
	OrbisLog("[disc] DVD-Video %s: %zu title(s), playing title %d (%.1f s)%s", path.c_str(), out.titles.size(), out.title,
		out.titles[size_t(out.title - 1)].seconds, want ? "" : ", the longest");
	return true;
}

void CloseTitle(TitleOpen& t)
{
	av_dict_free(&t.options);
	if (t.io)
	{
		av_freep(&t.io->buffer);
		avio_context_free(&t.io);
	}
	if (BdTitle* b = static_cast<BdTitle*>(t.owner))
	{
		if (b->bd)
			bd_close(b->bd);
		delete b;
	}
	t.owner = nullptr;
}

std::string TitleUrlForFolder(const std::string& folder)
{
	std::string f = folder;
	while (f.size() > 1 && f.back() == '/')
		f.pop_back();
	if (StartsWith(f, "disc://"))
	{
		std::string id, path;
		if (!SplitDiscUrl(f, id, path))
			return "";
		const std::string lp = Lower(path);
		if (!path.empty() && lp != "video_ts" && lp != "bdmv")
			return "";
		DriveState d;
		return GetDrive(id, &d) ? PlayUrl(d) : "";
	}
	auto file = [](const std::string& p) { return OrbisIsFile(p); };
	const std::string name = Lower(f.substr(f.find_last_of('/') + 1));
	const std::string parent = f.substr(0, f.find_last_of('/'));
	if (file(f + "/VIDEO_TS/VIDEO_TS.IFO") || file(f + "/video_ts/video_ts.ifo"))
		return "dvd://" + f;
	if (name == "video_ts" && (file(f + "/VIDEO_TS.IFO") || file(f + "/video_ts.ifo")))
		return "dvd://" + parent;
	if (file(f + "/BDMV/index.bdmv"))
		return "bluray://" + f;
	if (name == "bdmv" && file(f + "/index.bdmv"))
		return "bluray://" + parent;
	return "";
}

std::string PlayUrl(const DriveState& d)
{
	switch (d.kind)
	{
		case Kind::DvdVideo:
			return "dvd://" + d.id;
		case Kind::BluRay:
			return "bluray://" + d.id;
		case Kind::Encrypted:
			return (d.protection == "AACS" || d.protection == "BD+" ? "bluray://" : "dvd://") + d.id;
		default:
			return "";
	}
}

#ifndef __PROSPERO__
void HostEject(const std::string& id, bool out)
{
	{
		std::lock_guard<std::mutex> l(g_host_mu);
		g_host_out[id] = out;
	}
	if (out)
	{
		std::lock_guard<std::mutex> l(g_mu);
		for (auto& s : g_slots)
			if (s.id == id && s.media)
				s.media->gone = true;
	}
	OrbisLog("[disc] host: %s %s", id.c_str(), out ? "ejected" : "closed");
	Poke();
}
#endif
} // namespace disc
} // namespace mpc
