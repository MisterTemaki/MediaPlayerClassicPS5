#!/usr/bin/env python3
"""A VobSub pair (.idx + .sub) for the host tests (group 31), made from a PGS file (tests/make_pgs.py): FFmpeg turns
its pictures into DVD subtitles in an MPEG-PS (.sub); the .idx gets the encoder's header (size, palette), one
language and a timestamp and file position per picture, at the PGS lines' times.

  make_vobsub.py <in.sup> <out base>   ->   <out base>.sub, <out base>.idx
"""
import json
import os
import subprocess
import sys


def probe(path, *args):
    out = subprocess.run(["ffprobe", "-v", "error", "-of", "json", *args, path], check=True, capture_output=True,
                         text=True).stdout
    return json.loads(out)


def main():
    sup, base = sys.argv[1], sys.argv[2]
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", sup, "-c:s", "dvdsub", "-f", "vob", base + ".sub"], check=True)
    mkv = base + ".tmp.mkv"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", sup, "-c:s", "dvdsub", mkv], check=True)
    # the encoder's header (size, palette): the stream's extradata, shown by ffprobe as a hex dump
    dump = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_data", mkv], check=True,
                          capture_output=True, text=True).stdout
    os.unlink(mkv)
    hexes = []
    on = False
    for line in dump.splitlines():
        if line.startswith("extradata="):
            on = True
            continue
        if on:
            if not line.startswith("0000"):
                break
            hexes.append(line[10:50].replace(" ", ""))  # "00000090: 2061 6161 ...  text"
    header = bytes.fromhex("".join(hexes)).decode("ascii")
    # the PGS lines' start times (display sets with an object) and the DVD pictures' places in the .sub
    starts = []
    for p in probe(sup, "-show_packets")["packets"]:
        t = float(p["pts_time"])
        if int(p["size"]) > 100 and (not starts or abs(starts[-1] - t) > 1e-6):
            starts.append(t)
    packs = [(float(p["pts_time"]), int(p["pos"])) for p in probe(base + ".sub", "-show_packets")["packets"]]
    lines = [header.rstrip("\n"), "id: en, index: 0"]
    for (_, pos), t in zip(packs, starts):
        ms = int(round(t * 1000))
        lines.append("timestamp: %02d:%02d:%02d:%03d, filepos: %09x" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60,
                                                                         ms % 1000, pos - pos % 2048))
    open(base + ".idx", "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
