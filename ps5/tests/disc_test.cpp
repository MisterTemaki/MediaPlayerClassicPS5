// Media Player Classic PS5 host tests (1.4): player/mpc_disc.cpp on its own, against disc images, with faults put into
// the drive (a slow, failing or vanishing disc). Built by make host as build/host/mpc-disc-test; run by
// tests/run-tests.sh (group 26).
//
//   mpc-disc-test mount <image> <max seconds> <max MiB>   the disc is read (and Stop() returns) within the bounds
//   mpc-disc-test title <image> <dvd://cd0...>             OpenTitle, then FFmpeg reads the title to its end
//   mpc-disc-test list <image> <disc://cd0/...>            the folder's entries (name size)
//   mpc-disc-test sector <image> <disc://cd0/...>          the file's first sector
//   mpc-disc-test read <image> <disc://cd0/...> <ref>      the file read whole is <ref>'s bytes
//   mpc-disc-test open <image> <disc://cd0/...>            OpenAvio: "ok" or the error
//   mpc-disc-test swap <image> <dvd://cd0>                 a disc swapped between OpenTitle and FFmpeg's open: refused
//   mpc-disc-test stop-failing <image> <disc://cd0/file>   the drive fails (slowly) during read-ahead: Stop() returns
//   mpc-disc-test cancel <image> <disc://cd0/file>         a read stuck on a slow drive gives up when cancelled
//                                                          (the file: over 4 MiB)
//   mpc-disc-test negcache <image> <disc://cd0/file>       a part that can't be read isn't read again
//   mpc-disc-test scan <image> <disc://cd0/file> <max s>    the file read in order, whole, within max seconds
//   mpc-disc-test ui-read <image> <disc://cd0/file>        the UI's read of a disc file (File > Load Subtitle) on a
//                                                          drive that takes 2.5 s a read: given up after 1 s
//   mpc-disc-test stop-hung <fifo>                         Stop() while the watcher waits for a drive that doesn't
//                                                          answer (opening a FIFO no one writes to): within 3 s
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../player/mpc_disc.cpp"

#include <sys/resource.h>

#include <cstdarg>
#include <fstream>
#include <iterator>

bool OrbisIsDir(const std::string& p)
{
	struct stat s;
	return stat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
}
bool OrbisIsFile(const std::string& p)
{
	struct stat s;
	return stat(p.c_str(), &s) == 0 && S_ISREG(s.st_mode);
}
void OrbisLog(const char* f, ...)
{
	if (!getenv("DISC_TEST_LOG"))
		return;
	va_list v;
	va_start(v, f);
	vfprintf(stderr, f, v);
	va_end(v);
	fputc('\n', stderr);
}
namespace mpc
{
const char* T(const char* e)
{
	return e;
}
std::string Fmt(const char* f, ...)
{
	char b[1024];
	va_list v;
	va_start(v, f);
	vsnprintf(b, sizeof b, f, v);
	va_end(v);
	return b;
}
} // namespace mpc

using namespace mpc;
using namespace mpc::disc;

namespace
{
double MaxRssMiB()
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_maxrss / 1024.0;
}

// the disc in cd0 read (not Reading, not NoDisc), within max seconds
bool WaitMounted(double max, DriveState* d)
{
	const double t0 = Now();
	while (Now() - t0 < max)
	{
		if (GetDrive("cd0", d) && d->kind != Kind::Reading && d->kind != Kind::NoDisc)
			return true;
		usleep(10000);
	}
	return false;
}

// the sector 3 MiB into a disc file (the faults start there: the file system, and the start of the file read when
// it is opened, stay readable)
constexpr uint64_t kFaultAt = 3u << 20;
int64_t FaultSector(const std::string& url)
{
	std::string id, path;
	if (!SplitDiscUrl(url, id, path))
		return -1;
	auto m = MediaOf(id);
	FileReader f;
	if (!m || !OpenReaderOn(m, path, f))
		return -1;
	const Extent* e = f.Locate(kFaultAt);
	return e ? int64_t(e->lba + (kFaultAt - e->file_off) / kSector) : -1;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
		return 2;
	const std::string mode = argv[1];
	setenv("MPCPS5_HOST_DISC", argv[2], 1);
	Start();
	DriveState d;
	const double t0 = Now();
	if (mode == "stop-hung")
	{
		usleep(300000); // the watcher is in the drive now
		const double ts = Now();
		Stop();
		printf("stopped in %.2f s\n", Now() - ts);
		return Now() - ts < 3 ? 0 : 1;
	}
	if (mode == "mount")
	{
		const double max_s = argc > 3 ? atof(argv[3]) : 10, max_mib = argc > 4 ? atof(argv[4]) : 512;
		const bool ok = WaitMounted(max_s, &d);
		const double t = Now() - t0;
		const double ts = Now();
		Stop();
		const double stop_s = Now() - ts;
		printf("mounted %d kind %d cut %d damaged %d protection '%s' fs %s label %s in %.2f s, stop %.2f s, rss %.0f MiB\n",
			int(ok), int(d.kind), int(d.cut), int(d.damaged), d.protection.c_str(), d.fs.c_str(), d.label.c_str(), t, stop_s,
			MaxRssMiB());
		return ok && t < max_s && stop_s < 2 && MaxRssMiB() < max_mib ? 0 : 1;
	}
	if (!WaitMounted(20, &d))
	{
		printf("not mounted\n");
		return 1;
	}
	int rc = 0;
	if (mode == "title" && argc > 3)
	{
		TitleOpen t;
		const bool ok = OpenTitle(argv[3], t);
		printf("open %d %s titles %zu title %d\n", int(ok), t.error.c_str(), t.titles.size(), t.title);
		if (ok)
		{
			AVFormatContext* ic = avformat_alloc_context();
			if (t.io)
			{
				ic->pb = t.io;
				ic->flags |= AVFMT_FLAG_CUSTOM_IO;
			}
			AVDictionary* o = nullptr;
			av_dict_copy(&o, t.options, 0);
			const int r = avformat_open_input(&ic, t.ffmpeg_url.c_str(), t.format, &o);
			av_dict_free(&o);
			int n = 0;
			if (r >= 0)
			{
				AVPacket* p = av_packet_alloc();
				while (av_read_frame(ic, p) >= 0 && n < 100000)
				{
					n++;
					av_packet_unref(p);
				}
				av_packet_free(&p);
				avformat_close_input(&ic);
			}
			printf("ffmpeg %d packets %d\n", r, n);
			rc = r >= 0 && n > 0 ? 0 : 4;
		}
		else
			rc = 3;
		CloseTitle(t);
	}
	else if (mode == "sector" && argc > 3)
	{
		std::string id, path;
		FileReader f;
		auto m = SplitDiscUrl(argv[3], id, path) ? MediaOf(id) : nullptr;
		const Extent* e = m && OpenReaderOn(m, path, f) ? f.Locate(0) : nullptr;
		printf("%lld\n", e ? (long long)e->lba : -1LL);
		rc = e ? 0 : 1;
	}
	else if (mode == "swap" && argc > 3)
	{
		// OpenTitle, then the disc is taken out and another (here: the same image again, a new generation) put in
		// before FFmpeg opens the title: the demuxer mustn't read the new disc as if it were the old
		TitleOpen t;
		const bool ok = OpenTitle(argv[3], t);
		HostEject("cd0", true);
		usleep(300000);
		HostEject("cd0", false);
		DriveState d2;
		const double tw = Now();
		while (Now() - tw < 10 && !(GetDrive("cd0", &d2) && d2.kind == Kind::DvdVideo && d2.generation != d.generation))
			usleep(10000);
		AVFormatContext* ic = nullptr;
		AVDictionary* o = nullptr;
		av_dict_copy(&o, t.options, 0);
		const int r = ok ? avformat_open_input(&ic, t.ffmpeg_url.c_str(), t.format, &o) : -1;
		av_dict_free(&o);
		if (r >= 0)
			avformat_close_input(&ic);
		printf("generation %llu -> %llu: the old title's open %s\n", (unsigned long long)d.generation,
			(unsigned long long)d2.generation, r >= 0 ? "READ THE NEW DISC" : "refused");
		CloseTitle(t);
		rc = ok && d2.generation != d.generation && r < 0 ? 0 : 1;
	}
	else if (mode == "list" && argc > 3)
	{
		std::vector<Entry> e;
		std::string err;
		if (!List(argv[3], e, &err))
		{
			printf("error %s\n", err.c_str());
			rc = 1;
		}
		for (const auto& x : e)
			printf("%s %llu%s\n", x.name.c_str(), (unsigned long long)x.size, x.dir ? " dir" : "");
	}
	else if (mode == "read" && argc > 4)
	{
		std::vector<uint8_t> got;
		std::ifstream f(argv[4], std::ios::binary);
		const std::vector<uint8_t> want((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		const bool ok = ReadFile(argv[3], got, 1u << 30);
		printf("read %d: %zu bytes, %s\n", int(ok), got.size(), got == want ? "the same" : "DIFFERENT");
		rc = ok && got == want ? 0 : 1;
	}
	else if (mode == "open" && argc > 3)
	{
		void* owner = nullptr;
		std::string err;
		AVIOContext* io = OpenAvio(argv[3], &owner, &err);
		printf("%s\n", io ? "ok" : err.c_str());
		if (io)
			CloseAvio(io, owner);
	}
	else if (mode == "stop-failing" && argc > 3)
	{
		// reading in order (read-ahead going), the player closes the file, then the drive fails, slowly: Stop()
		// mustn't hang (1.4's first version: a disc's last owner was let go of under the drives' lock, and its
		// read-ahead thread, in a failed read, wanted that lock)
		void* owner = nullptr;
		std::string err;
		AVIOContext* io = OpenAvio(argv[3], &owner, &err);
		if (!io)
		{
			printf("open: %s\n", err.c_str());
			return 1;
		}
		uint8_t buf[65536];
		for (int i = 0; i < 4; i++)
			avio_read(io, buf, sizeof buf);
		CloseAvio(io, owner);
		g_fault_from = 0;
		g_fault_slow_ms = 300;
		g_fault_fail = true;
		HostEject("cd0", false); // (wakes the watcher)
		usleep(100000);
		alarm(20);
		const double ts = Now();
		Stop();
		printf("stopped in %.2f s\n", Now() - ts);
		return Now() - ts < 3 ? 0 : 1;
	}
	else if (mode == "cancel" && argc > 3)
	{
		// the drive answers in 1.5 s from 3 MiB into the file on; a reader there on its own thread is cancelled after
		// 0.2 s: it gives up at once
		const int64_t first = FaultSector(argv[3]);
		void* owner = nullptr;
		std::string err;
		g_fault_from = first;
		g_fault_slow_ms = 1500;
		std::atomic<bool> cancel{false};
		double gave_up = -1;
		int result = 0;
		std::thread reader([&] {
			CancelScope scope(&cancel);
			AVIOContext* io = OpenAvio(argv[3], &owner, &err);
			if (!io)
				return;
			uint8_t buf[4096];
			avio_seek(io, int64_t(kFaultAt), SEEK_SET);
			result = avio_read(io, buf, sizeof buf);
			gave_up = Now();
			CloseAvio(io, owner);
		});
		usleep(200000);
		const double tc = Now();
		cancel = true;
		reader.join();
		printf("read gave up %.2f s after the cancel (result %d)\n", gave_up - tc, result);
		g_fault_from = -1;
		rc = gave_up > 0 && gave_up - tc < 0.5 && result < 0 ? 0 : 1;
	}
	else if (mode == "negcache" && argc > 3)
	{
		// from 3 MiB into the file the drive fails (at once): reading the file twice tries the bad part once
		const int64_t first = FaultSector(argv[3]);
		g_fault_from = first;
		g_fault_fail = true;
		std::vector<uint8_t> out;
		const int before = g_fault_hits;
		ReadFile(argv[3], out, 1u << 30);
		const int one = g_fault_hits - before;
		ReadFile(argv[3], out, 1u << 30);
		const int two = g_fault_hits - before - one;
		printf("drive reads for the bad part: first time %d, again %d\n", one, two);
		g_fault_from = -1;
		g_fault_fail = false;
		rc = one > 0 && two == 0 ? 0 : 1;
	}
	else if (mode == "scan" && argc > 4)
	{
		FileReader f;
		std::string err;
		if (!OpenFileReader(argv[3], f, &err))
		{
			printf("open: %s\n", err.c_str());
			return 1;
		}
		std::vector<uint8_t> buf(65536);
		uint64_t total = 0;
		std::map<uint8_t, uint64_t> bytes;
		const double ts = Now();
		for (;;)
		{
			const int r = FileRead(&f, buf.data(), int(buf.size()));
			if (r <= 0)
				break;
			for (int i = 0; i < r; i += kSector)
				bytes[buf[size_t(i)]] += kSector;
			total += uint64_t(r);
		}
		const double t = Now() - ts;
		printf("read %llu of %llu bytes in %.2f s (%zu extents)", (unsigned long long)total, (unsigned long long)f.size, t,
			f.ext.size());
		for (const auto& b : bytes)
			printf(", '%c' %llu", b.first, (unsigned long long)b.second);
		printf("\n");
		rc = total == f.size && t < atof(argv[4]) ? 0 : 1;
	}
	else if (mode == "ui-read" && argc > 3)
	{
		std::string id, path;
		FileReader f;
		auto m = SplitDiscUrl(argv[3], id, path) ? MediaOf(id) : nullptr;
		const Extent* e = m && OpenReaderOn(m, path, f) ? f.Locate(0) : nullptr;
		if (!e)
			return 1;
		g_fault_from = int64_t(e->lba);
		g_fault_slow_ms = 2500;
		std::vector<uint8_t> out;
		const double ts = Now();
		const bool ok = ReadFileWithin(argv[3], out, 16u << 20, 1.0);
		const double t = Now() - ts;
		printf("read %d, back in %.2f s\n", int(ok), t);
		g_fault_from = -1;
		rc = !ok && t < 1.5 ? 0 : 1;
	}
	else
		rc = 2;
	Stop();
	return rc;
}
