#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Writes libc/exports.txt, mkprx.py's manifest, from facts: the functions the given eboots import from the system C
# library (libSceLibcInternal). Each becomes a forwarder that libc.prx exports in its library "libc"; setjmp and
# longjmp are also exported as _setjmp / _longjmp in a library "libc_setjmp". Data objects can't be forwarded and are
# left out (the eboots bind them to libSceLibcInternal directly).
#
#   make_manifest.py [--sdk-lib DIR] --out libc/exports.txt eboot.elf ...
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sceelf  # noqa: E402
import sce_imports  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sdk-lib", default=os.path.join(os.environ.get("PS5_PAYLOAD_SDK", "/opt/ps5-payload-sdk"),
                                                      "target", "lib"))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--source", action="append", default=[],
                    help="a line for the header naming where the eboots came from (repeatable)")
    args = ap.parse_args()
    known = sce_imports.stub_names(args.sdk_lib, args.nm)
    functions, objects = set(), set()
    for path in args.files:
        for n, lib, _, typ in sceelf.SceElf(path).imports():
            if lib != "libSceLibcInternal":
                continue
            if n not in known:
                raise SystemExit(f"{path}: import {n} from libSceLibcInternal: no SDK stub name for this NID")
            (functions if typ == 2 else objects).add(known[n])
    with open(args.out, "w", encoding="utf-8") as out:
        out.write("# SPDX-License-Identifier: MIT\n# Copyright (C) 2026 MisterTemaki\n#\n")
        out.write("# libc.prx's exports (tools/mkprx.py). Written by tools/make_manifest.py from the functions these\n")
        out.write("# eboots import from libSceLibcInternal (none of them imports anything from libc.prx itself):\n")
        for line in args.source:
            out.write(f"#   {line}\n")
        out.write(f"# {len(functions)} functions forwarded; {len(objects)} data objects left out: "
                  f"{' '.join(sorted(objects))}\n#\n")
        out.write("# marker  LIBRARY NAME           a 4-byte object, value 1\n")
        out.write("# forward LIBRARY NAME [TARGET]  jumps to TARGET (default NAME) of libSceLibcInternal\n\n")
        out.write("marker libc Need_sceLibc\n")
        for name in sorted(functions):
            out.write(f"forward libc {name}\n")
        if "setjmp" in functions and "longjmp" in functions:
            out.write("forward libc_setjmp _setjmp setjmp\nforward libc_setjmp _longjmp longjmp\n")
    print(f"{args.out}: {len(functions)} forwarded functions, {len(objects)} objects left out")
    return 0


if __name__ == "__main__":
    sys.exit(main())
