#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Lists what a PS5 ELF (an eboot.elf from ps5-native-tool link, or a module such as libc.prx's raw ELF) imports,
# by module and library, with the names found again from the SDK's stub libraries (NID of each exported name).
#
#   sce_imports.py [--sdk-lib DIR] [--library NAME] [--names] [--exports] file.elf ...
#
# --library NAME  only that import library (e.g. libSceLibcInternal, libc)
# --names         one plain name per line (the union over every file given), for a manifest
# --exports       the file's exports instead (library, NID, name when known)
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sceelf  # noqa: E402

TYPES = {0: "notype", 1: "object", 2: "func", 6: "tls"}


def stub_names(sdk_lib, nm):
    names = {}
    for f in sorted(os.listdir(sdk_lib)):
        if not f.endswith(".so"):
            continue
        out = subprocess.run([nm, "-D", "--defined-only", os.path.join(sdk_lib, f)],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                name = parts[2].split("@")[0]
                names.setdefault(sceelf.nid(name), name)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--sdk-lib", default=os.path.join(os.environ.get("PS5_PAYLOAD_SDK", "/opt/ps5-payload-sdk"),
                                                      "target", "lib"))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--library")
    ap.add_argument("--names", action="store_true")
    ap.add_argument("--exports", action="store_true")
    args = ap.parse_args()
    known = stub_names(args.sdk_lib, args.nm)
    union = {}
    for path in args.files:
        elf = sceelf.SceElf(path)
        if args.exports:
            for n, lib, typ, value, size in sorted(elf.exports(), key=lambda e: (e[1], e[0])):
                print(f"{path}: export {lib} {n} {known.get(n, '?')} {TYPES.get(typ, typ)} {value:#x} {size:#x}")
            continue
        if not args.names:
            print(f"{path}: module {elf.module_name}, needed {' '.join(elf.needed)}")
        per_lib = {}
        for n, lib, mod, typ in elf.imports():
            if args.library and lib != args.library:
                continue
            name = known.get(n, "?" + n)
            per_lib.setdefault((mod, lib), []).append((name, typ))
            union[name] = typ
        if not args.names:
            for (mod, lib), syms in sorted(per_lib.items()):
                print(f"  {mod} / {lib}: {len(syms)}")
    if args.names:
        for name, typ in sorted(union.items()):
            print(f"{name}|{TYPES.get(typ, typ)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
