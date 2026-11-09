#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Writes sce_module/libc.prx's ELF (before signing): the C runtime module a native PS5 title ships in its
# sce_module folder. The console loads it with the title and stops the title (PRX_SCE_MODULE_LOAD_ERROR) when it is
# missing. The titles themselves import nothing from it -- their C library is the system's libSceLibcInternal -- so
# this module is small:
#   - module "libc" (libc.prx), needing libkernel and libSceLibcInternal;
#   - exports, from libc/exports.txt: the Need_sceLibc marker (a 4-byte object, value 1), and functions that forward
#     to libSceLibcInternal (one indirect jump each through this module's own GOT), so that anything which binds to
#     libc.prx gets the system's implementation;
#   - an initialiser (DT_INIT, and DT_PREINIT_ARRAY's one entry; it runs once whichever the loader calls) that gives
#     libkernel the thread-exit hooks (empty: the C library has no per-thread destructors of its own here) and the
#     application heap (malloc / free / posix_memalign of libSceLibcInternal).
# The file layout is the PS5 dynamic-module format: an ELF64 of type 0xFE18 whose dynamic tables live in an unmapped
# load segment, symbols named NID#library#module, and the module-parameter, comment and version segments.
#
#   mkprx.py --manifest libc/exports.txt --out libc.raw.elf [--module-sdk 0x02000009] [--companion-sdk 0x08050001]
#
# The output is deterministic (same manifest and options, same bytes).
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sceelf  # noqa: E402

PAGE = 0x4000
FILE_BIAS = 0x4000            # file offset = virtual address + FILE_BIAS for the mapped segments
ET_SCE_DYNAMIC = 0xFE18

PT_LOAD, PT_DYNAMIC, PT_NOTE, PT_TLS = 1, 2, 4, 7
PT_SCE_MODULE_PARAM = 0x61000002
PT_GNU_EH_FRAME, PT_GNU_RELRO = 0x6474E550, 0x6474E552
PT_SCE_COMMENT, PT_SCE_VERSION = 0x6FFFFF00, 0x6FFFFF01
PF_X, PF_W, PF_R = 1, 2, 4

R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE = 7, 8

DT = {
    "NULL": 0, "NEEDED": 1, "PLTRELSZ": 2, "PLTGOT": 3, "HASH": 4, "STRTAB": 5, "SYMTAB": 6, "RELA": 7,
    "RELASZ": 8, "RELAENT": 9, "STRSZ": 10, "SYMENT": 11, "INIT": 12, "FINI": 13, "SONAME": 14, "PLTREL": 20,
    "JMPREL": 23, "INIT_ARRAY": 25, "FINI_ARRAY": 26, "INIT_ARRAYSZ": 27, "FINI_ARRAYSZ": 28,
    "PREINIT_ARRAY": 32, "PREINIT_ARRAYSZ": 33, "RELACOUNT": 0x6FFFFFF9,
    "SCE_MODULE_ATTR": 0x61000011, "SCE_EXPORT_LIB_ATTR": 0x61000017, "SCE_IMPORT_LIB_ATTR": 0x61000019,
    "SCE_HASHSZ": 0x6100003D, "SCE_SYMTABSZ": 0x6100003F, "SCE_ORIGINAL_FILENAME": 0x61000041,
    "SCE_MODULE_INFO": 0x61000043, "SCE_NEEDED_MODULE": 0x61000045, "SCE_EXPORT_LIB": 0x61000047,
    "SCE_IMPORT_LIB": 0x61000049,
}

MODULE = "libc"
SONAME = "libc.prx"
MODULE_VERSION = 0x0101       # 1.1
LIBRARY_VERSION = 0x0001
MODULE_PARAM_MAGIC = 0x3C13F4BF

# libkernel's hooks the C runtime module sets at load
KERNEL_HOOKS = ["_sceKernelSetThreadDtors", "_sceKernelSetThreadAtexitCount", "_sceKernelSetThreadAtexitReport",
                "_sceKernelRtldSetApplicationHeapAPI"]
# the heap table given to _sceKernelRtldSetApplicationHeapAPI: 9 entries, these filled (index: function)
HEAP_TABLE_ENTRIES = 9
HEAP_TABLE = {0: "malloc", 1: "free", 6: "posix_memalign"}


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def read_manifest(path):
    """[(kind, library, name, target)]: kind 'marker' or 'forward'."""
    entries = []
    for number, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "marker" and len(parts) == 3:
            entries.append(("marker", parts[1], parts[2], None))
        elif parts[0] == "forward" and len(parts) in (3, 4):
            entries.append(("forward", parts[1], parts[2], parts[3] if len(parts) == 4 else parts[2]))
        else:
            raise SystemExit(f"{path}:{number}: expected 'marker LIB NAME' or 'forward LIB NAME [TARGET]'")
    keys = [(lib, name) for _, lib, name, _ in entries]
    if len(set(keys)) != len(keys):
        raise SystemExit(f"{path}: a library exports the same name twice")
    if ("marker", "libc", "Need_sceLibc", None) not in entries:
        raise SystemExit(f"{path}: the Need_sceLibc marker of library libc is required")
    return entries


class Code:
    """A small x86-64 assembler for the few instruction forms the module needs (RIP-relative operands are
    resolved when the target addresses are known)."""

    def __init__(self, base):
        self.base = base
        self.out = bytearray()
        self.fixups = []        # (offset of disp32, end of instruction, target key)

    def here(self):
        return self.base + len(self.out)

    def raw(self, data):
        self.out += bytes(data)

    def rip(self, opcode, target, imm=b""):
        self.out += bytes(opcode)
        at = len(self.out)
        self.out += b"\0\0\0\0" + bytes(imm)
        self.fixups.append((at, len(self.out), target))

    def resolve(self, addresses):
        for at, end, target in self.fixups:
            disp = addresses[target] - (self.base + end)
            assert -(1 << 31) <= disp < (1 << 31)
            struct.pack_into("<i", self.out, at, disp)

    def pad(self, alignment, fill=0xCC):
        while len(self.out) % alignment:
            self.out.append(fill)


class Strings:
    def __init__(self):
        self.data = bytearray(b"\0")
        self.index = {}

    def add(self, text):
        if text not in self.index:
            self.index[text] = len(self.data)
            self.data += text.encode() + b"\0"
        return self.index[text]


def sysv_hash(name):
    h = 0
    for c in name.encode():
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


def hash_table(names):
    n = len(names)
    buckets = [0] * n
    chains = [0] * n
    for i in range(1, n):
        b = sysv_hash(names[i]) % n
        chains[i] = buckets[b]
        buckets[b] = i
    return struct.pack(f"<II{n}I{n}I", n, n, *buckets, *chains)


def build(entries, module_sdk, companion_sdk):
    # ---- libraries and modules: ids are shared by import and export libraries; module 0 is this one ----
    needed = [("libkernel", "libkernel.prx", "libkernel"),
              ("libSceLibcInternal", "libSceLibcInternal.prx", "libSceLibcInternal")]   # (module, file, library)
    export_libs = []
    for _, lib, _, _ in entries:
        if lib not in export_libs:
            export_libs.append(lib)
    module_id = {name: i + 1 for i, (name, _, _) in enumerate(needed)}
    import_lib_id = {lib: i for i, (_, _, lib) in enumerate(needed)}
    export_lib_id = {lib: len(needed) + i for i, lib in enumerate(export_libs)}

    # ---- imports: the kernel hooks, the heap functions and every forward target (functions, one GOT slot each) ----
    imports = [("libkernel", name) for name in KERNEL_HOOKS]
    for name in list(HEAP_TABLE.values()) + [t for k, _, _, t in entries if k == "forward"]:
        if ("libSceLibcInternal", name) not in imports:
            imports.append(("libSceLibcInternal", name))

    # ---- text ----
    text = Code(0)
    text.raw([0xCC] * 16)                               # nothing at address 0 (a zero DT_INIT reads as "none")
    addr = {}
    addr["init"] = text.here()
    text.rip([0x80, 0x3D], "guard", b"\x00")            # cmp byte [rip+guard], 0
    text.raw([0x75, 0x00])                              # jne done (patched below)
    jne_at = len(text.out) - 1
    text.rip([0xC6, 0x05], "guard", b"\x01")            # mov byte [rip+guard], 1
    text.raw([0x48, 0x83, 0xEC, 0x08])                  # sub rsp, 8
    for hook, callback in zip(KERNEL_HOOKS[:3], ["cb_thread_dtors", "cb_atexit_count", "cb_atexit_report"]):
        text.rip([0x48, 0x8D, 0x3D], callback)          # lea rdi, [rip+callback]
        text.rip([0xFF, 0x15], ("got", "libkernel", hook))   # call [rip+slot]
    for index, name in sorted(HEAP_TABLE.items()):
        text.rip([0x48, 0x8B, 0x05], ("got", "libSceLibcInternal", name))   # mov rax, [rip+slot]
        text.rip([0x48, 0x89, 0x05], ("heap", index))                       # mov [rip+heap+8*index], rax
    text.rip([0x48, 0x8D, 0x3D], ("heap", 0))           # lea rdi, [rip+heap]
    text.rip([0xFF, 0x15], ("got", "libkernel", KERNEL_HOOKS[3]))
    text.raw([0x48, 0x83, 0xC4, 0x08])                  # add rsp, 8
    text.out[jne_at] = len(text.out) - (jne_at + 1)
    text.raw([0x31, 0xC0, 0xC3])                        # done: xor eax, eax; ret
    text.pad(16)
    for name, body in (("fini", [0xC3]), ("cb_thread_dtors", [0xC3]), ("cb_atexit_count", [0x31, 0xC0, 0xC3]),
                       ("cb_atexit_report", [0xC3])):
        addr[name] = text.here()
        text.raw(body)
        text.pad(16)
    exports = []                                        # (library, name, kind, address, size)
    for kind, lib, name, target in entries:
        if kind == "forward":
            a = text.here()
            text.rip([0xFF, 0x25], ("got", "libSceLibcInternal", target))   # jmp [rip+slot]
            text.pad(8)
            exports.append((lib, name, "func", a, 8))
    text_end = text.here()

    # ---- read-only data: the marker, an empty unwind table (header + terminator) ----
    ro = align(text_end, PAGE)
    marker_addr = ro
    eh_hdr = ro + 8
    eh_frame = eh_hdr + 12
    ro_end = eh_frame + 4
    for kind, lib, name, _ in entries:
        if kind == "marker":
            exports.append((lib, name, "object", marker_addr, 4))

    # ---- RELRO: GOT, the preinit array, the module parameters ----
    relro = align(ro_end, PAGE)
    got = relro
    slot = {("got",) + imp: got + (3 + i) * 8 for i, imp in enumerate(imports)}
    preinit = got + (3 + len(imports)) * 8
    module_param = preinit + 8
    relro_end = module_param + 0x20

    # ---- data: the heap table, the initialiser's guard ----
    data = align(relro_end, PAGE)
    heap = data
    guard = heap + HEAP_TABLE_ENTRIES * 8
    data_end = guard + 8

    addr["guard"] = guard
    for i in range(HEAP_TABLE_ENTRIES):
        addr[("heap", i)] = heap + 8 * i
    addr.update(slot)
    text.resolve(addr)

    # ---- dynamic metadata (unmapped load segment) ----
    meta = align(data_end, PAGE)
    strings = Strings()
    sym_names = [""]
    symbols = [b"\0" * 24]

    def symbol(name, info, other, shndx, value, size):
        sym_names.append(name)
        symbols.append(struct.pack("<IBBHQQ", strings.add(name), info, other, shndx, value, size))
        return len(symbols) - 1

    for lib, name, kind, value, size in exports:
        full = f"{sceelf.nid(name)}#{sceelf.encode_id(export_lib_id[lib])}#{sceelf.encode_id(0)}"
        if kind == "func":
            symbol(full, (1 << 4) | 2, 0, 1, value, size)
        else:
            symbol(full, (1 << 4) | 1, 3, 2, value, size)
    import_symbol = {}
    for mod, name in imports:
        full = f"{sceelf.nid(name)}#{sceelf.encode_id(import_lib_id[mod])}#{sceelf.encode_id(module_id[mod])}"
        import_symbol[(mod, name)] = symbol(full, (1 << 4) | 2, 0, 0, 0, 0)

    dyn_strings = {}
    for mod, file, lib in needed:
        dyn_strings[file] = strings.add(file)
        dyn_strings[mod] = strings.add(mod)
        dyn_strings[lib] = strings.add(lib)
    for lib in export_libs:
        dyn_strings[lib] = strings.add(lib)
    dyn_strings[MODULE] = strings.add(MODULE)
    dyn_strings[SONAME] = strings.add(SONAME)

    jmprel = b"".join(struct.pack("<QQq", slot[("got",) + imp], (import_symbol[imp] << 32) | R_X86_64_JUMP_SLOT, 0)
                      for imp in imports)
    rela = struct.pack("<QQq", preinit, R_X86_64_RELATIVE, addr["init"])
    hashes = hash_table(sym_names)
    symtab = b"".join(symbols)
    strtab = bytes(strings.data)

    layout = {}
    cursor = meta
    for name, blob, alignment in (("strtab", strtab, 1), ("symtab", symtab, 8), ("jmprel", jmprel, 8),
                                  ("rela", rela, 8), ("hash", hashes, 8)):
        cursor = align(cursor, alignment)
        layout[name] = cursor
        cursor += len(blob)
    build_id_note = align(cursor, 4)
    cursor = build_id_note + 36
    dynamic = align(cursor, 16)

    def pack_id(name_offset, version, ident):
        return name_offset | (version << 32) | (ident << 48)

    tags = []
    for mod, file, lib in needed:
        tags += [(DT["NEEDED"], dyn_strings[file]),
                 (DT["SCE_NEEDED_MODULE"], pack_id(dyn_strings[mod], MODULE_VERSION, module_id[mod])),
                 (DT["SCE_IMPORT_LIB"], pack_id(dyn_strings[lib], LIBRARY_VERSION, import_lib_id[mod])),
                 (DT["SCE_IMPORT_LIB_ATTR"], (import_lib_id[mod] << 48) | 0x09)]
    tags += [(DT["SONAME"], dyn_strings[SONAME]),
             (DT["SCE_MODULE_INFO"], pack_id(dyn_strings[MODULE], MODULE_VERSION, 0)),
             (DT["SCE_MODULE_ATTR"], 0),
             (DT["SCE_ORIGINAL_FILENAME"], dyn_strings[SONAME])]
    for lib in export_libs:
        tags += [(DT["SCE_EXPORT_LIB"], pack_id(dyn_strings[lib], LIBRARY_VERSION, export_lib_id[lib])),
                 (DT["SCE_EXPORT_LIB_ATTR"], (export_lib_id[lib] << 48) | 0x01)]
    tags += [(DT["RELA"], layout["rela"]), (DT["RELASZ"], len(rela)), (DT["RELAENT"], 24), (DT["RELACOUNT"], 1),
             (DT["JMPREL"], layout["jmprel"]), (DT["PLTRELSZ"], len(jmprel)), (DT["PLTGOT"], got),
             (DT["PLTREL"], DT["RELA"]),
             (DT["SYMTAB"], layout["symtab"]), (DT["SYMENT"], 24), (DT["STRTAB"], layout["strtab"]),
             (DT["STRSZ"], len(strtab)), (DT["HASH"], layout["hash"]),
             (DT["PREINIT_ARRAY"], preinit), (DT["PREINIT_ARRAYSZ"], 8),
             (DT["INIT_ARRAY"], 0), (DT["INIT_ARRAYSZ"], 0), (DT["FINI_ARRAY"], 0), (DT["FINI_ARRAYSZ"], 0),
             (DT["INIT"], addr["init"]), (DT["FINI"], addr["fini"]),
             (DT["SCE_SYMTABSZ"], len(symtab)), (DT["SCE_HASHSZ"], len(hashes)), (DT["NULL"], 0)]
    dynamic_blob = b"".join(struct.pack("<qQ", t, v) for t, v in tags)
    meta_end = dynamic + len(dynamic_blob)

    # ---- the image ----
    image = bytearray(meta_end + FILE_BIAS)

    def put(va, blob):
        image[va + FILE_BIAS:va + FILE_BIAS + len(blob)] = blob

    put(0, text.out)
    put(marker_addr, struct.pack("<I", 1))
    put(eh_hdr, bytes([1, 0x1B, 0x03, 0x3B]) + struct.pack("<iI", eh_frame - (eh_hdr + 4), 0))
    put(got, struct.pack("<Q", dynamic))
    put(module_param, struct.pack("<QIIIII4x", 0x20, MODULE_PARAM_MAGIC, 3, companion_sdk, module_sdk, 1))
    for name, blob in (("strtab", strtab), ("symtab", symtab), ("jmprel", jmprel), ("rela", rela),
                       ("hash", hashes)):
        put(layout[name], blob)
    put(dynamic, dynamic_blob)
    # build id: a digest of everything above
    build_id = hashlib.sha1(bytes(image)).digest()
    put(build_id_note, struct.pack("<III", 4, 20, 3) + b"GNU\0" + build_id)

    # file-only records after the mapped image: comment (PATH), version, the SIE note
    comment_text = SONAME.encode() + b"\0"
    comment = bytearray(b"PATH" + struct.pack("<II", 0, len(comment_text)) + comment_text)
    comment += b"\0" * (align(len(comment), 4) - len(comment))
    struct.pack_into("<I", comment, 4, len(comment) - 8)
    label = (MODULE + ":").encode()
    body = bytes([8]) + label + struct.pack(">IIII", module_sdk, 1, module_sdk, 1)
    version = struct.pack("<HH", 0, len(body)) + body
    sie_note = struct.pack("<III", 4, 8, 3) + b"SIE\0" + b"\0" * 8
    comment_off = align(len(image), 16)
    version_off = align(comment_off + len(comment), 16)
    sie_off = align(version_off + len(version), 4)
    image += b"\0" * (comment_off - len(image)) + comment
    image += b"\0" * (version_off - len(image)) + version
    image += b"\0" * (sie_off - len(image)) + sie_note

    def loaded(va, end, flags):
        return (PT_LOAD, flags, va + FILE_BIAS, va, end - va, end - va, PAGE)

    phdrs = [
        loaded(0, text_end, PF_X),
        loaded(ro, ro_end, PF_R),
        loaded(relro, relro_end, PF_R | PF_W),
        (PT_GNU_RELRO, PF_R, relro + FILE_BIAS, relro, relro_end - relro, align(relro_end - relro, PAGE), 1),
        loaded(data, data_end, PF_R | PF_W),
        (PT_SCE_MODULE_PARAM, PF_R, module_param + FILE_BIAS, module_param, 0x20, 0x20, 8),
        (PT_DYNAMIC, PF_R | PF_W, dynamic + FILE_BIAS, dynamic, len(dynamic_blob), len(dynamic_blob), 8),
        (PT_TLS, PF_R, relro + FILE_BIAS, relro, 0, 0, 16),
        (PT_GNU_EH_FRAME, PF_R, eh_hdr + FILE_BIAS, eh_hdr, 12, 12, 4),
        (PT_LOAD, 0, meta + FILE_BIAS, meta, meta_end - meta, meta_end - meta, PAGE),
        (PT_SCE_COMMENT, 0, comment_off, 0, len(comment), 0, 16),
        (PT_SCE_VERSION, 0, version_off, 0, len(version), len(version), 1),
        (PT_NOTE, 0, build_id_note + FILE_BIAS, build_id_note, 36, 36, 4),
        (PT_NOTE, 0, sie_off, 0, len(sie_note), 0, 4),
    ]
    header = bytearray(64)
    header[0:16] = b"\x7fELF" + bytes([2, 1, 1, 9, 2]) + b"\0" * 7
    struct.pack_into("<HHIQQQIHHHHHH", header, 16, ET_SCE_DYNAMIC, 62, 1, 0, 64, 0, 0, 64, 56, len(phdrs), 64,
                     0, 0)
    image[0:64] = header
    for i, p in enumerate(phdrs):
        struct.pack_into("<IIQQQQQQ", image, 64 + i * 56, p[0], p[1], p[2], p[3], p[3], p[4], p[5], p[6])
    assert 64 + len(phdrs) * 56 <= FILE_BIAS
    return bytes(image), exports, imports


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--module-sdk", default="0x02000009")
    ap.add_argument("--companion-sdk", default="0x08050001")
    args = ap.parse_args()
    entries = read_manifest(args.manifest)
    image, exports, imports = build(entries, int(args.module_sdk, 0), int(args.companion_sdk, 0))
    with open(args.out, "wb") as f:
        f.write(image)
    print(f"{args.out}: {len(image)} bytes, {len(exports)} exports, {len(imports)} imports, "
          f"sha256 {hashlib.sha256(image).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
