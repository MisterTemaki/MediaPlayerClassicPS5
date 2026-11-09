# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Small helpers for PS5 dynamic ELF files (the kind ps5-native-tool link writes for eboot.elf, and the kind
# mkprx.py writes for libc.prx): the symbol NID, the library / module id letters, and a reader for the import and
# export tables. Written from the public ELF-64 specification and the published PS4/PS5 NID scheme
# (SHA-1 of the name and a fixed 16-byte suffix; the first 8 bytes of the digest, little-endian, in base64 with
# '-' for '/', 11 characters).
import base64
import hashlib
import struct

NID_SUFFIX = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

PT_LOAD, PT_DYNAMIC = 1, 2
DT_NEEDED, DT_STRTAB, DT_SYMTAB = 1, 5, 6
DT_SCE_NEEDED_MODULE = 0x61000045
DT_SCE_IMPORT_LIB = 0x61000049
DT_SCE_EXPORT_LIB = 0x61000047
DT_SCE_MODULE_INFO = 0x61000043
DT_SCE_SYMTABSZ = 0x6100003F


def nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    value = digest[:8][::-1]
    return base64.b64encode(value).decode()[:11].replace("/", "-")


def encode_id(value):
    if value == 0:
        return "A"
    out = ""
    while value:
        out = ID_ALPHABET[value % 64] + out
        value //= 64
    return out


def decode_id(text):
    value = 0
    for c in text:
        value = value * 64 + ID_ALPHABET.index(c)
    return value


class SceElf:
    """The dynamic tables of a PS5 ELF: needed modules, import / export libraries and the symbols."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise ValueError(f"{path}: not an ELF file (extract a signed container first)")
        self.e_type = struct.unpack_from("<H", d, 0x10)[0]
        phoff = struct.unpack_from("<Q", d, 0x20)[0]
        phnum = struct.unpack_from("<H", d, 0x38)[0]
        self.phdrs = [struct.unpack_from("<IIQQQQQQ", d, phoff + i * 56) for i in range(phnum)]
        dyn = [p for p in self.phdrs if p[0] == PT_DYNAMIC]
        if not dyn:
            raise ValueError(f"{path}: no PT_DYNAMIC")
        _, _, off, _, _, size, _, _ = dyn[0]
        self.dynamic = []
        for i in range(size // 16):
            tag, val = struct.unpack_from("<qQ", d, off + i * 16)
            if tag == 0:
                break
            self.dynamic.append((tag, val))
        strtab = self._addr(self._tag(DT_STRTAB))
        self.symtab_off = self._addr(self._tag(DT_SYMTAB))
        self.symtab_size = self._tag(DT_SCE_SYMTABSZ)
        self._str = lambda o: d[strtab + o:d.index(b"\0", strtab + o)].decode()
        self.needed = [self._str(v) for t, v in self.dynamic if t == DT_NEEDED]
        self.modules = {v >> 48: self._str(v & 0xFFFFFFFF) for t, v in self.dynamic if t == DT_SCE_NEEDED_MODULE}
        self.import_libs = {v >> 48: self._str(v & 0xFFFFFFFF) for t, v in self.dynamic if t == DT_SCE_IMPORT_LIB}
        self.export_libs = {v >> 48: self._str(v & 0xFFFFFFFF) for t, v in self.dynamic if t == DT_SCE_EXPORT_LIB}
        info = [v for t, v in self.dynamic if t == DT_SCE_MODULE_INFO]
        self.module_name = self._str(info[0] & 0xFFFFFFFF) if info else None
        self.symbols = []
        for i in range(1, self.symtab_size // 24):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", d, self.symtab_off + i * 24)
            self.symbols.append((self._str(name), info, other, shndx, value, size))

    def _tag(self, tag):
        for t, v in self.dynamic:
            if t == tag:
                return v
        raise KeyError(hex(tag))

    def _addr(self, va):
        for ptype, _, off, vaddr, _, filesz, _, _ in self.phdrs:
            if ptype == PT_LOAD and vaddr <= va < vaddr + filesz:
                return off + va - vaddr
        raise ValueError(f"address {va:#x} is in no loaded segment")

    def imports(self):
        """[(nid, library name, module name, symbol type)] of every undefined symbol."""
        out = []
        for name, info, _, shndx, _, _ in self.symbols:
            if shndx != 0 or "#" not in name:
                continue
            n, lib, mod = name.split("#")
            out.append((n, self.import_libs.get(decode_id(lib), "?" + lib),
                        self.modules.get(decode_id(mod), "?" + mod), info & 0xF))
        return out

    def exports(self):
        """[(nid, library name, symbol type, value, size)] of every defined symbol."""
        out = []
        for name, info, _, shndx, value, size in self.symbols:
            if shndx == 0 or "#" not in name:
                continue
            n, lib, _ = name.split("#")
            out.append((n, self.export_libs.get(decode_id(lib), "?" + lib), info & 0xF, value, size))
        return out
