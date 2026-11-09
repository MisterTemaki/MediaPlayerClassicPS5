#!/usr/bin/env python3
# Media Player Classic PS5 1.6: writes the PIE's import table into the PIE (mpcps5_import_table, ProsperoCrash.cpp).
#
#   embed_imports.py <mpc-pie.elf> <sdk-lib-dir> [extra stub .so ...]
#
# test2 on a console logged "sceSysmoduleLoadModule unresolved" (hardware decoding off) and crashed setting the GPU
# up, with no way to see which imports the console had bound. The app now reads each import's slot at its start and
# logs, per module, how many are bound and which are left at 0 ([imports] lines). For that it needs each slot's
# address and its symbol's module: this takes them from the PIE's dynamic relocations (GLOB_DAT, JUMP_SLOT, and an
# R_X86_64_64 for a symbol that has neither), the module from the NEEDED stubs in order (the first that defines the
# name, as ps5-native-tool binds it), and writes, over the placeholder "MPCIMP01...":
#   MPCIMP03\n<mpcps5_crash_anchor's address, hex>\n<the image's end, hex>\n<slot address, hex> <module> <symbol>\n ... \0
# The table's size is fixed (the placeholder's), so nothing else in the PIE moves. Fails (exit 1) when there is no
# placeholder or the table doesn't fit.
import os
import subprocess
import sys

MAGIC = b"MPCIMP01"


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def soname(so):
    for line in run("readelf", "-dW", so).splitlines():
        if "(SONAME)" in line and "[" in line:
            return line[line.index("[") + 1:line.rindex("]")]
    return None


def defined(so):
    names = set()
    for line in run("nm", "-D", "--defined-only", so).splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[2].split("@")[0])
    return names


def main():
    if len(sys.argv) < 3:
        print(__doc__ or "usage: embed_imports.py <pie> <sdk-lib> [stub.so ...]", file=sys.stderr)
        return 2
    pie, sdk = sys.argv[1], sys.argv[2]
    stubs = {}
    for f in sorted(os.listdir(sdk)):
        path = os.path.join(sdk, f)
        if not path.endswith(".so") or not os.path.isfile(path):
            continue
        name = soname(path)
        if name and name not in stubs:
            stubs[name] = path
    for f in sys.argv[3:]:
        name = soname(f)
        if name:
            stubs[name] = f
    needed = []
    for line in run("readelf", "-dW", pie).splitlines():
        if "(NEEDED)" in line and "[" in line:
            needed.append(line[line.index("[") + 1:line.rindex("]")])
    exports = [(n, defined(stubs[n])) for n in needed if n in stubs]

    def module_of(sym):
        for n, names in exports:
            if sym in names:
                return n
        return "?"

    # the dynamic relocations against a symbol: slot address -> symbol
    slots = {}
    other = {}
    for line in run("readelf", "-rW", pie).splitlines():
        parts = line.split()
        if len(parts) < 5 or not parts[0][:1].isalnum():
            continue
        try:
            off = int(parts[0], 16)
        except ValueError:
            continue
        kind, sym = parts[2], parts[4]
        if kind in ("R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT"):
            slots.setdefault(sym, off)
        elif kind == "R_X86_64_64" and sym not in other:
            other[sym] = off
    for sym, off in other.items():
        slots.setdefault(sym, off)
    # the imports hardware decoding needs: from their modules, and not weak (test2's eboot imported
    # sceSysmoduleLoadModule weak, from libSceSysmodule; the console left it at 0. VLC for PS5's ordinary imports of the
    # same functions are bound)
    required = {"sceSysmoduleLoadModule": "libSceSysmodule.sprx"}
    for f in ("QueryComputeMemoryInfo", "AllocateComputeQueue", "QueryDecoderMemoryInfo", "CreateDecoder",
              "DeleteDecoder", "Decode", "Flush", "Reset"):
        required["sceVideodec2" + f] = "libSceVideodec2.sprx"
    weak = set()
    for line in run("readelf", "--dyn-syms", "-W", pie).splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[4] == "WEAK" and parts[6] == "UND":
            weak.add(parts[7].split("@")[0])
    bad = []
    for sym, mod in required.items():
        if sym not in slots:
            bad.append("%s isn't imported" % sym)
        elif module_of(sym) != mod:
            bad.append("%s comes from %s, not %s" % (sym, module_of(sym), mod))
        elif sym in weak:
            bad.append("%s is a weak import" % sym)
    if bad:
        print("embed_imports: hardware decoding's imports: " + "; ".join(bad), file=sys.stderr)
        return 1
    anchor = None
    for line in run("nm", pie).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "mpcps5_crash_anchor":
            anchor = int(parts[0], 16)
    if anchor is None:
        print("embed_imports: no mpcps5_crash_anchor in", pie, file=sys.stderr)
        return 1
    # the image's end: every slot is below it (the app reads none past it: a malformed table stops there)
    span = 0
    for line in run("readelf", "-lW", pie).splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD":
            span = max(span, int(parts[2], 16) + int(parts[5], 16))
    text = "MPCIMP03\n%x\n%x\n" % (anchor, span)
    for sym, off in sorted(slots.items(), key=lambda kv: (module_of(kv[0]), kv[0])):
        text += "%x %s %s\n" % (off, module_of(sym), sym)
    data = bytearray(open(pie, "rb").read())
    # the placeholder: mpcps5_import_table's bytes in the file (its section's offset + its address in the section; the
    # same text is in the debug information too)
    table = None
    for line in run("nm", pie).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "mpcps5_import_table":
            table = int(parts[0], 16)
    at = -1
    for line in run("readelf", "-SW", pie).splitlines():
        parts = line.replace("[ ", "[").split()
        if len(parts) >= 7 and parts[0].startswith("[") and parts[2] == "PROGBITS" and table is not None:
            addr, off, size = int(parts[3], 16), int(parts[4], 16), int(parts[5], 16)
            if addr <= table < addr + size:
                at = off + (table - addr)
    if at < 0 or data[at:at + len(MAGIC)] != MAGIC:
        print("embed_imports: no placeholder (mpcps5_import_table) in %s" % pie, file=sys.stderr)
        return 1
    blob = text.encode() + b"\0"
    size = 64 * 1024
    if len(blob) > size:
        print("embed_imports: the table (%d bytes) doesn't fit in %d" % (len(blob), size), file=sys.stderr)
        return 1
    data[at:at + len(blob)] = blob
    open(pie, "wb").write(data)
    mods = {}
    for sym in slots:
        mods[module_of(sym)] = mods.get(module_of(sym), 0) + 1
    print("embed_imports: %d imports (%s) written into %s" %
          (len(slots), ", ".join("%s %d" % kv for kv in sorted(mods.items())), pie))
    return 0


if __name__ == "__main__":
    sys.exit(main())
