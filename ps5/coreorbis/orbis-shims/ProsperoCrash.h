// Media Player Classic PS5: a crash printer and stage markers, so a fault names where it happened (ProsperoCrash.cpp).
//
// Snes9x PS5 1.6 (the port this layer comes from) reached its shelf on the console and then died with
// "crashed before KStuff was paused" and nothing in boot.log to say where. This installs signal handlers (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP/SIGSYS) on a separate signal stack
// (so a stack overflow can still be reported), modelled on PS5SX2's orbis-shims/ProsperoCrash.cpp. On a
// fault it writes one block to boot.log: the signal, the fault address, rip, the current stage of each
// thread group, and a short list of return addresses, each as an offset from an anchor symbol so the
// address maps straight into build/app/mpc-pie.elf with llvm-addr2line.
//
// SPDX-License-Identifier: MIT
#pragma once

namespace crashlog
{
// Which part of the program a thread is in; the handler prints the latest of each. Setting one is a single
// pointer store, cheap enough to leave on the hot path.
enum class Area
{
	Boot,  // main thread, bring-up
	Shelf, // main thread, the game list
	Cover, // the cover worker
	Pool,  // the shelf's render threads
	Emu,   // the game: the core's frame, loading, closing
	Menu,  // the pause menu
	Count
};

// Install the handlers (once). Logs the anchor address used to map offsets back to the ELF.
void Install();

// The handler runs on an alternate signal stack, so a stack overflow still gets its report. That stack is
// per thread: each thread arms its own (BigThread and the audio thread do; the installing thread gets one in
// Install). Returns what DisarmThread frees when the thread ends; a no-op on the host.
void* ArmThread();
void DisarmThread(void* stack);

// Record where a thread group is now. `where` must be a string literal / long-lived pointer.
void Stage(Area area, const char* where);
// The same with a text of the moment (copied: the GPU's set-up steps, from its log lines)
void StageText(Area area, const char* text);

// 1.6 (test2's console run: hardware decoding off, the GPU start crashing): what the console bound.
// Each import's slot as the console left it (the link writes the table into the eboot: tools/embed_imports.py), logged
// per module at the start: "[imports] libSceSysmodule 1/1, libSceAgc 2/2, ...", and the names of those left at 0.
void ReportImports();
// Every import of `module` (its soname, e.g. "libSceAgc.sprx") bound; false and the first one that isn't otherwise.
// True when there is no table (the host build).
bool ImportsBound(const char* module, const char** missing);
// The system's modules and where their code is (logged once at the start), so that a crash names the module and offset
// of an address outside the eboot.
void NoteModules();
// Host tests (MPCPS5_HOST_CRASH_SELFTEST=1): the handler's pieces called directly -- its line building, its safe
// reads, its address naming -- and the result logged ("[crash] self-test: ok"). Nothing on the console.
void SelfTest();
} // namespace crashlog

// Short form used on the hot paths.
#define MPCPS5_STAGE(area, where) ::crashlog::Stage(::crashlog::Area::area, where)
