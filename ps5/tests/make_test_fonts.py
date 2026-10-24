#!/usr/bin/env python3
"""Fonts for the host tests (group 23): damaged ones the player must leave out without crashing, and weights of one family.

  make_test_fonts.py bad <dir> <font.ttf>       truncated copies, a name table pointing past the file, a .ttc whose
                                               second face starts far outside it, a tiny .ttc, junk
  make_test_fonts.py weights <dir> <font.ttf>  copies of one family with OS/2 weights 400 / 600 / 700 (bold bit) /
                                               800 / 900 (bold bit)
  make_test_fonts.py crafted <dir> <font.ttf>  well-formed files with bad insides: a GPOS pair adjustment (format 2) whose
                                               class counts don't fit in it; 'A' a composite glyph of 8 bytes
                                               whose component (read from the bytes after it) is 'A' itself; 'A' a
                                               simple glyph whose point count runs past its data
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


def glyph_of(data, cp):
    """the glyph index of cp (cmap format 4, Windows Unicode BMP)"""
    t = tables(data)
    cmap = t[b'cmap'][1]
    n = struct.unpack('>H', data[cmap + 2:cmap + 4])[0]
    for i in range(n):
        pl, en, off = struct.unpack('>HHI', data[cmap + 4 + 8 * i:cmap + 12 + 8 * i])
        sub = cmap + off
        if pl == 3 and en == 1 and struct.unpack('>H', data[sub:sub + 2])[0] == 4:
            seg = struct.unpack('>H', data[sub + 6:sub + 8])[0] // 2
            ends = struct.unpack('>%dH' % seg, data[sub + 14:sub + 14 + 2 * seg])
            starts = struct.unpack('>%dH' % seg, data[sub + 16 + 2 * seg:sub + 16 + 4 * seg])
            deltas = struct.unpack('>%dh' % seg, data[sub + 16 + 4 * seg:sub + 16 + 6 * seg])
            ro = sub + 16 + 6 * seg
            ranges = struct.unpack('>%dH' % seg, data[ro:ro + 2 * seg])
            for k in range(seg):
                if starts[k] <= cp <= ends[k]:
                    if ranges[k] == 0:
                        return (cp + deltas[k]) & 0xffff
                    a = ro + 2 * k + ranges[k] + 2 * (cp - starts[k])
                    return (struct.unpack('>H', data[a:a + 2])[0] + deltas[k]) & 0xffff
    raise SystemExit('no glyph')


def loca_of(data):
    t = tables(data)
    fmt = struct.unpack('>H', data[t[b'head'][1] + 50:t[b'head'][1] + 52])[0]
    return t[b'loca'][1], fmt, t[b'glyf'][1]


def crafted(d, src):
    data = open(src, 'rb').read()
    t = tables(data)
    # GPOS: one lookup of type 2, one PairPos format 2 subtable covering every glyph, 65535 x 65535 classes
    # header (format 2, coverage at 16, value formats 4 / 0, class defs at 26 and 36, the class counts), the coverage
    # (format 2: one range, every glyph), both class defs (every glyph in class 65534): class counts far larger than
    # the subtable holds
    sub = struct.pack('>8H', 2, 16, 4, 0, 26, 36, 0xffff, 0xffff) + struct.pack('>5H', 2, 1, 0, 0xffff, 0)
    sub += struct.pack('>5H', 2, 1, 0, 0xffff, 0xfffe) * 2
    lookup = struct.pack('>HHHH', 2, 0, 1, 8) + sub
    gpos = struct.pack('>HHHHH', 1, 0, 0, 0, 10) + struct.pack('>HH', 1, 4) + lookup
    b = bytearray(data) + gpos
    e = t[b'GPOS'][0]
    b[e + 8:e + 16] = struct.pack('>II', len(data), len(gpos))
    open(os.path.join(d, 'ba-gpos2.ttf'), 'wb').write(b)
    # 'A': numberOfContours -1 and 8 bytes long; its "component" (bytes 10..) is glyph A itself
    a = glyph_of(data, 0x41)
    loca, fmt, glyf = loca_of(data)
    def at(g):
        return struct.unpack('>I', data[loca + 4 * g:loca + 4 * g + 4])[0] if fmt else 2 * struct.unpack('>H', data[loca + 2 * g:loca + 2 * g + 2])[0]
    b = bytearray(data)
    g0 = glyf + at(a)
    b[g0:g0 + 2] = struct.pack('>h', -1)
    b[g0 + 10:g0 + 18] = struct.pack('>HHhh', 0x0003, a, 0, 0)
    if fmt:
        b[loca + 4 * (a + 1):loca + 4 * (a + 1) + 4] = struct.pack('>I', at(a) + 8)
    else:
        b[loca + 2 * (a + 1):loca + 2 * (a + 1) + 2] = struct.pack('>H', (at(a) + 8) // 2)
    open(os.path.join(d, 'bb-shortcomp.ttf'), 'wb').write(b)
    # 'A': a simple glyph whose last endPtsOfContours says 65000 points
    b = bytearray(data)
    n = struct.unpack('>h', data[g0:g0 + 2])[0]
    b[g0 + 10 + 2 * (n - 1):g0 + 12 + 2 * (n - 1)] = struct.pack('>H', 65000)
    open(os.path.join(d, 'bc-points.ttf'), 'wb').write(b)
    # 'A': a composite whose first component is point matched (flags without ARGS_ARE_XY_VALUES: no argument bytes
    # follow) and whose second component is 'A' itself; a walk that skips argument bytes anyway sees other components
    b = bytearray(data)
    bglyph = glyph_of(data, 0x42)
    comp = struct.pack('>h4h', -1, 0, 0, 0, 0) + struct.pack('>HH', 0x0020, bglyph) + struct.pack('>HHhh', 0x0003, a, 0, 0)
    b[g0:g0 + len(comp)] = comp
    open(os.path.join(d, 'bd-pointmatch.ttf'), 'wb').write(b)
    # 'A': one contour of one off-curve point (stb_truetype reads the point after it, one past its array)
    b = bytearray(data)
    simple = struct.pack('>h4h', 1, 0, 0, 100, 100) + struct.pack('>HH', 0, 0) + bytes([0x00]) + struct.pack('>hh', 50, 50)
    b[g0:g0 + len(simple)] = simple
    open(os.path.join(d, 'be-lastoff.ttf'), 'wb').write(b)


{'bad': bad, 'weights': weights, 'crafted': crafted}[sys.argv[1]](sys.argv[2], sys.argv[3])
