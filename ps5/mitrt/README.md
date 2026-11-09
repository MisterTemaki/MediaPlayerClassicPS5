<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 MisterTemaki -->
# ps5-native-runtime-mit

The C/C++ start-up and the `sce_module/libc.prx` of a native PS5 title (eboot.bin), under the MIT licence, for
programs that can't be combined with GPL code. It replaces the boilerplate's `app_crt.cpp`, `app_cpp_runtime.cpp`
and the `libc_builder` + manifests that wrote `libc.prx`. How it was written: [CLEANROOM.md](CLEANROOM.md).

| file | what |
|---|---|
| `src/ps5rt_start.cpp` | `_start`, `_init` / `_fini` (`.preinit_array`, `.init_array`, `.fini_array`), `catchReturnFromMain`: environment set-up (`_init_env`), constructors, `main`, `exit` with its status |
| `src/ps5rt_new.cpp` | every global `operator new` / `delete` (plain, array, nothrow, aligned, sized) on `malloc` / `posix_memalign` / `free`; a failed allocation throws `std::bad_alloc` |
| `src/ps5rt_netdb.cpp` | `getaddrinfo`, `freeaddrinfo`, `getnameinfo`, `gai_strerror`, `getprotobyname` / `bynumber`, `getservbyname` / `byport` on the console's resolver (`sceNetResolverStartNtoa`), IPv4; linked ahead of the payload SDK's `libc.a`, it keeps that library's `netdb.o` (GPL) out |
| `tools/mkprx.py` | writes `libc.prx`'s ELF from `libc/exports.txt` (sign it afterwards, e.g. `ps5-native-tool self --sign`) |
| `libc/exports.txt` | its exports: `Need_sceLibc`, and forwarders to libSceLibcInternal for the C library functions the eboots use |
| `tools/make_manifest.py` | writes `libc/exports.txt` from eboot ELFs (what they import from libSceLibcInternal) |
| `tools/sce_imports.py` | lists a PS5 ELF's imports (or exports) by module and library, with names from the SDK stubs |
| `tests/` | host tests (`make test`): the start-up, allocator and netdb (on a fake resolver, under ASan) built for Linux, `libc.prx` checked with objdump |
| `platform/` | `build/libps5rt_platform.a`: what a RADV title (Mesa's Vulkan driver linked into the eboot) needs that the console's libc lacks: thread stacks, open_memstream, the C locale's `*_l` functions, `qsort_r`, `fstatat` & co, emulated TLS; MIT linker scripts for the PIE (`platform/link`), MIT `ps5platform` headers to build RADV with (`platform/radv-shim`); its link flags, link checks and a test title ([platform/README.md](platform/README.md)) |

Use in a port: compile the three sources with the SDK's `prospero-clang++` (`-fno-exceptions` for the start-up,
`-fexceptions` for the allocator), link them first (netdb before the SDK's `libc.a`) in the eboot's PIE with `-e _start`, a linker script that provides
`__preinit_array_start` ... `__fini_array_end`, and libc++ / libc++abi / libunwind static; ship the signed
`libc.prx` as `sce_module/libc.prx`.

The titles import nothing from `libc.prx` (their C library is the system's libSceLibcInternal): the console only
needs the module to be there. What it does when loaded, and why: the header of `tools/mkprx.py`.
