#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Host tests: the start-up and the allocation functions built for Linux, and a libc.prx written and checked
# (tests/check_prx.py, GNU objdump for the code). Needs g++, objcopy, objdump, python3.
#   tests/host_test.sh [work-dir]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$(mktemp -d)}
mkdir -p "$W"
CXX=${HOST_CXX:-g++}
rc=0

# start-up: the real file, its symbols renamed so it can live in a normal Linux program
$CXX -std=c++20 -O2 -Wall -Wextra -fno-exceptions -c -o "$W/start.o" "$ROOT/src/ps5rt_start.cpp"
objcopy --redefine-sym _start=ps5rt_test_entry --redefine-sym _init=ps5rt_test_init \
	--redefine-sym _fini=ps5rt_test_fini --redefine-sym main=test_main --redefine-sym _init_env=test_init_env \
	--redefine-sym __preinit_array_start=test_preinit_start --redefine-sym __preinit_array_end=test_preinit_end \
	--redefine-sym __init_array_start=test_init_start --redefine-sym __init_array_end=test_init_end \
	--redefine-sym __fini_array_start=test_fini_start --redefine-sym __fini_array_end=test_fini_end \
	"$W/start.o" "$W/start-renamed.o"
$CXX -std=c++20 -O1 -Wall -Wextra -o "$W/test_start" "$ROOT/tests/test_start.cpp" "$W/start-renamed.o"
"$W/test_start" || rc=1

# operator new / delete
$CXX -std=c++20 -O1 -Wall -Wextra -o "$W/test_new" "$ROOT/tests/test_new.cpp" "$ROOT/src/ps5rt_new.cpp" \
	-Wno-alloc-size-larger-than
"$W/test_new" || rc=1

# netdb on a fake resolver: the real file, its functions renamed t_* (glibc has its own), under AddressSanitizer
$CXX -std=c++20 -O1 -g -Wall -Wextra -fno-exceptions -fsanitize=address -c -o "$W/netdb.o" "$ROOT/src/ps5rt_netdb.cpp"
objcopy $(for f in getaddrinfo freeaddrinfo getnameinfo gai_strerror getprotobyname getprotobynumber getservbyname \
	getservbyport; do printf -- '--redefine-sym %s=t_%s ' $f $f; done) "$W/netdb.o" "$W/netdb-renamed.o"
$CXX -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address -o "$W/test_netdb" "$ROOT/tests/test_netdb.cpp" "$W/netdb-renamed.o"
ASAN_OPTIONS=detect_leaks=1 "$W/test_netdb" || rc=1

# libc.prx: written twice (the same bytes), then checked
python3 -B "$ROOT/tools/mkprx.py" --manifest "$ROOT/libc/exports.txt" --out "$W/libc.raw.elf"
python3 -B "$ROOT/tools/mkprx.py" --manifest "$ROOT/libc/exports.txt" --out "$W/libc2.raw.elf" >/dev/null
if cmp -s "$W/libc.raw.elf" "$W/libc2.raw.elf"; then echo "ok   mkprx.py: deterministic"; else echo "FAIL mkprx.py: not deterministic"; rc=1; fi
python3 -B "$ROOT/tests/check_prx.py" "$W/libc.raw.elf" "$ROOT/libc/exports.txt" || rc=1

[ $rc = 0 ] && echo "host_test: ok" || echo "host_test: FAILED"
exit $rc
