#!/usr/bin/env python3
"""Reject an app package missing the PS5 HDR-capable application profile.

Profile 0x62000000 is used by VLC-PS5 and documented by ProsperoLight's PORTING.md.
This validates the package declaration, not the display's HDR capabilities.
"""
import json
from pathlib import Path
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_app_profile.py <sce_sys/param.json>")
    profile = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")).get("attribute")
    required = 0x62000000
    if type(profile) is not int or profile & required != required:
        sys.exit(f"HDR app profile missing: attribute={profile!r}, required bits={required:#x}")
    print(f"HDR app profile: attribute={profile:#x} OK (display support not inferred)")


if __name__ == "__main__":
    main()
