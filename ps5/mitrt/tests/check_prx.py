#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Checks a libc.prx ELF written by tools/mkprx.py against its manifest, decoding the code with GNU objdump (an
# independent disassembler):
#   - the module is "libc" (soname libc.prx), needs exactly libkernel and libSceLibcInternal, exports libraries
#     from the manifest only;
#   - every manifest entry is exported once, by NID, in its library: Need_sceLibc an object holding 1, each forwarder
#     a function whose code is one `jmp *slot(%rip)` to a GOT slot that a JUMP_SLOT relocation binds to the target's
#     NID in libSceLibcInternal;
#   - DT_INIT (and the preinit entry, through its RELATIVE relocation) is the initialiser: it calls the three thread
#     hooks and the heap hook of libkernel, and stores malloc / free / posix_memalign of libSceLibcInternal in the
#     table it hands to the heap hook (entries 0, 1 and 6);
#   - every import is used, and every GOT slot has its relocation.
#   check_prx.py libc.raw.elf libc/exports.txt
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import mkprx  # noqa: E402
import sceelf  # noqa: E402

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def disassemble(elf, start, end):
    data = elf.data[elf._addr(start):elf._addr(start) + (end - start)]
    with tempfile.NamedTemporaryFile(suffix=".bin") as f:
        f.write(data)
        f.flush()
        out = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", f"--adjust-vma={start:#x}", f.name],
                             check=True, capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(.*)$", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip()))
    return insns


def rip_target(text):
    m = re.search(r"#\s*0x([0-9a-f]+)", text)
    return int(m.group(1), 16) if m else None


def main():
    path, manifest = sys.argv[1], sys.argv[2]
    entries = mkprx.read_manifest(manifest)
    elf = sceelf.SceElf(path)
    d = elf.data
    dyn = {}
    for t, v in elf.dynamic:
        dyn.setdefault(t, []).append(v)

    check(elf.e_type == 0xFE18, "ELF type 0xFE18 (PS5 dynamic module)")
    check(elf.module_name == "libc", "module info: libc")
    check(elf.needed == ["libkernel.prx", "libSceLibcInternal.prx"], f"needed: {elf.needed}")
    check(sorted(elf.export_libs.values()) == sorted({lib for _, lib, _, _ in entries}),
          f"export libraries: {sorted(elf.export_libs.values())}")

    # relocations
    jmprel_off = elf._addr(dyn[23][0])
    slots = {}
    for i in range(dyn[2][0] // 24):
        offset, info, addend = struct.unpack_from("<QQq", d, jmprel_off + i * 24)
        check_type = info & 0xFFFFFFFF
        if check_type != 7:
            check(False, f"jmprel {i}: type {check_type}")
        name = elf.symbols[(info >> 32) - 1][0]
        n, lib, mod = name.split("#")
        slots[offset] = (n, elf.import_libs[sceelf.decode_id(lib)], elf.modules[sceelf.decode_id(mod)])
    rela_off = elf._addr(dyn[7][0])
    relative = {}
    for i in range(dyn[8][0] // 24):
        offset, info, addend = struct.unpack_from("<QQq", d, rela_off + i * 24)
        if info == 8:
            relative[offset] = addend
    check(dyn.get(0x6FFFFFF9, [0])[0] == len(relative), "RELACOUNT = the RELATIVE relocations")

    exports = {(lib, n): (typ, value, size) for n, lib, typ, value, size in elf.exports()}
    check(len(exports) == len(entries), f"{len(entries)} manifest entries, {len(exports)} exports")
    used_slots = set()
    bad = 0
    for kind, lib, name, target in entries:
        e = exports.get((lib, sceelf.nid(name)))
        if e is None:
            check(False, f"{lib} {name}: not exported")
            continue
        typ, value, size = e
        if kind == "marker":
            check(typ == 1 and size == 4 and struct.unpack_from("<I", d, elf._addr(value))[0] == 1,
                  f"{lib} {name}: an object of 4 bytes holding 1")
            continue
        insns = disassemble(elf, value, value + 6)
        slot = rip_target(insns[0][1]) if insns and insns[0][1].startswith("jmp") else None
        want = (sceelf.nid(target), "libSceLibcInternal", "libSceLibcInternal")
        if typ != 2 or slot not in slots or slots[slot] != want:
            bad += 1
            print(f"     {lib} {name}: {insns[:1]} slot {slot} -> {slots.get(slot)}")
        used_slots.add(slot)
    check(bad == 0, f"every forwarder is one jmp through the GOT slot of its target in libSceLibcInternal ({bad} bad)")

    # the initialiser
    init = dyn[12][0]
    preinit = dyn[32][0]
    check(init != 0 and relative.get(preinit) == init, "DT_INIT and the preinit entry are the initialiser")
    fini = dyn[13][0]
    check(disassemble(elf, fini, fini + 1)[0][1] == "ret", "DT_FINI returns")
    insns = disassemble(elf, init, init + 0x100)
    calls, stores, loads = [], {}, {}
    last_load = None
    heap = None
    for _, text in insns:
        if text.startswith("call"):
            calls.append(slots.get(rip_target(text), ("?",))[0])
        elif text.startswith("mov") and "%rip),%rax" in text:
            last_load = slots.get(rip_target(text), ("?",))[0]
        elif text.startswith("mov") and "%rax," in text and "%rip)" in text:
            stores[rip_target(text)] = last_load
        elif text.startswith("lea") and "%rdi" in text:
            heap = rip_target(text)
        elif text == "ret":
            break
        used_slots.update(s for s in [rip_target(text)] if s in slots)
    hooks = [sceelf.nid(h) for h in mkprx.KERNEL_HOOKS]
    check(calls == hooks, "the initialiser calls libkernel's three thread hooks, then the heap hook")
    want_table = {heap + 8 * i: sceelf.nid(n) for i, n in mkprx.HEAP_TABLE.items()}
    check(stores == want_table, "heap table: malloc at 0, free at 1, posix_memalign at 6, the table handed to the hook")
    check(set(slots) <= used_slots, f"every import is used ({len(slots)} slots)")

    # the SysV hash table finds every symbol by name
    hash_off = elf._addr(dyn[4][0])
    nbucket, nchain = struct.unpack_from("<II", d, hash_off)
    buckets = struct.unpack_from(f"<{nbucket}I", d, hash_off + 8)
    chains = struct.unpack_from(f"<{nchain}I", d, hash_off + 8 + 4 * nbucket)
    names = [""] + [s[0] for s in elf.symbols]
    found = 0
    for i in range(1, len(names)):
        j = buckets[mkprx.sysv_hash(names[i]) % nbucket]
        while j and names[j] != names[i]:
            j = chains[j]
        found += j == i
    check(nchain == len(names) and found == len(names) - 1,
          f"hash table: {found} of {len(names) - 1} symbols found by name")
    check(dyn[0x6100003D][0] == 8 + 4 * (nbucket + nchain) and dyn[0x6100003F][0] == 24 * len(names),
          "SCE_HASHSZ and SCE_SYMTABSZ match the tables")
    print(f"{path}: {'ok' if not failures else str(len(failures)) + ' failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
