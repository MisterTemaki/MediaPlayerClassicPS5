#!/usr/bin/env python3
"""Counts the pixels of one colour in a rectangle of a host flip dump (PPM).

  check_region.py file.ppm x0 y0 x1 y1 r g b tolerance min_count   succeeds when at least min_count match
"""
import sys

path = sys.argv[1]
x0, y0, x1, y1, r, g, b, tol, need = map(int, sys.argv[2:11])
data = open(path, "rb").read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
pix = parts[3]
n = 0
for y in range(max(0, y0), min(h, y1)):
    row = y * w * 3
    for x in range(max(0, x0), min(w, x1)):
        o = row + x * 3
        if abs(pix[o] - r) <= tol and abs(pix[o + 1] - g) <= tol and abs(pix[o + 2] - b) <= tol:
            n += 1
print(n)
sys.exit(0 if n >= need else 1)
