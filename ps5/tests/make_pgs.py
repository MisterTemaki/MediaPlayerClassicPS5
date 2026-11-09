#!/usr/bin/env python3
"""A Blu-ray (PGS, .sup) subtitle stream for the host tests (group 30): `count` lines, one every `every` seconds, each
shown for `shown` seconds as a w x h picture (an opaque white box), on a 1920x1080 screen.

  make_pgs.py <out.sup> <count> <every> <shown> <w> <h> [<screen w> <screen h>]   (the screen: 1920 x 1080)
  make_pgs.py <out.sup> epochs      normal display sets that use their epoch's palette and object (see epochs())
  make_pgs.py <out.sup> clearing    a line cleared by a normal display set (see clearing())
  make_pgs.py <out.sup> longepoch   999 normal lines in one epoch, the palette only at its start (see longepoch())

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


# Normal display sets that lean on their epoch (1.6 audit: the PGS palette on a re-decode): `make_pgs.py <out.sup>
# epochs` writes, on a 1920 x 1080 screen, object 0 drawn bottom-centre and cleared a second later each time:
#   0.2 s  epoch start: palette 0 = {1: white, 2: green}, object 0 1000x200 in colour 1          -> white
#   2.2 s  normal, a palette update only: palette 0 colour 1 = red, object 0 as it was            -> red, 1000x200
#   4.2 s  normal, a new object 0 1000x200 in colour 2, no palette (the epoch's)                  -> green
#   8.2 s  epoch start: palette 0 = {1: blue, 2: yellow}, object 0 200x100 in colour 1            -> blue, 200x100
# Decoded in order each line is as above; a line decoded with another epoch's objects and palettes (8.2 s's) would be
# a 200x100 red box at 2.2 s and a yellow one at 4.2 s.
YCRCB = {'white': (235, 128, 128), 'red': (63, 240, 102), 'green': (173, 26, 42), 'blue': (32, 118, 240),
         'yellow': (219, 138, 16)}


def epoch_set(pts, comp, state, w, h, palette=None, color=None, update=False):
    sw, sh = SCREEN
    x, y = (sw - w) // 2, sh - h - 60
    pcs = struct.pack('>HHBHBBBB', sw, sh, 0x10, comp, state, 0x80 if update else 0x00, 0, 1) + struct.pack('>HBBHH', 0, 0, 0, x, y)
    out = segment(0x16, pts, pcs)
    out += segment(0x17, pts, struct.pack('>BBHHHH', 1, 0, x, y, w, h))
    if palette:
        out += segment(0x14, pts, struct.pack('>BB', 0, 0) + b''.join(bytes([i, *YCRCB[c], 255]) for i, c in palette))
    if color is not None:
        data = rle(w, h, color)
        ods = struct.pack('>HBB', 0, 0, 0xC0) + struct.pack('>I', len(data) + 4)[1:] + struct.pack('>HH', w, h) + data
        out += segment(0x15, pts, ods)
    return out + segment(0x80, pts, b'')


def clear_set(pts, comp):
    sw, sh = SCREEN
    return segment(0x16, pts, struct.pack('>HHBHBBBB', sw, sh, 0x10, comp, 0x00, 0, 0, 0)) + segment(0x80, pts, b'')


def epochs(out):
    t = lambda s: int(round(s * 90000))
    sets = [
        epoch_set(t(0.2), 0, 0x80, 1000, 200, palette=[(1, 'white'), (2, 'green')], color=1), clear_set(t(1.2), 1),
        epoch_set(t(2.2), 2, 0x00, 1000, 200, palette=[(1, 'red')], update=True), clear_set(t(3.2), 3),
        epoch_set(t(4.2), 4, 0x00, 1000, 200, color=2), clear_set(t(5.2), 5),
        epoch_set(t(8.2), 6, 0x80, 200, 100, palette=[(1, 'blue'), (2, 'yellow')], color=1), clear_set(t(9.2), 7),
    ]
    open(out, 'wb').write(b''.join(sets))


# (review) a clearing display set after a seek: `make_pgs.py <out.sup> clearing` -- an epoch start at 10 s (a white
# 1000x200 box), cleared by a normal display set at 20 s; another epoch start at 25 s, cleared at 26 s
def clearing(out):
    t = lambda s: int(round(s * 90000))
    open(out, 'wb').write(b''.join([
        epoch_set(t(10), 0, 0x80, 1000, 200, palette=[(1, 'white')], color=1), clear_set(t(20), 1),
        epoch_set(t(25), 2, 0x80, 1000, 200, palette=[(1, 'white')], color=1), clear_set(t(26), 3)]))


# (review) a long epoch: `make_pgs.py <out.sup> longepoch` -- one epoch start at 0.2 s with the palette (colour 1
# white), then 999 normal lines, each a new object 0 (1000x200, colour 1) and no palette: every 0.09 s for 0.05 s,
# the last at 90.2 s for 2 s. ~6,000 packets: the last line's palette is defined ~6,000 packets before it
def longepoch(out):
    t = lambda s: int(round(s * 90000))
    sets = []
    for i in range(1000):
        start, shown = (90.2, 2.0) if i == 999 else (0.2 + i * 0.09, 0.05)
        sets.append(epoch_set(t(start), (2 * i) & 0xFFFF, 0x80 if i == 0 else 0x00, 1000, 200,
                              palette=[(1, 'white')] if i == 0 else None, color=1))
        sets.append(clear_set(t(start + shown), (2 * i + 1) & 0xFFFF))
    open(out, 'wb').write(b''.join(sets))


def main():
    if len(sys.argv) == 3 and sys.argv[2] in ('epochs', 'clearing', 'longepoch'):
        {'epochs': epochs, 'clearing': clearing, 'longepoch': longepoch}[sys.argv[2]](sys.argv[1])
        return
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
