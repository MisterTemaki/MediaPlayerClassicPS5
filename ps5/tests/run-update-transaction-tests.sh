#!/bin/sh
# SPDX-License-Identifier: MIT
# Needs only a Linux C/C++ compiler and Python 3. All writes stay in mktemp's directory.
set -eu
P=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SRC=${UPDATE_SOURCE_ROOT:-$P}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT HUP INT TERM
python3 - "$W" <<'PY'
import hashlib, io, pathlib, sys, warnings, zipfile
root = pathlib.Path(sys.argv[1])
base = [('eboot.bin', b'new eboot'), ('sce_sys/param.json', b'{"titleId":"PPSA99015"}')]
for name, extra in [('valid', []), ('reserved', [('update.journal', b'foreign journal')]),
                    ('newline', [('line\nbreak.bin', b'x')]), ('nul', [('bad!suffix', b'x')]),
                    ('traversal', [('../escape', b'x')]), ('duplicate', [('eboot.bin', b'new eboot')]),
                    ('staging', [('eboot.bin.new', b'x')]), ('backup', [('eboot.bin.old.100-1', b'x')]),
                    ('unlisted', [('extra.bin', b'x')]), ('self-manifest', [])]:
    files = base + extra
    listed = base if name == 'unlisted' else files
    manifest = ''.join(hashlib.sha256(b).hexdigest() + ' *' + n + '\n' for n, b in listed).encode()
    if name == 'self-manifest':
        manifest += b'0' * 64 + b' *manifest.sha256\n'
    buf = io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', UserWarning)
        with zipfile.ZipFile(buf, 'w', zipfile.ZIP_STORED) as z:
            for n, b in files:
                z.writestr('PPSA99015/' + n, b)
            z.writestr('PPSA99015/manifest.sha256', manifest)
    data = buf.getvalue()
    if name == 'nul':
        data = data.replace(b'bad!suffix', b'bad\0suffix')
    (root / (name + '.zip')).write_bytes(data)
PY
SAN=${UPDATE_TEST_SAN:--fsanitize=address,undefined -fno-omit-frame-pointer}
${HOST_CC:-cc} -std=c99 -O0 -g $SAN -I"$SRC/third_party/monocypher" -c \
    "$SRC/third_party/monocypher/monocypher.c" -o "$W/mono.o"
${HOST_CC:-cc} -std=c99 -O0 -g $SAN -I"$SRC/third_party/monocypher" -c \
    "$SRC/third_party/monocypher/monocypher-ed25519.c" -o "$W/ed.o"
${HOST_CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror $SAN \
    -I"$SRC/coreorbis/include-orbis" -I"$SRC/coreorbis/orbis-shims" -I"$SRC/third_party/monocypher" \
    "$P/tests/update_transaction_test.cpp" "$SRC/coreorbis/orbis-shims/ProsperoUpdate.cpp" \
    "$W/mono.o" "$W/ed.o" -Wl,--wrap=unlink -Wl,--wrap=fsync -pthread -o "$W/test-update"
ASAN_OPTIONS=detect_leaks=1 "$W/test-update" "$W/app-tests" "$W"
