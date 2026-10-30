#!/usr/bin/env python3
"""A Blu-ray (PGS, .sup) subtitle stream for the host tests (group 30): `count` lines, one every `every` seconds, each
shown for `shown` seconds as a w x h picture (an opaque white box), on a 1920x1080 screen.

  make_pgs.py <out.sup> <count> <every> <shown> <w> <h> [<screen w> <screen h>]   (the screen: 1920 x 1080)

Each line is a display set (PCS with one object, WDS, PDS, ODS, END) at its start and an empty one (a PCS with no
object, END) at its end, as Blu-ray authoring tools write them; FFmpeg's sup demuxer reads it and can mux it into MKV.
"""
import struct
import sys


def segment(kind, pts, data):
    # "PG", PTS and DTS (90 kHz), the segment type and size
    return b'PG' + struct.pack('>IIBH', pts, 0, kind, len(data)) + data


def rle(w, h, color):
    # PGS run-length lines: 00 11LLLLLL LLLLLLLL CC (L pixels of colour C, 64..16383), then 00 00 (end of the line)
    line = b'\x00' + struct.pack('>H', 0xC000 | w) + bytes([color]) + b'\x00\x00'
    return line * h


SCREEN = [1920, 1080]


def display_set(pts, comp, w, h, show):
    out = b''
    sw, sh = SCREEN
    x, y = max(0, (sw - w) // 2), max(0, sh - h - 60)  # a picture bigger than the screen: at 0, 0
    if show:
        pcs = struct.pack('>HHBHBBBB', sw, sh, 0x10, comp, 0x80, 0, 0, 1) + struct.pack('>HBBHH', 0, 0, 0, x, y)
        out += segment(0x16, pts, pcs)
        out += segment(0x17, pts, struct.pack('>BBHHHH', 1, 0, x, y, w, h))
        # palette 0: entry 1 opaque white (Y Cr Cb A)
        out += segment(0x14, pts, struct.pack('>BB', 0, 0) + bytes([1, 235, 128, 128, 255]))
        data = rle(w, h, 1)
        ods = struct.pack('>HBB', 0, 0, 0xC0) + struct.pack('>I', len(data) + 4)[1:] + struct.pack('>HH', w, h) + data
        out += segment(0x15, pts, ods)
    else:
        out += segment(0x16, pts, struct.pack('>HHBHBBBB', sw, sh, 0x10, comp, 0x00, 0, 0, 0))
        out += segment(0x17, pts, struct.pack('>BBHHHH', 1, 0, x, y, w, h))
    return out + segment(0x80, pts, b'')


def main():
    out, count, every, shown, w, h = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
    if len(sys.argv) > 8:
        SCREEN[:] = [int(sys.argv[7]), int(sys.argv[8])]
    data = []  # joined once at the end (a += on bytes copies it all each time: minutes for 30,000 lines)
    comp = 0
    for i in range(count):
        start = int(round((0.2 + i * every) * 90000))
        end = int(round((0.2 + i * every + shown) * 90000))
        data.append(display_set(start, comp & 0xFFFF, w, h, True))
        comp += 1
        data.append(display_set(end, comp & 0xFFFF, w, h, False))
        comp += 1
    open(out, 'wb').write(b''.join(data))


if __name__ == '__main__':
    main()
