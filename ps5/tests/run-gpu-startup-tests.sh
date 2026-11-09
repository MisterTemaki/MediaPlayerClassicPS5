#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SRC=${GPU_START_SOURCE_ROOT:-$P}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM
DEPS_HEADERS=${DEPS_HEADERS:-$P/build/deps/prefix-host/include}
[ -d "$DEPS_HEADERS" ] || DEPS_HEADERS=$P/build/deps/prefix-ps5/include
${HOST_CXX:-g++} -std=c++17 -O1 -g -mavx2 -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
    -I"$SRC/coreorbis/include-orbis" -I"$SRC/coreorbis/orbis-shims" -I"$SRC/player" \
    -I"$SRC/ps5-slang-gpu/include" -isystem "$DEPS_HEADERS" \
    "$P/tests/gpu_startup_test.cpp" -Wl,--gc-sections -Wl,--wrap=fsync -pthread -o "$W/test-gpu-startup"
ASAN_OPTIONS=detect_leaks=1 "$W/test-gpu-startup" "$W/root"
