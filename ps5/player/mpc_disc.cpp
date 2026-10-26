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
#include <arpa/inet.h>
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
#include <set>
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
	std::vector<Extent> extents; // ISO 9660 (UDF files are mapped through libudfread as they are read)
	std::vector<int> kids;
};

// the reads of this thread give up when *t_cancel turns true (CancelScope)
thread_local const std::atomic<bool>* t_cancel = nullptr;

bool Cancelled()
{
	return t_cancel && t_cancel->load();
}

std::atomic<bool> g_stop{false}; // the watcher is asked to end (Stop())

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
// host tests: a slow (and failing) drive from a sector on (MPCPS5_HOST_DISC_FAULT=slow=<ms>,from=<sector>[,fail=1])
std::atomic<int> g_fault_slow_ms{0};
std::atomic<int64_t> g_fault_from{-1};
std::atomic<bool> g_fault_fail{false};
std::atomic<int> g_fault_hits{0}; // reads that met the fault (tests/disc_test.cpp)
std::atomic<bool> g_fault_nosize{false}; // nosize=1: the drive doesn't say the disc's size
void ReadFaultSetting()
{
	const char* e = getenv("MPCPS5_HOST_DISC_FAULT");
	if (!e)
		return;
	std::string s = e;
	size_t p = 0;
	while (p < s.size())
	{
		size_t c = s.find(',', p);
		if (c == std::string::npos)
			c = s.size();
		const std::string kv = s.substr(p, c - p);
		const size_t eq = kv.find('=');
		const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
		if (k == "slow")
			g_fault_slow_ms = atoi(v.c_str());
		else if (k == "from")
			g_fault_from = atoll(v.c_str());
		else if (k == "fail")
			g_fault_fail = atoi(v.c_str()) != 0;
		else if (k == "nosize")
			g_fault_nosize = atoi(v.c_str()) != 0;
		p = c + 1;
	}
}
#endif

void Poke();

// ---------------------------------------------------------------------------------------------------------------
// The drive's side of one disc: the device, a cache of chunks, the reading thread. Shared by the Media and its
// thread, so a Media can go (the player closed, the disc taken out) while a read is still in the drive: the thread
// finishes it and closes the device after.
struct Chunk
{
	uint64_t index = UINT64_MAX;
	std::vector<uint8_t> data;
	uint64_t ok = 0; // a bit per sector read: a chunk whose reads failed stays known (they aren't tried again)
	uint32_t count = 0; // sectors in it
	bool loading = false;
	uint64_t stamp = 0;
};

struct DriveIO
{
	std::string id;
	int fd = -1;
	uint64_t sectors = 0; // 0: not known (the drive didn't say): reads aren't held to an end, a short read finds it
	std::atomic<bool> gone{false}, stop{false};
	std::atomic<double> last_io{0};
	std::mutex mu;
	std::condition_variable cv;
	std::vector<Chunk> chunks;
	std::deque<uint64_t> want; // chunks to read: asked for at the front, read-ahead at the back
	uint64_t clock = 0, last_idx = UINT64_MAX;
	bool running = false;

	DriveIO()
	{
		chunks.resize(kChunks);
		for (auto& c : chunks)
			c.data.resize(size_t(kChunk) * kSector);
	}
	~DriveIO()
	{
		if (fd >= 0)
			close(fd);
	}

	// straight from the drive. check: on a failure, is the disc still there? (sector 16 always reads on one)
	bool DeviceRead(uint64_t lba, uint32_t count, uint8_t* out, bool check)
	{
#ifndef __PROSPERO__
		if (HostOut(id))
		{
			gone = true;
			return false;
		}
		if (g_fault_from >= 0 && int64_t(lba + count) > g_fault_from)
		{
			g_fault_hits++;
			if (g_fault_slow_ms > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(int(g_fault_slow_ms)));
			if (g_fault_fail)
			{
				errno = EIO;
				return false;
			}
		}
#endif
		last_io = Now();
		const size_t want_bytes = size_t(count) * kSector;
		size_t got = 0;
		int tries = 0;
		while (got < want_bytes)
		{
			const ssize_t r = pread(fd, out + got, want_bytes - got, off_t(lba * kSector + got));
			if (r > 0)
			{
				got += size_t(r);
				continue;
			}
			if (r < 0 && errno == EINTR)
				continue;
			const int e = r < 0 ? errno : 0;
			if (++tries < 2 && r < 0 && !stop && !gone)
				continue; // once more (a drive spinning up)
			if (check)
			{
				OrbisLog("[disc] %s: read of sectors %llu-%llu failed: %s", id.c_str(), (unsigned long long)lba,
					(unsigned long long)(lba + count - 1), e ? strerror(e) : "end of the disc");
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
		for (auto& c : chunks)
			if (c.index == idx)
				return &c;
		return nullptr;
	}

	Chunk* Victim()
	{
		Chunk* v = nullptr;
		for (auto& c : chunks)
			if (!c.loading && (!v || c.stamp < v->stamp))
				v = &c;
		return v;
	}
};

// the reading thread: one chunk at a time, in the order they are wanted
void IoThread(std::shared_ptr<DriveIO> io)
{
	std::unique_lock<std::mutex> l(io->mu);
	while (!io->stop)
	{
		if (io->want.empty() || io->gone)
		{
			io->cv.wait(l);
			continue;
		}
		const uint64_t idx = io->want.front();
		io->want.pop_front();
		if (io->Find(idx))
			continue;
		Chunk* c = io->Victim();
		if (!c)
			continue;
		c->index = idx;
		c->loading = true;
		c->ok = 0;
		c->stamp = ++io->clock;
		const uint64_t first = idx * kChunk;
		const uint32_t n = io->sectors ? uint32_t(std::min<uint64_t>(kChunk, io->sectors > first ? io->sectors - first : 0)) : kChunk;
		uint8_t* data = c->data.data();
		l.unlock();
		uint64_t ok = 0;
		if (n && io->DeviceRead(first, n, data, true))
			ok = n == 64 ? ~uint64_t(0) : (uint64_t(1) << n) - 1;
		else
		{
			// what of it can be read, sector by sector (a scratch, the disc's end); four failures in a row: the rest
			// isn't tried (it stays known as unreadable: read-ahead won't try it again)
			int bad = 0;
			for (uint32_t s = 0; s < n && !io->gone && !io->stop && bad < 4; s++)
				if (io->DeviceRead(first + s, 1, data + size_t(s) * kSector, false))
				{
					ok |= uint64_t(1) << s;
					bad = 0;
				}
				else
					bad++;
		}
		l.lock();
		c->ok = ok;
		c->count = n;
		c->loading = false;
		io->cv.notify_all();
	}
	io->running = false;
}

// One disc in one drive: its reader, its file system.
class Media
{
public:
	std::string id, dev;
	uint64_t gen = 0;
	std::shared_ptr<DriveIO> io;
	std::atomic<bool>& gone;
	uint64_t& sectors;
	// the file system
	std::mutex udf_mu; // libudfread isn't for several threads at once
	udfread* udf = nullptr;
	std::vector<Node> nodes; // [0]: the root
	std::string fs, label;
	bool cut = false; // read in part (kMaxNodes, a loop, too deep, too big)
	// (udf_mu held) the blocks libudfread may still read in the call going on (UdfCall), -1: no bound; over: it
	// asked for more (libudfread reads a directory or a file entry whole, however big the disc says it is)
	int64_t udf_left = -1;
	bool udf_over = false;

	Media(std::string i, std::string d, int f, uint64_t g)
		: id(std::move(i)), dev(std::move(d)), gen(g), io(std::make_shared<DriveIO>()), gone(io->gone), sectors(io->sectors)
	{
		io->id = id;
		io->fd = f;
	}
	~Media()
	{
		if (udf)
			udfread_close(udf);
		// the reading thread ends on its own, after a read still in the drive (the device closes with it)
		std::lock_guard<std::mutex> l(io->mu);
		io->stop = true;
		io->want.clear();
		io->cv.notify_all();
	}

	// sectors [lba, lba + count) into out; false when they can't be read, the disc is gone, or this thread's reads
	// are cancelled (CancelScope): then at once, whatever the drive is doing
	bool Read(uint64_t lba, uint32_t count, uint8_t* out)
	{
		while (count > 0)
		{
			if (gone || Cancelled())
				return false;
			const uint64_t idx = lba / kChunk;
			const uint32_t in = uint32_t(lba % kChunk);
			const uint32_t n = std::min<uint32_t>(count, kChunk - in);
			std::unique_lock<std::mutex> l(io->mu);
			for (;;)
			{
				Chunk* c = io->Find(idx);
				if (c && !c->loading)
				{
					const uint64_t need = (n == 64 ? ~uint64_t(0) : ((uint64_t(1) << n) - 1)) << in;
					if (in + n > c->count || (c->ok & need) != need)
						return false; // known unreadable (or past the disc's end)
					memcpy(out, c->data.data() + size_t(in) * kSector, size_t(n) * kSector);
					c->stamp = ++io->clock;
					// in order (a film playing): the next chunks in the background
					if (idx == io->last_idx + 1 || idx == io->last_idx)
						for (int k = 1; k <= kAhead; k++)
						{
							const uint64_t a = idx + uint64_t(k);
							if ((!io->sectors || a * kChunk < io->sectors) && !io->Find(a) &&
								std::find(io->want.begin(), io->want.end(), a) == io->want.end())
								io->want.push_back(a);
						}
					io->last_idx = idx;
					Wake();
					break;
				}
				if (!c && std::find(io->want.begin(), io->want.end(), idx) == io->want.end())
					io->want.push_front(idx);
				Wake();
				io->cv.wait_for(l, std::chrono::milliseconds(50));
				if (gone || io->stop || Cancelled())
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
			return ioctl(io->fd, CDIOREADTOCHEADER, &th) == 0 && th.ending_track >= th.starting_track && th.starting_track > 0;
		}
#endif
		uint8_t s[kSector];
		return io->DeviceRead(16, 1, s, false);
	}

	// the disc's size as an image (libdvdread): when the drive doesn't say, as big as a disc can be (a read past its
	// end fails, as on any disc)
	uint64_t ImageSectors() const { return sectors ? sectors : uint64_t(UINT32_MAX); }

	double LastIo() const { return io->last_io; }
	int Fd() const { return io->fd; }

private:
	// (io->mu held) the reading thread, started when first needed
	void Wake()
	{
		if (!io->want.empty() && !io->running)
		{
			io->running = true;
			std::thread(IoThread, io).detach();
		}
		io->cv.notify_all();
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
	if (m->udf_left >= 0)
	{
		if (int64_t(n) > m->udf_left)
		{
			m->udf_over = true;
			return -1;
		}
		m->udf_left -= n;
	}
	return m->Read(lba, n, static_cast<uint8_t*>(buf)) ? int(n) : -1;
}

// a libudfread call that may read at most blocks blocks (udf_mu held, or the watcher's Mount)
class UdfCall
{
public:
	UdfCall(Media& m, int64_t blocks) : m_(m)
	{
		m_.udf_left = blocks;
		m_.udf_over = false;
		start_ = blocks;
	}
	~UdfCall() { m_.udf_left = -1; }
	int64_t Used() const { return start_ - m_.udf_left; }
	bool Over() const { return m_.udf_over; }

private:
	Media& m_;
	int64_t start_;
};

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
// reading the file system into Media::nodes, within bounds: a damaged or hostile disc (a directory inside itself, a
// huge directory) is read in part, in little memory, and quickly
constexpr int kMaxDepth = 24;
constexpr uint64_t kMaxDirBytes = 4u << 20; // one directory (a bigger one: ISO 9660 read in part, UDF not read)
constexpr uint64_t kMaxAllDirBytes = 32u << 20; // all of them
constexpr uint64_t kMaxWalkBytes = 128u << 20; // all a walk reads (UDF: the directories and the files' entries)
constexpr size_t kMaxDirs = 20000;
constexpr int64_t kUdfEntryBlocks = 16; // a file's entry, read for its size in a walk
constexpr int64_t kUdfOpenBlocks = 512; // a file's entry and its allocation extents (a file in up to ~129000 pieces)

// A directory too deep, or one already read (inside itself), is passed over -- that branch only; the walk ends when
// the budget is spent (entries, directories, bytes) or it is stopped.
struct WalkBudget
{
	std::set<uint64_t> seen; // ISO 9660: the directories' extents already read
	uint64_t dir_bytes = 0, read_bytes = 0;
	size_t dirs = 0;
	bool full = false;
};

bool WalkStop()
{
	return g_stop || Cancelled();
}

bool WalkEnd(const Media& m, const WalkBudget& b)
{
	return WalkStop() || b.full || m.nodes.size() >= kMaxNodes;
}

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

void SkipDir(Media& m, const std::string& what, const char* why)
{
	OrbisLog("[disc] %s: directory %s %s: passed over", m.id.c_str(), what.c_str(), why);
	m.cut = true;
}

// (udf_mu held) the directory at path, read through libudfread within the budget; nullptr: passed over (or the
// budget spent: b.full)
UDFDIR* UdfOpenDir(Media& m, const std::string& path, WalkBudget& b)
{
	const uint64_t left = std::min(kMaxAllDirBytes - std::min(kMaxAllDirBytes, b.dir_bytes),
		kMaxWalkBytes - std::min(kMaxWalkBytes, b.read_bytes));
	if (left < kSector)
	{
		b.full = m.cut = true;
		return nullptr;
	}
	const bool dir_bound = kMaxDirBytes <= left;
	UdfCall call(m, int64_t(std::min(kMaxDirBytes, left) / kSector) + 1); // + its file entry
	UDFDIR* d = udfread_opendir(m.udf, path.empty() ? "/" : path.c_str());
	b.dir_bytes += uint64_t(call.Used()) * kSector;
	b.read_bytes += uint64_t(call.Used()) * kSector;
	if (call.Over())
	{
		if (d)
			udfread_closedir(d);
		if (dir_bound)
			SkipDir(m, path.empty() ? "/" : path, "over 4 MiB");
		else
			b.full = m.cut = true;
		return nullptr;
	}
	return d;
}

void UdfWalk(Media& m, const std::string& path, int node, int depth, WalkBudget& b)
{
	if (WalkEnd(m, b))
		return;
	if (depth > kMaxDepth)
	{
		SkipDir(m, path.empty() ? "/" : path, "too deep");
		return;
	}
	if (++b.dirs > kMaxDirs)
	{
		b.full = m.cut = true;
		return;
	}
	UDFDIR* d = UdfOpenDir(m, path, b);
	if (!d)
		return;
	std::vector<std::pair<std::string, bool>> list;
	udfread_dirent e;
	while (udfread_readdir(d, &e))
	{
		if (!e.d_name || !strcmp(e.d_name, ".") || !strcmp(e.d_name, ".."))
			continue;
		list.push_back({e.d_name, e.d_type == UDF_DT_DIR});
		if (list.size() > kMaxNodes)
		{
			m.cut = true;
			break;
		}
	}
	for (const auto& x : list)
	{
		if (WalkEnd(m, b))
			break;
		Node n;
		n.name = x.first;
		n.dir = x.second;
		if (!n.dir)
		{
			UdfCall call(m, kUdfEntryBlocks);
			if (UDFFILE* f = udfread_file_openat(d, x.first.c_str()))
			{
				n.size = uint64_t(std::max<int64_t>(0, udfread_file_size(f)));
				udfread_file_close(f);
			}
			b.read_bytes += uint64_t(call.Used()) * kSector;
			if (b.read_bytes >= kMaxWalkBytes)
				b.full = m.cut = true;
		}
		const int i = AddNode(m, node, std::move(n));
		if (i < 0)
			break;
		if (x.second)
			UdfWalk(m, path + "/" + x.first, i, depth + 1, b);
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

// a directory's records, read a few sectors at a time (records don't cross sectors)
void IsoWalk(Media& m, uint64_t lba, uint64_t bytes, int node, bool joliet, int depth, WalkBudget& b)
{
	if (WalkEnd(m, b))
		return;
	if (depth > kMaxDepth || !b.seen.insert(lba).second)
	{
		// too deep, or a directory already read (one inside itself: a damaged or hostile disc): that branch only
		SkipDir(m, "at sector " + std::to_string(lba), depth > kMaxDepth ? "too deep" : "already read");
		return;
	}
	if (++b.dirs > kMaxDirs || b.dir_bytes >= kMaxAllDirBytes)
	{
		b.full = m.cut = true;
		return;
	}
	if (bytes > kMaxDirBytes || b.dir_bytes + bytes > kMaxAllDirBytes)
	{
		// read in part
		m.cut = true;
		bytes = std::min(kMaxDirBytes, kMaxAllDirBytes - b.dir_bytes);
	}
	b.dir_bytes += bytes;
	const uint32_t total = uint32_t((bytes + kSector - 1) / kSector);
	constexpr uint32_t kStep = 16;
	std::vector<uint8_t> buf(size_t(kStep) * kSector);
	int multi = -1; // a file whose last record said "more extents follow" (flag 0x80)
	for (uint32_t at = 0; at < total && !WalkStop(); at += kStep)
	{
		const uint32_t n = std::min(kStep, total - at);
		if (!m.Read(lba + at, n, buf.data()))
			return;
		for (uint32_t s = 0; s < n; s++)
		{
			const uint8_t* sec = &buf[size_t(s) * kSector];
			size_t p = 0;
			while (p < kSector)
			{
				const uint8_t len = sec[p];
				if (len == 0)
					break; // the rest of this sector is padding
				if (len < 34 || p + len > kSector)
					break;
				const uint8_t* r = sec + p;
				p += len;
				const uint8_t nl = r[32];
				if (33u + nl > len)
					break;
				if (nl == 1 && (r[33] == 0 || r[33] == 1))
					continue; // . and ..
				uint32_t ext_at, size;
				memcpy(&ext_at, r + 2, 4);
				memcpy(&size, r + 10, 4);
				const uint8_t flags = r[25];
				std::string name = joliet ? Ucs2ToUtf8(r + 33, nl) : std::string(reinterpret_cast<const char*>(r + 33), nl);
				const size_t semi = name.find(';');
				if (semi != std::string::npos)
					name.resize(semi);
				if (!joliet && !name.empty() && name.back() == '.')
					name.pop_back();
				const bool dir = (flags & 2) != 0;
				// the next extent of a file in several (ECMA-119 9.1.6: every record but the last has flag 0x80)
				if (!dir && multi >= 0 && m.nodes[multi].name == name)
				{
					Node& prev = m.nodes[multi];
					prev.extents.push_back({prev.size, ext_at, size});
					prev.size += size;
					if (!(flags & 0x80))
						multi = -1;
					continue;
				}
				multi = -1;
				Node nd;
				nd.name = name;
				nd.dir = dir;
				nd.size = dir ? 0 : size;
				nd.mtime = IsoTime(r + 18);
				if (!dir)
					nd.extents.push_back({0, ext_at, size});
				const int i = AddNode(m, node, std::move(nd));
				if (i < 0)
					return;
				if (!dir && (flags & 0x80))
					multi = i;
				if (dir)
					IsoWalk(m, ext_at, size, i, joliet, depth + 1, b);
				if (WalkEnd(m, b))
					return;
			}
		}
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

// the whole of a small file, through libudfread (data inside its file entry)
bool UdfReadAll(Media& m, const std::string& path, std::vector<uint8_t>& out, size_t max_bytes)
{
	std::lock_guard<std::mutex> l(m.udf_mu);
	UdfCall call(m, kUdfOpenBlocks + int64_t(max_bytes / kSector) + 1);
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
// a file on a disc, read through the sector reader: its extents -- ISO 9660's from its records, UDF's mapped once
// when it is opened (libudfread's allocation descriptors, neighbours joined) -- found by a binary search
constexpr size_t kMaxExtents = 262144; // a file in more pieces than this is read up to there

struct FileReader
{
	std::shared_ptr<Media> m;
	std::vector<Extent> ext; // in file order, not overlapping
	std::vector<uint8_t> mem; // a file read whole (UDF data inside its entry)
	bool in_mem = false;
	uint64_t size = 0, pos = 0;
	std::vector<uint8_t> tmp;

	const Extent* Locate(uint64_t at) const
	{
		auto it = std::upper_bound(ext.begin(), ext.end(), at, [](uint64_t a, const Extent& x) { return a < x.file_off; });
		if (it == ext.begin())
			return nullptr;
		--it;
		return at < it->file_off + it->bytes ? &*it : nullptr;
	}
};

// (udf_mu held) a UDF file's extents, once: false when it can't be mapped (cancelled, or none recorded at its start)
bool MapUdfFile(const Media& m, UDFFILE* uf, uint64_t size, std::vector<Extent>& ext)
{
	uint64_t off = 0;
	uint32_t lba = 0, blocks = 0;
	for (uint32_t i = 0; off < size && udfread_file_extent(uf, i, &lba, &blocks); i++)
	{
		if ((i & 4095) == 4095 && (Cancelled() || m.gone))
			return false;
		const uint64_t bytes = std::min<uint64_t>(uint64_t(blocks) * kSector, size - off);
		if (lba && bytes)
		{
			if (!ext.empty() && ext.back().file_off + ext.back().bytes == off && ext.back().bytes % kSector == 0 &&
				ext.back().lba + ext.back().bytes / kSector == lba)
				ext.back().bytes += bytes; // the next blocks on the disc too: one extent
			else if (ext.size() >= kMaxExtents)
			{
				OrbisLog("[disc] %s: a file in over %zu pieces: read up to %llu bytes", m.id.c_str(), kMaxExtents,
					(unsigned long long)off);
				break;
			}
			else
				ext.push_back({off, lba, bytes});
		}
		off += uint64_t(blocks) * kSector;
	}
	return !ext.empty() && ext.front().file_off == 0;
}

// the file at path on the Media (no checks of the drive's state: the caller's)
bool OpenReaderOn(const std::shared_ptr<Media>& m, const std::string& path, FileReader& f)
{
	f.m = m;
	const int n = FindNode(*m, path);
	if (n < 0 || m->nodes[n].dir)
		return false;
	f.size = m->nodes[n].size;
	if (!m->udf)
	{
		f.ext = m->nodes[n].extents;
		return true;
	}
	{
		std::lock_guard<std::mutex> l(m->udf_mu);
		UdfCall call(*m, kUdfOpenBlocks);
		UDFFILE* uf = udfread_file_open(m->udf, ("/" + path).c_str());
		if (!uf)
			return false;
		f.size = uint64_t(std::max<int64_t>(0, udfread_file_size(uf)));
		const bool mapped = f.size == 0 || MapUdfFile(*m, uf, f.size, f.ext);
		udfread_file_close(uf);
		if (mapped)
			return true;
		f.ext.clear();
		if (Cancelled() || m->gone)
			return false;
	}
	// data inside the file entry (a tiny file), or not recorded: read whole through libudfread
	if (UdfReadAll(*m, path, f.mem, 16u << 20))
	{
		f.in_mem = true;
		f.size = f.mem.size();
		return true;
	}
	return false;
}

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
	const Extent* e = f->Locate(f->pos);
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
		return Cancelled() ? AVERROR_EXIT : AVERROR(EIO);
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

// count bytes at offset into out (whole sectors' worth), false on a read failure
bool ReadAt(FileReader& f, uint64_t offset, uint8_t* out, size_t count)
{
	f.pos = offset;
	size_t done = 0;
	while (done < count)
	{
		const int r = FileRead(&f, out + done, int(std::min<size_t>(count - done, 1u << 20)));
		if (r <= 0)
			return false;
		done += size_t(r);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// the drives
struct Slot
{
	std::string id, dev;
	bool usb = false;
	std::shared_ptr<Media> media;
	DriveState state;
};

// (never destroyed: a watcher stuck in a drive that doesn't answer may outlive Stop(), even the app's exit)
std::mutex& g_mu = *new std::mutex;
std::condition_variable& g_cv = *new std::condition_variable;
std::vector<Slot>& g_slots = *new std::vector<Slot>;
std::thread g_thread;
bool g_poke = false, g_started = false;
bool g_watching = false; // (g_mu) the watcher thread runs
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

// Runs of 16 sectors spread across a title VOB (a few for a small one, never the same twice): any marked scrambled?
// failures: the runs that couldn't be read.
template <typename ReadAtFn>
bool VobScrambled(uint64_t sectors, ReadAtFn read, int* attempts, int* failures)
{
	if (sectors == 0)
		return false;
	std::vector<uint8_t> buf(16 * kSector);
	const uint64_t span = sectors > 16 ? sectors - 16 : 0;
	const int k = int(std::min<uint64_t>(4, sectors / 16 + 1));
	uint64_t last = UINT64_MAX;
	for (int i = 0; i < k; i++)
	{
		const uint64_t a = k > 1 ? span * uint64_t(i) / uint64_t(k - 1) : 0;
		if (a == last)
			continue;
		last = a;
		const uint32_t n = uint32_t(std::min<uint64_t>(16, sectors - a));
		(*attempts)++;
		if (!read(a, n, buf.data()))
		{
			(*failures)++;
			continue;
		}
		if (Scrambled(buf.data(), n))
			return true;
	}
	return false;
}

void Classify(const std::shared_ptr<Media>& mp, DriveState& st)
{
	Media& m = *mp;
	auto has = [&](const char* p) { return FindNode(m, p) >= 0; };
	if (has("VIDEO_TS/VIDEO_TS.IFO"))
	{
		st.kind = Kind::DvdVideo;
#ifdef __PROSPERO__
		// the drive's copyright information (lead-in): CSS / CPPM, CPRM
		dvd_struct ds = {};
		ds.format = DVD_STRUCT_COPYRIGHT;
		if (ioctl(m.Fd(), DVDIOCREADSTRUCTURE, &ds) == 0 && ds.cpst != 0)
		{
			OrbisLog("[disc] %s: copyright protection system %u (the drive says so)", m.id.c_str(), ds.cpst);
			st.protection = ds.cpst == 2 ? "CPRM" : "CSS";
		}
#endif
		// the title VOBs' packs (every title set's, every part), sampled
		const int vt = FindNode(m, "VIDEO_TS");
		int attempts = 0, failures = 0;
		if (st.protection.empty() && vt >= 0)
			for (int k : m.nodes[vt].kids)
			{
				const std::string n = Lower(m.nodes[k].name);
				if (n.size() != 12 || n.compare(0, 4, "vts_") || n.compare(6, 1, "_") || n[7] < '1' || n[7] > '9' ||
					n.compare(8, 4, ".vob"))
					continue;
				FileReader f;
				if (!OpenReaderOn(mp, "VIDEO_TS/" + m.nodes[k].name, f))
					continue;
				auto read = [&](uint64_t s, uint32_t cnt, uint8_t* out) { return ReadAt(f, s * kSector, out, size_t(cnt) * kSector); };
				if (VobScrambled(f.size / kSector, read, &attempts, &failures))
				{
					OrbisLog("[disc] %s: %s has scrambled packs", m.id.c_str(), m.nodes[k].name.c_str());
					st.protection = "CSS";
					break;
				}
				if (m.gone || WalkStop())
					break;
			}
		if (!st.protection.empty())
			st.kind = Kind::Encrypted;
		else if (failures >= 3 && failures * 2 >= attempts && !m.gone)
		{
			// its films' sectors don't read: a scratched or damaged disc (or one the drive won't read without the
			// authentication this app never does) -- not called encrypted on that alone
			OrbisLog("[disc] %s: %d of %d samples of the title VOBs don't read", m.id.c_str(), failures, attempts);
			st.kind = Kind::Unreadable;
			st.damaged = true;
		}
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
// the disc's size in sectors, as 5S2CD finds it: a DVD's physical format (its data area starts at 0x30000 -- the
// PS5's drive answers for CDs too; the medium's size when larger), else a CD's table of contents (its lead-out), else
// the medium's size; 0 when none says (reads then aren't held to an end)
// which size: a DVD's physical format's, or the medium's when larger (a recordable DVD may give a physical end
// short of what was written); else the table of contents'; else the medium's (0: none known)
uint64_t PickSectors(uint64_t dvd, uint64_t toc, uint64_t media)
{
	if (dvd)
		return std::max(dvd, media);
	return toc ? toc : media;
}

uint64_t DiscSectors(int fd)
{
#ifdef __PROSPERO__
	off_t mb = 0;
	const uint64_t media = ioctl(fd, DIOCGMEDIASIZE, &mb) == 0 && mb > 0 ? uint64_t(mb) / kSector : 0;
	uint64_t dvd = 0;
	dvd_struct ds = {};
	ds.format = DVD_STRUCT_PHYSICAL;
	ds.layer_num = 0;
	if (ioctl(fd, DVDIOCREADSTRUCTURE, &ds) == 0)
	{
		dvd_layer l0;
		memcpy(&l0, ds.data, sizeof l0);
		const uint32_t start = l0.start_sector, end = l0.end_sector;
		if (end && end >= start && start == 0x30000)
		{
			if (l0.nlayers == 0)
				dvd = uint64_t(end - start) + 1;
			else if (l0.track_path == 0)
			{
				// parallel track path: layer 1's own range
				dvd_struct ds1 = {};
				ds1.format = DVD_STRUCT_PHYSICAL;
				ds1.layer_num = 1;
				if (ioctl(fd, DVDIOCREADSTRUCTURE, &ds1) == 0)
				{
					dvd_layer l1;
					memcpy(&l1, ds1.data, sizeof l1);
					if (l1.end_sector >= l1.start_sector)
						dvd = uint64_t(end - start) + 1 + uint64_t(l1.end_sector - l1.start_sector) + 1;
				}
			}
			else
			{
				// opposite track path: layer 1 runs from the complement of layer 0's end
				const uint32_t end0 = l0.end_sector_l0, start1 = ~end0 & 0xFFFFFFu;
				if (end0 >= start && end >= start1)
					dvd = uint64_t(end0 - start) + 1 + uint64_t(end - start1) + 1;
			}
		}
	}
	uint64_t toc = 0;
	ioc_toc_header th = {};
	if (ioctl(fd, CDIOREADTOCHEADER, &th) == 0 && th.ending_track >= th.starting_track && th.starting_track > 0)
	{
		const int n = th.ending_track - th.starting_track + 2; // + the lead-out
		std::vector<cd_toc_entry> entries(static_cast<size_t>(n));
		ioc_read_toc_entry te = {};
		te.address_format = CD_LBA_FORMAT;
		te.starting_track = th.starting_track;
		te.data_len = static_cast<u_short>(size_t(n) * sizeof(cd_toc_entry));
		te.data = entries.data();
		if (ioctl(fd, CDIOREADTOCENTRYS, &te) == 0)
			for (const cd_toc_entry& e : entries)
				if (e.track == 0xAA)
				{
					// cd(4) gives LBAs in network byte order (5S2CD: one beyond any disc means it didn't)
					const uint32_t raw = uint32_t(e.addr.lba), sw = ntohl(raw);
					const uint32_t lba = sw < 0x1000000 ? sw : raw;
					if (lba > 0 && lba < 0x1000000)
						toc = lba;
				}
	}
	return PickSectors(dvd, toc, media);
#else
	if (g_fault_nosize)
		return 0;
	struct stat sb = {};
	return PickSectors(0, 0, fstat(fd, &sb) == 0 && sb.st_size > 0 ? uint64_t(sb.st_size) / kSector : 0);
#endif
}

void Mount(const std::shared_ptr<Media>& mp, DriveState& st)
{
	Media& m = *mp;
	m.sectors = DiscSectors(m.Fd());
	if (!m.sectors)
		OrbisLog("[disc] %s: the drive doesn't say the disc's size: read to where it ends", m.id.c_str());
	// the volume descriptors from sector 16: ISO 9660 (+ Joliet), and the UDF recognition sequence
	uint8_t s[kSector];
	uint64_t iso_root = 0, iso_root_len = 0, jol_root = 0, jol_root_len = 0;
	std::string iso_label, jol_label;
	bool udf = false;
	uint64_t vol_sectors = 0;
	for (uint64_t sec = 16; sec < 16 + 64 && !WalkStop(); sec++)
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
	// (a size not known stays 0 for the reader -- the volume's own size is only shown: a wrong one mustn't cut reads)
	const uint64_t shown_sectors = m.sectors ? m.sectors : vol_sectors;
	m.nodes.clear();
	m.nodes.push_back(Node{"", true, 0, 0, {}, {}});
	WalkBudget budget;
	if (udf && !WalkStop())
	{
		UdfInput* in = new UdfInput;
		in->in.close = UdfClose;
		in->in.read = UdfRead;
		in->in.size = m.sectors ? UdfSize : nullptr;
		in->m = &m;
		m.udf = udfread_init();
		bool opened = false;
		if (m.udf)
		{
			// the volume's descriptors and its root directory (a root over 4 MiB: the disc is read as ISO 9660, if it
			// is that too)
			UdfCall call(m, kUdfOpenBlocks + int64_t(kMaxDirBytes / kSector) + 1);
			opened = udfread_open_input(m.udf, &in->in) >= 0;
			budget.read_bytes += uint64_t(call.Used()) * kSector;
		}
		if (opened)
		{
			m.fs = "UDF";
			const char* v = udfread_get_volume_id(m.udf);
			m.label = v ? TrimRight(v) : "";
			std::lock_guard<std::mutex> l(m.udf_mu);
			UdfWalk(m, "", 0, 0, budget);
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
	if (!m.udf && (jol_root || iso_root) && !WalkStop())
	{
		const bool j = jol_root != 0;
		m.fs = j ? "ISO 9660 + Joliet" : "ISO 9660";
		m.label = j && !jol_label.empty() ? jol_label : iso_label;
		IsoWalk(m, j ? jol_root : iso_root, j ? jol_root_len : iso_root_len, 0, j, 0, budget);
	}
	st.fs = m.fs;
	st.label = m.label;
	st.sectors = shown_sectors;
	st.cut = m.cut;
	if (m.fs.empty())
	{
		st.kind = Kind::Unreadable;
		OrbisLog("[disc] %s: no file system this player reads (%llu sectors)", m.id.c_str(), (unsigned long long)shown_sectors);
		return;
	}
	if (m.cut)
		OrbisLog("[disc] %s: its file system was read in part (%zu entries, %llu KiB of directories): too big or damaged",
			m.id.c_str(), m.nodes.size() - 1, (unsigned long long)(budget.dir_bytes >> 10));
	Classify(mp, st);
	static const char* const kKinds[] = {"no disc", "reading", "data", "DVD-Video", "Blu-ray", "encrypted", "unreadable"};
	OrbisLog("[disc] %s: \"%s\", %s, %llu sectors, %zu entries%s: %s%s%s", m.id.c_str(), m.label.c_str(), m.fs.c_str(),
		(unsigned long long)shown_sectors, m.nodes.size() - 1, m.cut ? " (cut)" : "", kKinds[int(st.kind)],
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
	std::vector<std::shared_ptr<Media>> drop; // let go of after the lock (never destroyed while it is held)
	{
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
			if (s.media)
			{
				s.media->gone = true; // a USB drive unplugged
				drop.push_back(std::move(s.media));
			}
		g_slots = std::move(next);
	}
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
		if (g_stop)
			return;
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
			if (there && Now() - copy.media->LastIo() > 5 && !copy.media->Probe())
				there = false;
#endif
			if (!there)
			{
				copy.media->gone = true;
				std::shared_ptr<Media> drop;
				{
					std::lock_guard<std::mutex> l(g_mu);
					for (auto& s : g_slots)
						if (s.id == id && s.media == copy.media)
						{
							drop = std::move(s.media);
							s.state.kind = Kind::NoDisc;
							s.state.label.clear();
							s.state.fs.clear();
							s.state.protection.clear();
							s.state.generation = 0;
							s.state.sectors = 0;
							s.state.damaged = s.state.cut = false;
						}
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
		st.damaged = st.cut = false;
		st.protection.clear();
		try
		{
			Mount(m, st);
		}
		catch (const std::bad_alloc&)
		{
			OrbisLog("[disc] %s: out of memory reading its file system", id.c_str());
			m->nodes.clear();
			m->nodes.push_back(Node{"", true, 0, 0, {}, {}});
			st.kind = Kind::Unreadable;
		}
		if (g_stop)
			return;
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
	CancelScope stop_reads(&g_stop); // Stop(): a disc being read gives up at once
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
	g_watching = false;
	g_cv.notify_all();
}

// ---------------------------------------------------------------------------------------------------------------
// URLs
// a path on a disc as FindNode walks it: no empty or "." parts ("//VIDEO_TS/./x" is "VIDEO_TS/x")
std::string NormalPath(const std::string& p)
{
	std::string out;
	size_t at = 0;
	while (at <= p.size())
	{
		size_t e = p.find('/', at);
		if (e == std::string::npos)
			e = p.size();
		const std::string part = p.substr(at, e - at);
		if (!part.empty() && part != ".")
		{
			if (!out.empty())
				out += '/';
			out += part;
		}
		at = e + 1;
	}
	return out;
}

// disc://cd0/a/b -> ("cd0", "a/b")
bool SplitDiscUrl(const std::string& url, std::string& id, std::string& path)
{
	if (!StartsWith(url, "disc://"))
		return false;
	const std::string rest = url.substr(7);
	const size_t slash = rest.find('/');
	id = rest.substr(0, slash);
	path = slash == std::string::npos ? "" : NormalPath(rest.substr(slash + 1));
	return !id.empty();
}

// a path inside an encrypted disc's film structure (its VOBs, its M2TS)
bool InFilmStructure(const std::string& path)
{
	const std::string l = Lower(path);
	return StartsWith(l, "video_ts/") || StartsWith(l, "bdmv/") || StartsWith(l, "aacs/") || StartsWith(l, "bdsvm/");
}

bool OpenFileReader(const std::string& url, FileReader& f, std::string* error)
{
	std::string id, path;
	if (!SplitDiscUrl(url, id, path))
		return false;
	auto m = MediaOf(id);
	DriveState d;
	if (!m || !GetDrive(id, &d) || d.generation != m->gen)
	{
		if (error)
			*error = "No disc";
		return false;
	}
	// an encrypted disc's films aren't read as files either (scrambled data would go to FFmpeg)
	if (d.kind == Kind::Encrypted && InFilmStructure(path))
	{
		OrbisLog("[disc] %s: %s is on an encrypted disc (%s): refused", id.c_str(), path.c_str(), d.protection.c_str());
		if (error)
			*error = EncryptedText();
		return false;
	}
	if (OpenReaderOn(m, path, f))
		return true;
	if (error)
		*error = "The file can't be opened from the disc.";
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// libdvdread reads "mpcdisc:cd0:<generation>" through the sector reader -- a UDF disc as an image, an ISO 9660-only
// one as a folder of files -- and anything else (a VIDEO_TS folder on a drive) as files. A disc swapped since the
// URL was made isn't read (its generation is another).
struct DvdHandle
{
	std::shared_ptr<Media> m;
	std::unique_ptr<FileReader> file; // a file on the disc (folder mode)
	uint64_t pos = 0;
	int fd = -1;
};

struct DvdDir
{
	DIR* d = nullptr;
	std::vector<std::string> names; // a folder on the disc
	size_t i = 0;
};

// "mpcdisc:cd0:7/VIDEO_TS/VTS_01_1.VOB" -> the disc (when it is still generation 7) and "VIDEO_TS/VTS_01_1.VOB"
std::shared_ptr<Media> DvdMedia(const char* path, std::string* sub = nullptr)
{
	if (strncmp(path, "mpcdisc:", 8) != 0)
		return nullptr;
	std::string p = path + 8;
	const size_t slash = p.find('/');
	const std::string root = p.substr(0, slash);
	if (sub)
		*sub = slash == std::string::npos ? "" : NormalPath(p.substr(slash + 1));
	const size_t colon = root.find(':');
	if (colon == std::string::npos)
		return nullptr;
	auto m = MediaOf(root.substr(0, colon));
	if (!m || std::to_string(m->gen) != root.substr(colon + 1))
		return nullptr;
	return m;
}

int DvdStat(dvd_reader_filesystem_h*, const char* path, dvdstat_t* st)
{
	if (strncmp(path, "mpcdisc:", 8) == 0)
	{
		std::string sub;
		auto m = DvdMedia(path, &sub);
		if (!m)
			return -1;
		if (sub.empty() && m->udf)
		{
			st->size = m->ImageSectors() * kSector;
			st->st_mode = DVD_S_IFBLK;
			return 0;
		}
		const int n = FindNode(*m, sub);
		if (n < 0)
			return -1;
		st->size = m->nodes[n].size;
		st->st_mode = m->nodes[n].dir ? DVD_S_IFDIR : DVD_S_IFREG;
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
	DvdDir* d = new DvdDir;
	if (strncmp(dirname, "mpcdisc:", 8) == 0)
	{
		std::string sub;
		auto m = DvdMedia(dirname, &sub);
		const int n = m ? FindNode(*m, sub) : -1;
		if (n >= 0 && m->nodes[n].dir)
		{
			for (int k : m->nodes[n].kids)
				d->names.push_back(m->nodes[k].name);
			return d;
		}
	}
	else if ((d->d = opendir(dirname)) != nullptr)
		return d;
	delete d;
	return nullptr;
}

int DvdDirRead(void* dir, dvd_dirent_t* entry)
{
	DvdDir* d = static_cast<DvdDir*>(dir);
	if (!d->d)
	{
		if (d->i >= d->names.size())
			return 1;
		strncpy(entry->d_name, d->names[d->i++].c_str(), sizeof(entry->d_name) - 1);
		entry->d_name[sizeof(entry->d_name) - 1] = 0;
		return 0;
	}
	errno = 0;
	dirent* e = readdir(d->d);
	if (!e)
		return errno ? -errno : 1;
	strncpy(entry->d_name, e->d_name, sizeof(entry->d_name) - 1);
	entry->d_name[sizeof(entry->d_name) - 1] = 0;
	return 0;
}

void DvdDirClose(void* dir)
{
	DvdDir* d = static_cast<DvdDir*>(dir);
	if (d && d->d)
		closedir(d->d);
	delete d;
}

void* DvdFileOpen(dvd_reader_filesystem_h*, const char* name)
{
	DvdHandle* h = new DvdHandle;
	if (strncmp(name, "mpcdisc:", 8) == 0)
	{
		std::string sub;
		h->m = DvdMedia(name, &sub);
		if (h->m && sub.empty() && h->m->udf)
			return h; // the disc as an image
		if (h->m && !sub.empty())
		{
			h->file.reset(new FileReader);
			if (OpenReaderOn(h->m, sub, *h->file))
				return h;
		}
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
	if (h->file)
	{
		size_t done = 0;
		while (done < size)
		{
			const int r = FileRead(h->file.get(), reinterpret_cast<uint8_t*>(buf) + done, int(std::min<size_t>(size - done, 1u << 20)));
			if (r == AVERROR_EOF)
				break;
			if (r < 0)
				return done ? ssize_t(done) : -1;
			done += size_t(r);
		}
		return ssize_t(done);
	}
	const uint64_t end = h->m->ImageSectors() * kSector;
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
	if (h->file)
	{
		const int64_t r = FileSeek(h->file.get(), offset, whence);
		return r < 0 ? -1 : r;
	}
	const int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? int64_t(h->pos) + offset : int64_t(h->m->ImageSectors() * kSector) + offset;
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
		return fail("Out of memory.");
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
		if (g_watching)
		{
			OrbisLog("[disc] the last watcher hasn't ended (a drive not answering): not started again");
			return;
		}
		g_started = g_watching = true;
		g_stop = false;
	}
#ifndef __PROSPERO__
	ReadFaultSetting();
#endif
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
	{
		// the watcher ends at once -- unless it is in a drive that doesn't answer (opening a disc, asking whether it
		// is still there): then it isn't waited for past 2 s; it ends when the drive answers
		std::unique_lock<std::mutex> l(g_mu);
		const bool ended = g_cv.wait_for(l, std::chrono::seconds(2), [] { return !g_watching; });
		l.unlock();
		if (!ended)
		{
			OrbisLog("[disc] the watcher is in a drive that doesn't answer: not waited for");
			g_thread.detach();
		}
		else if (g_thread.joinable())
			g_thread.join();
	}
	std::vector<Slot> drop; // the discs let go of after the lock (never destroyed while it is held)
	{
		std::lock_guard<std::mutex> l(g_mu);
		drop.swap(g_slots);
		g_started = false;
	}
}

CancelScope::CancelScope(const std::atomic<bool>* flag) : prev_(t_cancel)
{
	t_cancel = flag;
}

CancelScope::~CancelScope()
{
	t_cancel = prev_;
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

const char* DamagedText()
{
	return "This disc can't be read: it is scratched or damaged, or it is an encrypted film.";
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
			if (d.damaged)
				return T(DamagedText());
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

bool ReadFileWithin(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, double max_seconds)
{
	// read on a thread of its own, cancelled when the time is up: the read gives up at once (a sector still in the
	// drive is finished by the disc's reading thread, not waited for here)
	struct Job
	{
		std::atomic<bool> cancel{false}, done{false};
		std::vector<uint8_t> data;
		bool ok = false;
	};
	auto job = std::make_shared<Job>();
	std::thread t([job, url, max_bytes] {
		CancelScope scope(&job->cancel);
		job->ok = ReadFile(url, job->data, max_bytes);
		job->done = true;
	});
	const double t0 = Now();
	while (!job->done)
	{
		if (Now() - t0 > max_seconds || Cancelled() || g_stop)
		{
			if (!job->cancel)
				OrbisLog("[disc] %s: not read within %.1f s: given up", url.c_str(), max_seconds);
			job->cancel = true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	t.join();
	if (!job->ok || job->cancel)
		return false;
	out.swap(job->data);
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
			*error = "Out of memory.";
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
		if (d.kind == Kind::Unreadable && d.damaged)
		{
			out.error = DamagedText();
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
	// a disc: "mpcdisc:<drive>:<generation>" (a UDF disc read as an image, an ISO 9660-only one as a folder of files)
	const std::string path = m ? "mpcdisc:" + id + ":" + std::to_string(m->gen) : folder;
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
					int attempts = 0, failures = 0;
					scrambled = VobScrambled(secs, [&](uint64_t s, uint32_t n, uint8_t* b) {
						return DVDReadBlocks(f, int(s), n, b) == ssize_t(n);
					}, &attempts, &failures);
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
		out.error = "This build can't play DVD-Video (FFmpeg without its DVD-Video demuxer).";
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
