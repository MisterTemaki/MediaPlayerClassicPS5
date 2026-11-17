#!/usr/bin/env python3
"""Reject an app package whose param.json lost what the release depends on.

- The PS5 HDR-capable application profile: attribute bits 0x62000000, as used by VLC-PS5 and documented by
  ProsperoLight's PORTING.md. This validates the package declaration, not the display's HDR capabilities.
- The application category 65536 (0x10000, "Prospero Native Media App"), as VLC-PS5 declares it: the home
  screen lists the app under Media instead of Games (issue #6, 1.6.3). 0 is a native game.
- With a version argument (the Makefile's VERSION, e.g. 1.6.3): contentVersion must be its "01.063.000" form,
  since the installer compares contentVersion to decide whether an installed copy is older.
"""
import json
from pathlib import Path
import re
import sys

MEDIA_APP = 0x10000
HDR_PROFILE = 0x62000000


def content_version(version):
    # a test build's suffix ("1.6.3-test1") doesn't change the content version
    m = re.fullmatch(r"(\d+)\.(\d)(?:\.(\d))?(?:-[0-9A-Za-z.]+)?", version)
    if not m:
        sys.exit(f"version {version!r} is not major.minor[.patch] with one-digit minor and patch")
    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)
    return f"{major:02d}.{minor * 10 + patch:03d}.000"


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: check_app_profile.py <sce_sys/param.json> [version]")
    param = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    profile = param.get("attribute")
    if type(profile) is not int or profile & HDR_PROFILE != HDR_PROFILE:
        sys.exit(f"HDR app profile missing: attribute={profile!r}, required bits={HDR_PROFILE:#x}")
    category = param.get("applicationCategoryType")
    if category != MEDIA_APP:
        sys.exit(f"application category is {category!r}, not {MEDIA_APP} (a media app, listed under Media)")
    if len(sys.argv) == 3:
        want = content_version(sys.argv[2])
        if param.get("contentVersion") != want:
            sys.exit(f"contentVersion is {param.get('contentVersion')!r}, but version {sys.argv[2]} needs {want!r}")
    print(f"app profile: attribute={profile:#x} (HDR bits present; display support not inferred), "
          f"applicationCategoryType={category} (media app)"
          + (f", contentVersion={param['contentVersion']} matches {sys.argv[2]}" if len(sys.argv) == 3 else "")
          + ": OK")


if __name__ == "__main__":
    main()
