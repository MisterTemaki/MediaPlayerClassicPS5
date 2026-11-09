#!/usr/bin/env python3
"""Reject implemented RADV/WSI Vulkan entry points lost through weak archive references.

Unimplemented Vulkan extensions also have weak entries, so only demand symbols that
the actual driver archive defines. nm is the host's ELF-capable binutils tool.
"""
import re
import subprocess
import sys


def entrypoints(path):
    output = subprocess.check_output(
        ["nm", "--defined-only", path], text=True, stderr=subprocess.PIPE)
    return {m[1] for line in output.splitlines()
            if (m := re.search(r"\b[TtWw]\s+((?:radv|wsi)_[A-Z]\w*)$", line))}


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check_radv_link.py <player-pie.elf> <radv-archive.a>")
    player, archive = sys.argv[1:]
    implemented = entrypoints(archive)
    if not implemented:
        sys.exit("RADV check: no Vulkan entry points in archive")
    missing = sorted(implemented - entrypoints(player))
    if missing:
        print("RADV check: implemented Vulkan entry points absent from player:", file=sys.stderr)
        print("\n".join(missing), file=sys.stderr)
        return 1
    print(f"RADV check: all {len(implemented)} implemented Vulkan entry points linked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
