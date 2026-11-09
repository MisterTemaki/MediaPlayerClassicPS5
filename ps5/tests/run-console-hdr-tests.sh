#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM
${HOST_CXX:-clang++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$P/coreorbis/include-orbis" -I"$P/coreorbis/orbis-shims" \
    "$P/tests/console_hdr_test.cpp" -pthread -o "$W/test-console-hdr"
ASAN_OPTIONS=detect_leaks=1 "$W/test-console-hdr"
