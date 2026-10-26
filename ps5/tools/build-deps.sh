#!/bin/bash
# Media Player Classic PS5: the libraries the player links, built for the PS5 and for the host tests.
#
#   tools/build-deps.sh [ps5|host|all]      (default all) -> build/deps/prefix-{ps5,host}/{include,lib}
#   tools/build-deps.sh [ps5-subs|host-subs|subs]   only the subtitle renderer's libraries (FreeType, FriBidi,
#                                                    HarfBuzz, libass) into the same prefixes, FFmpeg left as it is
#   tools/build-deps.sh [ps5-disc|host-disc|disc]   1.4: only the disc libraries (libudfread, libbluray, libdvdread,
#                                                    libdvdnav), then FFmpeg again (its DVD-Video demuxer needs them)
#
# Sources:
#   FFmpeg n7.1.1        https://github.com/FFmpeg/FFmpeg (GPLv3 build: --enable-gpl --enable-version3)
#   dav1d 1.5.1          https://github.com/videolan/dav1d (AV1, BSD-2-Clause)
#   libsmb2              https://github.com/sahlberg/libsmb2 (SMB2/3, LGPL-2.1), commit below
#   Mbed TLS 3.6.7       third_party/mbedtls (Apache-2.0), with mpc_mbedtls_config.h (FFmpeg's https://)
#   zlib                 third_party/zlib
#   FreeType 2.14.3      https://github.com/freetype/freetype (FTL or GPLv2; used under the FTL, GPLv3-compatible)
#   FriBidi 1.0.16       https://github.com/fribidi/fribidi (LGPL-2.1)
#   HarfBuzz 14.5.1      https://github.com/harfbuzz/harfbuzz (MIT ("Old MIT")): the core only (no glib, ICU, cairo)
#   libass 0.17.5        https://github.com/libass/libass (ISC): ASS/SSA subtitles; no fontconfig (no font provider
#                        on the console: the fonts are the app's own, the MKV's attachments and /data/mpcps5/fonts)
#   (1.4, disc drives -- VideoLAN's release tarballs, https://download.videolan.org/pub/videolan/<name>/, SHA-256 below)
#   libudfread 1.2.0     UDF file systems (LGPL-2.1-or-later)
#   libbluray 1.5.0      Blu-ray (LGPL-2.1-or-later): no BD-J (no Java), no libxml2 / fontconfig / FreeType;
#                        tools/patches/libbluray-1.5.0-mpcps5.patch: it loads nothing at run time (no libaacs,
#                        libbdplus, libmmbd, Java VM: dl_dlopen() finds nothing)
#   libdvdread 7.1.1     DVD-Video structures (GPL-2.0-or-later): built without libdvdcss;
#                        tools/patches/libdvdread-7.1.1-mpcps5.patch: no decryption of any kind (no libdvdcss, linked
#                        or loaded), a folder read as files (no device looked up behind a mount point: that was for CSS),
#                        DVDSetFilesystemFactory() (the player reads the drives through its own sector reader)
#   libdvdnav 7.0.0      DVD navigation (GPL-2.0-or-later), for FFmpeg's dvdvideo demuxer
#   FFmpeg's dvdvideo demuxer: tools/patches/ffmpeg-n7.1.1-dvdvideo-seek-after-end.patch (a seek after the title's end
#   was read goes back into the title first)
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
FREETYPE_TAG=VER-2-14-3
FRIBIDI_TAG=v1.0.16
HARFBUZZ_TAG=14.5.1
LIBASS_TAG=0.17.5
UDFREAD_VER=1.2.0
UDFREAD_SHA=bb477cbd4cfbfc7787d9d05b71ee5e70430f5cfebf1297497f7e83547958050f
BLURAY_VER=1.5.0
BLURAY_SHA=f676408e91a5d321abf8b8d4dfdae36205c297dab5c54c3ec519639025f474a2
DVDREAD_VER=7.1.1
DVDREAD_SHA=a0d47876548bec806774bbf8dbf20bb19ba139464383156b32eb8e59915b90a9
DVDNAV_VER=7.0.0
DVDNAV_SHA=a2a18f5ad36d133c74bf9106b6445806fa253b09141a46392550394b647b221e
mkdir -p "$SRC"

fetch() { # dir url ref
	if [ ! -d "$SRC/$1" ]; then
		git clone -q "$2" "$SRC/$1"
	fi
	git -C "$SRC/$1" -c advice.detachedHead=false checkout -q "$3"
}
# a release tag, shallow (HarfBuzz's whole history is big)
fetch_tag() { # dir url tag
	if [ ! -d "$SRC/$1" ] || [ "$(git -C "$SRC/$1" describe --tags --exact-match 2>/dev/null)" != "$3" ]; then
		rm -rf "$SRC/$1"
		git -c advice.detachedHead=false clone -q --depth 1 -b "$3" "$2" "$SRC/$1"
	fi
}
fetch_subs() {
	fetch_tag freetype https://github.com/freetype/freetype.git "$FREETYPE_TAG"
	fetch_tag fribidi https://github.com/fribidi/fribidi.git "$FRIBIDI_TAG"
	fetch_tag harfbuzz https://github.com/harfbuzz/harfbuzz.git "$HARFBUZZ_TAG"
	fetch_tag libass https://github.com/libass/libass.git "$LIBASS_TAG"
	# the port's patch: with no font provider (no fontconfig on the console), a character the chosen font hasn't got
	# comes from any loaded font that has it (tools/patches/libass-0.17.5-coverage-fallback.patch)
	local patch=$PWD/tools/patches/libass-0.17.5-coverage-fallback.patch
	if ! git -C "$SRC/libass" apply --reverse --check "$patch" 2>/dev/null; then
		git -C "$SRC/libass" apply "$patch"
	fi
}
# a release tarball, checked, unpacked fresh and patched (tools/patches/<name>-<version>-mpcps5.patch)
fetch_tarball() { # name version sha256
	local t="$SRC/$1-$2.tar.xz"
	if [ ! -f "$t" ]; then
		curl -fL -o "$t.part" "https://download.videolan.org/pub/videolan/$1/$2/$1-$2.tar.xz"
		mv "$t.part" "$t"
	fi
	echo "$3  $t" | sha256sum --check --quiet
	rm -rf "$SRC/$1-$2"
	tar xf "$t" -C "$SRC"
	if [ -f "$PWD/tools/patches/$1-$2-mpcps5.patch" ]; then
		git -C "$SRC/$1-$2" apply "$PWD/tools/patches/$1-$2-mpcps5.patch"
	fi
}
fetch_disc() {
	fetch_tarball libudfread "$UDFREAD_VER" "$UDFREAD_SHA"
	fetch_tarball libbluray "$BLURAY_VER" "$BLURAY_SHA"
	fetch_tarball libdvdread "$DVDREAD_VER" "$DVDREAD_SHA"
	fetch_tarball libdvdnav "$DVDNAV_VER" "$DVDNAV_SHA"
}
case "$WHAT" in
	*subs) ;;
	*disc)
		fetch ffmpeg https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_TAG"
		;;
	*)
		fetch ffmpeg https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_TAG"
		fetch dav1d https://github.com/videolan/dav1d.git "$DAV1D_TAG"
		fetch libsmb2 https://github.com/sahlberg/libsmb2.git "$LIBSMB2_COMMIT"
		;;
esac
case "$WHAT" in
	*subs) fetch_subs ;;
	*disc) fetch_disc ;;
	*) fetch_subs && fetch_disc ;;
esac
# FFmpeg's DVD-Video demuxer: a seek after the end (tools/patches/ffmpeg-n7.1.1-dvdvideo-seek-after-end.patch)
ff_patch=$PWD/tools/patches/ffmpeg-n7.1.1-dvdvideo-seek-after-end.patch
if [ -d "$SRC/ffmpeg" ] && ! git -C "$SRC/ffmpeg" apply --reverse --check "$ff_patch" 2>/dev/null; then
	git -C "$SRC/ffmpeg" apply "$ff_patch"
fi

FF_OPTS="--enable-gpl --enable-version3 --disable-programs --disable-doc --disable-avdevice --disable-postproc
	--disable-iconv --disable-vaapi --disable-vdpau --disable-xlib --disable-sdl2 --disable-bzlib --disable-lzma --disable-libdrm
	--disable-debug --enable-network --enable-zlib --enable-mbedtls --enable-libdav1d
	--disable-encoders --enable-encoder=png --disable-muxers --disable-filters
	--enable-filter=bwdif,yadif,scale,format,aformat,aresample,atempo,volume,buffer,buffersink,abuffer,abuffersink,null,anull,copy,hflip,vflip,transpose,crop,pad,setsar,dynaudnorm,equalizer,superequalizer,deband,unsharp,hqdn3d,eq
	--disable-indevs --disable-outdevs --disable-devices --enable-libdvdnav --enable-libdvdread"

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

	build_disc "$t"
	build_ffmpeg "$t"
	build_subs "$t"
	echo "== $t: done -> $P"
}

# FFmpeg, into prefix-$t (its own build folder, build/deps/build-$t-ffmpeg)
build_ffmpeg() { # ps5|host
	local t=$1 P=$DEPS/prefix-$1 B=$DEPS/build-$1-ffmpeg CC AR CFLAGS
	rm -rf "$B" && mkdir -p "$B"
	if [ "$t" = ps5 ]; then
		CC=$PS5_PAYLOAD_SDK/bin/prospero-clang AR=$PS5_PAYLOAD_SDK/bin/prospero-ar CFLAGS="-O2 -fPIC -march=znver2"
	else
		CC=clang AR=ar CFLAGS="-O2 -fPIC"
	fi
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
}

# The disc libraries (1.4): libudfread, libbluray, libdvdread, libdvdnav; static, with meson, minimal (their own build
# folders, build/deps/build-$t-disc)
build_disc() { # ps5|host
	local t=$1 P=$DEPS/prefix-$1 B=$DEPS/build-$1-disc CFLAGS MESON cross=()
	rm -rf "$B" && mkdir -p "$B" "$P/lib/pkgconfig" "$P/include"
	if [ "$t" = ps5 ]; then
		CFLAGS="-O2 -fPIC -march=znver2"
		MESON=$PS5_PAYLOAD_SDK/bin/prospero-meson
		printf "[binaries]\npkg-config = 'pkg-config'\n[properties]\npkg_config_libdir = ['%s']\n" "$P/lib/pkgconfig" >"$B/prefix.ini"
		cross=(--cross-file="$B/prefix.ini")
		export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig
	else
		CFLAGS="-O2 -fPIC" MESON=meson
		export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig CC=clang CXX=clang++
	fi
	local common=(--default-library=static --buildtype=release -Db_pie=true --prefix="$P" --libdir=lib --wrap-mode=nofallback)
	disc() { # name src extra-cflags options...
		local n=$1 s=$2 x=$3
		shift 3
		echo "== $t: $n"
		CFLAGS="$CFLAGS $x" $MESON setup "$s" "$B/$n" "${cross[@]}" "${common[@]}" "$@" >"$B/$n.log"
		ninja -C "$B/$n" -j"$JOBS" install >>"$B/$n.log"
	}
	disc libudfread "$SRC/libudfread-$UDFREAD_VER" "" -Denable_examples=false
	# the SDK's FreeBSD headers declare gettimeofday() for XSI only (libbluray asks for POSIX 2008)
	local xsi=""
	[ "$t" = ps5 ] && xsi=-D_XOPEN_SOURCE=700
	disc libbluray "$SRC/libbluray-$BLURAY_VER" "$xsi" -Denable_tools=false -Denable_devtools=false \
		-Denable_examples=false -Denable_docs=false -Dbdj_jar=disabled -Dfontconfig=disabled -Dfreetype=disabled \
		-Dlibxml2=disabled
	disc libdvdread "$SRC/libdvdread-$DVDREAD_VER" "" -Denable_docs=false -Dlibdvdcss=disabled
	disc libdvdnav "$SRC/libdvdnav-$DVDNAV_VER" "" -Denable_docs=false -Denable_examples=false
	unset PKG_CONFIG_LIBDIR CC CXX
	echo "== $t: disc libraries -> $P"
}

# The subtitle renderer: FreeType, FriBidi, HarfBuzz (core only), libass; static, with meson. Only their own
# build folders are made again (build/deps/build-$t-subs), so this runs without touching FFmpeg.
build_subs() { # ps5|host
	local t=$1 P=$DEPS/prefix-$1 B=$DEPS/build-$1-subs CFLAGS MESON cross=()
	rm -rf "$B" && mkdir -p "$B" "$P/lib/pkgconfig" "$P/include"
	if [ "$t" = ps5 ]; then
		CFLAGS="-O2 -fPIC -march=znver2"
		MESON=$PS5_PAYLOAD_SDK/bin/prospero-meson
		# the SDK's pkg-config looks in its own sysroot: ours looks in the prefix
		printf "[binaries]\npkg-config = 'pkg-config'\n[properties]\npkg_config_libdir = ['%s']\n" "$P/lib/pkgconfig" >"$B/prefix.ini"
		cross=(--cross-file="$B/prefix.ini")
		export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig
	else
		CFLAGS="-O2 -fPIC" MESON=meson
		export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig CC=clang CXX=clang++
	fi
	local common=(--default-library=static --buildtype=release -Db_pie=true --prefix="$P" --libdir=lib --wrap-mode=nofallback)
	sub() { # name src options...
		local n=$1 s=$2
		shift 2
		echo "== $t: $n"
		CFLAGS="$CFLAGS" CXXFLAGS="$CFLAGS" $MESON setup "$s" "$B/$n" "${cross[@]}" "${common[@]}" "$@" >"$B/$n.log"
		ninja -C "$B/$n" -j"$JOBS" install >>"$B/$n.log"
	}
	sub freetype "$SRC/freetype" -Dbrotli=disabled -Dbzip2=disabled -Dharfbuzz=disabled -Dpng=disabled -Dzlib=disabled \
		-Dtests=disabled
	sub fribidi "$SRC/fribidi" -Ddocs=false -Dbin=false -Dtests=false
	sub harfbuzz "$SRC/harfbuzz" -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled -Dpng=disabled \
		-Dzlib=disabled -Dicu=disabled -Dfreetype=disabled -Draster=disabled -Dvector=disabled -Dgpu=disabled \
		-Dsubset=disabled -Dtests=disabled -Dintrospection=disabled -Ddocs=disabled -Dutilities=disabled \
		-Dbenchmark=disabled
	sub libass "$SRC/libass" -Dfontconfig=disabled -Ddirectwrite=disabled -Dcoretext=disabled -Dlibunibreak=disabled \
		-Drequire-system-font-provider=false -Dasm=enabled -Dtest=disabled -Dcompare=disabled -Dprofile=disabled \
		-Dfuzz=disabled -Dcheckasm=disabled
	unset PKG_CONFIG_LIBDIR CC CXX
	echo "== $t: subtitle libraries -> $P"
}

case "$WHAT" in
	ps5) build ps5 ;;
	host) build host ;;
	all) build ps5 && build host ;;
	ps5-subs) build_subs ps5 ;;
	ps5-disc) build_disc ps5 && build_ffmpeg ps5 ;;
	host-disc) build_disc host && build_ffmpeg host ;;
	disc) build_disc ps5 && build_ffmpeg ps5 && build_disc host && build_ffmpeg host ;;
	host-subs) build_subs host ;;
	subs) build_subs ps5 && build_subs host ;;
	*) echo "usage: $0 [ps5|host|all|ps5-subs|host-subs|subs|ps5-disc|host-disc|disc]"; exit 1 ;;
esac
