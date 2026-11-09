#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SRC=${INSTALL_SOURCE_ROOT:-$P}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM
${HOST_CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -I"$SRC/coreorbis/include-orbis" -I"$SRC/coreorbis/orbis-shims" \
    "$P/tests/install_content_test.cpp" -Wl,--gc-sections -Wl,--wrap=stat -pthread -o "$W/test-install"
ASAN_OPTIONS=detect_leaks=1 "$W/test-install" "$W/root"
