// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Start-up of a native PS5 title (eboot.bin): the entry point, the C runtime's environment set-up, static
// constructors and destructors, and the exit status.
//
// What it does, in order:
//   1. _start (the ELF entry; the link names it with -e _start) gets the argument block in rdi, as on FreeBSD/amd64:
//      a 64-bit argc followed by argv[]. The stack is realigned to 16 bytes in case the loader jumped here instead of
//      calling, and the frame pointer is cleared so a backtrace stops here.
//   2. _init_env, the system C library's (libSceLibcInternal) set-up of the process environment, is called before any
//      other C library use. Every argument register points at a block of null pointers, so whichever of the usual
//      shapes the function has -- no argument, an environment array, the argument block, or (argc, argv, envp) -- it
//      sees an empty, well-formed environment.
//   3. _fini is registered with atexit before any constructor runs, so at exit the destructors that constructors
//      register with __cxa_atexit (which run first, last-in first-out) are followed by .fini_array.
//   4. _init runs .preinit_array then .init_array (the C++ static constructors), in order.
//   5. main(argc, argv, envp); its value goes to catchReturnFromMain (weak: a program may supply its own), which
//      calls exit() -- the C library's exit runs the atexit / __cxa_atexit handlers, flushes stdio and ends the
//      process with that status.
// _init and _fini are idempotent: whether or not the system loader also calls them through DT_INIT / DT_FINI (lld
// points those tags at the symbols of these names), each array runs once.

#include <cstdint>

extern "C" {

// from the system C library (libSceLibcInternal)
void _init_env(void *, void *, void *);
int atexit(void (*)(void));
[[noreturn]] void exit(int);

int main(int, char **, char **);

// the bounds of the arrays, from the link's linker script (PROVIDE_HIDDEN); weak, so a link without them still works
using InitFunction = void (*)(int, char **, char **);
using FiniFunction = void (*)(void);
extern InitFunction __preinit_array_start[] __attribute__((weak, visibility("hidden")));
extern InitFunction __preinit_array_end[] __attribute__((weak, visibility("hidden")));
extern InitFunction __init_array_start[] __attribute__((weak, visibility("hidden")));
extern InitFunction __init_array_end[] __attribute__((weak, visibility("hidden")));
extern FiniFunction __fini_array_start[] __attribute__((weak, visibility("hidden")));
extern FiniFunction __fini_array_end[] __attribute__((weak, visibility("hidden")));

void _init(void);
void _fini(void);
[[noreturn]] void ps5rt_start_c(std::uintptr_t *args);
void catchReturnFromMain(int status) __attribute__((weak));
}

namespace {

// a zeroed block: an empty argv / envp, or an argument block with argc 0
char *empty_block[8];

// what _start found, for _init's array calls
int start_argc;
char **start_argv = empty_block;
char **start_envp = empty_block;

bool init_done;
bool fini_done;

void run_init_array(InitFunction *begin, InitFunction *end)
{
    if (begin == nullptr || end == nullptr)
        return;
    for (InitFunction *f = begin; f < end; ++f)
        if (*f != nullptr && reinterpret_cast<std::uintptr_t>(*f) != ~std::uintptr_t{0})
            (*f)(start_argc, start_argv, start_envp);
}

} // namespace

extern "C" void _init(void)
{
    if (init_done)
        return;
    init_done = true;
    run_init_array(__preinit_array_start, __preinit_array_end);
    run_init_array(__init_array_start, __init_array_end);
}

extern "C" void _fini(void)
{
    if (fini_done)
        return;
    fini_done = true;
    if (__fini_array_start == nullptr || __fini_array_end == nullptr)
        return;
    for (FiniFunction *f = __fini_array_end; f > __fini_array_start;)
    {
        --f;
        if (*f != nullptr && reinterpret_cast<std::uintptr_t>(*f) != ~std::uintptr_t{0})
            (*f)();
    }
}

extern "C" void catchReturnFromMain(int status)
{
    exit(status);
}

extern "C" [[noreturn]] void ps5rt_start_c(std::uintptr_t *args)
{
    // the argument block: argc (64 bits), argv[argc], a null pointer. Anything implausible is taken as no arguments.
    if (args != nullptr && args[0] > 0 && args[0] < 4096)
    {
        start_argc = static_cast<int>(args[0]);
        start_argv = reinterpret_cast<char **>(args + 1);
    }
    _init_env(empty_block, empty_block, empty_block);
    atexit(_fini);
    _init();
    const int status = main(start_argc, start_argv, start_envp);
    catchReturnFromMain(status);
    exit(status);
}

// The entry point. rdi = the argument block (kept), rsi = the loader's clean-up routine (not registered: its form on
// this system isn't documented, and exit() ends the process either way).
asm(R"(
    .text
    .globl _start
    .type _start, @function
    .p2align 4
_start:
    .cfi_startproc
    .cfi_undefined rip
    xorl %ebp, %ebp
    andq $-16, %rsp
    call ps5rt_start_c
    ud2
    .cfi_endproc
    .size _start, . - _start
)");
