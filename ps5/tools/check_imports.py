#!/usr/bin/env python3
# Media Player Classic PS5 1.5.1 (1.5.2: the C++ runtime too): checks the app's PIE (build/app/mpc-pie.elf) before it becomes eboot.bin.
#
#   check_imports.py <mpc-pie.elf> <sdk-lib-dir> [extra stub .so ...]
#
# 1.5 crashed at launch on the console (#3, SIGSYS): statfs / getfsstat are only in the SDK's libkernel_sys, so the
# eboot gained NEEDED libkernel_sys.sprx -- a system process's library, not a title's. This fails (exit 1) when:
# - a NEEDED module isn't one of those the app is allowed (what 1.4 and 1.5 used, without libkernel_sys);
# - an imported symbol isn't defined by the stub of an allowed module (e.g. one only libkernel_sys / libkernel_web has);
# - 1.5.2: a C++ runtime symbol (__cxa_* but __cxa_atexit, __gxx_personality_v0, _Unwind_*, any _Z name such as a
#   type_info vtable) is imported instead of linked in: mixed with the static __cxa_throw and unwinder it breaks every
#   throw (Stella PS5 1.0's crash starting a game; up to 1.5.1 this eboot imported the personality routine too).
# It prints the NEEDED list and how many symbols come from each module.
import os
import subprocess
import sys

ALLOWED = [
    "libkernel.sprx",
    "libSceLibcInternal.sprx",
    "libSceVideoOut.sprx",
    "libSceAudioOut.sprx",
    "libScePad.sprx",
    "libSceUserService.sprx",
    "libSceSystemService.sprx",
    "libSceNet.sprx",
    "libSceSysmodule.sprx",
    "libSceVideodec2.sprx",
]


# C runtime pieces the system's libc provides (not part of the exception ABI)
CXA_FROM_LIBC = {"__cxa_atexit", "__cxa_finalize", "__cxa_thread_atexit", "__cxa_thread_atexit_impl"}


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def needed(elf):
    out = []
    for line in run("readelf", "-dW", elf).splitlines():
        if "(NEEDED)" in line and "[" in line:
            out.append(line[line.index("[") + 1:line.rindex("]")])
    return out


def defined(so):
    names = set()
    for line in run("nm", "-D", "--defined-only", so).splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[2].split("@")[0])
    return names


def undefined(elf):
    names = set()
    for line in run("nm", "-D", "--undefined-only", elf).splitlines():
        parts = line.split()
        if parts:
            names.add(parts[-1].split("@")[0])
    return names


def main():
    if len(sys.argv) < 3:
        print(__doc__ or "usage: check_imports.py <pie> <sdk-lib> [stub.so ...]", file=sys.stderr)
        return 2
    elf, sdk = sys.argv[1], sys.argv[2]
    stubs = {}  # soname -> stub file
    for extra in sys.argv[3:]:
        stubs[os.path.basename(extra)[:-3] + ".sprx"] = extra
    for name in ALLOWED:
        stubs.setdefault(name, os.path.join(sdk, name[:-5] + ".so"))
    errors = []
    mods = needed(elf)
    print("check_imports: %s NEEDED: %s" % (os.path.basename(elf), " ".join(mods)))
    for m in mods:
        if m not in ALLOWED:
            errors.append("NEEDED %s: not a module the app may load (allowed: %s)" % (m, ", ".join(ALLOWED)))
    providers = {m: defined(stubs[m]) for m in mods if m in ALLOWED and os.path.exists(stubs[m])}
    count = {m: 0 for m in providers}
    others = None
    for sym in sorted(undefined(elf)):
        found = [m for m in mods if m in providers and sym in providers[m]]
        if found:
            count[found[0]] += 1
            continue
        if others is None:
            others = {f: defined(os.path.join(sdk, f)) for f in sorted(os.listdir(sdk)) if f.endswith(".so")}
        where = [f for f, names in others.items() if sym in names]
        errors.append("import %s: not in an allowed module (only in %s)" % (sym, ", ".join(where) or "no SDK stub"))
    # the C++ runtime must be the eboot's own, whole (Makefile): a personality routine, __cxa_* or type_info vtable from
    # the system's libSceLibcInternal next to the static __cxa_throw / libunwind breaks every throw
    for sym in sorted(undefined(elf)):
        if sym.startswith(("_Z", "__gxx_", "_Unwind_")) or (sym.startswith("__cxa_") and sym not in CXA_FROM_LIBC):
            errors.append("import %s: the C++ runtime (exceptions, type_info, std classes) must be the eboot's own, "
                          "not a system module's" % sym)
    print("check_imports: imports by module: " + ", ".join("%s %d" % (m, n) for m, n in count.items()))
    for e in errors:
        print("check_imports: ERROR: " + e)
    if errors:
        print("check_imports: FAILED (a title can't load libkernel_sys / libkernel_web: 1.5's crash at launch, #3; the C++ "
              "runtime must be the eboot's own)")
        return 1
    print("check_imports: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
