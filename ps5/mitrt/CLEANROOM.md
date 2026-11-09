<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 MisterTemaki -->
# How this runtime was written

Goal: replace three GPL-3.0-or-later pieces of ps5-native-app-boilerplate (BlackBearReloaded) that the PS5 ports put
into their app: `app_crt.cpp` (start-up), `app_cpp_runtime.cpp` (operator new / delete), and `sce_module/libc.prx`
(written by `libc_builder` from `runtime/api-surface.txt` and `runtime/imports.txt`). Everything here was written
fresh, under MIT, from interface facts. Written by Claude (Anthropic) for MisterTemaki, November 2026.

## Not opened

`app_crt.cpp`, `app_cpp_runtime.cpp`, `api-surface.txt` and `imports.txt` were not opened, read, diffed or copied.
Not one line of them was seen. To be exact about the edges: three recursive searches run for other reasons scanned
them along with everything else, and none of their content was seen. A search for `catchReturnFromMain` and similar
names in fbneo-ps5/ps5 had its hits in `proto/native/tooling/native/app_*` filtered out before display. A
`git grep` for file names (`app_crt`, `api-surface`, ...) excluded the two `.cpp` files and matched nothing in the
two manifests. A search for `_init_env` over the work folder timed out with no output.

## Facts used, and where they came from

1. **The objects' interface** (`llvm-nm`, `readelf -S`, and the symbol names in `readelf -r` per section, but not
   their code). These come from build/app/app_crt.o and app_cpp_runtime.o of fbneo-ps5.
   - `app_crt.o` defines `_start`, `_init`, `_fini`, a weak `catchReturnFromMain` and an 8-byte
     `ps5_early_stack_guard_address`. It references `_init_env`, `atexit`, `exit` and `main`, and weak
     `__preinit_array_*`, `__init_array_*` and `__fini_array_*`. `_start` references `_init_env`, `atexit`, `exit`,
     `main`, `_init`, `catchReturnFromMain` and `_fini`. `_init` references the preinit and init array bounds, and
     `_fini` the fini array bounds.
   - `app_cpp_runtime.o` defines the 20 `operator new` / `delete` forms (plain, array, nothrow, aligned, sized,
     aligned + nothrow, sized + aligned) and `ps5ObserveOwnedAllocation`. It references `malloc`, `posix_memalign`,
     `free`, `std::bad_alloc` and `__cxa_throw`.
   - `ps5_early_stack_guard_address`, `ps5ObserveOwnedAllocation` and `catchReturnFromMain` are referenced by no
     port source (grep of fbneo-ps5/ps5 and snes9x-ps5/ps5, outside proto/). They are left out, except `catchReturnFromMain`,
     which a program may override.
2. **The link** (fbneo-ps5 `ps5/Makefile`, and the linker inputs it names).
   - `-e _start`, `-pie`.
   - `ps5-pie.ld` provides `__preinit_array_start` ... `__fini_array_end` with `PROVIDE_HIDDEN`.
   - `localize.map` makes every defined symbol local. libc++, libc++abi and libunwind are static and come before the
     system stubs.
   - The old objects were built with `-fno-exceptions` (start-up) and `-fexceptions` (allocator).
   - `ps5-native-tool link` turns lld's DT_INIT / DT_FINI (the `_init` / `_fini` symbols) and the init arrays into the
     eboot's dynamic table. Seen in `sce_module_writer.cpp`, which is not one of the four files.
3. **The SDK** (/opt/ps5-payload-sdk).
   - The stub `libSceLibcInternal.so` exports `_init_env`, `atexit`, `exit`, `__cxa_atexit`, `malloc`, `free`,
     `posix_memalign`, `setjmp` and `longjmp`.
   - `libkernel.so` exports `_sceKernelSetThreadDtors`, `_sceKernelSetThreadAtexitCount`,
     `_sceKernelSetThreadAtexitReport` and `_sceKernelRtldSetApplicationHeapAPI`.
   - In `libc++abi.a`, `std::get_new_handler` lives in `cxa_handlers.cpp.o`. Its own `operator new` is weak, in
     `stdlib_new_delete.cpp.o`.
   - `prospero-clang++` passes `-target x86_64-sie-ps5 -fno-plt -femulated-tls -fno-stack-protector`.
4. **What the eboots import** (`tools/sce_imports.py` on eboot ELFs).
   - FBNeo PS5 2.3 and Snes9x PS5 were built locally.
   - Genesis Plus GX PS5 2.2.1, NES FCEUmm PS5 1.1, Beetle PCE PS5 1.1.1 and Stella PS5 1.0.2 come from their GitHub
     releases; `eboot.bin` was extracted with `ps5-native-tool self --extract`.
   - None of the six imports anything from a module `libc`. Their C library is libSceLibcInternal. The union of the
     functions they import from it, 229 of them, is `libc/exports.txt`, written by `tools/make_manifest.py`.
5. **The old libc.prx's output** (build/app/libc.raw.elf, inspected with `readelf`, a dynamic-table dump and
   `objdump`), and **`libc_builder.cpp`** (allowed: it is not one of the four files). Facts taken:
   - the module format: ELF type 0xFE18, the segment kinds and their flags, file offset = address + 0x4000, the
     dynamic tags, module "libc" 1.1 needing libkernel and libSceLibcInternal, library ids and attributes (9 for
     import, 1 for export), the module-parameter block (magic 0x3C13F4BF, 3, the two SDK versions, 1), and the
     PATH comment and version records;
   - an export `Need_sceLibc` (an object, value 1), and `_setjmp` / `_longjmp` in a library `libc_setjmp`;
   - what its initialiser does, which gives the behaviour mirrored here because it is the form proven on consoles:
     it hands libkernel empty thread-exit hooks and a 9-entry heap table with malloc at 0, free at 1 and
     posix_memalign at 6.

   Not taken:
   - its export list (the 2566 NIDs of the GPL manifest);
   - its byte layout, code bytes or strings: `mkprx.py` assembles its own code (see the next point) and lays out the
     file itself;
   - its design of an init trampoline and "return 0" stubs. Here every export forwards to the real function.
6. **Public specifications.**
   - ELF-64 and its SysV hash.
   - The x86-64 instruction encodings, from the Intel SDM.
   - The FreeBSD/amd64 entry convention: rdi points at argc, then argv.
   - The Itanium C++ ABI (`__cxa_atexit` and its LIFO with `atexit`) and C++17 [new.delete].
   - The published PS4/PS5 NID scheme: SHA-1 with the fixed suffix, 8 bytes reversed, base64, `/` → `-`. Checked
     against `malloc` = `gQX+4GDQjpM`.

## netdb (src/ps5rt_netdb.cpp)

This replaces the payload SDK's `libc.a(netdb.o)`, which is GPL-3.0-or-later (John Törnblom). The rule was not to
read that source.

**Disclosure.** Before the rule was given, while I was finding out which libc.a members the PIE links and under which
licence, two looks at `rsdeps/PS5_PayloadSDK/libc/netdb.c` showed:
- its licence lines;
- the first line of each top-level declaration and function: about 20 lines of names and prototypes, such as
  `sceNetResolverStartNtoa(int, const char*, in_addr_t*, int, int, int)`, `resolve_ntoa(const char *hostname,
  in_addr_t *addr)` and `getaddrinfo(const char *hostname, const char *servname, ...`.

No function body was seen. Nothing here depends on those lines: the resolver's interface came from the port's own
MIT code (below).

Facts used:
1. **What the link takes from netdb.o.** Found with `ld.lld -y` and `--why-extract` on fbneo-ps5's PIE link, and
   `llvm-nm` on `libc.a`:
   - libnfs's `libnfs-sync.o` pulls it in for `getnameinfo` (NI_NUMERICHOST, server discovery);
   - libsmb2's `socket.o` uses `getprotobyname("tcp")`;
   - `getaddrinfo` / `freeaddrinfo` were already taken by the port's own `--wrap` shim;
   - netdb.o defines `getaddrinfo`, `freeaddrinfo`, `getnameinfo`, `gai_strerror`, `getprotobyname`,
     `getservbyname(_r)`, `getservbyport(_r)`, `gethostbyname(_r)`, `gethostbyaddr`, `hstrerror` and `__h_errno`.
2. **How the callers use them** (their source in fbneo-ps5/ps5/third_party and netfs):
   - libsmb2 calls `getaddrinfo(host, "445", NULL, ...)` and walks the list;
   - libnfs calls `getaddrinfo(server, NULL or "%d", NULL, ...)` and `getnameinfo(..., NI_NUMERICHOST)`;
   - netfs's `backend_smb.cpp` asks with AF_INET and SOCK_STREAM.
3. **The resolver's interface, from the port's own MIT code** (`netfs/netfs_shims_ps5.cpp`, `frontend/fe_tlshttp.cpp`,
   `coreorbis/include-orbis/ProsperoSce.h`):
   - `sceNetInit`, `sceNetPoolCreate(name, size, flags)`, `sceNetResolverCreate(name, pool, flags)`,
     `sceNetResolverStartNtoa(resolver, name, uint32_t *address, timeout_us, retries, flags)` and
     `sceNetResolverDestroy`;
   - 5 s and 2 retries, an address in network order.

   The SDK's stub `libSceNet.so` exports these names. The SDK's headers don't declare them.
4. **The SDK's headers:** `netdb.h` (the EAI_, AI_ and NI_ values, `struct addrinfo`, `protoent` and `servent`,
   FreeBSD's `getnameinfo` with `size_t` lengths) and `netinet/in.h` (`sin_len`).
5. **Public specifications:** POSIX `getaddrinfo` / `getnameinfo`, RFC 5952 (IPv6 text form) and the IANA port numbers.

## Choices made without a fact (to try on a console)

- `_init_env`'s parameters aren't known. It gets a block of null pointers in rdi, rsi and rdx, which is an empty
  environment whatever its shape: no argument, envp, the argument block, or (argc, argv, envp).
- `_start` doesn't register the loader's clean-up routine (rsi) with `atexit`, because its form isn't documented here.
  `exit()` ends the process either way.
- `_init` / `_fini` are idempotent. So is libc.prx's initialiser, which is reachable from both DT_INIT and
  DT_PREINIT_ARRAY. Each runs once, whichever entry the loader uses.
- Exported data objects aren't forwarded, because they can't be.
- netdb: one IPv4 result per `getaddrinfo` call (SOCK_STREAM unless asked otherwise), no reverse lookup in
  `getnameinfo` (numeric, NI_NAMEREQD is EAI_NONAME), a short service table. A failed lookup is EAI_NONAME; no
  resolver pool or resolver is EAI_FAIL.
