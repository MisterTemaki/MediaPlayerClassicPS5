#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM
DEPS_HEADERS=${DEPS_HEADERS:-$P/build/deps/prefix-host/include}
[ -d "$DEPS_HEADERS" ] || DEPS_HEADERS=$P/build/deps/prefix-ps5/include
${HOST_CXX:-clang++} -std=c++17 -O1 -g -mavx2 -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -I"$P/coreorbis/include-orbis" -I"$P/coreorbis/orbis-shims" -I"$P/player" -isystem "$DEPS_HEADERS" \
    "$P/tests/vdec_memory_test.cpp" -Wl,--gc-sections -pthread -o "$W/test-vdec-memory"
ASAN_OPTIONS=detect_leaks=1 "$W/test-vdec-memory"
