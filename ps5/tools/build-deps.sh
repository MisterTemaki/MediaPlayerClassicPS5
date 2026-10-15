#!/bin/bash
# Media Player Classic PS5: the libraries the player links, built for the PS5 and for the host tests.
#
#   tools/build-deps.sh [ps5|host|all]      (default all) -> build/deps/prefix-{ps5,host}/{include,lib}
#
# Sources:
#   FFmpeg n7.1.1        https://github.com/FFmpeg/FFmpeg (GPLv3 build: --enable-gpl --enable-version3)
#   dav1d 1.5.1          https://github.com/videolan/dav1d (AV1, BSD-2-Clause)
#   libsmb2              https://github.com/sahlberg/libsmb2 (SMB2/3, LGPL-2.1), commit below
#   Mbed TLS 3.6.7       third_party/mbedtls (Apache-2.0), with mpc_mbedtls_config.h (FFmpeg's https://)
#   zlib                 third_party/zlib
# Fetched into build/deps/src at the pinned versions; built with the ps5-payload-dev SDK's prospero-* tools
# (clang, meson, cmake) for the console and with clang for the host. Needs git, nasm, meson, ninja, cmake.
#
# FFmpeg is cut down to what a player needs: every decoder, demuxer, parser and protocol; no encoders but PNG
# (File > Save Image), no muxers, a few filters (bwdif/yadif deinterlacing, atempo for the playback rate,
# aresample/aformat for the downmix, the equalizers).
set -euo pipefail
cd "$(dirname "$0")/.."
PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
WHAT=${1:-all}
DEPS=$PWD/build/deps
SRC=$DEPS/src
JOBS=${JOBS:-$(nproc)}
FFMPEG_TAG=n7.1.1
DAV1D_TAG=1.5.1
LIBSMB2_COMMIT=fc710a3ebd58a3c15d0ef24322f748e38a3d3a90
mkdir -p "$SRC"

fetch() { # dir url ref
	if [ ! -d "$SRC/$1" ]; then
		git clone -q "$2" "$SRC/$1"
	fi
	git -C "$SRC/$1" -c advice.detachedHead=false checkout -q "$3"
}
fetch ffmpeg https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_TAG"
fetch dav1d https://github.com/videolan/dav1d.git "$DAV1D_TAG"
fetch libsmb2 https://github.com/sahlberg/libsmb2.git "$LIBSMB2_COMMIT"

FF_OPTS="--enable-gpl --enable-version3 --disable-programs --disable-doc --disable-avdevice --disable-postproc
	--disable-iconv --disable-vaapi --disable-vdpau --disable-xlib --disable-sdl2 --disable-bzlib --disable-lzma --disable-libdrm
	--disable-debug --enable-network --enable-zlib --enable-mbedtls --enable-libdav1d
	--disable-encoders --enable-encoder=png --disable-muxers --disable-filters
	--enable-filter=bwdif,yadif,scale,format,aformat,aresample,atempo,volume,buffer,buffersink,abuffer,abuffersink,null,anull,copy,hflip,vflip,transpose,crop,pad,setsar,dynaudnorm,equalizer,superequalizer,deband,unsharp,hqdn3d,eq
	--disable-indevs --disable-outdevs --disable-devices"

build() { # ps5|host
	local t=$1 P=$DEPS/prefix-$1 B=$DEPS/build-$1 CC AR CFLAGS MESON CMAKE
	rm -rf "$B" && mkdir -p "$B" "$P/lib" "$P/include"
	if [ "$t" = ps5 ]; then
		CC=$PS5_PAYLOAD_SDK/bin/prospero-clang AR=$PS5_PAYLOAD_SDK/bin/prospero-ar CFLAGS="-O2 -fPIC -march=znver2"
		MESON=$PS5_PAYLOAD_SDK/bin/prospero-meson CMAKE=$PS5_PAYLOAD_SDK/bin/prospero-cmake
	else
		CC=clang AR=ar CFLAGS="-O2 -fPIC" MESON=meson CMAKE=cmake
	fi
	echo "== $t: zlib, Mbed TLS"
	mkdir -p "$B/z" "$B/m"
	for f in third_party/zlib/*.c; do $CC $CFLAGS -DZ_HAVE_UNISTD_H -c "$f" -o "$B/z/$(basename "$f" .c).o"; done
	rm -f "$P/lib/libz.a" && $AR rcs "$P/lib/libz.a" "$B"/z/*.o
	cp third_party/zlib/zlib.h third_party/zlib/zconf.h "$P/include/"
	for f in third_party/mbedtls/library/*.c third_party/mbedtls/mpc_entropy.c; do
		$CC $CFLAGS -Ithird_party/mbedtls/include -Ithird_party/mbedtls '-DMBEDTLS_USER_CONFIG_FILE="mpc_mbedtls_config.h"' \
			-c "$f" -o "$B/m/$(basename "$f" .c).o"
	done
	rm -f "$P"/lib/libmbed*.a
	$AR rcs "$P/lib/libmbedtls.a" "$B"/m/ssl*.o
	$AR rcs "$P/lib/libmbedx509.a" "$B"/m/x509*.o "$B"/m/pkcs7*.o
	ls "$B"/m/*.o | grep -v "/ssl\|/x509\|/pkcs7" | xargs $AR rcs "$P/lib/libmbedcrypto.a"
	rm -rf "$P/include/mbedtls" "$P/include/psa"
	cp -r third_party/mbedtls/include/mbedtls third_party/mbedtls/include/psa "$P/include/"
	# the user config, appended to the installed mbedtls_config.h (FFmpeg includes it without the define)
	grep -v "pragma once" third_party/mbedtls/mpc_mbedtls_config.h >>"$P/include/mbedtls/mbedtls_config.h"

	echo "== $t: dav1d"
	CFLAGS="$CFLAGS" $MESON setup "$SRC/dav1d" "$B/dav1d" --default-library=static --buildtype=release -Denable_tools=false \
		-Denable_tests=false -Denable_examples=false -Db_pie=true --prefix="$P" --libdir=lib >"$B/dav1d.log"
	ninja -C "$B/dav1d" install >>"$B/dav1d.log"

	echo "== $t: libsmb2"
	$CMAKE -S "$SRC/libsmb2" -B "$B/smb2" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF \
		-DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_C_FLAGS="$CFLAGS" \
		-DCMAKE_INSTALL_PREFIX="$P" >"$B/smb2.log"
	make -C "$B/smb2" -j"$JOBS" install >>"$B/smb2.log"

	echo "== $t: FFmpeg $FFMPEG_TAG"
	mkdir -p "$B/ffmpeg"
	local cross=""
	if [ "$t" = ps5 ]; then
		cross="--enable-cross-compile --target-os=freebsd --arch=x86_64 --cc=$CC --cxx=$PS5_PAYLOAD_SDK/bin/prospero-clang++
			--ar=$AR --ranlib=$PS5_PAYLOAD_SDK/bin/prospero-ranlib --nm=$PS5_PAYLOAD_SDK/bin/prospero-nm
			--strip=$PS5_PAYLOAD_SDK/bin/prospero-strip --disable-stripping"
	else
		cross="--cc=clang --cxx=clang++"
	fi
	(cd "$B/ffmpeg" && PKG_CONFIG_LIBDIR=$P/lib/pkgconfig "$SRC/ffmpeg/configure" --prefix="$P" $cross --x86asmexe=nasm \
		--pkg-config=pkg-config --pkg-config-flags=--static --enable-pic --enable-static --disable-shared $FF_OPTS \
		--extra-cflags="$CFLAGS -I$P/include" --extra-ldflags="-L$P/lib" >"$B/ffmpeg.log")
	make -C "$B/ffmpeg" -j"$JOBS" >>"$B/ffmpeg.log" 2>&1
	make -C "$B/ffmpeg" install >>"$B/ffmpeg.log" 2>&1
	echo "== $t: done -> $P"
}

case "$WHAT" in
	ps5) build ps5 ;;
	host) build host ;;
	all) build ps5 && build host ;;
	*) echo "usage: $0 [ps5|host|all]"; exit 1 ;;
esac
