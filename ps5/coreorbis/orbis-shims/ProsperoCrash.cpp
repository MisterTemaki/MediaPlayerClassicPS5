// Media Player Classic PS5: the crash printer and stage markers (ProsperoCrash.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoCrash.h"

#include "OrbisPaths.h"

#if defined(__linux__)
#define _GNU_SOURCE 1
#include <sys/ucontext.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

// An anchor the handler reports addresses against: rip - &mpcps5_crash_anchor + (its vaddr in mpc-pie.elf,
// from `nm`) is the ELF address llvm-addr2line wants. extern "C" so nm finds it by a plain name.
extern "C" __attribute__((noinline, used)) void mpcps5_crash_anchor()
{
	__asm__ volatile("");
}

// The imports' table, written into the PIE by tools/embed_imports.py after the link ("MPCIMP02", the anchor's address in
// the PIE, then a line per import: its slot's address, its module, its name). Initialised data, so it has bytes in the
// file to write over; the host build keeps the placeholder.
#ifndef __PROSPERO__
// host tests: words a synthetic import table names (bound, left at 0, bound)
extern "C" {
__attribute__((used, aligned(8))) volatile uintptr_t mpcps5_test_slots[3] = {1, 0, 1};
}
#endif
extern "C" {
__attribute__((used, aligned(16))) char mpcps5_import_table[64 * 1024] = "MPCIMP01 (no table: the host build)";
}

#if defined(__PROSPERO__)
extern "C" {
int sceKernelGetModuleList(int* handles, size_t count, size_t* actual);
int sceKernelGetModuleInfo(int handle, void* info);
}
#endif

namespace crashlog
{
namespace
{
std::atomic<const char*> g_stage[int(Area::Count)] = {};
std::atomic<uintptr_t> g_anchor{0};
char g_sigstack[128 * 1024];
// StageText's copies, four in turn, each taken with a fetch_add: two writers never share one, and the one shown is
// written again only three texts later
char g_stage_text[int(Area::Count)][4][160];
std::atomic<unsigned> g_stage_turn[int(Area::Count)] = {};

// the system's modules (NoteModules), for the crash report
struct ModuleSpan
{
	char name[48];
	uintptr_t start, end;
};
ModuleSpan g_modules[64];
int g_module_count = 0;

const ModuleSpan* ModuleOf(uintptr_t a)
{
	for (int i = 0; i < g_module_count; i++)
		if (a >= g_modules[i].start && a < g_modules[i].end)
			return &g_modules[i];
	return nullptr;
}

// (test4 review) The handler's output without snprintf, fileno or locks: lines built in a buffer of its own, numbers
// written by hand, out with write() to the log's fd (kept by orbis_paths.cpp as it opens and closes) and stdout.
struct Line
{
	char b[400];
	size_t n = 0;
	Line& S(const char* t)
	{
		while (t && *t && n < sizeof(b) - 1)
			b[n++] = *t++;
		return *this;
	}
	Line& Hex(uint64_t v)
	{
		char t[19];
		int k = 0;
		do
		{
			t[k++] = "0123456789abcdef"[v & 15];
			v >>= 4;
		} while (v && k < 16);
		S("0x");
		while (k > 0 && n < sizeof(b) - 1)
			b[n++] = t[--k];
		return *this;
	}
	Line& Dec(int64_t v)
	{
		char t[21];
		int k = 0;
		const bool neg = v < 0;
		uint64_t u = neg ? uint64_t(-(v + 1)) + 1 : uint64_t(v);
		do
		{
			t[k++] = char('0' + u % 10);
			u /= 10;
		} while (u && k < 20);
		if (neg)
			S("-");
		while (k > 0 && n < sizeof(b) - 1)
			b[n++] = t[--k];
		return *this;
	}
	Line& Off(int64_t v) // a signed offset, +/-
	{
		if (v >= 0)
			S("+");
		return Dec(v);
	}
};

void Out(const char* s, size_t n)
{
	// (test6 review) the copy of the log's fd made while logging (OrbisCrashLogFd), never a dup in here
	const int fd = OrbisCrashLogFd();
	if (fd >= 0)
	{
		ssize_t r = write(fd, s, n);
		(void)r;
	}
	ssize_t r2 = write(1, s, n);
	(void)r2;
}

void EmitLine(Line& l)
{
	l.S("\n");
	Out(l.b, l.n);
}

void EmitLine(const char* s)
{
	Line l;
	l.S(s);
	EmitLine(l);
}

// an address as "module+off", "anchor+off" or itself
void Where(Line& l, uintptr_t a, uintptr_t anchor)
{
	l.Hex(a);
	if (const ModuleSpan* m = ModuleOf(a))
		l.S(" = ").S(m->name).S("+").Hex(a - m->start);
	else if (anchor)
		l.S(" = anchor").Off(int64_t(a - anchor));
}

const char* SignalName(int sig)
{
	switch (sig)
	{
		case SIGSEGV: return "SIGSEGV";
		case SIGBUS: return "SIGBUS";
		case SIGILL: return "SIGILL";
		case SIGFPE: return "SIGFPE";
		case SIGABRT: return "SIGABRT";
		case SIGTRAP: return "SIGTRAP";
		case SIGSYS: return "SIGSYS";
		default: return "signal";
	}
}

bool PlausibleText(uintptr_t v)
{
	// our eboot's text and the system modules, so the list is return addresses, not data
	return (v >= 0x10000 && v < 0x100000000ULL) || (v >= 0x200000000ULL && v < 0x900000000ULL);
}

// (test4 review) a word read without faulting: the kernel copies it into a pipe (EFAULT for an address that isn't
// mapped), then it is read back. The pipe is made at Install.
int g_probe[2] = {-1, -1};
bool SafeRead(uintptr_t addr, uintptr_t* out)
{
	if (g_probe[0] < 0 || (addr & 7) || addr < 0x10000)
		return false;
	if (write(g_probe[1], reinterpret_cast<const void*>(addr), sizeof(uintptr_t)) != ssize_t(sizeof(uintptr_t)))
		return false;
	return read(g_probe[0], out, sizeof(uintptr_t)) == ssize_t(sizeof(uintptr_t));
}

// 1.6 (test2: _exit came back, and the ud2 the compiler puts after it ended the app with the system's "privileged
// instruction" dialog): the report on the storage, then out with 128 + the signal; if the exit returns, the process is
// killed; never a trap.
[[noreturn]] void End(int sig)
{
	const int fd = OrbisCrashLogFd();
	if (fd >= 0)
		fsync(fd);
	_exit(128 + sig);
	kill(getpid(), SIGKILL);
	for (;;)
		sleep(1);
}

void Handler(int sig, siginfo_t* info, void* ctx)
{
	// One report. The handler runs with SA_NODEFER (test4 review), so a fault inside it comes back here at once and
	// ends the process (the report so far is on the storage); another thread crashing meanwhile waits for the first
	// report's exit instead of cutting it short.
	static std::atomic<int> entered{0};
	static std::atomic<uintptr_t> owner{0};
	const uintptr_t self = uintptr_t(pthread_self());
	if (entered.fetch_add(1) != 0)
	{
		// (test5 review) the first one may not have said who it is yet (test6 review: a plain spin, no call)
		for (long i = 0; i < 100000000L && owner.load() == 0; i++)
			__builtin_ia32_pause();
		if (owner.load() == self)
			End(sig);
		for (;;)
			sleep(1);
	}
	owner.store(self);

	const uintptr_t anchor = g_anchor.load();
	uintptr_t rip = 0, rsp = 0, rbp = 0;
	if (ctx)
	{
		ucontext_t* uc = static_cast<ucontext_t*>(ctx);
#if defined(__FreeBSD__) || defined(__PROSPERO__)
		rip = uintptr_t(uc->uc_mcontext.mc_rip);
		rsp = uintptr_t(uc->uc_mcontext.mc_rsp);
		rbp = uintptr_t(uc->uc_mcontext.mc_rbp);
#elif defined(__linux__)
		rip = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
		rsp = uintptr_t(uc->uc_mcontext.gregs[REG_RSP]);
		rbp = uintptr_t(uc->uc_mcontext.gregs[REG_RBP]);
#endif
	}
	const uintptr_t fault = info ? uintptr_t(info->si_addr) : 0;

	EmitLine("");
	EmitLine("== CRASH =======================================================");
	{
		Line l;
		l.S("signal ").Dec(sig).S(" (").S(SignalName(sig)).S("), fault address ").Hex(fault).S(", tid ").Dec(int64_t(self));
		EmitLine(l);
	}
	{
		Line l;
		l.S("rip=").Hex(rip).S(" rsp=").Hex(rsp).S(" rbp=").Hex(rbp);
		EmitLine(l);
	}
	if (anchor && rip)
	{
		Line l;
		l.S("rip ");
		Where(l, rip, anchor);
		l.S("  (llvm-addr2line -e mpc-pie.elf -f <anchor-vaddr ").Off(int64_t(rip - anchor)).S(">)");
		EmitLine(l);
	}
	// the context's first words as they are (should the system's layout differ from FreeBSD's mcontext: rip, rsp and
	// rbp above would be wrong, and these show where they are)
	if (ctx)
	{
		const uintptr_t* q = static_cast<const uintptr_t*>(ctx);
		for (int row = 0; row < 6; row++)
		{
			Line l;
			l.S("ctx[").Dec(row * 8).S("]");
			for (int i = 0; i < 8; i++)
				l.S(" ").Hex(q[row * 8 + i]);
			EmitLine(l);
		}
	}
	static const char* const kNames[] = {"boot", "ui", "open", "pool", "play", "menu"};
	for (int i = 0; i < int(Area::Count); i++)
	{
		const char* st = g_stage[i].load();
		Line l;
		l.S("stage[").S(kNames[i]).S("] = ").S(st ? st : "(none)");
		EmitLine(l);
	}

	// Frame-pointer walk, then a scan of the stack, each word read safely (SafeRead), each address as anchor+off so
	// it maps into the pie ELF (or as a system module's)
	EmitLine("backtrace (ret = anchor + off):");
	uintptr_t fp = rbp;
	for (int i = 0; i < 24 && fp >= 0x10000 && (fp & 7) == 0; i++)
	{
		uintptr_t ret = 0, next = 0;
		if (!SafeRead(fp + 8, &ret) || !SafeRead(fp, &next))
			break;
		if (PlausibleText(ret) && anchor)
		{
			Line l;
			l.S("  bt").Dec(i).S(" ret=");
			Where(l, ret, anchor);
			l.S("  off=").Off(int64_t(ret - anchor));
			EmitLine(l);
		}
		if (next <= fp || next - fp > (1u << 20)) // a frame further up the same stack
			break;
		fp = next;
	}
	if (rsp >= 0x10000 && (rsp & 7) == 0)
	{
		for (int i = 0, shown = 0; i < 256 && shown < 24; i++)
		{
			uintptr_t w = 0;
			if (!SafeRead(rsp + uintptr_t(i) * 8, &w))
				break; // the end of the mapped stack
			if (PlausibleText(w) && anchor)
			{
				Line l;
				l.S("  st[").Dec(i).S("] =");
				Where(l, w, anchor);
				l.S("  off=").Off(int64_t(w - anchor));
				EmitLine(l);
				shown++;
			}
		}
	}
	EmitLine("== END CRASH ===================================================");
	EmitLine("");
	End(sig);
}
constexpr size_t kAltStack = 64 * 1024;
} // namespace

void* ArmThread()
{
#if !defined(__linux__)
	void* mem = malloc(kAltStack);
	if (!mem)
		return nullptr;
	stack_t ss = {};
	ss.ss_sp = mem;
	ss.ss_size = kAltStack;
	ss.ss_flags = 0;
	if (sigaltstack(&ss, nullptr) != 0)
	{
		free(mem);
		return nullptr;
	}
	return mem;
#else
	return nullptr; // the sanitizer owns the signal stacks on the host
#endif
}

void DisarmThread(void* stack)
{
	if (!stack)
		return;
	stack_t ss = {};
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, nullptr);
	free(stack);
}

void Install()
{
	g_anchor.store(reinterpret_cast<uintptr_t>(&mpcps5_crash_anchor));
	OrbisLog("[crash] handler armed; anchor mpcps5_crash_anchor = %#lx (map an off= with: llvm-addr2line-18 -e "
			 "build/app/mpc-pie.elf -f  $((<anchor vaddr from nm> + off)))",
		(unsigned long)reinterpret_cast<uintptr_t>(&mpcps5_crash_anchor));

	signal(SIGPIPE, SIG_IGN);

	// On the host the sanitizer owns the signal handlers and its own alternate stacks; leave them be (the
	// stage markers above still work, and ASan reports faults). Arm our handler only on the console.
#if !defined(__linux__)
	stack_t ss = {};
	ss.ss_sp = g_sigstack;
	ss.ss_size = sizeof(g_sigstack);
	ss.ss_flags = 0;
	sigaltstack(&ss, nullptr);

	struct sigaction sa = {};
	sa.sa_sigaction = Handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER; // (test4 review: a fault in the handler ends it, see Handler)
	if (pipe(g_probe) != 0)
		g_probe[0] = g_probe[1] = -1;
	// 1.5.2: SIGSYS too -- a call the console refuses to a title ends it with SIGSYS (#3 was one, at load time)
	for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS})
		sigaction(sig, &sa, nullptr);
#else
	(void)g_sigstack;
	(void)&Handler;
#endif
}

void SelfTest()
{
#ifndef __PROSPERO__
	if (!getenv("MPCPS5_HOST_CRASH_SELFTEST"))
		return;
	if (g_probe[0] < 0 && pipe(g_probe) != 0)
		g_probe[0] = g_probe[1] = -1;
	std::string bad;
	Line l;
	l.S("a").Hex(0x1f).S(" ").Dec(-42).S(" ").Off(7).S(" ").Dec(0).S(" ").Hex(0);
	if (std::string(l.b, l.n) != "a0x1f -42 +7 0 0x0")
		bad += " line(" + std::string(l.b, l.n) + ")";
	uintptr_t word = 0x1234567890abcdefULL, got = 0;
	if (!SafeRead(reinterpret_cast<uintptr_t>(&word), &got) || got != word)
		bad += " read";
	if (SafeRead(0x1000, &got) || SafeRead(reinterpret_cast<uintptr_t>(&word) + 1, &got))
		bad += " low-or-misaligned";
	void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page != MAP_FAILED)
	{
		munmap(page, 4096);
		if (SafeRead(reinterpret_cast<uintptr_t>(page), &got))
			bad += " unmapped";
	}
	Line w;
	Where(w, 0x1000 + 16, 0x1000);
	if (std::string(w.b, w.n) != "0x1010 = anchor+16")
		bad += " where(" + std::string(w.b, w.n) + ")";
	// (test6 review) the handler's fd: made while logging, not the log's own number, and it writes into boot.log
	const int cfd = OrbisCrashLogFd();
	if (cfd < 0 || cfd == OrbisLogFd())
		bad += " crash-fd(" + std::to_string(cfd) + " " + std::to_string(OrbisLogFd()) + ")";
	OrbisLog("[crash] self-test: %s", bad.empty() ? "ok (line, safe reads, an unmapped page, address naming, its own fd)" : bad.c_str());
	OrbisLogSync();
	static const char kLine[] = "[crash] self-test: written through the handler's own fd\n";
	Out(kLine, sizeof(kLine) - 1);
#endif
}

void Stage(Area area, const char* where)
{
	g_stage[int(area)].store(where, std::memory_order_relaxed);
}

void StageText(Area area, const char* text)
{
	const int a = int(area);
	const unsigned t = g_stage_turn[a].fetch_add(1) & 3;
	snprintf(g_stage_text[a][t], sizeof(g_stage_text[a][t]), "%s", text ? text : "");
	g_stage[a].store(g_stage_text[a][t], std::memory_order_release);
}

namespace
{
struct ImportLine
{
	uintptr_t vaddr;
	const char* module; // (pointers into the table: each field ends with a space or a newline, see Next)
	size_t module_len;
	const char* name;
	size_t name_len;
};
// the table's next line; false at its end
bool NextImport(const char*& p, ImportLine& l)
{
	char* end = nullptr;
	l.vaddr = uintptr_t(strtoull(p, &end, 16));
	if (!end || end == p || *end != ' ')
		return false;
	l.module = end + 1;
	const char* sp = strchr(l.module, ' ');
	if (!sp)
		return false;
	l.module_len = size_t(sp - l.module);
	l.name = sp + 1;
	const char* nl = strchr(l.name, '\n');
	if (!nl)
		return false;
	l.name_len = size_t(nl - l.name);
	p = nl + 1;
	return true;
}
// the table's anchor address, the image's end (every slot below it) and its first line; false without a table
uintptr_t g_table_span = 0;
bool TableStart(const char*& p, uintptr_t& base)
{
	if (strncmp(mpcps5_import_table, "MPCIMP03\n", 9) != 0)
		return false;
	char* end = nullptr;
	const uintptr_t anchor_vaddr = uintptr_t(strtoull(mpcps5_import_table + 9, &end, 16));
	if (!end || *end != '\n')
		return false;
	g_table_span = uintptr_t(strtoull(end + 1, &end, 16));
	if (!end || *end != '\n' || !g_table_span)
		return false;
	base = reinterpret_cast<uintptr_t>(&mpcps5_crash_anchor) - anchor_vaddr;
	p = end + 1;
	return true;
}
// (test4 review) a slot the table names inside the image, aligned; anything else: the table is malformed
bool SlotOk(const ImportLine& l)
{
	return l.vaddr + sizeof(uintptr_t) <= g_table_span && (l.vaddr & 7) == 0;
}
uintptr_t SlotValue(uintptr_t base, uintptr_t vaddr)
{
	return *reinterpret_cast<const volatile uintptr_t*>(base + vaddr);
}
} // namespace

void ReportImports()
{
#ifndef __PROSPERO__
	// host tests: MPCPS5_HOST_IMPORT_TABLE=file: that table in place of the placeholder (its slots: the host's own)
	if (const char* f = getenv("MPCPS5_HOST_IMPORT_TABLE"))
		if (FILE* t = fopen(f, "rb"))
		{
			const size_t n = fread(mpcps5_import_table, 1, sizeof(mpcps5_import_table) - 1, t);
			mpcps5_import_table[n] = 0;
			fclose(t);
		}
#endif
	const char* p = nullptr;
	uintptr_t base = 0;
	if (!TableStart(p, base))
	{
		OrbisLog("[imports] no table in this build");
		return;
	}
	struct Count
	{
		std::string module;
		unsigned all = 0, bound = 0;
	};
	std::vector<Count> counts;
	std::string missing;
	unsigned missing_n = 0;
	ImportLine l;
	while (NextImport(p, l))
	{
		if (!SlotOk(l))
		{
			OrbisLog("[imports] table malformed (a slot at %#lx, the image ends at %#lx): not read further",
				(unsigned long)l.vaddr, (unsigned long)g_table_span);
			break;
		}
		const std::string m(l.module, l.module_len);
		auto it = std::find_if(counts.begin(), counts.end(), [&](const Count& c) { return c.module == m; });
		if (it == counts.end())
		{
			counts.push_back({m, 0, 0});
			it = counts.end() - 1;
		}
		it->all++;
		if (SlotValue(base, l.vaddr))
			it->bound++;
		else if (missing_n++ < 24)
			missing += " " + std::string(l.name, l.name_len);
	}
	std::string line;
	for (const Count& c : counts)
		line += " " + c.module + " " + std::to_string(c.bound) + "/" + std::to_string(c.all) + ",";
	if (!line.empty())
		line.pop_back();
	OrbisLog("[imports] bound by the console:%s", line.c_str());
	if (missing_n)
		OrbisLog("[imports] %u left at 0:%s%s", missing_n, missing.c_str(), missing_n > 24 ? " ..." : "");
}

bool ImportsBound(const char* module, const char** missing)
{
	const char* p = nullptr;
	uintptr_t base = 0;
	if (!TableStart(p, base))
		return true;
	ImportLine l;
	static std::string first;
	while (NextImport(p, l))
	{
		if (!SlotOk(l))
			return true; // (a malformed table says nothing)
		if (l.module_len == strlen(module) && strncmp(l.module, module, l.module_len) == 0 && !SlotValue(base, l.vaddr))
		{
			first.assign(l.name, l.name_len);
			if (missing)
				*missing = first.c_str();
			return false;
		}
	}
	return true;
}

void NoteModules()
{
#if defined(__PROSPERO__)
	// FreeBSD / Orbis's module information: size, name[256], then four segments (address, size, protection)
	struct Info
	{
		uint64_t size;
		char name[256];
		struct
		{
			uint64_t address;
			uint32_t size;
			int32_t prot;
		} seg[4];
		uint32_t seg_count;
		uint8_t fingerprint[20];
	};
	int handles[64];
	size_t n = 0;
	const int rc = sceKernelGetModuleList(handles, 64, &n);
	if (rc != 0)
	{
		OrbisLog("[crash] the module list: %#x", unsigned(rc));
		return;
	}
	n = std::min<size_t>(n, 64);
	std::string line;
	for (size_t i = 0; i < n && g_module_count < 64; i++)
	{
		Info info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(handles[i], &info) != 0 || info.seg_count == 0 || info.seg_count > 4)
			continue;
		uintptr_t lo = UINTPTR_MAX, hi = 0;
		for (uint32_t k = 0; k < info.seg_count; k++)
		{
			lo = std::min<uintptr_t>(lo, info.seg[k].address);
			hi = std::max<uintptr_t>(hi, info.seg[k].address + info.seg[k].size);
		}
		ModuleSpan& m = g_modules[g_module_count++];
		snprintf(m.name, sizeof(m.name), "%s", info.name);
		m.start = lo;
		m.end = hi;
		char b[96];
		snprintf(b, sizeof(b), " %s@%lx+%lx", m.name, (unsigned long)lo, (unsigned long)(hi - lo));
		line += b;
	}
	OrbisLog("[crash] %d modules:%s", g_module_count, line.c_str());
#endif
}
} // namespace crashlog
