// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Host test of src/ps5rt_start.cpp. host_test.sh builds that file for Linux and renames its symbols (objcopy), so
// that its _start, _init, _fini, main, _init_env and array bounds become the ps5rt_test_* / test_* names below. A child
// process enters it the way the loader would (rdi = argument block) and the parent checks what ran, in which order,
// and the exit status.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
void ps5rt_test_entry(std::uintptr_t *args);   // _start
void ps5rt_test_init(void);                    // _init
}

namespace {

char log_buffer[1024];
std::size_t log_length;

void event(const char *text)
{
    std::size_t n = std::strlen(text);
    if (log_length + n + 1 < sizeof(log_buffer))
    {
        std::memcpy(log_buffer + log_length, text, n);
        log_length += n;
        log_buffer[log_length++] = ' ';
    }
}

int ctor_runs;

} // namespace

extern "C" {

void test_init_env(void *a, void *b, void *c)
{
    char *const *block = static_cast<char *const *>(a);
    event(a == b && b == c && block[0] == nullptr && block[1] == nullptr ? "env(empty)" : "env(BAD)");
}

void test_pre1(int, char **, char **) { event("pre1"); }
void test_ctor1(int argc, char **argv, char **)
{
    ++ctor_runs;
    event(argc == 2 && std::strcmp(argv[1], "arg1") == 0 ? "ctor1(args)" : "ctor1(BAD)");
}
void test_handler(void) { event("atexit-handler"); }
void test_ctor2(int, char **, char **)
{
    ++ctor_runs;
    event("ctor2");
    std::atexit(test_handler);   // registered after _fini: runs before .fini_array
}
void test_fini1(void) { event("fini1"); }
void test_fini2(void)
{
    event("fini2");
    // the last thing to run: the log goes out, in one write
    if (write(1, log_buffer, log_length) < 0)
        _exit(99);
}

int test_main(int argc, char **argv, char **)
{
    alignas(16) volatile char probe[16] = {};
    event((reinterpret_cast<std::uintptr_t>(probe) & 15) == 0 ? "main(aligned)" : "main(MISALIGNED)");
    event(argc == 2 && std::strcmp(argv[0], "prog") == 0 ? "main(args)" : "main(BAD)");
    ps5rt_test_init();   // a second call (as through DT_INIT): nothing runs again
    event(ctor_runs == 2 ? "init-once" : "init-TWICE");
    return 7;
}
}

asm(R"(
    .section .data.rel.ro.test_arrays, "aw"
    .p2align 3
    .globl test_preinit_start, test_preinit_end, test_init_start, test_init_end, test_fini_start, test_fini_end
test_preinit_start:
    .quad test_pre1
test_preinit_end:
test_init_start:
    .quad test_ctor1, test_ctor2
test_init_end:
test_fini_start:
    .quad test_fini2, test_fini1
test_fini_end:
    .text
)");

int main()
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return 2;
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipefd[1], 1);
        static char prog[] = "prog", arg1[] = "arg1";
        static std::uintptr_t block[] = {2, reinterpret_cast<std::uintptr_t>(prog),
                                         reinterpret_cast<std::uintptr_t>(arg1), 0, 0};
        ps5rt_test_entry(block);
        _exit(98);   // not reached: _start ends in exit()
    }
    close(pipefd[1]);
    std::string out;
    char buf[512];
    for (ssize_t n; (n = read(pipefd[0], buf, sizeof buf)) > 0;)
        out.append(buf, static_cast<std::size_t>(n));
    int status = 0;
    waitpid(pid, &status, 0);
    const std::string want = "env(empty) pre1 ctor1(args) ctor2 main(aligned) main(args) init-once atexit-handler "
                             "fini1 fini2 ";
    // .fini_array runs last to first: test_fini1 (the second entry) before test_fini2
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 7 && out == want;
    std::printf("start-up: exit %d, ran: %s\n%s\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1, out.c_str(),
                ok ? "test_start: ok" : ("test_start: FAILED, wanted: " + want).c_str());
    return ok ? 0 : 1;
}
