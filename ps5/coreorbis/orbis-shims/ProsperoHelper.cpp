// Media Player Classic PS5: the helper payload's side of the jailbreak (ProsperoJailbreak.h).
//
// Runs in MediaPlayerClassicPS5.elf (after it installs the app) and in MediaPlayerClassicPS5-helper.elf (which the app sends to the
// ELF loader when no helper answers). One request at a time on 127.0.0.1:9085 only.
//
// What the jailbreak changes in the Media Player Classic PS5 process, with the payload SDK's kernel access (the same calls
// ps5-payload-dev's elfldr makes for the payloads it starts): the root and jail folders become the kernel's
// root (so /data and /mnt/usbN are visible), uid/gid 0, and every SCE capability. The authid stays the
// app's own, so the system keeps treating it as the foreground title (pads, video, sound).
//
// 1.5.4 (#3): it also opens the app again after an update (kCmdRelaunch): the app asks, ends the normal way, and a
// thread of the helper waits for it to be gone (and 8 s more) before it asks the system to open the title.
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"
#include "ProsperoAppPlace.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"
#include "ProsperoThread.h"

#include "OrbisPaths.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iterator>
#include <poll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#ifndef MPCPS5_TITLE_ID
#define MPCPS5_TITLE_ID "PPSA99015"
#endif

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <sys/sysctl.h>
extern "C" int sceKernelGetAppInfo(int pid, void* info);
// libkernel_sys (linked): loads and starts a system library, returns its handle (> 0)
extern "C" int sceKernelLoadStartModule(const char* path, size_t argc, const void* argv, uint32_t flags, void* opt,
	int* res);
#endif

namespace jailbreak
{
namespace
{
// The first PPSA/CUSA-style title id (4 letters + 5 digits) in a buffer, or "".
std::string FindTitleId(const unsigned char* p, size_t n)
{
	for (size_t i = 0; i + 9 <= n; i++)
	{
		bool ok = true;
		for (size_t k = 0; k < 4 && ok; k++)
			ok = p[i + k] >= 'A' && p[i + k] <= 'Z';
		for (size_t k = 4; k < 9 && ok; k++)
			ok = p[i + k] >= '0' && p[i + k] <= '9';
		if (ok)
			return std::string(reinterpret_cast<const char*>(p + i), 9);
	}
	return "";
}

// The title the process belongs to: sceKernelGetAppInfo's title id. "" when it can't be told (quiet: not logged).
std::string TitleOf(int pid, bool quiet = false)
{
#ifdef __PROSPERO__
	unsigned char info[512];
	memset(info, 0, sizeof(info));
	const int rc = sceKernelGetAppInfo(pid, info);
	if (rc != 0)
	{
		if (!quiet)
			OrbisLog("[helper] sceKernelGetAppInfo(%d) -> %x", pid, unsigned(rc));
		return "";
	}
	return FindTitleId(info, sizeof(info));
#else
	(void)quiet;
	// host: the test says which title the asking process is
	if (kill(pid, 0) != 0)
		return "";
	const char* t = getenv("MPCPS5_HOST_JB_TITLE");
	const std::string title = t ? t : MPCPS5_TITLE_ID;
	return FindTitleId(reinterpret_cast<const unsigned char*>(title.data()), title.size());
#endif
}

int HostMs(const char* name, int fallback)
{
#ifndef __PROSPERO__
	if (const char* v = getenv(name))
		if (*v)
			return atoi(v);
#else
	(void)name;
#endif
	return fallback;
}

// the one lock of the SDK's kernel R/W (KernelRw)
pthread_mutex_t g_kernel_rw = PTHREAD_MUTEX_INITIALIZER;
} // namespace

KernelRw::KernelRw(const char* what) : what_(what)
{
	pthread_mutex_lock(&g_kernel_rw);
#ifndef __PROSPERO__
	if (HostMs("MPCPS5_HOST_KERNEL_RW_MS", 0) > 0)
		OrbisLog("[helper] kernel R/W: %s: begins", what_);
#else
	(void)what_; // (named for the host tests' log)
#endif
}

KernelRw::~KernelRw()
{
#ifndef __PROSPERO__
	if (const int ms = HostMs("MPCPS5_HOST_KERNEL_RW_MS", 0); ms > 0)
	{
		usleep(useconds_t(ms) * 1000);
		OrbisLog("[helper] kernel R/W: %s: ends", what_);
	}
#endif
	pthread_mutex_unlock(&g_kernel_rw);
}

bool JailbreakProcess(int pid, std::string& why)
{
	if (pid <= 0)
	{
		why = "bad pid";
		return false;
	}
	// Fail closed: a process whose title can't be read (a payload, a daemon, an exited pid) is refused, as is
	// any other title. The requester can claim any pid, so the title is the only thing that decides.
	const std::string title = TitleOf(pid);
	if (title != MPCPS5_TITLE_ID)
	{
		why = title.empty() ? "title unknown: not Media Player Classic PS5" : "title " + title + " is not Media Player Classic PS5";
		return false;
	}
	OrbisLog("[helper] pid %d (title %s): letting it out", pid, title.c_str());
	// 1.5.4: the whole sequence under the kernel R/W lock (the relaunch thread may be looking up its calls)
	KernelRw rw("unlock");
#ifdef __PROSPERO__
	if (kernel_get_proc(pid) == 0)
	{
		why = "no such process";
		return false;
	}
	uint8_t caps[16];
	memset(caps, 0xff, sizeof(caps));
	const intptr_t root = kernel_get_root_vnode();
	const int r_caps = kernel_set_ucred_caps(pid, caps);
	const int r_uid = kernel_set_ucred_uid(pid, 0);
	const int r_ruid = kernel_set_ucred_ruid(pid, 0);
	const int r_svuid = kernel_set_ucred_svuid(pid, 0);
	const int r_rgid = kernel_set_ucred_rgid(pid, 0);
	const int r_svgid = kernel_set_ucred_svgid(pid, 0);
	const int r_root = root ? kernel_set_proc_rootdir(pid, root) : -1;
	const int r_jail = root ? kernel_set_proc_jaildir(pid, root) : -1;
	OrbisLog("[helper]   caps %d, uid %d/%d/%d, gid %d/%d, root vnode %lx: rootdir %d, jaildir %d", r_caps, r_uid,
		r_ruid, r_svuid, r_rgid, r_svgid, (unsigned long)root, r_root, r_jail);
	if (r_root != 0)
	{
		why = "could not change the process's root folder";
		return false;
	}
	if (r_caps != 0 || r_uid != 0 || r_ruid != 0 || r_svuid != 0 || r_rgid != 0 || r_svgid != 0 || r_jail != 0)
		why = "ok, but some rights were not changed (see the helper's log)"; // reported, not fatal: the root folder is
#endif
	return true;
}

namespace
{
// Every connection gets this long in total, however slowly the other side sends: one slow client can't hold the
// (single) serving loop.
constexpr int kConnectionMs = 2000;

int64_t NowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Waits for fd to be readable (events = POLLIN) or writable (POLLOUT) before the deadline.
bool WaitFd(int fd, short events, int64_t deadline)
{
	for (;;)
	{
		const int64_t left = deadline - NowMs();
		if (left <= 0)
			return false;
		pollfd p = {fd, events, 0};
		const int r = poll(&p, 1, int(left));
		if (r > 0)
			return true;
		if (r == 0 || errno != EINTR)
			return false;
	}
}

size_t RecvUntil(int fd, void* buf, size_t size, int64_t deadline)
{
	size_t got = 0;
	while (got < size && WaitFd(fd, POLLIN, deadline))
	{
		const ssize_t n = recv(fd, static_cast<char*>(buf) + got, size - got, 0);
		if (n <= 0)
			break;
		got += size_t(n);
	}
	return got;
}

bool SendUntil(int fd, const void* buf, size_t size, int64_t deadline)
{
	const char* p = static_cast<const char*>(buf);
	while (size > 0)
	{
		if (!WaitFd(fd, POLLOUT, deadline))
			return false;
		const ssize_t n = send(fd, p, size, 0);
		if (n <= 0)
			return false;
		p += n;
		size -= size_t(n);
	}
	return true;
}

// covers/wanted.txt: a regular file of ours (symbolic links refused), at most kMaxWantedBytes, cut at a line end.
std::string ReadWantedList()
{
	std::string text;
	const int fd = open((OrbisDir("covers") + "/wanted.txt").c_str(), O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return text;
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
	{
		char buf[16384];
		ssize_t n;
		while (text.size() <= size_t(kMaxWantedBytes) && (n = read(fd, buf, sizeof(buf))) > 0)
			text.append(buf, size_t(n));
		if (text.size() > size_t(kMaxWantedBytes))
		{
			const size_t cut = text.rfind('\n', size_t(kMaxWantedBytes) - 1);
			text.resize(cut == std::string::npos ? 0 : cut + 1); // whole lines only
		}
	}
	close(fd);
	return text;
}

// ---- 1.5.4 (#3): opening the app again after an update (kCmdRelaunch) ----------------------------------------------
// Restart App used to be sceSystemServiceLoadExec(eboot), which only ended the app (back to the home screen). Now the
// app asks the helper, then ends the normal way; the helper waits for its process to be gone, waits a little more and
// asks the system to open the title, the way a launcher does.

// how often it looks; how long the app gets to end; how long after (ps5upload measured that a title opened while its
// old copy is still closing fails with 0x80940010)
constexpr int kRelaunchPollMs = 200;
constexpr int kRelaunchWaitMs = 30000;  // host tests: MPCPS5_HOST_RELAUNCH_WAIT_MS
constexpr int kRelaunchSettleMs = 8000; // host tests: MPCPS5_HOST_RELAUNCH_SETTLE_MS

// one relaunch at a time
std::atomic<bool> g_relaunch_busy{false};

enum class Relaunch
{
	Launched,
	AlreadyRuns, // a copy of the title runs (opened meanwhile): nothing to do
	Failed,
};

// The notice when the app couldn't be opened again: a fixed text in the app's language (the codes the app sends, in
// mpc_i18n.cpp's order), never text from the request; any other code gets the English one.
const char* const kRelaunchLanguages[] = {"en", "es", "pt-BR", "fr", "de"};
const char* const kRelaunchNotices[] = {
	"Media Player Classic PS5 couldn't open again by itself: open it from the home screen to use the new version.",
	"Media Player Classic PS5 no pudo volver a abrirse solo: ábrelo desde la pantalla de inicio para usar la nueva "
	"versión.",
	"O Media Player Classic PS5 não conseguiu abrir de novo sozinho: abra-o pela tela inicial para usar a nova versão.",
	"Media Player Classic PS5 n'a pas pu se rouvrir tout seul : ouvrez-le depuis l'écran d'accueil pour utiliser la "
	"nouvelle version.",
	"Media Player Classic PS5 konnte sich nicht selbst wieder öffnen: Öffne es über den Startbildschirm, um die neue "
	"Version zu verwenden.",
};
static_assert(std::size(kRelaunchLanguages) == std::size(kRelaunchNotices), "a notice for every language");

// the request's language code as one of ours ("other" when it isn't one): only these go further (log, notice)
const char* KnownLanguage(const std::string& code)
{
	for (const char* k : kRelaunchLanguages)
		if (code == k)
			return k;
	return "other";
}

const char* RelaunchNotice(const char* language)
{
	for (size_t i = 0; i < std::size(kRelaunchLanguages); i++)
		if (strcmp(language, kRelaunchLanguages[i]) == 0)
			return kRelaunchNotices[i];
	return kRelaunchNotices[0];
}

// a process that exists (one we may not signal still does)
bool Alive(int pid)
{
	return kill(pid, 0) == 0 || errno == EPERM;
}

bool Contains(const std::vector<int>& pids, int pid)
{
	return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

// The processes of `title` running now, from the console's process list. False when the list can't be read (it then
// decides nothing).
bool ProcessesOf(const std::string& title, std::vector<int>& pids)
{
	pids.clear();
#ifdef __PROSPERO__
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
	size_t size = 0;
	if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0)
		return false;
	size += size / 4; // room for processes started in between
	std::vector<unsigned char> buf(size);
	if (sysctl(mib, 4, buf.data(), &size, nullptr, 0) != 0 || size > buf.size())
		return false;
	// kinfo_proc records: the first int is the record's size, the pid is at byte 72
	for (size_t off = 0; off < size;)
	{
		int rec = 0, pid = 0;
		if (size - off < sizeof(rec))
			return false;
		memcpy(&rec, &buf[off], sizeof(rec));
		if (rec <= 76 || size_t(rec) > size - off)
			return false; // not a record: the list can't be trusted
		memcpy(&pid, &buf[off + 72], sizeof(pid));
		if (pid > 0 && TitleOf(pid, true) == title)
			pids.push_back(pid);
		off += size_t(rec);
	}
	return true;
#else
	// host tests: MPCPS5_HOST_TITLE_PIDS_FILE lists the title's processes, one pid a line (the live ones count); named
	// but missing: the list can't be read. Not named: no process of the title but the one asking.
	const char* file = getenv("MPCPS5_HOST_TITLE_PIDS_FILE");
	if (!file || !*file)
		return true;
	FILE* f = fopen(file, "r");
	if (!f)
		return false;
	int pid = 0;
	while (fscanf(f, "%d", &pid) == 1)
		if (pid > 0 && Alive(pid) && !Contains(pids, pid))
			pids.push_back(pid);
	fclose(f);
	(void)title;
	return true;
#endif
}

// ---- the launch calls, found at run time: the helper payload links neither libSceUserService nor libSceSystemService
// (a library import that doesn't resolve stops the whole payload from loading) ----
#ifndef __PROSPERO__
// host: every library is "loaded"; the calls are host/sce_host.cpp's stand-ins (MPCPS5_HOST_LAUNCH_UNRESOLVED: none)
uint32_t HostDynlibHandle(const char* name)
{
	return strcmp(name, "libSceSystemService.sprx") == 0 ? 0x10 : strcmp(name, "libSceUserService.sprx") == 0 ? 0x11 : 0x12;
}

intptr_t HostDlsym(const char* name)
{
	if (getenv("MPCPS5_HOST_LAUNCH_UNRESOLVED"))
		return 0;
	if (strcmp(name, "sceUserServiceInitialize") == 0)
		return reinterpret_cast<intptr_t>(&sceUserServiceInitialize);
	if (strcmp(name, "sceUserServiceGetForegroundUser") == 0)
		return reinterpret_cast<intptr_t>(&sceUserServiceGetForegroundUser);
	if (strcmp(name, "sceSystemServiceLaunchApp") == 0)
		return reinterpret_cast<intptr_t>(&sceSystemServiceLaunchApp);
	return 0;
}
#endif

// A library's handle in this process if it is loaded, else 0.
uint32_t DynlibHandle(const char* name)
{
	uint32_t h = 0;
	const std::string what = std::string("kernel_dynlib_handle ") + name;
	KernelRw rw(what.c_str());
#ifdef __PROSPERO__
	if (kernel_dynlib_handle(getpid(), name, &h) != 0)
		h = 0;
#else
	h = HostDynlibHandle(name);
#endif
	return h;
}

// A function of a loaded library, or 0 (an address outside user space is refused).
uintptr_t Symbol(uint32_t handle, const char* name)
{
	intptr_t p = 0;
	{
		const std::string what = std::string("kernel_dynlib_dlsym ") + name;
		KernelRw rw(what.c_str());
#ifdef __PROSPERO__
		p = kernel_dynlib_dlsym(getpid(), handle, name);
#else
		(void)handle;
		p = HostDlsym(name);
#endif
	}
	if (p <= 0x10000 || p >= 0x800000000000)
	{
		OrbisLog("[helper] relaunch: %s not found (%lx)", name, static_cast<unsigned long>(p));
		return 0;
	}
	return uintptr_t(p);
}

int LoadStartModule(const std::string& path)
{
#ifdef __PROSPERO__
	return sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, nullptr);
#else
	(void)path;
	return -1;
#endif
}

// A system library's handle, loaded when it isn't (as the SDK's rtld loads one, crt/rtld_sprx.c): libSceSysmodule's
// sceSysmoduleLoadModuleInternal with the library's id, then sceKernelLoadStartModule. 0 when it can't be had. The
// module loading runs outside the kernel R/W lock (its start code runs then).
uint32_t LoadModule(const char* name, uint32_t sysmodule_id)
{
	if (const uint32_t h = DynlibHandle(name))
	{
		OrbisLog("[helper] relaunch: %s is loaded (handle %#x)", name, h);
		return h;
	}
	uint32_t sysmodule = DynlibHandle("libSceSysmodule.sprx"); // the SDK's rtld loaded it
	if (!sysmodule)
	{
		const int r = LoadStartModule("/system/common/lib/libSceSysmodule.sprx");
		OrbisLog("[helper] relaunch: sceKernelLoadStartModule(libSceSysmodule.sprx) -> %x", unsigned(r));
		sysmodule = r > 0 ? uint32_t(r) : 0;
	}
	if (sysmodule)
		if (const uintptr_t f = Symbol(sysmodule, "sceSysmoduleLoadModuleInternal"))
		{
			const int r = reinterpret_cast<int (*)(uint32_t)>(f)(sysmodule_id);
			OrbisLog("[helper] relaunch: sceSysmoduleLoadModuleInternal(%#x) -> %x", sysmodule_id, unsigned(r));
		}
	const std::string path = std::string("/system/common/lib/") + name;
	const int r = LoadStartModule(path);
	OrbisLog("[helper] relaunch: sceKernelLoadStartModule(%s) -> %x", path.c_str(), unsigned(r));
	const uint32_t h = r > 0 ? uint32_t(r) : DynlibHandle(name);
	if (!h)
		OrbisLog("[helper] relaunch: %s couldn't be loaded", name);
	return h;
}

// Opens the title for the user in front. No authid change: the system's launch is asked as any launcher asks it (the
// ELF loader's authid stays the helper's).
Relaunch LaunchTitle(const std::string& title, std::string& why)
{
	using InitFn = int (*)(void*);
	using UserFn = int (*)(int32_t*);
	using LaunchFn = int (*)(const char*, const char**, SceSystemServiceLaunchCtx*);
	const uint32_t users = LoadModule("libSceUserService.sprx", 0x80000011);  // the SDK's ids (crt/rtld_sprx.c)
	const uint32_t system = LoadModule("libSceSystemService.sprx", 0x80000010);
	const auto user_init = reinterpret_cast<InitFn>(users ? Symbol(users, "sceUserServiceInitialize") : 0);
	const auto get_user = reinterpret_cast<UserFn>(users ? Symbol(users, "sceUserServiceGetForegroundUser") : 0);
	const auto launch = reinterpret_cast<LaunchFn>(system ? Symbol(system, "sceSystemServiceLaunchApp") : 0);
	if (!user_init || !get_user || !launch)
	{
		why = "the system's launch calls couldn't be found";
		return Relaunch::Failed;
	}
	const int rc_init = user_init(nullptr); // (already initialised is fine)
	OrbisLog("[helper] relaunch: sceUserServiceInitialize -> %x", unsigned(rc_init));
	int32_t user = -1;
	const int rc_user = get_user(&user);
	OrbisLog("[helper] relaunch: sceUserServiceGetForegroundUser -> %x, user %#x", unsigned(rc_user), unsigned(user));
	if (rc_user != 0 || user == 0 || user == -1)
	{
		why = "no foreground user";
		return Relaunch::Failed;
	}
	// the list again right before the call: a copy opened meanwhile isn't opened a second time
	std::vector<int> pids;
	if (!ProcessesOf(title, pids))
	{
		why = "the process list couldn't be read right before the launch";
		return Relaunch::Failed;
	}
	if (!pids.empty())
	{
		OrbisLog("[helper] relaunch: %s runs already (pid %d): not opened again", title.c_str(), pids[0]);
		return Relaunch::AlreadyRuns;
	}
	SceSystemServiceLaunchCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.structsize = sizeof(ctx);
	ctx.user_id = uint32_t(user);
	const char* argv[] = {nullptr};
	OrbisLog("[helper] relaunch: sceSystemServiceLaunchApp(%s, user %#x)", title.c_str(), unsigned(user));
	const int rc = launch(title.c_str(), argv, &ctx);
	OrbisLog("[helper] relaunch: sceSystemServiceLaunchApp -> %x", unsigned(rc));
	if (rc < 0)
	{
		char b[64];
		snprintf(b, sizeof(b), "the system didn't open it (%x)", unsigned(rc));
		why = b;
		return Relaunch::Failed;
	}
	return Relaunch::Launched;
}

// Waits for the process `pid` to end (up to 30 s), then 8 s more, then opens the title -- never while a copy of it
// runs: it ended when it is neither alive as this title nor in the process list.
Relaunch RelaunchOnce(int pid, const std::string& title, std::string& why)
{
	const int wait_ms = HostMs("MPCPS5_HOST_RELAUNCH_WAIT_MS", kRelaunchWaitMs);
	const int settle_ms = HostMs("MPCPS5_HOST_RELAUNCH_SETTLE_MS", kRelaunchSettleMs);
	const int64_t t0 = NowMs();
	std::vector<int> pids;
	bool readable = false, ended = false;
	for (;;)
	{
		readable = ProcessesOf(title, pids);
		if (readable)
		{
			for (const int p : pids)
				if (p != pid)
				{
					OrbisLog("[helper] relaunch: another copy of %s runs (pid %d): not opened again", title.c_str(), p);
					return Relaunch::AlreadyRuns;
				}
			if (!Contains(pids, pid) && (!Alive(pid) || TitleOf(pid, true) != title))
			{
				ended = true;
				break;
			}
		}
		if (NowMs() - t0 >= wait_ms)
			break;
		usleep(kRelaunchPollMs * 1000);
	}
	if (!ended)
	{
		why = readable ? "it was still running after the wait: not opened (never two copies)"
					   : "the process list couldn't be read: whether it ended is unknown";
		return Relaunch::Failed;
	}
	OrbisLog("[helper] relaunch: pid %d ended (after %lld ms); opening %s in %d ms", pid, (long long)(NowMs() - t0),
		title.c_str(), settle_ms);
	const int64_t t1 = NowMs();
	for (;;)
	{
		const int64_t left = settle_ms - (NowMs() - t1);
		if (left <= 0)
			break;
		usleep(useconds_t(std::min<int64_t>(left, kRelaunchPollMs)) * 1000);
		if (ProcessesOf(title, pids) && !pids.empty())
		{
			OrbisLog("[helper] relaunch: %s was opened meanwhile (pid %d): not opened again", title.c_str(), pids[0]);
			return Relaunch::AlreadyRuns;
		}
	}
	return LaunchTitle(title, why);
}

// clears the busy flag however the thread ends
struct RelaunchDone
{
	~RelaunchDone() { g_relaunch_busy = false; }
};

void RelaunchThread(int pid, const char* language)
{
	RelaunchDone done;
	std::string why;
	const Relaunch r = RelaunchOnce(pid, MPCPS5_TITLE_ID, why);
	if (r == Relaunch::Launched)
		OrbisLog("[helper] relaunch: %s opened again", MPCPS5_TITLE_ID);
	else if (r == Relaunch::AlreadyRuns)
		OrbisLog("[helper] relaunch: nothing to do (it runs)");
	else
	{
		OrbisLog("[helper] relaunch failed: %s; notice in %s", why.c_str(), language);
		ProsperoNotify("%s", RelaunchNotice(language));
	}
}
} // namespace

bool StartRelaunch(int pid, const std::string& title, const std::string& language, std::string& why)
{
	if (pid <= 0)
	{
		why = "bad pid";
		return false;
	}
	if (title != MPCPS5_TITLE_ID)
	{
		why = "not this title";
		return false;
	}
	// any process can claim any pid: only this title's own process may ask
	const std::string asker = TitleOf(pid);
	if (asker != MPCPS5_TITLE_ID)
	{
		why = asker.empty() ? "the asking process isn't Media Player Classic PS5" : "asked by " + asker;
		return false;
	}
	bool idle = false;
	if (!g_relaunch_busy.compare_exchange_strong(idle, true))
	{
		why = "a relaunch is already waiting";
		return false;
	}
	const char* lang = KnownLanguage(language);
	OrbisLog("[helper] relaunch: pid %d asks to be opened again once it has ended (language %s)", pid, lang);
	ps5::BigThread t([pid, lang] { RelaunchThread(pid, lang); }, 256 * 1024, "relaunch");
	if (!t.joinable())
	{
		g_relaunch_busy = false;
		why = "no thread for it";
		return false;
	}
	t.detach();
	return true;
}

bool ServeHelper(void (*on_ready)())
{
	signal(SIGPIPE, SIG_IGN);
	const char* env = getenv("MPCPS5_HELPER_PORT");
	const int port = (env && *env) ? atoi(env) : kHelperPort;
	const int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0)
	{
		OrbisLog("[helper] socket: errno %d", errno);
		return false;
	}
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
#ifdef __FreeBSD__
	sa.sin_len = sizeof(sa);
#endif
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr.s_addr = htonl(0x7F000001); // this console only
	if (bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || listen(srv, 4) != 0)
	{
		OrbisLog("[helper] port %d is taken (errno %d): a helper is already running", port, errno);
		close(srv);
		return false;
	}
	OrbisLog("[helper] listening on 127.0.0.1:%d, pid %d", port, int(getpid()));
	if (on_ready)
		on_ready();
	for (;;)
	{
		const int c = accept(srv, nullptr, nullptr);
		if (c < 0)
		{
			if (errno == EINTR)
				continue;
			OrbisLog("[helper] accept: errno %d", errno);
			usleep(200 * 1000);
			continue;
		}
		OrbisLogRefresh(); // the "Debug logs" setting may have changed since the helper started
		const int64_t deadline = NowMs() + kConnectionMs;
		Request req;
		memset(&req, 0, sizeof(req));
		const size_t got = RecvUntil(c, &req, sizeof(req), deadline);
		if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdWantedCovers)
		{
			// covers/wanted.txt, written by the app after its last scan: a fixed file of ours, nothing else
			const std::string text = ReadWantedList();
			req.ret = int32_t(text.size());
			// this helper downloads them itself, in the background: the app has nothing to wait for
			memset(req.msg2, 0, sizeof(req.msg2));
			snprintf(req.msg2, sizeof(req.msg2), "%s", kBackgroundCovers);
			OrbisLog("[helper] pid %d: wanted-covers list, %zu bytes", req.pid, text.size());
			if (SendUntil(c, &req, sizeof(req), deadline))
				SendUntil(c, text.data(), text.size(), deadline);
		}
		else if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdAppPlace)
		{
			// 1.5.1: where the app the console runs is, read here (statfs / getfsstat are a payload's to call, not a
			// title's: #3). Only this title, and only an absolute default folder of it
			req.msg1[sizeof(req.msg1) - 1] = 0;
			req.msg2[sizeof(req.msg2) - 1] = 0;
			const std::string title = req.msg1, dflt = req.msg2;
			const std::string tail = "/" + title;
			std::string text;
			if (title == MPCPS5_TITLE_ID && !dflt.empty() && dflt[0] == '/' && dflt.size() > tail.size() &&
				dflt.compare(dflt.size() - tail.size(), tail.size(), tail) == 0)
				text = SerializeAppPlace(FindAppPlace(title, dflt));
			req.ret = text.empty() || text.size() > size_t(kMaxAppPlaceBytes) ? -1 : int32_t(text.size());
			OrbisLog("[helper] pid %d: where %s is: %s", req.pid, title.c_str(), req.ret < 0 ? "refused" : "answered");
			if (SendUntil(c, &req, sizeof(req), deadline) && req.ret > 0)
				SendUntil(c, text.data(), text.size(), deadline);
		}
		else if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdRelaunch)
		{
			// 1.5.4: answered at once; the waiting is the relaunch thread's
			req.msg1[sizeof(req.msg1) - 1] = 0;
			req.msg2[sizeof(req.msg2) - 1] = 0;
			std::string why;
			const bool ok = StartRelaunch(req.pid, req.msg1, req.msg2, why);
			req.ret = ok ? 0 : -1;
			memset(req.msg1, 0, sizeof(req.msg1));
			memset(req.msg2, 0, sizeof(req.msg2));
			snprintf(req.msg1, sizeof(req.msg1), "%s", ok ? "ok" : why.c_str());
			OrbisLog("[helper] pid %d: relaunch: %s", req.pid, req.msg1);
			SendUntil(c, &req, sizeof(req), deadline);
		}
		else if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdJailbreak)
		{
			std::string why;
			const bool ok = JailbreakProcess(req.pid, why);
			req.ret = ok ? 0 : -1;
			memset(req.msg1, 0, sizeof(req.msg1));
			snprintf(req.msg1, sizeof(req.msg1), "%s", ok ? "ok" : why.c_str());
			OrbisLog("[helper] pid %d: %s", req.pid, req.msg1);
			SendUntil(c, &req, sizeof(req), deadline);
		}
		else
			OrbisLog("[helper] ignored a request of %zu bytes (magic %x, cmd %d)", got, req.magic, req.cmd);
		close(c);
	}
}
} // namespace jailbreak
