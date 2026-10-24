#!/usr/bin/env python3
"""Fonts for the host tests (group 23): damaged ones the player must leave out without crashing, and weights of one family.

  make_test_fonts.py bad <dir> <font.ttf>       truncated copies, a name table pointing past the file, a .ttc whose
                                               second face starts far outside it, a tiny .ttc, junk
  make_test_fonts.py weights <dir> <font.ttf>  copies of one family with OS/2 weights 400 / 600 / 700 (bold bit) /
                                               800 / 900 (bold bit)
"""
import os
import struct
import sys


def tables(data, base=0):
    n = struct.unpack('>H', data[base + 4:base + 6])[0]
    out = {}
    for i in range(n):
        e = base + 12 + 16 * i
        out[data[e:e + 4]] = (e, *struct.unpack('>II', data[e + 8:e + 16]))
    return out


def bad(d, src):
    data = open(src, 'rb').read()
    for name, size in (('aa-cut681.ttf', 681000), ('ab-cut100.ttf', 100), ('ac-cut5000.ttf', 5000),
                       ('ad-cut-half.ttf', len(data) // 2)):
        open(os.path.join(d, name), 'wb').write(data[:size])
    t = tables(data)
    # the name table's directory entry: past the end of the file
    b = bytearray(data)
    e = t[b'name'][0]
    b[e + 8:e + 16] = struct.pack('>II', len(data) - 8, 4096)
    open(os.path.join(d, 'ae-namepast.ttf'), 'wb').write(b)
    # the name table inside the file, its strings far past it (the font stays usable, its names unread)
    b = bytearray(data)
    off = t[b'name'][1]
    b[off + 4:off + 6] = struct.pack('>H', 0xfff0)
    open(os.path.join(d, 'af-namestr.ttf'), 'wb').write(b)
    # a collection: face 0 a good copy (its table offsets moved by the header), face 1 at 0x7ffffff0
    head = b'ttcf' + struct.pack('>IIII', 0x10000, 2, 20, 0x7ffffff0)
    f = bytearray(data)
    for tag, (e, o, l) in tables(data).items():
        f[e + 8:e + 12] = struct.pack('>I', o + len(head))
    open(os.path.join(d, 'ag-badoff.ttc'), 'wb').write(head + f)
    open(os.path.join(d, 'ah-tiny.ttc'), 'wb').write(b'ttcf\x00\x01\x00\x00\x00\x00\x00\x09')
    open(os.path.join(d, 'ai-junk.otf'), 'wb').write(bytes((i * 131 + 7) & 255 for i in range(3000)))


def weights(d, src):
    data = open(src, 'rb').read()
    os2 = tables(data)[b'OS/2'][1]
    for name, weight, boldbit in (('a-regular.ttf', 400, False), ('b-semibold.ttf', 600, False),
                                  ('c-black.ttf', 900, True), ('d-bold.ttf', 700, True), ('e-extrabold.ttf', 800, False)):
        b = bytearray(data)
        b[os2 + 4:os2 + 6] = struct.pack('>H', weight)
        sel = struct.unpack('>H', b[os2 + 62:os2 + 64])[0] & ~0x61
        sel |= 0x20 if boldbit else 0x40
        b[os2 + 62:os2 + 64] = struct.pack('>H', sel)
        open(os.path.join(d, name), 'wb').write(b)


{'bad': bad, 'weights': weights}[sys.argv[1]](sys.argv[2], sys.argv[3])
