#!/usr/bin/env python3
"""Media Player Classic PS5 1.6: two screenshots (PPM, the same size) compared over a rectangle, channel by channel.

  compare_ppm.py <a.ppm> <b.ppm> <x0> <y0> <x1> <y1> <max mean> <max p99>

Prints the mean and the 99th percentile of |a - b| (0..255) and exits 1 when either is over its limit."""
import sys

from PIL import Image, ImageChops, ImageStat

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
lim_mean, lim_p99 = float(sys.argv[7]), float(sys.argv[8])
d = ImageChops.difference(a.crop((x0, y0, x1, y1)), b.crop((x0, y0, x1, y1)))
mean = sum(ImageStat.Stat(d).mean) / 3
hist = [0] * 256
for band in d.split():
    for i, n in enumerate(band.histogram()):
        hist[i] += n
total = sum(hist)
acc, p99 = 0, 0
for i, n in enumerate(hist):
    acc += n
    if acc >= total * 0.99:
        p99 = i
        break
print("mean %.2f p99 %d" % (mean, p99))
sys.exit(0 if mean <= lim_mean and p99 <= lim_p99 else 1)
