#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SRC=${LOG_SOURCE_ROOT:-$P}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM
${HOST_CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$SRC/coreorbis/include-orbis" \
    "$P/tests/log_password_test.cpp" "$SRC/coreorbis/orbis-shims/orbis_paths.cpp" -pthread -o "$W/test-log"
rc=0
ASAN_OPTIONS=detect_leaks=1 "$W/test-log" "$W/root" >"$W/output.log" || rc=$?
grep -E '^(ok  |FAIL) ' "$W/output.log"
exit "$rc"
