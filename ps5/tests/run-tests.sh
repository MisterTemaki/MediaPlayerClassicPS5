#!/bin/bash
# Media Player Classic PS5 host tests: the real port code (main-boot, the PS5 layer, the player: FFmpeg, libsmb2,
# the MPC-HC window; the installer and the helper) on Linux, with the PS5 calls faked by host/sce_host.cpp. A
# scripted pad (and a script of commands, MPCPS5_HOST_SCRIPT) drives the app; flips are de-tiled to PPM and
# checked. The media are made by tests/make_media.sh with the host's ffmpeg.
set -u
cd "$(dirname "$0")/.."
BIN=${MPC_HOST_BIN:-$PWD/build/host/mpc-ps5-host} # (another build: e.g. the last release's, for a fail-before)
INSTALLER=${MPC_HOST_INSTALLER:-$PWD/build/host/mpc-ps5-installer}
HELPER=${MPC_HOST_HELPER:-$PWD/build/host/mpc-ps5-helper} # (1.5.4: other builds of these too, for a fail-before)
# ports nobody listens on, so the app's ordinary runs find no helper and no ELF loader
export MPCPS5_HELPER_PORT=$((20000 + RANDOM % 10000)) MPCPS5_ELFLDR_PORT=$((30000 + RANDOM % 10000)) MPCPS5_JB_NO_OTHERS=1
# debug logs are off by default (1.1.1); the tests read boot.log, so their default is on (group 18 checks the real one)
export MPCPS5_LOGS_DEFAULT=1
CHECK="python3 $PWD/tests/check_ppm.py"
REGION="python3 $PWD/tests/check_region.py"
WORK=${WORK:-$(mktemp -d)}
ONLY=${ONLY:-}
PASS=0
FAIL=0
MEDIA=$WORK/media
[ -f "$MEDIA/nopts.vob" ] || bash tests/make_media.sh "$MEDIA" >/dev/null

# pad bits
CROSS=4000; CIRCLE=2000; TRIANGLE=1000; SQUARE=8000; UP=10; DOWN=40; LEFT=80; RIGHT=20; L1=400; R1=800; L2=100; R2=200
L3=2; R3=4; OPTIONS=8

ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
# the version being tested (Makefile, app/sce_sys/param.json)
VER=$(sed -n 's/^VERSION ?= //p' Makefile)
CVER=$(sed -n 's/.*"contentVersion": "\([0-9.]*\)".*/\1/p' app/sce_sys/param.json)
want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }

newroot() {
	local t=$WORK/$1
	rm -rf "$t" && mkdir -p "$t/root/media" "$t/dump"
	echo "$t"
}

# run dir pad script dumps [args...]: realtime flips (60 Hz), the player's clock against them
run() {
	local t=$1 pad=$2 script=$3 dumps=$4
	shift 4
	MPCPS5_HOST_OFFLINE=${OFFLINE-1} MPCPS5_HOST_REALTIME=1 MPCPS5_HOST_SCRIPT="$script" \
	MPCPS5_PS5_ROOT=$t/root MPCPS5_PS5_HOMEBREW=$t/homebrew MPCPS5_HOST_DUMP_DIR=$t/dump MPCPS5_HOST_DUMP=$dumps \
		MPCPS5_HOST_MAX_FLIPS=${MAXFLIPS-3000} MPCPS5_HOST_PAD=$pad ASAN_OPTIONS=detect_leaks=0 timeout 180 "$BIN" "$@" >"$t/out.txt" 2>&1
	echo $?
}
nosan() { expect "! grep -q 'runtime error\|AddressSanitizer' $1/out.txt" "no sanitizer reports"; }
state() { grep "\[test\] state" "$1/root/logs/boot.log" | sed -n "${2}p"; }
# the n-th state line's field: st pos dur video audio sub picture
field() { state "$1" "$2" | sed -E "s/.* state ([0-9]+) pos ([0-9.]+) dur ([0-9.]+) video (-?[0-9]+) audio (-?[0-9]+) sub (-?[0-9]+) picture ([0-9]+).*/\\$3/"; }
# the first boot.log line number matching $2 (test10 review: defined here, for any group)
line() { grep -n -- "$2" "$1/root/logs/boot.log" | head -1 | cut -d: -f1; }
near() { python3 -c "import sys; sys.exit(0 if abs(float('$1') - float('$2')) <= float('$3') else 1)"; }
# 1.5.2: a test server's port taking connections (up to 20 s: a loaded machine starts Python slowly; a fixed sleep
# made group 6's SMB server late now and then)
listening() { for i in $(seq 1 200); do python3 -c "import socket, sys; s = socket.socket(); sys.exit(s.connect_ex(('127.0.0.1', $1)))" 2>/dev/null && return 0; sleep 0.1; done; return 1; }

if want 1; then
echo "== 1. the window without a file: MPC-HC's chrome, the logo; File > Exit from the keyboard menu"
T=$(newroot t1)
# Options opens the File menu (keyboard), Up wraps to Exit, Cross runs it
rc=$(run "$T" "0:0;60:$OPTIONS;62:0;70:$UP;72:0;80:$CROSS;82:0" "" "40,75")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'Media Player Classic PS5 $(sed -n 's/^VERSION ?= //p' Makefile), FFmpeg' $T/root/logs/boot.log" "boot log names the player and FFmpeg"
expect "grep -q '\[boot\] exceptions: ok (our throw caught, libc++.s std::invalid_argument caught, dynamic_cast right, operator new.s std::bad_alloc not tried on the host)' $T/root/logs/boot.log" "1.5.2: the start checks C++ exceptions and dynamic_cast (debug logs on): ok"
expect "$CHECK $T/dump/flip00040.ppm 900 10 255 255 255 2 >/dev/null" "the white title bar"
expect "$CHECK $T/dump/flip00040.ppm 960 1062 0 0 0 2 >/dev/null" "the black status bar"
expect "$CHECK $T/dump/flip00040.ppm 900 1025 240 240 240 2 >/dev/null" "the toolbar in COLOR_BTNFACE"
expect "$CHECK $T/dump/flip00040.ppm 995 480 25 95 175 70 >/dev/null" "the logo's blue bar in the middle"
expect "$CHECK $T/dump/flip00075.ppm 30 584 145 201 247 6 >/dev/null" "the File menu open, Exit highlighted (Windows 10's menu blue)"
expect "grep -q 'Volume=100' $T/root/mpc-hc.ini" "settings written to mpc-hc.ini"
nosan "$T"
fi

if want 2; then
echo "== 2. a video: picture, status, pause and play from the pad, seek, the subtitles beside it (ANSI)"
T=$(newroot t2)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
printf '1\r\n00:00:00,500 --> 00:00:05,000\r\nOl\xe1, legenda!\r\n' >"$T/root/media/movie.srt"
rc=$(run "$T" "0:0;100:$CROSS;102:0;160:$CROSS;162:0" "60:state;130:state;200:state;205:cmd pause;210:seek 4;260:state;270:cmd close;280:state;290:quit" \
	"90,140" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'opened .*movie.mp4 .* video #0 h264 640x360, audio #1 aac' $T/root/logs/boot.log" "opened: H.264 + AAC"
expect "[ \"\$(field $T 1 1)\" = 2 ] && [ \"\$(field $T 1 7)\" = 1 ]" "playing, a picture shown"
expect "$CHECK $T/dump/flip00090.ppm 960 400 red >/dev/null" "the picture is red"
expect "grep -q 'subtitles loaded: .*movie.srt' $T/root/logs/boot.log && [ \"\$(field $T 1 6)\" = 1000 ]" "movie.srt loaded beside it and chosen"
expect "$REGION $T/dump/flip00090.ppm 500 700 1420 960 255 255 255 30 2000 >/dev/null" "its line drawn (white text at the bottom)"
expect "$CHECK $T/dump/flip00090.ppm 960 1062 0 0 0 2 >/dev/null" "the status bar"
expect "[ \"\$(field $T 2 1)\" = 3 ]" "Cross (no pointer): paused"
P2=$(field $T 2 2); P3=$(field $T 3 2)
expect "[ \"\$(field $T 3 1)\" = 2 ]" "Cross again: playing"
expect "near $(field $T 4 2) 4.0 0.1" "paused, seek to 4 s: the picture at $(field $T 4 2) s"
expect "[ \"\$(field $T 5 1)\" = 0 ]" "File > Close: closed"
expect "[ -n \"\$P2\" ] && [ -n \"\$P3\" ]" "positions reported ($P2, $P3)"
nosan "$T"
fi

if want 3; then
echo "== 3. chapters, tracks: the default audio and subtitles, R1/L1 by chapter, another audio track"
T=$(newroot t3)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
# R1 at 60 (chapter 2: green), R1 at 140 (chapter 3: blue), L1 at 220 (back to green). Each from a known position (a
# seek just before): Next skips a chapter less than a second ahead and Previous one less than 3 s back (MPC-HC), so
# where a slow run happens to be would change the chapter
rc=$(run "$T" "0:0;60:$R1;62:0;140:$R1;142:0;220:$L1;222:0" "40:state;55:seek 0.5;120:state;135:seek 3.5;200:state;215:seek 7;280:state;290:cmd 2001;330:state;340:quit" \
	"30,50,120,200,280" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(field $T 1 5)\" = 2 ]" "the default audio track (Portuguese, stream 2)"
expect "[ \"\$(field $T 1 6)\" = 3 ]" "the default subtitle track (ASS, stream 3)"
expect "$CHECK $T/dump/flip00030.ppm 960 300 red >/dev/null" "chapter 1: red"
expect "$CHECK $T/dump/flip00120.ppm 960 300 green >/dev/null" "R1: chapter 2, green"
expect "$CHECK $T/dump/flip00200.ppm 960 300 blue >/dev/null" "R1: chapter 3, blue"
expect "$CHECK $T/dump/flip00280.ppm 960 300 green >/dev/null" "L1: back to chapter 2, green"
expect "$REGION $T/dump/flip00050.ppm 500 700 1420 960 255 255 255 30 3000 >/dev/null" "the embedded subtitle drawn (white text at the bottom; it starts at 0.5 s: frame 50)"
expect "[ \"\$(field $T 5 5)\" = 1 ]" "Play > Audio Track > English: audio stream 1"
nosan "$T"
# a slow frame (half a second) while R1 is pressed briefly: one chapter, not a repeat (it counts as held only across
# polls, a poll adding 0.1 s at most)
T=$(newroot t3h)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0;60:$R1;63:0" "55:seek 0.5;61:stall 600;120:state;130:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && grep -q 'frame 61: stall 600' $T/root/logs/boot.log && python3 -c \"import sys; sys.exit(0 if 2.9 <= \$(field $T 1 2) < 5.9 else 1)\"" "R1 pressed during a hitch: chapter 2, not 3"
nosan "$T"
fi

if want 4; then
echo "== 4. audio: an MP3 shows its cover; HEVC Main 10; interlaced MPEG-2 deinterlaced; AV1 (dav1d)"
T=$(newroot t4)
cp "$MEDIA/song.mp3" "$MEDIA/hevc10.mkv" "$MEDIA/interlaced.ts" "$MEDIA/av1.mkv" "$T/root/media/"
rc=$(run "$T" "0:0" "40:state;60:open $T/root/media/hevc10.mkv;120:state;140:open $T/root/media/interlaced.ts;200:state;220:open $T/root/media/av1.mkv;270:state;280:quit" \
	"35,115,195,265" "$T/root/media/song.mp3")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(field $T 1 1)\" = 2 ] && [ \"\$(field $T 1 4)\" = -1 ]" "song.mp3 plays, no video track"
expect "$CHECK $T/dump/flip00035.ppm 960 500 blue >/dev/null" "its cover art (blue) in place of the picture"
expect "grep -q 'hevc 640x360' $T/root/logs/boot.log && $CHECK $T/dump/flip00115.ppm 960 400 255 0 255 60 >/dev/null" "HEVC Main 10: magenta"
expect "grep -q 'interlaced video: deinterlacing with bwdif' $T/root/logs/boot.log" "MPEG-2 interlaced: bwdif"
expect "$CHECK $T/dump/flip00195.ppm 960 400 green >/dev/null" "the interlaced picture: green"
expect "grep -q 'video #0 libdav1d 640x360' $T/root/logs/boot.log && $CHECK $T/dump/flip00265.ppm 960 400 255 255 0 60 >/dev/null" "AV1 through dav1d: yellow"
nosan "$T"
fi

if want 5; then
echo "== 5. the file position is remembered; a playlist goes on to the next file"
T=$(newroot t5)
cp "$MEDIA/chapters.mkv" "$MEDIA/red.mp4" "$MEDIA/list.m3u" "$T/root/media/"
rc=$(run "$T" "0:0" "30:seek 5.5;32:cmd pause;60:quit" "" "$T/root/media/chapters.mkv")
expect "grep -q 'FilePos0=.*chapters.mkv|5\\.[0-9]' $T/root/mpc-hc.ini" "the position (5.5 s, paused) saved in mpc-hc.ini"
rc=$(run "$T" "0:0" "10:cmd pause;30:state;40:quit" "" "$T/root/media/chapters.mkv")
expect "grep -q 'open .*chapters.mkv at 5\\.' $T/root/logs/boot.log" "opened again: resumes at 5.5 s"
expect "grep -q 'File1=.*chapters.mkv' $T/root/mpc-hc.ini" "Recent Files has it"
rc=$(run "$T" "0:0" "20:cmd quickopen;30:quit" "" "$T/root/media/list.m3u")
T2=$(newroot t5b)
cp "$MEDIA/chapters.mkv" "$MEDIA/red.mp4" "$MEDIA/list.m3u" "$T2/root/media/"
rc=$(MAXFLIPS=700 run "$T2" "0:0" "650:quit" "" "$T2/root/media/list.m3u")
expect "grep -q 'open .*/red.mp4' $T2/root/logs/boot.log && grep -q 'open .*/chapters.mkv' $T2/root/logs/boot.log" "list.m3u: red.mp4, then at its end chapters.mkv"
nosan "$T2"
fi

if want 6; then
echo "== 6. http:// (FFmpeg's HTTP) and smb:// (libsmb2, an SMB2 server)"
T=$(newroot t6)
HPORT=$((40000 + RANDOM % 10000))
python3 tests/http_range.py "$MEDIA" "$HPORT" >"$T/http.txt" 2>&1 &
HPID=$!
listening $HPORT
rc=$(run "$T" "0:0" "60:state;70:quit" "55" "http://127.0.0.1:$HPORT/red.mp4")
expect "[ \"\$(field $T 1 1)\" = 2 ]" "http://.../red.mp4 plays"
expect "$CHECK $T/dump/flip00055.ppm 960 400 red >/dev/null" "its picture: red"
kill $HPID 2>/dev/null
SPORT=$((40000 + RANDOM % 10000))
python3 - "$SPORT" "$MEDIA" >"$WORK/smb.txt" 2>&1 <<'PY' &
import sys
from impacket import smbserver
s = smbserver.SimpleSMBServer(listenAddress='127.0.0.1', listenPort=int(sys.argv[1]))
s.addShare('VIDEOS', sys.argv[2], 'test media')
s.setSMB2Support(True)
s.setSMBChallenge('')
s.start()
PY
SPID=$!
listening $SPORT
T6=$T
T=$(newroot t6b)
rc=$(run "$T" "0:0" "80:state;90:quit" "75" "smb://guest@127.0.0.1:$SPORT/VIDEOS/red.mp4")
expect "grep -q '\[smb\] opened smb://.*red.mp4' $T/root/logs/boot.log" "libsmb2 opened smb://127.0.0.1/VIDEOS/red.mp4"
expect "[ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00075.ppm 960 400 red >/dev/null" "it plays: red"
kill $SPID 2>/dev/null
nosan "$T"
fi

if want 7; then
echo "== 7. the pointer: the left stick moves it, Cross clicks (the File menu), Circle closes, Square: context menu"
T=$(newroot t7)
# up-left for 45 frames: the pointer stops at the corner; then it is put on "File" and Cross clicks there
rc=$(run "$T" "0:0,0,0;45:0;50:$CROSS;52:0;70:$CIRCLE;72:0;94:$SQUARE;96:0" "48:mouse 23,49;90:mouse 300,300;130:quit" "44,65,85,120")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00044.ppm 0 4 0 0 0 2 >/dev/null && $CHECK $T/dump/flip00044.ppm 300 4 255 255 255 2 >/dev/null" "the stick took the pointer to the top-left corner"
expect "$CHECK $T/dump/flip00065.ppm 100 85 242 242 242 4 >/dev/null" "Cross on File: its menu open"
expect "$CHECK $T/dump/flip00085.ppm 100 85 0 0 0 4 >/dev/null" "Circle: closed"
expect "$REGION $T/dump/flip00120.ppm 300 300 500 700 242 242 242 2 20000 >/dev/null" "Square: the context menu at the pointer"
nosan "$T"
fi

if want 8; then
echo "== 8. Go To and the on-screen keyboard: L3, Cross, Clear, 5, Done -> 00:05"
T=$(newroot t8)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
# L3: Go To (the Time box has the focus); Cross: the keyboard; Down x4 -> the last row; Right -> Clear; Cross;
# Up x4 -> the digits row ("1", under Clear); Right x4 -> 5; Cross; Options -> Done (the dialog goes to 5 s)
P="0:0;40:$L3;42:0;60:$CROSS;62:0;70:$DOWN;72:0;74:$DOWN;76:0;78:$DOWN;80:0;82:$DOWN;84:0;86:$RIGHT;88:0;90:$CROSS;92:0"
P="$P;100:$UP;102:0;104:$UP;106:0;108:$UP;110:0;112:$UP;114:0;116:$RIGHT;118:0;120:$RIGHT;122:0;124:$RIGHT;126:0;128:$RIGHT;130:0"
P="$P;140:$CROSS;142:0;150:$OPTIONS;152:0"
rc=$(run "$T" "$P" "20:cmd pause;180:state;190:quit" "65,145" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00065.ppm 960 1000 31 31 31 4 >/dev/null || $CHECK $T/dump/flip00065.ppm 300 900 31 31 31 4 >/dev/null" "the keyboard is up (osk.exe's dark keys)"
expect "near $(field $T 1 2) 5 0.3" "Go To 5: at $(field $T 1 2) s"
nosan "$T"
fi

if want 9; then
echo "== 9. the Options sheet; View toggles, the volume and mute saved in mpc-hc.ini"
T=$(newroot t9)
rc=$(run "$T" "0:0" "10:cmd options;20:quit" "18")
expect "$CHECK $T/dump/flip00018.ppm 960 600 240 240 240 2 >/dev/null" "the Options sheet drawn"
T=$(newroot t9b)
rc=$(run "$T" "0:0" "10:cmd info;11:cmd stats;12:cmd playlist;13:cmd voldown;14:cmd voldown;15:cmd mute;20:quit" "")
expect "grep -q 'ShowInfo=1' $T/root/mpc-hc.ini && grep -q 'ShowStatistics=1' $T/root/mpc-hc.ini && grep -q 'ShowPlaylist=1' $T/root/mpc-hc.ini" "View > Information/Statistics/Playlist saved"
expect "grep -q 'Volume=90' $T/root/mpc-hc.ini && grep -q 'Mute=1' $T/root/mpc-hc.ini" "volume 90, muted, saved"
nosan "$T"
fi

waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/mpc-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }

if want 10; then
echo "== 10. MediaPlayerClassicPS5.elf installs the app (inflated from the payload), stays as the helper, repairs"
T=$(newroot t10)
APP=$T/homebrew/PPSA99015
inst() { # dir -> runs the installer; it stays running as the helper when the port is free
	MPCPS5_PS5_ROOT=$1/root MPCPS5_PS5_HOMEBREW=$1/homebrew MPCPS5_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
stop_helpers
inst "$T" &
expect "waitfor $T/root/logs/installer.log 'listening on 127.0.0.1'" "the installer stays running as the helper"
expect "cmp -s $APP/sce_sys/icon0.png app/sce_sys/icon0.png" "icon0.png installed"
expect "cmp -s $APP/sce_sys/pic0.dds app/sce_sys/pic0.dds && cmp -s $APP/sce_sys/pic1.dds app/sce_sys/pic1.dds && cmp -s $APP/sce_sys/param.json app/sce_sys/param.json" "pic0.dds/pic1.dds (inflated) and param.json installed"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin" "eboot.bin installed"
expect "cmp -s $APP/sce_module/libc.prx tests/fake-libc.prx" "sce_module/libc.prx installed"
expect "grep -q 'Media Player Classic PS5 [0-9.]* installed. Open it from the Media Player Classic PS5 icon' $T/inst.txt" "install notification says to open the icon"
expect "! ls $APP/*.part $APP/sce_sys/*.part $APP/sce_module/*.part 2>/dev/null | grep -q ." "no .part files left"
inst "$T"
expect "grep -q 'is up to date' $T/root/logs/installer.log" "sent again: nothing rewritten"
echo broken > $APP/sce_sys/pic0.dds
inst "$T"
expect "cmp -s $APP/sce_sys/pic0.dds app/sce_sys/pic0.dds" "a damaged background is put back"
stop_helpers
expect "[ \$(find $T -path '*PPSA99009*' -o -path '*PPSA99012*' -o -path '*PPSA99013*' -o -path '*PPSA99014*' -o -path '*PPSA99203*' | wc -l) = 0 ]" "nothing written for the other ports' titles (99009, 99012, 99013, 99014) or PS5SX2"
fi

if want 11; then
echo "== 11. the app asks the helper to let it out; other titles are refused; no helper: it starts its own"
T=$(newroot t11)
stop_helpers
MPCPS5_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "0:0" "30:quit" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'MPC helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper said yes"
expect "grep -q 'letting it out' $T/hroot/logs/helper.log" "the helper let the app's process out"
stop_helpers
T=$(newroot t11b)
MPCPS5_HOST_JB_TITLE=PPSA99013 MPCPS5_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "0:0" "30:quit" "")
expect "grep -q 'title PPSA99013 is not Media Player Classic PS5' $T/hroot/logs/helper.log" "the helper lets out no other title"
stop_helpers
T=$(newroot t11c)
python3 - "$MPCPS5_ELFLDR_PORT" "$T" <<'PY' &
import os, socket, subprocess, sys
port, t = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(1); s.settimeout(60)
c, _ = s.accept()
data = b''
while True:
    d = c.recv(65536)
    if not d: break
    data += d
path = t + '/received.elf'
open(path, 'wb').write(data); os.chmod(path, 0o755)
env = dict(os.environ, MPCPS5_PS5_ROOT=t + '/hroot', ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen(['timeout', '30', path], env=env, stdout=open(t + '/helper.txt', 'w'), stderr=subprocess.STDOUT)
PY
LPID=$!
sleep 0.5
rc=$(run "$T" "0:0" "30:quit" "")
expect "cmp -s $T/received.elf $HELPER" "the ELF loader got the helper built into the app"
expect "grep -q 'jailbreak: MPC helper (started by the app)' $T/root/logs/boot.log" "then the helper it started let it out"
wait $LPID 2>/dev/null
stop_helpers
fi

if want 12; then
echo "== 12. no /data: the window says what to do (network streams still work)"
T=$(newroot t12)
rm -rf "$T/root" && echo "not a folder" >"$T/root"
rc=$(run "$T" "0:0;40:$CROSS;42:0" "60:quit" "30")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'no access to /data' $T/out.txt" "logged: no access to /data"
expect "$CHECK $T/dump/flip00030.ppm 960 540 255 255 255 2 >/dev/null" "a message box on the screen"
fi

# the pixel at (x, y) of a dump, as "r g b"
pixel() { python3 -c "from PIL import Image; print(*Image.open('$1').convert('RGB').getpixel(($2, $3)))"; }
# within lo..hi on all three channels
grey_in() { python3 -c "
from PIL import Image
p = Image.open('$1').convert('RGB').getpixel(($2, $3))
import sys; sys.exit(0 if all($4 <= c <= $5 for c in p) else 1)"; }
# per-channel RMS of a raw S16 dump: rms file channels -> "c0 c1 ..."
rms() { python3 -c "
import array
a = array.array('h'); a.frombytes(open('$1', 'rb').read())
n = $2
print(*[round((sum(x * x for x in a[c::n]) / max(1, len(a[c::n]))) ** 0.5) for c in range(n)])"; }
ini() { mkdir -p "$1/root" && printf '[Settings]\n%s\n' "$2" >"$1/root/mpc-hc.ini"; }
# el dir A B: the seconds between the script's frames A and B (their boot.log lines): the flips run at 60 a second
# only on a fast machine (with the sanitizers, 1080p, they can be half that), so a position after a seek is checked
# against the time that really passed. at dir base A B nominal: base + that time - (B - A) / 60 + nominal
el() { python3 -c "
import re
t = {}
for l in open('$1/root/logs/boot.log', errors='replace'):
    m = re.match(r'\[\s*([0-9.]+)\] \[test\] frame (\d+):', l)
    if m and m.group(2) not in t:
        t[m.group(2)] = float(m.group(1))
print('%.3f' % (t['$3'] - t['$2']))"; }
at() { python3 -c "print('%.3f' % ($2 + $(el $1 $3 $4)))"; }

if want 13; then
echo "== 13. the PS5's video decoder (a stand-in on the host): H.264 / HEVC on it, seeking, handing back to FFmpeg"
T=$(newroot t13)
cp "$MEDIA/hevc10.mkv" "$MEDIA/red.mp4" "$MEDIA/av1.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "50:state;60:open $T/root/media/red.mp4;120:state;130:seek 4;190:state;200:open $T/root/media/av1.mkv;250:state;260:quit" \
	"45,115,185,245" "$T/root/media/hevc10.mkv")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'console decoder: on (H.264, HEVC 8 / 10-bit, VP9 8 / 10-bit, up to 4K)' $T/root/logs/boot.log" "the decoder's library and compute queue made at start"
expect "grep -q 'video decoder: HEVC 640x360 (console decoder)' $T/root/logs/boot.log && state $T 1 | grep -q ' hw 1 gpu'" "HEVC Main 10 on the console's decoder"
expect "grep -q 'pictures 640x368 (shown 640x360) 10-bit' $T/root/logs/boot.log" "10-bit pictures, the padding left out"
expect "grep -q \"frame_format 2 with 2-byte samples isn't on the confirmed list yet\" $T/root/logs/boot.log" "a frame_format not confirmed on a console yet: said in the log"
expect "$CHECK $T/dump/flip00045.ppm 960 400 255 0 255 12 >/dev/null" "its picture: magenta"
expect "grep -q 'video decoder: H.264 640x360 (console decoder)' $T/root/logs/boot.log && state $T 2 | grep -q ' hw 1 gpu'" "H.264 on it too"
expect "$CHECK $T/dump/flip00115.ppm 960 400 red >/dev/null" "its picture: red"
expect "near \$(field $T 3 2) \$(at $T 3.5 130 190) 1.2 && grep -q 'going on from a keyframe' $T/root/logs/boot.log" "a seek: on from the next keyframe (\$(field $T 3 2) s)"
expect "$CHECK $T/dump/flip00185.ppm 960 400 red >/dev/null" "the picture after the seek"
expect "grep -q 'video decoder: FFmpeg (not H.264, HEVC or VP9)' $T/root/logs/boot.log && state $T 4 | grep -q ' hw 0 gpu'" "AV1 stays with FFmpeg"
expect "$CHECK $T/dump/flip00245.ppm 960 400 255 255 0 60 >/dev/null" "its picture: yellow"
nosan "$T"
T=$(newroot t13b)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
# chapters.mkv: a keyframe every second; red 0-3 s, green 3-6 s, blue 6-9 s
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_FAIL=20 run "$T" "0:0" "480:state;490:quit" "470" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \$(grep -c 'stream error 0x811d0301: reset' $T/root/logs/boot.log) = 5 ]" "stream errors: reset, on from the next keyframe, five times"
expect "grep -q 'handing the video to FFmpeg: stream errors keep coming' $T/root/logs/boot.log && grep -q 'video decoder: FFmpeg from now on' $T/root/logs/boot.log" "then the video is handed to FFmpeg"
expect "state $T 1 | grep -q ' hw 0 gpu' && near \$(field $T 1 2) 8.5 0.8 && $CHECK $T/dump/flip00470.ppm 960 400 blue >/dev/null" "it plays on to the end: the blue part"
nosan "$T"
T=$(newroot t13c)
cp "$MEDIA/red.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_REFUSE=1 run "$T" "0:0" "60:state;70:quit" "55" "$T/root/media/red.mp4")
expect "grep -q \"video decoder: FFmpeg (the decoder couldn't be created)\" $T/root/logs/boot.log && $CHECK $T/dump/flip00055.ppm 960 400 red >/dev/null" "a decoder that can't be made: FFmpeg"
T=$(newroot t13d)
cp "$MEDIA/red.mp4" "$T/root/media/"
ini "$T" "HardwareDecoding=0"
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "60:state;70:quit" "55" "$T/root/media/red.mp4")
expect "grep -q 'video decoder: FFmpeg (turned off in the options)' $T/root/logs/boot.log && state $T 1 | grep -q ' hw 0 gpu'" "Options > Output > Hardware decoding off: FFmpeg"
fi

if want 14; then
echo "== 14. HDR10 and HLG on the SDR picture: tone mapping (BT.2390 by default), the options"
T=$(newroot t14)
cp "$MEDIA/hdr_pq.mkv" "$MEDIA/hdr_hlg.mkv" "$T/root/media/"
rc=$(run "$T" "0:0" "50:state;60:open $T/root/media/hdr_hlg.mkv;110:state;120:quit" "45,105" "$T/root/media/hdr_pq.mkv")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'HDR10 (PQ) picture: tone mapped to SDR (BT.2390, white 250 nits, film peak 1000 nits)' $T/root/logs/boot.log" "PQ seen, its mastering peak read (1000 nits)"
expect "grey_in $T/dump/flip00045.ppm 480 520 168 179" "100 nits: below the knee, 100/250 of white (gamma 2.4: ~174)"
expect "grey_in $T/dump/flip00045.ppm 1440 520 248 255" "1000 nits (the film's peak): the screen's white"
expect "grep -q 'HLG picture: tone mapped to SDR' $T/root/logs/boot.log" "HLG seen"
expect "grey_in $T/dump/flip00105.ppm 480 520 125 138 && grey_in $T/dump/flip00105.ppm 1440 520 205 235" "HLG: its OOTF (1000 nits) then the curve"
nosan "$T"
T=$(newroot t14b)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "state $T 1 | grep -q ' hw 1 gpu' && grey_in $T/dump/flip00045.ppm 480 520 168 179 && grey_in $T/dump/flip00045.ppm 1440 520 248 255" "the same from the console's decoder (10-bit pictures)"
T=$(newroot t14c)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
ini "$T" "HdrToneMapping=0"
rc=$(run "$T" "0:0" "50:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "! grep -q 'tone mapped' $T/root/logs/boot.log && grey_in $T/dump/flip00045.ppm 480 520 124 136 && grey_in $T/dump/flip00045.ppm 1440 520 186 198" "Off: the PQ signal as it comes (washed out)"
T=$(newroot t14d)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
ini "$T" "HdrToneMapping=2
HdrPeak=150"
rc=$(run "$T" "0:0" "50:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "grep -q 'tone mapped to SDR (Hable, white 150 nits' $T/root/logs/boot.log && grey_in $T/dump/flip00045.ppm 1440 520 240 255" "Hable, white at 150 nits, from the settings"
fi

if want 15; then
echo "== 15. surround: 5.1 / 7.1 on the PS5's 8-channel output (L R C LFE Ls Rs Lb Rb); stereo mixes them down"
T=$(newroot t15)
cp "$MEDIA/surround51.mkv" "$MEDIA/surround71.mkv" "$T/root/media/"
ini "$T" "AudioOutput=1"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/a51 run "$T" "0:0" "100:quit" "" "$T/root/media/surround51.mkv")
expect "[ $rc = 0 ] && grep -q 'sound: 6 channels -> 5.1 (8-channel output)' $T/root/logs/boot.log" "5.1 to the 8-channel port"
expect "python3 -c \"import sys; r = list(map(int, '\$(rms $T/a51.8ch 8)'.split())); sys.exit(0 if r[2] > 1000 and max(r[:2] + r[3:]) < 50 else 1)\"" "the centre channel's tone in C only"
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/a71 run "$T" "0:0" "100:quit" "" "$T/root/media/surround71.mkv")
expect "grep -q 'sound: 8 channels -> 7.1 (8-channel output)' $T/root/logs/boot.log" "7.1 to the 8-channel port"
expect "python3 -c \"import sys; r = list(map(int, '\$(rms $T/a71.8ch 8)'.split())); sys.exit(0 if r[4] > 1000 and max(r[:4] + r[5:]) < 50 else 1)\"" "the left side channel's tone in Ls (FFmpeg's order mapped to the console's)"
T=$(newroot t15b)
cp "$MEDIA/surround51.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/st run "$T" "0:0" "100:quit" "" "$T/root/media/surround51.mkv")
expect "[ ! -f $T/st.8ch ] && python3 -c \"import sys; r = list(map(int, '\$(rms $T/st.2ch 2)'.split())); sys.exit(0 if r[0] > 300 and abs(r[0] - r[1]) < 20 else 1)\"" "Stereo (the default): the centre mixed into both sides"
nosan "$T"
fi

# a release on the test server (groups 16 and 33): $T/homebrew/PPSA99015 as its folder, signed with $T/release.pem;
# $SRV/latest.json says what GitHub would
release() { # tag contentVersion [bad|nodigest|unsigned|badsig]
	rm -rf "$T/rel" && mkdir -p "$T/rel" && cp -r "$T/homebrew/PPSA99015" "$T/rel/PPSA99015" && rm -f "$T/rel/PPSA99015/manifest.sha256" "$T/rel/PPSA99015/sce_sys/old.bin"
	echo "eboot $1" >"$T/rel/PPSA99015/eboot.bin"; echo "new art" >"$T/rel/PPSA99015/sce_sys/icon0.png"; echo extra >"$T/rel/PPSA99015/sce_sys/extra.bin"
	sed -i "s/\"contentVersion\": \"[0-9.]*\"/\"contentVersion\": \"$2\"/" "$T/rel/PPSA99015/sce_sys/param.json"
	python3 tools/make_release_zip.py "$T/rel/PPSA99015" PPSA99015 "$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip" >/dev/null
	local sha size
	sha=$(sha256sum "$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip" | cut -c1-64); size=$(stat -c %s "$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip")
	local z="$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip" signer=$T/release.pem sigasset
	rm -f "$z.sig"
	[ "${3:-}" = badsig ] && signer=$T/other.pem
	[ "${3:-}" != unsigned ] && openssl pkeyutl -sign -inkey $signer -rawin -in "$z" -out "$z.sig"
	sigasset=",{\"name\":\"MediaPlayerClassicPS5-$1-PPSA99015.zip.sig\",\"size\":64,\"browser_download_url\":\"http://127.0.0.1:$SPORT/MediaPlayerClassicPS5-$1-PPSA99015.zip.sig\"}"
	[ "${3:-}" = unsigned ] && sigasset=""
	[ "${3:-}" = bad ] && sha=0000${sha:4}
	local digest="\"digest\":\"sha256:$sha\","
	[ "${3:-}" = nodigest ] && digest=""
	printf '{"tag_name":"%s","name":"Media Player Classic PS5 %s","assets":[{"name":"MediaPlayerClassicPS5-%s.elf","size":1,"browser_download_url":"http://127.0.0.1:%s/x.elf"},{"name":"MediaPlayerClassicPS5-%s-PPSA99015.zip","size":%s,%s"browser_download_url":"http://127.0.0.1:%s/MediaPlayerClassicPS5-%s-PPSA99015.zip"}%s]}' \
		"$1" "$1" "$1" $SPORT "$1" $size "$digest" $SPORT "$1" "$sigasset" >"$SRV/latest.json"
}

if want 16; then
echo "== 16. Help > Check for updates: the app checks GitHub's newest release, downloads it, checks every file, installs it"
T=$(newroot t16)
stop16() { pkill -f 'build/host/mpc-ps5-(installer|helper)' 2>/dev/null; pkill -f 'http.server 1[89][0-9][0-9][0-9]' 2>/dev/null; sleep 0.3; }
stop16
SRV=$T/srv; mkdir -p "$SRV" "$T/appmeta/PPSA99015"; echo old >"$T/appmeta/PPSA99015/icon0.png"
SPORT=$((18000 + RANDOM % 1900))
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the app folder, as the payload installs it (then its helper is stopped)
MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$T/homebrew MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >"$T/inst.txt" 2>&1 &
for i in $(seq 1 80); do grep -q 'listening on 127.0.0.1' $T/root/logs/installer.log 2>/dev/null && break; sleep 0.1; done
stop16
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the release key of the tests (the app gets its public half through MPCPS5_HOST_RELEASE_KEY), and another one
openssl genpkey -algorithm ed25519 -out $T/release.pem 2>/dev/null
openssl genpkey -algorithm ed25519 -out $T/other.pem 2>/dev/null
PUBHEX=$(openssl pkey -in $T/release.pem -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')
# 1.5.1: the app doesn't read the mount table (no statfs in a title: #3): it asks the helper, started here with the
# (faked) mount table; stoph16 stops it
h16() {
	MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_HOST_MOUNTS="$1" MPCPS5_PS5_SHADOWMNT=${SHM-${M:-$T}/shadowmnt} MPCPS5_PS5_USER_APP=${M:-$T}/userapp \
		MPCPS5_PS5_ROOT=$T/hroot16 ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper16.txt" 2>&1 &
	H16=$!
	for i in $(seq 1 100); do grep -q 'listening on 127.0.0.1' $T/helper16.txt 2>/dev/null && break; sleep 0.1; done
}
stoph16() { kill $H16 2>/dev/null; wait $H16 2>/dev/null; }
u16run() { MPCPS5_PS5_SHADOWMNT=${SHM-${M:-$T}/shadowmnt} MPCPS5_HOST_RELEASE_KEY=$PUBHEX MPCPS5_UPDATE_API=http://127.0.0.1:$SPORT/latest.json MPCPS5_PS5_APPMETA=$T/appmeta run "$@"; }
cp "$T/homebrew/PPSA99015/eboot.bin" "$T/eboot.before"
# a: the daily check at start finds this version: nothing offered; Help > Check for updates says "the latest version"
release v$VER $CVER
rc=$(u16run "$T" "0:0" "150:cmd 601;250:key enter;300:quit" "")
expect "[ $rc = 0 ] && grep -q '\[update\] daily check' $T/root/logs/boot.log && [ -f $T/root/update-check.txt ]" "the daily check at start"
expect "[ \$(grep -c '\[update\] up to date ($VER)' $T/root/logs/boot.log) = 2 ]" "this version is the newest: up to date (at start, then from the Help menu)"
# b: a newer release whose download isn't what GitHub says: refused, nothing changed
echo gone >"$T/homebrew/PPSA99015/sce_sys/old.bin"
printf '%s *sce_sys/old.bin\n' "$(sha256sum <"$T/homebrew/PPSA99015/sce_sys/old.bin" | cut -c1-64)" >"$T/homebrew/PPSA99015/manifest.sha256"
release v9.9 99.000.000 bad
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;450:quit" "")
expect "grep -q '\[update\] 9.9 is available ([0-9]* bytes): offered' $T/root/logs/boot.log" "Help > Check for updates: 9.9 is available, offered"
expect "grep -q \"\[update\] failed: .*SHA-256 doesn't match\" $T/root/logs/boot.log && cmp -s $T/homebrew/PPSA99015/eboot.bin $T/eboot.before && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "Yes: the download isn't GitHub's file: refused, nothing changed"
# b2: a release GitHub gives no SHA-256 for: refused (it can't be checked), nothing changed
release v9.9 99.000.000 nodigest
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;450:quit" "")
expect "grep -q \"\[update\] failed: GitHub gave no SHA-256\" $T/root/logs/boot.log && cmp -s $T/homebrew/PPSA99015/eboot.bin $T/eboot.before && [ -z \"\$(find $T/homebrew -name '*.new' -o -name '*.old.*' -o -name 'update.journal*')\" ]" "no SHA-256 from GitHub: refused, nothing changed"
# b2b: a release without a signature from the app's key, and one signed by another key: refused, nothing changed
for how in unsigned badsig; do
	release v9.9 99.000.000 $how
	rm -f $T/root/logs/boot.log
	rc=$(u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;450:quit" "")
	want="The release has no signature (.sig) from this app's key"
	[ $how = badsig ] && want="The release's signature doesn't match this app's key"
	expect "grep -q \"\[update\] failed: $want: nothing was changed\" $T/root/logs/boot.log && cmp -s $T/homebrew/PPSA99015/eboot.bin $T/eboot.before" "$how: refused, nothing changed"
done
# b3: a file that can't be put in place half way (param.json, after others): every old file put back
release v9.9 99.000.000
( cd $T/homebrew/PPSA99015 && find . -type f | sort | xargs sha256sum ) >$T/before.sums
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_UPDATE_FAIL_RENAME=sce_sys/param.json u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;450:quit" "")
expect "grep -q \"\[update\] failed: Couldn't replace sce_sys/param.json .*the old version was put back\" $T/root/logs/boot.log" "a rename fails half way: the install says the old version was put back"
expect "( cd $T/homebrew/PPSA99015 && find . -type f | sort | xargs sha256sum ) | cmp -s - $T/before.sums" "... and it was: every file as before (no .new, .old or journal left)"
# b4: an install cut off half way (the console turned off): put back when the app next starts
H=$T/homebrew/PPSA99015
sha() { sha256sum <"$1" | cut -c1-64; }
cp $H/eboot.bin $H/eboot.bin.old.123-1
printf 'mpcps5-update-journal 1 123-1\n1\t%s\teboot.bin\n0\t-\tsce_sys/added.bin\n1\t%s\tsce_sys/param.json\nend\n' "$(sha $H/eboot.bin)" "$(sha $H/sce_sys/param.json)" >$H/update.journal
echo "half-installed eboot" >$H/eboot.bin
echo "a file the cut-off install added" >$H/sce_sys/added.bin
echo "new param" >$H/sce_sys/param.json.new
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
expect "grep -q 'an install was cut off half way: the old version put back (3 file(s))' $T/root/logs/boot.log" "at the next start: the interrupted install found"
sums() { ( cd $H && find . -type f | sort | xargs sha256sum ); }
expect "sums | cmp -s - $T/before.sums" "... and the old version put back (the old eboot.bin, the added file gone, no journal)"
# b5: a rename fails and one old file can't be put back either: the error says so, the journal keeps that file,
# the next start puts it back (and the start after a failed put-back tells the user)
release v9.9 99.000.000
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_UPDATE_FAIL_RENAME=sce_sys/param.json MPCPS5_HOST_UPDATE_FAIL_RESTORE=sce_sys/icon0.png u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;800:quit" "")
expect "grep -q \"\[update\] failed: Couldn't replace sce_sys/param.json .*couldn't be put back completely (1 file(s))\" $T/root/logs/boot.log" "a put-back that fails too: the error says the old version isn't completely back"
expect "[ \$(grep -c '	sce_sys/icon0.png$' $H/update.journal) = 1 ] && [ \$(wc -l <$H/update.journal) = 3 ] && tail -1 $H/update.journal | grep -qx end && ls $H/sce_sys/icon0.png.old.* >/dev/null" "... the journal kept (whole: header, that file, end), and its old copy kept"
# b5b: a new install while that one isn't put back: refused, the old copy kept
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_UPDATE_FAIL_RESTORE=sce_sys/icon0.png u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;800:quit" "")
expect "grep -q '0 file(s) put back, 1 not (journal kept for the next start)' $T/root/logs/boot.log && grep -q 'send the payload again to repair it' $T/out.txt && [ -f $H/update.journal ]" "a start that can't put it back either: journal kept, the user told"
expect "grep -q \"\[update\] failed: An earlier update isn't finished being put back: nothing was installed\" $T/root/logs/boot.log && ls $H/sce_sys/icon0.png.old.* >/dev/null && ! grep -q 'eboot v9.9' $H/eboot.bin" "an install while one isn't put back: refused, its old copy kept"
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
expect "grep -q 'the old version put back (1 file(s))' $T/root/logs/boot.log && sums | cmp -s - $T/before.sums" "the next start puts it back: every file as before, no journal"
# b6: the disk can't confirm the new files (a folder's sync fails): everything put back, checked again at the next start
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_UPDATE_FAIL_SYNC=sce_sys u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;800:quit" "")
expect "grep -q \"failed: The new files couldn't be written to the disk for sure: the old version was put back, but the disk didn't confirm it\" $T/root/logs/boot.log && [ -f $H/update.journal ]" "a sync that fails at the end: put back, the journal kept until the disk confirms"
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
expect "grep -q 'the old version put back' $T/root/logs/boot.log && ! grep -q 'without their old copy\|its old copy is gone' $T/root/logs/boot.log && sums | cmp -s - $T/before.sums" "the next start: already back (the old files recognised by their SHA-256), the journal goes"
# b6c (1.5.2): an eboot.bin over 1 MiB (the real one is tens of MB): the journal holds its whole SHA-256 -- 1.5.1 hashed
# its first MiB only, so a file whose first MiB matched was taken as the old one -- and a start after the install was
# cut off recognises it as already back by that
head -c 3145728 /dev/urandom >$T/eboot.big
cp $T/eboot.big $H/eboot.bin
release v9.9 99.000.000
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_UPDATE_FAIL_RENAME=manifest.sha256 MPCPS5_HOST_UPDATE_FAIL_RESTORE=eboot.bin u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;800:quit" "")
expect "grep -q \"^1	$(sha $T/eboot.big)	eboot.bin\$\" $H/update.journal && ls $H/eboot.bin.old.* >/dev/null" "an eboot.bin of 3 MiB not put back: the journal keeps its whole SHA-256"
mv $H/eboot.bin.old.* $H/eboot.bin
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
expect "grep -q 'the old version put back' $T/root/logs/boot.log && ! grep -q 'without their old copy\|its old copy is gone' $T/root/logs/boot.log && [ ! -e $H/update.journal ] && cmp -s $H/eboot.bin $T/eboot.big" "its old copy put back meanwhile: the next start recognises it by that SHA-256, the journal goes"
# b6d (1.5.2): a journal 1.5.1 wrote names a file over 1 MiB by the SHA-256 of its first MiB: put back meanwhile, it is
# recognised by that too (taken as lost, its old copy gone, by the first 1.5.2 build)
cp $T/eboot.big $H/eboot.bin
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_HOST_UPDATE_FAIL_RENAME=manifest.sha256 MPCPS5_HOST_UPDATE_FAIL_RESTORE=eboot.bin u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;800:quit" "")
first=$(head -c 1048576 $T/eboot.big | sha256sum | cut -c1-64)
sed -i "s/^1	$(sha $T/eboot.big)	eboot.bin\$/1	$first	eboot.bin/" $H/update.journal
edited=$(grep -c "^1	$first	eboot.bin\$" $H/update.journal)
mv $H/eboot.bin.old.* $H/eboot.bin
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
expect "[ $edited = 1 ] && grep -q 'the old version put back' $T/root/logs/boot.log && ! grep -q 'without their old copy\|its old copy is gone' $T/root/logs/boot.log && [ ! -e $H/update.journal ] && cmp -s $H/eboot.bin $T/eboot.big" "a 1.5.1 journal (the first MiB's SHA-256 of a 3 MiB eboot.bin), its old copy put back meanwhile: recognised, the journal goes"
cp $T/eboot.before $H/eboot.bin
# b7: a damaged journal (empty): nothing guessed, nothing installed, the payload repairs the app
: >$H/update.journal
echo "something" >$H/eboot.bin.old.9-9
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;800:quit" "")
expect "grep -q \"can't be read as a whole journal: left alone\" $T/root/logs/boot.log && grep -q 'its record is damaged' $T/out.txt && [ -f $H/update.journal ]" "an empty journal: left alone, the user told to send the payload"
expect "grep -q \"failed: An earlier update isn't finished being put back\" $T/root/logs/boot.log && ! grep -q 'eboot v9.9' $H/eboot.bin" "... and no update installed over it"
MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$T/homebrew MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >"$T/inst-repair.txt" 2>&1 &
for i in $(seq 1 300); do grep -q 'listening on 127.0.0.1' $T/inst-repair.txt 2>/dev/null && break; sleep 0.1; done
stop16
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
sleep 0.5
expect "grep -q 'an update wasn.t finished; repairing the app' $T/inst-repair.txt && [ ! -e $H/update.journal ] && [ ! -e $H/eboot.bin.old.9-9 ] && sums | cmp -s - $T/before.sums" "the payload repairs it: its files, the journal and the leftovers gone"
# c: the right file: the daily check (a day later) offers it, Yes installs it; Yes to the restart, with no helper to open
# the app again (1.5.4): the user is told to (up to 1.5.3: sceSystemServiceLoadExec(eboot.bin), which only ended the app)
release v9.9 99.000.000
echo 0 >"$T/root/update-check.txt"
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "200:key enter;450:key enter;520:key enter;560:quit" "300")
expect "grep -q '\[update\] 9.9 is available ([0-9]* bytes): offered' $T/root/logs/boot.log" "the daily check offers 9.9"
expect "grep -q '\[update\] 9.9 installed (in .*): offering the restart' $T/root/logs/boot.log && grep -q \"the release's Ed25519 signature checks out\" $T/root/logs/boot.log" "Yes: downloaded, signature and every file checked, installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99015/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $T/homebrew/PPSA99015/sce_sys/param.json" "the new eboot.bin and param.json in place"
expect "[ -f $T/homebrew/PPSA99015/sce_sys/extra.bin ] && [ ! -e $T/homebrew/PPSA99015/sce_sys/old.bin ] && grep -q 'new art' $T/appmeta/PPSA99015/icon0.png && [ -z \"\$(find $T/homebrew -name '*.new' -o -name '*.old.*' -o -name 'update.journal*')\" ]" "a new file added, a gone one removed, the home screen's art refreshed (no .new, .old or journal left)"
expect "grep -q 'restart: not by the helper (no helper)' $T/root/logs/boot.log && grep -q '\[ui\] message: Close Media Player Classic PS5 and open it again to use the new version.' $T/root/logs/boot.log && ! grep -q 'sceSystemServiceLoadExec(.*eboot.bin)' $T/out.txt" "Yes again, no helper running: the app says to close it and open it again (no LoadExec of eboot.bin)"
expect "grep -q \"(The home screen's App Information may keep showing an older version.)\" $T/root/logs/boot.log" "the restart question says App Information may keep showing an older version"
expect "grep -q 'is available: Help, Check for updates' $T/out.txt" "a notification says a new version is out"
nosan "$T"
# d: the 1.1 payload sent again (an autoload): the newer version is left alone
MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$T/homebrew MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >"$T/inst2.txt" 2>&1 &
for i in $(seq 1 80); do grep -q 'a newer version is installed' $T/inst2.txt 2>/dev/null && break; sleep 0.1; done
expect "grep -q 'a newer version is installed' $T/inst2.txt && grep -q 'eboot v9.9' $T/homebrew/PPSA99015/eboot.bin && grep -q 'new art' $T/appmeta/PPSA99015/icon0.png" "an older payload sent again leaves the update (and its art) alone"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/inst.txt $T/inst2.txt" "the payload: no sanitizer reports"
stop16
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
sleep 0.5
# 1.5 (Sulfurico-One64, #1): after "Close & Restart" the app was still the old version.
# e: the start after c: still 1.5 although 9.9 was installed: the app says so and what to do (once a start, three times)
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:key enter;80:quit" "")
expect "[ $rc = 0 ] && grep -q '\[update\] after the update to 9.9 (in $T/homebrew/PPSA99015): this is $VER: still the old version' $T/root/logs/boot.log && grep -q '\[ui\] message: Media Player Classic PS5 9.9 was installed in $T/homebrew/PPSA99015, but this is still $VER' $T/root/logs/boot.log" "the next start: still the old version: the app says so (close it, ShadowMount+ again, or restart)"
printf '1.5\n/x\n0\n' >$T/root/update-pending.txt
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
expect "[ $rc = 0 ] && grep -q 'this is $VER: the new version runs' $T/root/logs/boot.log && grep -q '\[osd\] Media Player Classic PS5 updated to $VER.' $T/root/logs/boot.log && [ ! -e $T/root/update-pending.txt ]" "an update seen running: said once (the OSD), the note gone"
# the mount table's names are cut at 87 bytes, as the console's: with a long work dir the folders mounted (f-l) live
# under a short one
M=$T; [ ${#T} -le 56 ] || M=$(mktemp -d /tmp/mpc16.XXXXXX)
# f: ShadowMount+ runs the app from another folder (a USB drive's homebrew), nullfs-mounted on /system_ex/app: the
# update goes there, through the mount; /data/homebrew isn't touched; the restart: the helper (1.5.4) takes it, the app
# ends
SX=$M/sysex; U=$M/usb0/homebrew/PPSA99015
rm -rf $SX $M/usb0 && mkdir -p $SX/app $M/usb0/homebrew && cp -r $T/homebrew/PPSA99015 $U
echo "eboot usb 1.5" >$U/eboot.bin; rm -f $U/manifest.sha256 $U/update.journal
ln -s $U $SX/app/PPSA99015
( cd $T/homebrew/PPSA99015 && find . -type f | sort | xargs sha256sum ) >$T/home.sums
MNT="$SX/app/PPSA99015|nullfs|$U|rw;$M/usb0|exfatfs|/dev/da0s1|rw"
release v9.9 99.000.000
rm -f $T/root/logs/boot.log $T/root/update-pending.txt
h16 "$MNT"
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX u16run "$T" "0:0" "100:cmd 601;200:key enter;450:key enter;520:quit" "")
stoph16
expect "grep -q \"\[update\] (from the helper) $SX/app/PPSA99015 is a nullfs mount of $U (on $M/usb0, exfatfs, from /dev/da0s1): files go through $SX/app/PPSA99015\" $T/root/logs/boot.log" "the app runs from a USB drive's folder (a nullfs mount): found, written through the mount"
expect "grep -q 'eboot v9.9' $U/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $U/sce_sys/param.json && [ -z \"\$(find $U -name '*.new' -o -name '*.old.*' -o -name 'update.journal*')\" ]" "... the update is in that folder (eboot.bin, param.json)"
expect "( cd $T/homebrew/PPSA99015 && find . -type f | sort | xargs sha256sum ) | cmp -s - $T/home.sums" "... /data/homebrew's copy untouched (no second copy written)"
expect "[ $rc = 0 ] && grep -q 'restart: the helper opens the app again once it has ended' $T/root/logs/boot.log && grep -q 'relaunch: pid [0-9]* asks to be opened again once it has ended (language en)' $T/hroot16/logs/helper.log && grep -q 'sceSystemServiceLoadExec(exit)' $T/out.txt && ! grep -q 'sceSystemServiceLoadExec(.*eboot.bin)' $T/out.txt && grep -q '^9.9$' $T/root/update-pending.txt" "... the restart: the helper will open the app again, the app ends the normal way; the next start checks the version"
# g: from an image ShadowMount+ mounted (.ffpkg, read-only): Check for updates says so before downloading; nothing written
I=$M/shadowmnt/img1/PPSA99015
rm -rf $M/shadowmnt $SX/app/PPSA99015 && mkdir -p $M/shadowmnt/img1 && cp -r $U $I && ln -s $I $SX/app/PPSA99015
( cd $I && find . -type f | sort | xargs sha256sum ) >$T/img.sums
release v9.9 99.000.000
rm -f $T/root/logs/boot.log $T/root/update-pending.txt
h16 "$SX/app/PPSA99015|nullfs|$I|rw;$M/shadowmnt/img1|ufs|/dev/lvd2|ro"
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
stoph16
expect "grep -q '\[ui\] message: Media Player Classic PS5 9.9 is available (you have $VER).' $T/root/logs/boot.log && grep -q 'This copy runs from an image that ShadowMount+ mounted ($I)' $T/root/logs/boot.log" "an image: Check for updates says it can't be written, before downloading"
expect "! grep -q 'Downloading\|9.9 installed\|GET .*\.zip' $T/root/logs/boot.log && ( cd $I && find . -type f | sort | xargs sha256sum ) | cmp -s - $T/img.sums" "... nothing downloaded, nothing written"
# h: a read-only USB folder: the same, in its words
rm -f $T/root/logs/boot.log
SHM=$M/no-images h16 "$SX/app/PPSA99015|nullfs|$I|rw;$M/shadowmnt/img1|exfatfs|/dev/da1s1|ro"
rc=$(SHM=$M/no-images MPCPS5_PS5_SYSTEM_EX=$SX u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
stoph16
expect "grep -q \"This copy runs from $I, where the app can't write\" $T/root/logs/boot.log && ( cd $I && find . -type f | sort | xargs sha256sum ) | cmp -s - $T/img.sums" "a read-only folder: said before downloading, nothing written"
# i: the payload with ShadowMount+ running the app from that USB folder: it brings that copy up to date (through the
# mount), not a second one in /data/homebrew; from the image: it says so, writes nothing, and the helper still runs
rm -rf $SX/app/PPSA99015 && ln -s $U $SX/app/PPSA99015
echo "eboot usb old" >$U/eboot.bin; sed -i 's/"contentVersion": "[0-9.]*"/"contentVersion": "01.040.000"/' $U/sce_sys/param.json
stop16
MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_HOST_MOUNTS="$MNT" MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$M/hb2 MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >"$T/inst3.txt" 2>&1 &
for i in $(seq 1 100); do grep -q 'listening on 127.0.0.1' $T/inst3.txt 2>/dev/null && break; sleep 0.1; done
stop16
expect "grep -q 'updated to $VER (in $U, the copy ShadowMount+ runs)' $T/inst3.txt && ! grep -q 'eboot usb old' $U/eboot.bin && grep -q '\"contentVersion\": \"$CVER\"' $U/sce_sys/param.json && [ ! -e $M/hb2 ]" "the payload: the copy ShadowMount+ runs (a USB folder) updated through the mount, no second copy"
# the payload with env ($2...) set, its output in $T/$1.txt (until its helper listens)
inst16() {
	local out=$T/$1.txt; shift
	env "$@" MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_PS5_SHADOWMNT=$M/shadowmnt MPCPS5_PS5_USER_APP=$M/userapp MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$M/hb2 MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >"$out" 2>&1 &
	for i in $(seq 1 100); do grep -q 'listening on 127.0.0.1' $out 2>/dev/null && break; sleep 0.1; done
	stop16
}
sums() { ( cd $1 && find . -type f | sort | xargs sha256sum ); }
ver() { sed -i "s/\"contentVersion\": \"[0-9.]*\"/\"contentVersion\": \"$2\"/" $1/sce_sys/param.json; }
# ... an image holding a newer version (99, from f) or this one: an autoloaded payload says the app is newer / ready,
# not "can't be written" at every start; only an older image is said to need updating the way it was made
IMNT="$SX/app/PPSA99015|nullfs|$I|rw;$M/shadowmnt/img1|ufs|/dev/lvd2|ro"
rm -rf $SX/app/PPSA99015 && ln -s $I $SX/app/PPSA99015
inst16 inst4 MPCPS5_HOST_MOUNTS="$IMNT"
expect "grep -q 'a newer version is installed; this payload ($VER) left it as it is' $T/inst4.txt && ! grep -q 'can.t be written' $T/inst4.txt && grep -q 'listening on 127.0.0.1' $T/inst4.txt && sums $I | cmp -s - $T/img.sums && [ ! -e $M/hb2 ]" "the payload and an image with a newer version: says the app is newer, writes nothing, the helper runs"
ver $I $CVER; sums $I >$T/img.sums
inst16 inst5 MPCPS5_HOST_MOUNTS="$IMNT"
expect "grep -q 'Media Player Classic PS5 $VER is ready' $T/inst5.txt && ! grep -q 'can.t be written' $T/inst5.txt && sums $I | cmp -s - $T/img.sums && [ ! -e $M/hb2 ]" "... with this version: ready, nothing written"
ver $I 01.040.000; sums $I >$T/img.sums
inst16 inst6 MPCPS5_HOST_MOUNTS="$IMNT"
expect "grep -q 'runs from an image that ShadowMount+ mounted ($I): an update can.t be written into it' $T/inst6.txt && grep -q 'listening on 127.0.0.1' $T/inst6.txt && sums $I | cmp -s - $T/img.sums && [ ! -e $M/hb2 ]" "... with an older version: says it can't be written there, writes nothing, the helper runs"
# j: nothing written where it isn't sure to be the app the console runs:
# j1: a folder whose param.json names another title (mounted there all the same): not written, by the payload or
# the app's Check for updates
O=$M/other/PPSA99015
rm -rf $M/other $SX/app/PPSA99015 && mkdir -p $M/other && cp -r $U $O && ln -s $O $SX/app/PPSA99015
sed -i 's/"titleId": "PPSA99015"/"titleId": "PPSA00001"/' $O/sce_sys/param.json; ver $O 01.040.000; sums $O >$T/o.sums
inst16 inst7 MPCPS5_HOST_MOUNTS="$SX/app/PPSA99015|nullfs|$O|rw"
expect "grep -q 'mounted on $SX/app/PPSA99015 can.t be identified for sure' $T/inst7.txt && sums $O | cmp -s - $T/o.sums && [ ! -e $M/hb2 ]" "a mounted folder whose param.json names another title: the payload writes nothing (no second copy)"
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
sleep 0.5
release v9.9 99.000.000
rm -f $T/root/logs/boot.log
h16 "$SX/app/PPSA99015|nullfs|$O|rw"
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
stoph16
stop16
expect "grep -q \"The app can't tell for sure which folder this copy runs from\" $T/root/logs/boot.log && ! grep -q 'Downloading\|GET .*\.zip' $T/root/logs/boot.log && sums $O | cmp -s - $T/o.sums" "... the app: Check for updates says so before downloading, nothing written"
# j2: a backport's unionfs over ShadowMount+'s nullfs mount: not written through it; the folder beneath is
rm -rf $SX/app/PPSA99015 && ln -s $U $SX/app/PPSA99015; ver $U 01.040.000
inst16 inst8 MPCPS5_HOST_MOUNTS="$SX/app/PPSA99015|nullfs|$U|rw;$M/usb0|exfatfs|/dev/da0s1|rw;$SX/app/PPSA99015|unionfs|$M/backport|rw"
expect "grep -q \"$SX/app/PPSA99015 is a unionfs mount over a nullfs mount of $U (on $M/usb0, exfatfs, from /dev/da0s1): files go through $U\" $T/inst8.txt && grep -q '\"contentVersion\": \"$CVER\"' $U/sce_sys/param.json && [ ! -e $M/hb2 ]" "a unionfs layer over the mount: the folder beneath written directly, not through the layer"
# j3: a source name the system cut at 87 bytes: only through the mount; the cut path never made
L=$M/long/$(printf 'x%.0s' $(seq 1 $((91 - ${#M} - 16))))/PPSA99015
rm -rf $M/long $SX/app/PPSA99015 && mkdir -p $(dirname $L) && cp -r $U $L && ln -s $L $SX/app/PPSA99015; ver $L 01.040.000
inst16 inst9 MPCPS5_HOST_MOUNTS="$SX/app/PPSA99015|nullfs|$L|rw"
expect "[ \${#L} = 91 ] && grep -q 'its name cut short by the system: \"${L:0:87}\"); written only through the mount' $T/inst9.txt && grep -q 'files go through $SX/app/PPSA99015' $T/inst9.txt && grep -q '\"contentVersion\": \"$CVER\"' $L/sce_sys/param.json && [ ! -e ${L:0:87} ] && [ ! -e $M/hb2 ]" "a source name cut short (87 bytes): written only through the mount, the cut path not made"
# j4: statfs fails: the mount table (getfsstat) says what is mounted
rm -rf $SX/app/PPSA99015 && ln -s $U $SX/app/PPSA99015; ver $U 01.040.000
inst16 inst10 MPCPS5_HOST_NO_STATFS=1 MPCPS5_HOST_MOUNTS="$MNT"
expect "grep -q \"$SX/app/PPSA99015 is a nullfs mount of $U (on $M/usb0, exfatfs, from /dev/da0s1): files go through $SX/app/PPSA99015\" $T/inst10.txt && grep -q '\"contentVersion\": \"$CVER\"' $U/sce_sys/param.json" "statfs failing: the mount table finds the mount"
# j5: neither answering while the mount point is there: nothing written, no second copy
ver $U 01.040.000; sums $U >$T/u.sums
inst16 inst11 MPCPS5_HOST_NO_STATFS=1 MPCPS5_HOST_NO_GETFSSTAT=1 MPCPS5_HOST_MOUNTS="$MNT"
expect "grep -q 'what is mounted on $SX/app/PPSA99015 can.t be read' $T/inst11.txt && sums $U | cmp -s - $T/u.sums && [ ! -e $M/hb2 ]" "neither statfs nor the mount table answering: nothing written, no second copy"
# k: the payload before ShadowMount+ has mounted the app (autoload order): the folder from ShadowMount+'s link
# /user/app/PPSA99015/mount.lnk is updated, no second copy in /data/homebrew; a link to a folder not there: nothing
rm -rf $SX/app/PPSA99015 $M/userapp && mkdir -p $M/userapp/PPSA99015 && echo "$U" >$M/userapp/PPSA99015/mount.lnk
inst16 inst12 MPCPS5_HOST_MOUNTS="$M/usb0|exfatfs|/dev/da0s1|rw"
expect "grep -q 'updated to $VER (in $U, the copy ShadowMount+ runs)' $T/inst12.txt && grep -q '\"contentVersion\": \"$CVER\"' $U/sce_sys/param.json && [ ! -e $M/hb2 ]" "no mount yet: the folder of ShadowMount+'s mount.lnk updated, no second copy"
echo "$M/gone/PPSA99015" >$M/userapp/PPSA99015/mount.lnk
inst16 inst13
expect "grep -q 'runs the app from $M/gone/PPSA99015, which isn.t there now' $T/inst13.txt && grep -q 'listening on 127.0.0.1' $T/inst13.txt && [ ! -e $M/hb2 ] && [ ! -e $M/gone ]" "... its folder not there (a drive not connected yet): nothing written, no second copy"
# ... the link naming an image's folder ShadowMount+ hasn't mounted yet (under /mnt/shadowmnt, or with mount_img.lnk
# beside it): the payload just says it is ready, writes nothing (an autoloaded payload doesn't nag at every boot)
echo "$M/shadowmnt/img2/PPSA99015" >$M/userapp/PPSA99015/mount.lnk
inst16 inst15
expect "grep -q 'Media Player Classic PS5 $VER is ready' $T/inst15.txt && grep -q 'an image.s folder, $M/shadowmnt/img2/PPSA99015 .*not mounted yet' $T/inst15.txt && ! grep -q 'isn.t there now' $T/inst15.txt && [ ! -e $M/hb2 ] && [ ! -e $M/shadowmnt/img2 ]" "... a link to an image not mounted yet: ready, nothing written"
echo "$M/images/PPSA99015" >$M/userapp/PPSA99015/mount.lnk; echo "/data/x.ffpkg" >$M/userapp/PPSA99015/mount_img.lnk
inst16 inst16
expect "grep -q 'Media Player Classic PS5 $VER is ready' $T/inst16.txt && ! grep -q 'isn.t there now' $T/inst16.txt && [ ! -e $M/hb2 ] && [ ! -e $M/images ]" "... the same with mount_img.lnk beside the link"
rm -f $M/userapp/PPSA99015/mount_img.lnk
# ... a link that isn't an absolute path: no link (a first install goes to /data/homebrew)
echo "PPSA99015" >$M/userapp/PPSA99015/mount.lnk
inst16 inst17
expect "grep -q 'link holds \"PPSA99015\", not a folder.s path: ignored' $T/inst17.txt && grep -q 'Media Player Classic PS5 $VER installed' $T/inst17.txt && [ -f $M/hb2/PPSA99015/eboot.bin ]" "... a link that isn't a path: ignored, installed in /data/homebrew"
rm -rf $M/hb2
rm -rf $M/userapp
# l: extended storage on an LVD device but writable: not taken for an image; written
mkdir -p $SX/app && ln -s $U $SX/app/PPSA99015; ver $U 01.040.000
inst16 inst14 MPCPS5_HOST_MOUNTS="$SX/app/PPSA99015|nullfs|$U|rw;$M/usb0|ufs|/dev/lvd5|rw"
expect "grep -q 'updated to $VER (in $U, the copy ShadowMount+ runs)' $T/inst14.txt && grep -q '\"contentVersion\": \"$CVER\"' $U/sce_sys/param.json" "a writable folder on an LVD device: updated (not taken for an image)"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/inst*.txt" "the payload: no sanitizer reports"
nosan "$T"
# m (1.5.1, #3): the app without the helper's answer -- a 1.4 / 1.5 helper (it closes the connection on the new
# request) or none: ShadowMount+'s link, checked against what /system_ex/app shows (param.json, the same eboot.bin),
# is updated directly; without a link, or with one to another folder, nothing is written
rm -rf $SX/app/PPSA99015 && ln -s $U $SX/app/PPSA99015; ver $U $CVER; echo "eboot usb 1.5" >$U/eboot.bin
mkdir -p $M/userapp/PPSA99015 && echo "$U" >$M/userapp/PPSA99015/mount.lnk
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
python3 tests/fake_helper.py $MPCPS5_HELPER_PORT old >"$T/oldh.txt" 2>&1 &
OLDH=$!
sleep 0.5
release v9.9 99.000.000
rm -f $T/root/logs/boot.log $T/root/update-pending.txt
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_PS5_USER_APP=$M/userapp u16run "$T" "0:0" "100:cmd 601;200:key enter;450:key enter;520:quit" "")
kill $OLDH 2>/dev/null; wait $OLDH 2>/dev/null
expect "grep -q \"\[update\] the helper didn't say where the app is\" $T/root/logs/boot.log && grep -q \"\[update\] $SX/app/PPSA99015 shows the app (no mount table); ShadowMount+'s link: $U ($M/userapp/PPSA99015/mount.lnk): files go through $U\" $T/root/logs/boot.log" "an older helper (the request not known): the app goes by ShadowMount+'s link"
expect "grep -q 'eboot v9.9' $U/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $U/sce_sys/param.json && [ -z \"\$(find $U -name '*.new' -o -name '*.old.*' -o -name 'update.journal*')\" ]" "... the update is in that folder"
ver $U $CVER; echo "eboot usb 1.5" >$U/eboot.bin; rm -f $U/manifest.sha256; sums $U >$T/u.sums
rm -f $M/userapp/PPSA99015/mount.lnk $T/root/logs/boot.log $T/root/update-pending.txt
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_PS5_USER_APP=$M/userapp u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
expect "grep -q '\[update\] no helper on port' $T/root/logs/boot.log && grep -q \"The app can't tell for sure which folder this copy runs from\" $T/root/logs/boot.log && ! grep -q 'Downloading\|GET .*\.zip' $T/root/logs/boot.log && sums $U | cmp -s - $T/u.sums" "no helper and no ShadowMount+ link: Check for updates says it can't tell, nothing written"
O2=$M/other2/PPSA99015
rm -rf $M/other2 && mkdir -p $M/other2 && cp -r $U $O2 && echo "another eboot" >$O2/eboot.bin && sums $O2 >$T/o2.sums
echo "$O2" >$M/userapp/PPSA99015/mount.lnk
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_PS5_USER_APP=$M/userapp u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
stop16
expect "grep -q \"ShadowMount+'s link: $O2 .*: not the folder the mount shows\" $T/root/logs/boot.log && grep -q \"The app can't tell for sure which folder\" $T/root/logs/boot.log && sums $U | cmp -s - $T/u.sums && sums $O2 | cmp -s - $T/o2.sums" "... a link to another folder than the one shown (another eboot.bin): nothing written"
# n: a helper answering with a folder that isn't this app's (its param.json names another title), "writable": the
# app checks it and writes nothing
rm -rf $M/other3 && mkdir -p $M/other3 && cp -r $U $M/other3/PPSA99015 && O3=$M/other3/PPSA99015
sed -i 's/"titleId": "PPSA99015"/"titleId": "PPSA00001"/' $O3/sce_sys/param.json; sums $O3 >$T/o3.sums
printf 'place=1\nmount=%s\nsource=%s\ndir=%s\nfs=ufs\nwritable=1\nimage=0\nunsure=0\nvia_link=0\nimage_pending=0\nreason=\nlog=a fake answer\n' \
	"$SX/app/PPSA99015" "$O3" "$O3" >$T/place.txt
python3 tests/fake_helper.py $MPCPS5_HELPER_PORT place $T/place.txt >"$T/fakeh.txt" 2>&1 &
FH=$!
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
sleep 0.5
release v9.9 99.000.000
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_PS5_USER_APP=$M/userapp u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
kill $FH 2>/dev/null; wait $FH 2>/dev/null
expect "grep -q \"(from the helper) a fake answer; but the folder to write holds no param.json naming PPSA99015: not written\" $T/root/logs/boot.log && grep -q \"The app can't tell for sure which folder\" $T/root/logs/boot.log && sums $O3 | cmp -s - $T/o3.sums" "a helper naming another title's folder (writable): the app checks its param.json, writes nothing"
# o: a helper that never answers (wedged): the unlock waits its 5 s, the question where the app is isn't asked again --
# start-up doesn't take 10 s
python3 tests/fake_helper.py $MPCPS5_HELPER_PORT silent >"$T/silenth.txt" 2>&1 &
FH=$!
sleep 0.5
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "60:quit" "")
kill $FH 2>/dev/null; wait $FH 2>/dev/null
el16() { python3 - "$T/root/logs/boot.log" <<'PY'
import re, sys
t = {}
for l in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[\s*([0-9.]+)\] (.*)', l)
    if m and 'asks to leave the sandbox' in m.group(2) and 'a' not in t:
        t['a'] = float(m.group(1))
    if m and m.group(2).startswith('[update] (no mount table)') and 'b' not in t:
        t['b'] = float(m.group(1))
print('%.2f' % (t['b'] - t['a']))
PY
}
expect "[ $rc = 0 ] && grep -q 'not asking the helper where the app is: at start it no reply within 5 s' $T/root/logs/boot.log && python3 -c \"import sys; sys.exit(0 if \$(el16) < 7.5 else 1)\"" "a helper that never answers: asked once (5 s), not again where the app is"
stop16
# p: a helper silent at start that answers later: Check for updates asks it again and goes by its answer
rm -rf $SX/app/PPSA99015 && ln -s $U $SX/app/PPSA99015; ver $U $CVER; echo "eboot usb" >$U/eboot.bin
printf 'place=1\nmount=%s\nsource=%s\ndir=%s\nfs=exfatfs\nwritable=1\nimage=0\nunsure=0\nvia_link=0\nimage_pending=0\nreason=\nlog=the late answer\n' \
	"$SX/app/PPSA99015" "$U" "$SX/app/PPSA99015" >$T/place-late.txt
python3 tests/fake_helper.py $MPCPS5_HELPER_PORT late $T/place-late.txt >"$T/lateh.txt" 2>&1 &
FH=$!
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
sleep 0.5
release v9.9 99.000.000
rm -f $T/root/logs/boot.log
rc=$(MPCPS5_PS5_SYSTEM_EX=$SX MPCPS5_PS5_USER_APP=$M/userapp u16run "$T" "0:0" "100:cmd 601;200:key enter;300:quit" "")
kill $FH 2>/dev/null; wait $FH 2>/dev/null
stop16
expect "grep -q 'not asking the helper where the app is: at start' $T/root/logs/boot.log && grep -q '\[update\] looked again: (from the helper) the late answer' $T/root/logs/boot.log && grep -q 'Download it' $T/root/logs/boot.log && ! grep -q \"The app can't tell for sure\" $T/root/logs/boot.log" "a helper silent at start, answering later: Check for updates asks again and offers the update"
rm -rf $M/userapp $M/other2 $M/other3
[ "$M" = "$T" ] || rm -rf "$M"
fi

if want 17; then
echo "== 17. languages: English by default; Spanish, Portuguese (Brazil), French and German translated"
expect "python3 $PWD/tests/check_i18n.py $PWD/player/mpc_i18n.cpp >/dev/null" "every translation has its five languages and the English format specifiers"
# the menus (the bar, every menu and the context menu), the bars, the dialogs (About, Options, Open, the file
# browser, Properties, Go To with the keyboard, Add Favorite), the OSD, a message box
S="5:cmd info;6:cmd stats;20:key menu;30:key right;40:key right;50:key right;60:key right;70:key right;80:key escape"
S="$S;82:key escape;88:mouse 960,540;90:rclick;100:key escape;110:cmd about;130:key escape;140:cmd options;143:key down"
S="$S;146:key down;149:key down;152:key down;155:key down;158:key down;161:key down;170:key escape"
S="$S;180:cmd open;195:key escape;200:cmd quickopen;220:key escape;230:cmd props;245:key escape;250:cmd goto;258:key enter"
S="$S;270:key escape;275:key escape;280:cmd addfav;290:key escape;300:cmd volup;305:cmd mute;310:cmd faster;315:cmd next"
S="$S;320:cmd pause;325:cmd opendir;340:key escape;350:open $WORK/no-such-file.mkv;380:key enter"
# 1.4: the disc drives in the panes (an encrypted DVD, an empty USB drive), File > Open DVD/BD, the encrypted disc's message
S="$S;390:cmd opendvdbd;405:key escape;410:open dvd://cd0;440:key enter;450:quit"
for L in 1 2 3 4; do
	T=$(newroot t17_$L)
	cp "$MEDIA/chapters.mkv" "$T/root/media/"
	ini "$T" "Language=$L"
	rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd-css.iso,$WORK/no-disc.iso MPCPS5_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0" "$S" "145,255,262,400,430" "$T/root/media/chapters.mkv")
	touch $T/missing.txt
	# what was shown untranslated: only names (the file's, its tracks', the buttons') and texts made from translated formats
	python3 - $T/missing.txt $PWD/player/mpc_i18n.cpp > $T/untranslated.txt <<'PY'
import re, sys
src = open(sys.argv[2], encoding='utf-8').read()
translated = set(re.findall(r'"((?:[^"\\]|\\.)*)"', src))
spec = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsp])')
formats = [re.compile('^' + ''.join('.*' if spec.fullmatch(part) else re.escape(part)
                                    for part in re.split('(' + spec.pattern + ')', f)) + '$', re.S)
           for f in translated if '%' in f]
names = {'L', 'R', 'L1', 'R1', 'L2', 'R2', 'L3', 'R3', 'Roboto', 'MediaInfo', 'Selawik (Segoe UI)', 'English', 'Español',
         'Português (Brasil)', 'Français', 'Deutsch', 'Media Player Classic Home Cinema', 'chapters.mkv',
         'No such file or directory'}  # the last one: FFmpeg's own error text
for line in open(sys.argv[1], encoding='utf-8'):
    t = line.rstrip('\n')
    if t in translated or t in names or not re.search('[A-Za-z]{2}', t):
        continue
    if any(f.match(t) for f in formats):
        continue
    # names: recent files (&1 ...), chapters (1. name  [time]), tracks (... [eng] (CODEC, ...)), the file's tags
    if re.match(r'&\d ', t) or re.match(r'\d+\. ', t) or re.search(r' \[[a-z]{2,3}\] \(', t) or re.match(r'[a-z_]+$|[A-Z_]+$', t):
        continue
    print(t)
PY
	expect "[ $rc = 0 ] && [ ! -s $T/untranslated.txt ]" "language $L: every text shown is translated ($(head -c 300 $T/untranslated.txt | tr '\n' '|'))"
	nosan "$T"
done
expect "grep -q 'Language=4' $T/root/mpc-hc.ini" "the language is kept in mpc-hc.ini"
# English stays the default: no setting, the texts pass through as they are, the file says Language=0
T=$(newroot t17en)
rc=$(MPCPS5_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0" "10:cmd options;20:key escape;30:cmd about;40:key escape;50:quit" "")
expect "[ $rc = 0 ] && [ ! -s $T/missing.txt ] && grep -q '^Language=0$' $T/root/mpc-hc.ini" "English by default (Language=0): nothing is looked up"
nosan "$T"
fi

if want 18; then
echo "== 18. debug logs: off by default (nothing written), on with debug_logs=1 in mpc-hc.ini"
T=$(newroot t18)
rc=$(MPCPS5_LOGS_DEFAULT= run "$T" "0:0" "30:quit" "")
expect "[ $rc = 0 ] && [ ! -e $T/root/logs/boot.log ] && ! grep -q '^\[mpc-ps5' $T/out.txt" "no settings: no boot.log, nothing on stdout"
expect "grep -q '^debug_logs=0$' $T/root/mpc-hc.ini" "the settings file says debug_logs=0"
nosan "$T"
T=$(newroot t18b)
ini "$T" "debug_logs=1"
rc=$(MPCPS5_LOGS_DEFAULT= run "$T" "0:0" "30:quit" "")
expect "[ $rc = 0 ] && grep -q 'Media Player Classic PS5 $(sed -n 's/^VERSION ?= //p' Makefile), FFmpeg' $T/root/logs/boot.log" "debug_logs=1: boot.log written"
expect "grep -q '^debug_logs=1$' $T/root/mpc-hc.ini" "and kept when the settings are saved"
nosan "$T"
fi

if want 19; then
echo "== 19. 1.2: the audio boost clips (no wrap-around), HDR workers kept, the decoder's pictures checked, SMB passwords kept out of mpc-hc.ini and the logs"
# a: a full-scale tone with the boost at its most (+300%: 4x): held at the 16-bit limits, never wrapped to the other sign
T=$(newroot t19)
cp "$MEDIA/loud.mkv" "$T/root/media/"
ini "$T" "AudioBoost=300"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/a run "$T" "0:0" "100:quit" "" "$T/root/media/loud.mkv")
expect "[ $rc = 0 ] && python3 -c \"
import array, sys
a = array.array('h'); a.frombytes(open('$T/a.2ch', 'rb').read())
l = a[0::2]
flips = sum(1 for i in range(1, len(l)) if (l[i - 1] > 16000 and l[i] < -16000) or (l[i - 1] < -16000 and l[i] > 16000))
sys.exit(0 if max(l) == 32767 and min(l) == -32768 and flips == 0 else 1)\"" "boost 4x on a full-scale tone: clipped at 32767 / -32768, never wrapped to the other sign"
nosan "$T"
# b: HDR: the tone mapping's workers made once and kept, over two films
T=$(newroot t19b)
cp "$MEDIA/hdr_pq.mkv" "$MEDIA/hdr_hlg.mkv" "$T/root/media/"
rc=$(run "$T" "0:0" "60:open $T/root/media/hdr_hlg.mkv;110:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "[ $rc = 0 ] && [ \$(grep -c '\[tonemap\] 5 worker threads started (kept for the run)' $T/root/logs/boot.log) = 1 ]" "five workers started once for two HDR films"
expect "grey_in $T/dump/flip00045.ppm 480 520 168 179 && grey_in $T/dump/flip00045.ppm 1440 520 248 255" "the picture as before (100 nits ~174, the peak white)"
expect "grep -q '\[tonemap\] 5 worker threads stopped' $T/root/logs/boot.log" "at the end the workers are stopped and joined"
nosan "$T"
# c: the console decoder's pictures checked before they are read: an odd layout or a too-small buffer goes to FFmpeg
for odd in pitch size format depth; do
	T=$(newroot t19c$odd)
	cp "$MEDIA/red.mp4" "$T/root/media/"
	rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_ODD=$odd run "$T" "0:0" "80:state;90:quit" "75" "$T/root/media/red.mp4")
	want_why="a picture layout the player doesn't know"
	[ $odd = size ] && want_why="a picture bigger than its buffer"
	[ $odd = format ] && want_why="the pictures' layout changed in the middle of the stream"
	[ $odd = depth ] && want_why="16-bit samples for an 8-bit stream"
	expect "[ $rc = 0 ] && grep -q \"handing the video to FFmpeg: $want_why\" $T/root/logs/boot.log" "decoder output with an odd $odd: not read, the video handed to FFmpeg"
	expect "state $T 1 | grep -q ' hw 0 gpu' && $CHECK $T/dump/flip00075.ppm 960 400 red >/dev/null" "... and it plays on: red"
	nosan "$T"
done
# d: an SMB share with a password
SPORT=$((40000 + RANDOM % 10000))
python3 - "$SPORT" "$MEDIA" >"$WORK/smb19.txt" 2>&1 <<'PY' &
import sys
from impacket import smbserver
from impacket.ntlm import compute_lmhash, compute_nthash
s = smbserver.SimpleSMBServer(listenAddress='127.0.0.1', listenPort=int(sys.argv[1]))
s.addShare('VIDEOS', sys.argv[2], 'test media')
s.setSMB2Support(True)
s.setSMBChallenge('')
s.addCredential('bob', 1000, compute_lmhash('s3cret'), compute_nthash('s3cret'))
s.start()
PY
SPID=$!
sleep 2
T=$(newroot t19d)
rc=$(run "$T" "0:0" "80:state;90:quit" "75" "smb://bob:s3cret@127.0.0.1:$SPORT/VIDEOS/red.mp4")
expect "[ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00075.ppm 960 400 red >/dev/null" "smb://bob:s3cret@... plays"
expect "grep -q 'smb://bob:\*\*\*@127.0.0.1' $T/root/logs/boot.log && ! grep -rq s3cret $T/root/logs $T/out.txt" "the password is *** in the log; nowhere in the logs or the output"
expect "! grep -q s3cret $T/root/mpc-hc.ini && grep -q '^File1=smb://bob@127.0.0.1:$SPORT/VIDEOS/red.mp4$' $T/root/mpc-hc.ini" "mpc-hc.ini keeps the address without the password"
expect "grep -q \"smb://bob@127.0.0.1:$SPORT	s3cret\" $T/root/credentials.ini && [ \$(stat -c %a $T/root/credentials.ini) = 600 ]" "the password in credentials.ini, readable by its owner only"
rm -f $T/root/logs/boot.log
rc=$(run "$T" "0:0" "80:state;90:quit" "75" "smb://bob@127.0.0.1:$SPORT/VIDEOS/red.mp4")
expect "[ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00075.ppm 960 400 red >/dev/null" "smb://bob@... typed without the password: the kept one is used, it plays"
nosan "$T"
# an mpc-hc.ini of 1.1.1, the password in its addresses: moved out at the first start
T=$(newroot t19e)
ini "$T" "Volume=100
[Network]
Server0=Home|smb://bob:s3cret@127.0.0.1:$SPORT/VIDEOS"
rc=$(run "$T" "0:0" "30:quit" "")
expect "[ $rc = 0 ] && grep -q 'passwords moved from mpc-hc.ini to credentials.ini' $T/root/logs/boot.log && ! grep -q s3cret $T/root/mpc-hc.ini && grep -q '^Server0=Home|smb://bob@127.0.0.1:$SPORT/VIDEOS$' $T/root/mpc-hc.ini && grep -q s3cret $T/root/credentials.ini" "an older mpc-hc.ini: its passwords moved to credentials.ini at once"
nosan "$T"
# the 8-channel port refused once: stereo for that film; asked again (10 s later) for the next surround one
T=$(newroot t19h)
cp "$MEDIA/surround51.mkv" "$MEDIA/surround71.mkv" "$T/root/media/"
ini "$T" "AudioOutput=1"
rc=$(MPCPS5_HOST_AUDIO8_FAIL=1 run "$T" "0:0" "720:open $T/root/media/surround71.mkv;800:quit" "" "$T/root/media/surround51.mkv")
expect "[ \$(grep -c 'sceAudioOutOpen(48 kHz, S16 8 channels, 256) -> -' $T/root/logs/boot.log) = 1 ] && grep -q 'sound: 8 channels -> 7.1 (8-channel output)' $T/root/logs/boot.log && grep -q '\[audio\] 8-channel output' $T/root/logs/boot.log" "the 8-channel port refused once: asked again for the next surround film, which gets it"
nosan "$T"
# credentials.ini can't be written: mpc-hc.ini is left as it was, so no password is lost; the next start moves it
T=$(newroot t19f)
ini "$T" "Volume=100
[Network]
Server0=Home|smb://bob:s3cret@127.0.0.1:$SPORT/VIDEOS"
rc=$(MPCPS5_HOST_SETTINGS_FAIL=credentials run "$T" "0:0" "30:quit" "")
expect "[ $rc = 0 ] && grep -q \"couldn't write credentials.ini: mpc-hc.ini left as it was\" $T/root/logs/boot.log && grep -q '^Server0=Home|smb://bob:s3cret@' $T/root/mpc-hc.ini && [ ! -e $T/root/credentials.ini ] && [ ! -e $T/root/credentials.ini.tmp ] && [ ! -e $T/root/mpc-hc.ini.tmp ]" "credentials.ini can't be written: mpc-hc.ini untouched, the password still there"
rc=$(run "$T" "0:0" "30:quit" "")
expect "! grep -q s3cret $T/root/mpc-hc.ini && grep -q s3cret $T/root/credentials.ini" "the next start: moved to credentials.ini"
# mpc-hc.ini can't be replaced: credentials.ini keeps the old passwords too (the old mpc-hc.ini still needs them)
cp $T/root/mpc-hc.ini $T/ini.before
rc=$(MPCPS5_HOST_SETTINGS_FAIL=settings run "$T" "0:0" "20:quit" "smb://alice:pw2@127.0.0.1:$SPORT/VIDEOS/red.mp4")
expect "cmp -s $T/root/mpc-hc.ini $T/ini.before && grep -q s3cret $T/root/credentials.ini" "mpc-hc.ini can't be replaced: left as it was, its passwords kept"
nosan "$T"
kill $SPID 2>/dev/null
# a password changed: the old one stays until the new mpc-hc.ini is in place (generations)
T=$(newroot t19g)
U=127.0.0.1:1/VIDEOS/x.mkv
rc=$(run "$T" "0:0" "30:quit" "" "smb://bob:oldpass@$U")
expect "grep -qx '; generation 1' $T/root/credentials.ini && grep -q 'oldpass' $T/root/credentials.ini && grep -qx 'Generation=1' $T/root/mpc-hc.ini" "a password kept: credentials.ini and mpc-hc.ini both say generation 1"
rc=$(MPCPS5_HOST_SETTINGS_FAIL=settings run "$T" "0:0" "30:quit" "" "smb://bob:NEWpass@$U")
expect "grep -q 'oldpass' $T/root/credentials.ini && ! grep -q NEWpass $T/root/credentials.ini && grep -qx 'Generation=1' $T/root/mpc-hc.ini && [ ! -e $T/root/credentials.ini.new ]" "the password changed but mpc-hc.ini can't be replaced: the old password and settings stay together"
rc=$(MPCPS5_HOST_SETTINGS_FAIL=credentials-rename run "$T" "0:0" "30:quit" "" "smb://bob:NEWpass@$U")
expect "grep -qx 'Generation=2' $T/root/mpc-hc.ini && grep -q oldpass $T/root/credentials.ini && grep -q NEWpass $T/root/credentials.ini.new" "cut off after mpc-hc.ini was replaced: the new password waits in credentials.ini.new (generation 2)"
rm -f $T/root/logs/boot.log
rc=$(run "$T" "0:0" "30:quit" "")
expect "grep -q 'credentials.ini of generation 2 put in place' $T/root/logs/boot.log && grep -q NEWpass $T/root/credentials.ini && ! grep -q oldpass $T/root/credentials.ini && [ ! -e $T/root/credentials.ini.new ]" "the next start takes it: credentials.ini has the new password"
printf '; generation 99\nsmb://bob@127.0.0.1:1\tstale\n' >$T/root/credentials.ini.new
rc=$(run "$T" "0:0" "30:quit" "")
expect "[ ! -e $T/root/credentials.ini.new ] && grep -q NEWpass $T/root/credentials.ini && ! grep -q stale $T/root/credentials.ini" "a credentials.ini.new of another generation (a save that didn't finish): dropped"
nosan "$T"
fi

if want 20; then
echo "== 20. ASS/SSA drawn by libass: \\pos, styles, rotation, the delay; an MKV's attached font; SRT as before"
# the 640x360 picture fills x 148..1771, y 65..977 of the window (2.54 screen pixels per script pixel)
T=$(newroot t20)
cp "$MEDIA/grey.mp4" "$T/root/media/movie.mp4"
cp "$MEDIA/styled.ass" "$T/root/media/movie.ass"
rc=$(run "$T" "0:0" "60:state;80:quit" "60" "$T/root/media/movie.mp4")
D=$T/dump/flip00060.ppm
expect "[ $rc = 0 ] && grep -q 'subtitles loaded: .*movie.ass (ASS, libass)' $T/root/logs/boot.log && [ \"\$(field $T 1 6)\" = 1000 ]" "movie.ass beside it: loaded into libass and chosen"
expect "$REGION $D 150 65 700 220 0 255 0 40 800 >/dev/null && ! $REGION $D 0 300 1920 1080 0 255 0 40 20 >/dev/null" "(a) \\pos(20,20) \\an7: green text at the top left, none lower down"
expect "$REGION $D 700 780 1220 977 255 255 0 40 2000 >/dev/null" "(b) the Big style: 48 px yellow text at the bottom centre"
expect "$REGION $D 700 780 1220 977 0 0 255 40 1000 >/dev/null" "(b) ... with its thick blue outline"
expect "! $REGION $D 0 0 1920 700 255 255 0 40 20 >/dev/null" "(b) ... and nowhere higher"
expect "$REGION $D 900 330 1020 710 255 0 0 40 1000 >/dev/null" "(c) \\frz90: red text standing up in the middle"
expect "! $REGION $D 560 470 890 570 255 0 0 40 30 >/dev/null && ! $REGION $D 1030 470 1360 570 255 0 0 40 30 >/dev/null" "(c) ... not lying across it"
expect "grep -q 'Using default font family: (Arial, 700, 0) -> Selawik-Bold' $T/root/logs/boot.log" "a font nobody has (Arial): the app's own Selawik"
nosan "$T"
# the subtitle delay (Options) moves libass's lines too: +2 s, nothing at 1 s, the lines at 3.3 s
T=$(newroot t20b)
cp "$MEDIA/grey.mp4" "$T/root/media/movie.mp4"
cp "$MEDIA/styled.ass" "$T/root/media/movie.ass"
ini "$T" "SubDelay=2000"
rc=$(run "$T" "0:0" "220:quit" "60,200" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && ! $REGION $T/dump/flip00060.ppm 150 65 700 220 0 255 0 40 20 >/dev/null && ! $REGION $T/dump/flip00060.ppm 700 780 1220 977 255 255 0 40 20 >/dev/null" "delay +2 s: nothing yet at 1 s"
expect "$REGION $T/dump/flip00200.ppm 150 65 700 220 0 255 0 40 800 >/dev/null && $REGION $T/dump/flip00200.ppm 700 780 1220 977 255 255 0 40 2000 >/dev/null" "... the lines at 3.3 s"
nosan "$T"
# an MKV with an attached font its style asks for
T=$(newroot t20c)
cp "$MEDIA/fonts.mkv" "$T/root/media/"
AF=$(cut -d'|' -f1 "$MEDIA/attached-font.txt"); AFAM=$(cut -d'|' -f2 "$MEDIA/attached-font.txt")
rc=$(run "$T" "0:0" "60:state;80:quit" "60" "$T/root/media/fonts.mkv")
expect "[ $rc = 0 ] && grep -q \"attached font loaded: $AF\" $T/root/logs/boot.log" "the MKV's attached font ($AF) given to libass"
expect "grep -q \"fontselect: ($AFAM, 400, 0) -> \" $T/root/logs/boot.log && ! grep -q \"Using default font family: ($AFAM\" $T/root/logs/boot.log" "the style's font ($AFAM) found: the attached one, not the default"
expect "$REGION $T/dump/flip00060.ppm 600 65 1320 260 255 0 255 40 800 >/dev/null" "its line drawn: magenta at the top centre"
nosan "$T"
# SRT: the old path (the Subtitles options), libass not even started
T=$(newroot t20d)
cp "$MEDIA/grey.mp4" "$T/root/media/movie.mp4"
printf '1\r\n00:00:00,000 --> 00:00:05,000\r\nPlain SRT line\r\n' >"$T/root/media/movie.srt"
rc=$(run "$T" "0:0" "60:state;80:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'subtitles loaded: .*movie.srt$' $T/root/logs/boot.log && ! grep -q 'libass' $T/root/logs/boot.log" "movie.srt: FFmpeg's text, libass not started"
expect "$REGION $T/dump/flip00060.ppm 500 700 1420 977 255 255 255 30 1500 >/dev/null" "its line drawn as before: white at the bottom"
nosan "$T"
fi

if want 21; then
echo "== 21. a character the chosen font hasn't got comes from a loaded font that has it; the idle window drawn twice a second"
T=$(newroot t21)
mkdir -p $T/root/fonts
cp /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf $T/root/fonts/aa-LiberationSans.ttf
cp "$(readlink -f /usr/share/fonts/truetype/fonts-japanese-gothic.ttf)" $T/root/fonts/zz-gothic.ttf
cp "$MEDIA/grey.mp4" "$T/root/media/movie.mp4"
printf '[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,40,&H0000FF00,&H0000FF00,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\\pos(20,20)}\xe6\x97\xa5\xe6\x9c\xac \xe2\x98\x83\n' >"$T/root/media/movie.ass"
rc=$(run "$T" "0:0" "60:state;80:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'glyph 0x65E5 from a loaded font that has it: IPAGothic' $T/root/logs/boot.log && ! grep -q 'failed to find any fallback' $T/root/logs/boot.log" "Japanese (and a snowman), which Selawik and the folder's first font lack: taken from the folder's font that has them"
expect "$REGION $T/dump/flip00060.ppm 150 65 700 220 0 255 0 40 800 >/dev/null" "... and drawn (green, top left)"
nosan "$T"
# idle: a paused film with nothing happening is drawn twice a second; 320 frames fit in 250 flips
T=$(newroot t21b)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
rc=$(MPCPS5_HOST_IDLE=1 MAXFLIPS=250 run "$T" "0:0" "20:cmd pause;300:mouse 400,400;320:quit" "" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q '\[ui\] idle: the window drawn twice a second' $T/root/logs/boot.log" "paused, nothing pressed: idle (320 frames in under 250 flips)"
expect "grep -q '\[ui\] active again: drawn at every refresh' $T/root/logs/boot.log" "the pointer moved: the full rate at once"
nosan "$T"
fi

if want 22; then
echo "== 22. 1.3: the Default Style's font from the fonts folder, colours, the opaque box; the controller's Keys page"
MONO=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
MONOB=/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf
srt22() { printf '1\r\n00:00:00,000 --> 00:00:09,000\r\n%s\r\n' "$2" >"$1/root/media/movie.srt"; cp "$MEDIA/grey.mp4" "$1/root/media/movie.mp4"; }
# a: an SRT in a monospaced font from the folder (20 i's: as wide as 20 M's, about three times Selawik's), its bold face
T=$(newroot t22)
mkdir -p $T/root/fonts && cp $MONO $T/root/fonts/aa-mono.ttf && cp $MONOB $T/root/fonts/bb-mono-bold.ttf
srt22 $T "iiiiiiiiiiiiiiiiiiii"
ini "$T" "SubFont=aa-mono.ttf"
rc=$(run "$T" "0:0" "60:state;80:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'Default Style font: DejaVu Sans Mono (aa-mono.ttf); bold: bb-mono-bold.ttf' $T/root/logs/boot.log" "SubFont=aa-mono.ttf read: DejaVu Sans Mono, Bold from the folder's bold face of the family"
expect "$REGION $T/dump/flip00060.ppm 640 750 790 977 255 255 255 30 150 >/dev/null && $REGION $T/dump/flip00060.ppm 1130 750 1280 977 255 255 255 30 150 >/dev/null" "the line drawn in it: 20 i's reach x 640..1280 (monospaced)"
expect "grep -q '^SubFont=aa-mono.ttf$' $T/root/mpc-hc.ini" "kept in mpc-hc.ini"
nosan "$T"
# b: a font no longer in the folder: Selawik (logged), the same line far narrower
T=$(newroot t22b)
srt22 $T "iiiiiiiiiiiiiiiiiiii"
ini "$T" "SubFont=gone.ttf"
rc=$(run "$T" "0:0" "60:state;80:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q \"the Default Style's font gone.ttf isn't in the fonts folder: Selawik\" $T/root/logs/boot.log" "a missing font file: Selawik, logged"
expect "$REGION $T/dump/flip00060.ppm 860 750 1060 977 255 255 255 30 150 >/dev/null && ! $REGION $T/dump/flip00060.ppm 640 750 790 977 255 255 255 30 20 >/dev/null" "... the line drawn in Selawik (narrow: nothing at x 640..790)"
nosan "$T"
# c: a font copied into the folder while the app runs: listed when Options opens, chosen (Right x2: Selawik, Roboto,
# DejaVu Sans Mono), OK; synthetic bold (no bold face of it in the folder)
T=$(newroot t22c)
mkdir -p $T/root/fonts
srt22 $T "iiiiiiiiiiiiiiiiiiii"
( for i in $(seq 1 100); do grep -q 'fonts folder: 0 font(s) loaded' $T/root/logs/boot.log 2>/dev/null && break; sleep 0.1; done; cp $MONO $T/root/fonts/mono.ttf ) &
CP=$!
P="200:cmd options"
for f in 210 213 216 219 222 225; do P="$P;$f:key down"; done
rc=$(run "$T" "0:0" "$P;235:key r1;240:key right;243:key right;250:key menu;290:state;300:quit" "247,290" "$T/root/media/movie.mp4")
wait $CP
expect "[ $rc = 0 ] && grep -q 'Default Style fonts: 1 in .*/fonts (1 new)' $T/root/logs/boot.log" "a font copied in after the start: read when Options opens"
expect "grep -q 'Default Style font: DejaVu Sans Mono (mono.ttf); bold: synthetic' $T/root/logs/boot.log && grep -q '^SubFont=mono.ttf$' $T/root/mpc-hc.ini" "chosen in Default Style, OK: drawn in it (synthetic bold), saved"
expect "$REGION $T/dump/flip00290.ppm 640 750 790 977 255 255 255 30 150 >/dev/null" "... the line in the new font at once"
nosan "$T"
# d: colours: yellow text, a thick blue outline; then an opaque red box
T=$(newroot t22d)
srt22 $T "Coloured line"
ini "$T" "SubColor=FFFF00
SubOutlineColor=0000FF
SubOutline=4"
rc=$(run "$T" "0:0" "60:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && $REGION $T/dump/flip00060.ppm 300 750 1620 977 255 255 0 40 1500 >/dev/null && $REGION $T/dump/flip00060.ppm 300 750 1620 977 0 0 255 40 1500 >/dev/null && ! $REGION $T/dump/flip00060.ppm 300 750 1620 977 255 255 255 30 50 >/dev/null" "SubColor / SubOutlineColor: yellow text, blue outline (no white)"
T=$(newroot t22e)
srt22 $T "Boxed line"
ini "$T" "SubBorderStyle=1
SubOutlineColor=FF0000
SubBoxOpacity=100
SubShadow=0"
rc=$(run "$T" "0:0" "60:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && $REGION $T/dump/flip00060.ppm 300 750 1620 977 255 0 0 30 15000 >/dev/null && $REGION $T/dump/flip00060.ppm 300 750 1620 977 255 255 255 30 1000 >/dev/null" "Opaque box: white text on a red box behind the line"
expect "! $REGION $T/dump/flip00060.ppm 300 750 1620 977 0 0 0 20 50 >/dev/null" "... no outline (black) around the letters"
nosan "$T"
# f: Keys: R2 = subtitle delay + (the interval 200 ms) chosen on the page; R2: the delay and MPC-HC's OSD; Default puts
# R2's 20 s jump back (Options opens again at Keys, the page it was left on, as MPC-HC)
T=$(newroot t22f)
srt22 $T "Delay line"
ini "$T" "SubDelayInterval=200"
P="30:cmd options;40:key down;50:key r1"
f=55; for i in $(seq 1 11); do P="$P;$f:key down"; f=$((f + 3)); done
P="$P;95:key r1"
f=100; for i in $(seq 1 14); do P="$P;$f:key right"; f=$((f + 3)); done
P="$P;150:key menu;165:key r2;200:cmd options;215:key r1;220:key r1;225:key r1;230:key enter;235:key menu;250:key r2;270:quit"
rc=$(run "$T" "0:0" "$P" "92,170" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q '\[ui\] R2: SubDelayUp' $T/root/logs/boot.log && grep -q '\[osd\] Subtitle delay: 200 ms' $T/root/logs/boot.log" "Keys: R2 set to Subtitle delay +; R2 adds the interval (200 ms), the OSD says so"
expect "$REGION $T/dump/flip00170.ppm 150 65 800 140 224 224 224 20 150 >/dev/null" "... the OSD on the picture"
# (1.5.3: the list 16 dialog units shorter, for the text under it: the row 33 pixels higher)
expect "$REGION $T/dump/flip00092.ppm 790 542 1400 566 204 232 255 4 8000 >/dev/null" "the Keys page drawn, R2's row selected (scrolled to)"
expect "grep -q '\[ui\] R2: JumpForwardLarge' $T/root/logs/boot.log && grep -q '^KeyR2=JumpForwardLarge$' $T/root/mpc-hc.ini && grep -q '^SubDelay=200$' $T/root/mpc-hc.ini" "Default: R2 jumps 20 s again (saved); the delay kept"
nosan "$T"
# g: the keys from mpc-hc.ini: Triangle = next audio track (MPC-HC's OSD "Audio: ..."), Square = next subtitle (off, on);
# L2 + Left: 20 s back while L2 keeps its default; remapped L2 acts by itself and Left jumps 5 s
T=$(newroot t22g)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyTriangle=NextAudio
KeySquare=NextSubtitle"
rc=$(run "$T" "0:0;50:$TRIANGLE;52:0;80:$SQUARE;82:0;100:$SQUARE;102:0;130:100;135:180;137:100;140:0" "40:state;70:state;120:seek 8;150:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \"\$(field $T 1 5)\" = 2 ] && [ \"\$(field $T 2 5)\" = 1 ] && grep -q '\[osd\] Audio: English' $T/root/logs/boot.log" "Triangle = Next audio track: stream 2 -> 1, the OSD says Audio: English..."
expect "grep -q '\[ui\] next subtitle: off' $T/root/logs/boot.log && grep -q '\[osd\] Subtitle: off' $T/root/logs/boot.log && grep -q '\[ui\] next subtitle: 3' $T/root/logs/boot.log" "Square = Next subtitle: off, then the track again"
expect "grep -q '\[ui\] D-pad Left: JumpBackLarge' $T/root/logs/boot.log" "L2 held + Left (L2 at its default): 20 s back, as before"
T=$(newroot t22h)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyL2=SubDelayDown"
rc=$(run "$T" "0:0;50:100;55:180;57:100;60:0" "40:seek 8;80:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && grep -q '\[ui\] L2: SubDelayDown' $T/root/logs/boot.log && grep -q '\[ui\] D-pad Left: JumpBack$' $T/root/logs/boot.log && grep -q '\[osd\] Subtitle delay: -100 ms' $T/root/logs/boot.log" "L2 remapped: it does its own action (delay -100 ms), Left jumps 5 s"
nosan "$T"
fi

if want 23; then
echo "== 23. 1.3 review: damaged fonts left out (no crash), Bold's face by weight, the stick's and the triggers' repeats, Debug logs"
SANS=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
SERIF=/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf
srt23() { printf '1\r\n00:00:00,000 --> 00:00:09,000\r\n%s\r\n' "$2" >"$1/root/media/movie.srt"; cp "$MEDIA/grey.mp4" "$1/root/media/movie.mp4"; }
# a: truncated files (an unfinished copy), a name table past the end, a .ttc face far outside the file, a tiny .ttc,
# junk; a font named C#Mono.ttf; a partial copy finished while the app runs. Options opened twice.
T=$(newroot t23)
mkdir -p $T/root/fonts
python3 tests/make_test_fonts.py bad $T/root/fonts $SANS
cp /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf "$T/root/fonts/C#Mono.ttf"
head -c 300000 $SERIF >$T/root/fonts/zz-partial.ttf
srt23 $T "A line over damaged fonts"
ini "$T" "SubFont=aa-cut681.ttf"
( for i in $(seq 1 200); do grep -q 'Default Style fonts:' $T/root/logs/boot.log 2>/dev/null && break; sleep 0.1; done
  sleep 1.2; cp $SERIF $T/root/fonts/zz-partial.ttf ) &
CP=$!
rc=$(run "$T" "0:0" "60:state;70:cmd options;80:key down;83:key down;86:key down;89:key down;92:key down;95:key down;110:key escape;260:cmd options;275:key escape;300:quit" "60" "$T/root/media/movie.mp4")
wait $CP
expect "[ $rc = 0 ] && ! grep -q 'runtime error\|AddressSanitizer\|SEGV' $T/out.txt" "the app runs on: no crash, no sanitizer report"
for f in aa-cut681.ttf ab-cut100.ttf ac-cut5000.ttf ad-cut-half.ttf ae-namepast.ttf ah-tiny.ttc ai-junk.otf zz-partial.ttf; do
	expect "[ \$(grep -c '\[fonts\] $f left out' $T/root/logs/boot.log) = 1 ]" "$f: left out, logged once (not read again when Options opens)"
done
expect "grep -q '\[fonts\] ag-badoff.ttc#1: face 1 refused' $T/root/logs/boot.log" "ag-badoff.ttc: its face far outside the file refused..."
expect "grep -q \"the Default Style's font aa-cut681.ttf isn't in the fonts folder: Selawik\" $T/root/logs/boot.log && $REGION $T/dump/flip00060.ppm 400 750 1520 977 255 255 255 30 1000 >/dev/null" "the damaged file chosen in the settings: Selawik, the line drawn"
expect "grep -q 'Default Style fonts: 3 in .*(0 new)' $T/root/logs/boot.log" "listed: ag-badoff.ttc's good face, af-namestr.ttf (its names unread), C#Mono.ttf"
expect "grep -q 'Default Style fonts: 4 in .*(1 new)' $T/root/logs/boot.log" "zz-partial.ttf copied whole meanwhile: read again (a new size), listed"
# b: Bold from the family's faces: Bold (700, the bold bit), not SemiBold (first by name), ExtraBold or Black;
# #RRGGBB colours read
T=$(newroot t23b)
mkdir -p $T/root/fonts
python3 tests/make_test_fonts.py weights $T/root/fonts $SANS
srt23 $T "Weights"
ini "$T" "SubFont=a-regular.ttf
SubColor=#FFFF00"
rc=$(run "$T" "0:0" "60:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'Default Style font: DejaVu Sans (a-regular.ttf); bold: d-bold.ttf' $T/root/logs/boot.log" "Bold: d-bold.ttf (700, marked bold), not b-semibold, e-extrabold or c-black"
expect "$REGION $T/dump/flip00060.ppm 400 750 1520 977 255 255 0 40 1000 >/dev/null" "SubColor=#FFFF00 (as the page shows a colour not in its palette): yellow"
nosan "$T"
# c: a saved font not in the folder: kept when Options is closed with OK
T=$(newroot t23c)
ini "$T" "SubFont=gone.ttf"
rc=$(run "$T" "0:0" "30:cmd options;45:key menu;70:quit" "")
expect "[ $rc = 0 ] && grep -q '^SubFont=gone.ttf$' $T/root/mpc-hc.ini" "SubFont=gone.ttf kept through Options, OK"
# d: the right stick as Play/Pause: pushed gently and held (a wheel step every half second), it acts once; a second
# push acts again
T=$(newroot t23d)
srt23 $T "Stick"
ini "$T" "KeyStickUp=PlayPause"
rc=$(run "$T" "0:0;60:0,128,128,128,90;200:0,128,128,128,128;260:0,128,128,128,0;275:0,128,128,128,128" "320:quit" "" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && [ \$(grep -c '\[osd\] Pause$\|\[osd\] Play$' $T/root/logs/boot.log) = 2 ] && [ \$(grep -c '\[ui\] Right stick up: PlayPause' $T/root/logs/boot.log) = 2 ]" "a held push is one press: Pause, then Play for the second push"
nosan "$T"
# e: L2 as subtitle delay + repeats while held (100 ms each)
T=$(newroot t23e)
srt23 $T "Trigger"
ini "$T" "KeyL2=SubDelayUp"
rc=$(run "$T" "0:0;50:100;150:0" "170:quit" "" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && [ \$(sed -n 's/^SubDelay=//p' $T/root/mpc-hc.ini) -ge 500 ]" "L2 = Subtitle delay +, held 1.6 s: it repeats (delay \$(sed -n 's/^SubDelay=//p' $T/root/mpc-hc.ini) ms)"
# f: Left as volume down: L2 (its default 20 s jump) pressed while Left is held acts, not swallowed as a modifier
T=$(newroot t23f)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyLeft=VolumeDown"
rc=$(run "$T" "0:0;50:80;55:180;60:80;65:0" "40:seek 5;90:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && grep -q '\[ui\] D-pad Left: VolumeDown' $T/root/logs/boot.log && grep -q '\[ui\] L2: JumpBackLarge' $T/root/logs/boot.log" "Left = Volume down held, L2 pressed: L2 jumps back 20 s"
# g: Play > Subtitle Track's delay items made from translated formats are not looked up again
T=$(newroot t23g)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "Language=1"
rc=$(MPCPS5_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0" "30:key menu;40:key right;50:key right;60:key escape;65:key escape;80:quit" "" "$T/root/media/chapters.mkv")
touch $T/missing.txt
expect "[ $rc = 0 ] && ! grep -q '^Retardo' $T/missing.txt" "Spanish: 'Retardo +100 ms' and 'Retardo: +0 ms' not translated twice"
# h: Options > Miscellaneous > Debug logs (off by default): turned on, saved as debug_logs=1, the log written at once
T=$(newroot t23h)
P="20:cmd options"
f=30; for i in $(seq 1 8); do P="$P;$f:key down"; f=$((f + 3)); done
rc=$(MPCPS5_LOGS_DEFAULT= run "$T" "0:0" "$P;60:key r1;63:key r1;66:key r1;70:key enter;80:key menu;100:cmd volup;120:quit" "75")
expect "[ $rc = 0 ] && grep -q '^debug_logs=1$' $T/root/mpc-hc.ini" "the Debug logs box on, OK: debug_logs=1 in mpc-hc.ini"
expect "grep -q '\[log\] debug logs turned on' $T/root/logs/boot.log && grep -q '\[osd\] Volume: 100%' $T/root/logs/boot.log && ! grep -q 'fonts folder:' $T/root/logs/boot.log" "boot.log written from then on, at once (nothing from before it was on)"
rc=$(MPCPS5_LOGS_DEFAULT= run "$T" "0:0" "20:quit" "")
expect "grep -q 'Media Player Classic PS5 .*FFmpeg' $T/root/logs/boot.log" "the next start: logged from the start"
nosan "$T"
# i: well-formed files with bad insides: a composite 'A' of 8 bytes made of itself, a simple 'A' whose points run past
# its data (both refused), a GPOS pair adjustment whose class records don't fit (the font kept, its GPOS kerning not)
for pick in bb-shortcomp.ttf bd-pointmatch.ttf be-lastoff.ttf ba-gpos2.ttf; do
	T=$(newroot t23i_${pick%%.*})
	mkdir -p $T/root/fonts
	python3 tests/make_test_fonts.py crafted $T/root/fonts $SANS
	srt23 $T "AVATAR WAVE To"
	ini "$T" "SubFont=$pick"
	rc=$(run "$T" "0:0" "60:state;70:cmd options;80:key escape;100:quit" "60" "$T/root/media/movie.mp4")
	expect "[ $rc = 0 ] && $REGION $T/dump/flip00060.ppm 400 750 1520 977 255 255 255 30 1000 >/dev/null" "SubFont=$pick: no crash, the line drawn"
	nosan "$T"
done
expect "grep -q 'bb-shortcomp.ttf: face 0 refused: a glyph shorter than its header' $T/root/logs/boot.log && grep -q 'bc-points.ttf: face 0 refused: a glyph whose points run past its data' $T/root/logs/boot.log" "the 8-byte composite and the simple glyph past its data: refused"
expect "grep -q 'bd-pointmatch.ttf: face 0 refused: a composite glyph that is too deep, made of itself' $T/root/logs/boot.log" "a point-matched component (no argument bytes) followed by 'A' itself: the walk reads what stb_truetype reads, refused"
expect "grep -q 'Default Style font: DejaVu Sans (be-lastoff.ttf)' $WORK/t23i_be-lastoff/root/logs/boot.log" "an 'A' of one off-curve point: the font used, drawn without reading past its points (no sanitizer report above)"
expect "grep -q 'ba-gpos2.ttf: face 0: its GPOS kerning left out' $T/root/logs/boot.log && grep -q 'Default Style font: DejaVu Sans (ba-gpos2.ttf)' $T/root/logs/boot.log" "the GPOS that doesn't fit: kerning from it left out, the font used"
fi

if want 24; then
echo "== 24. 1.3.1: Options > Audio Switcher > Normalize, Max amplification, Regain volume (MPC-HC's normalizer)"
# norm.mkv: a 440 Hz tone at 0.02 of full scale (RMS 463), a 0.5 s burst at 0.5 (RMS 11585) at 2 s, quiet again to 7.5 s.
# normstat dump channels channel -> "before burst after late rail max min": RMS 1.5..0.5 s before the burst, in it,
# 0.2..0.7 s and 4.0..4.5 s after it; flattened tops (two neighbours both at the 16-bit limit: a 440 Hz sine at full
# scale has none, a clipped one hundreds); the extremes
normstat() { python3 -c "
import array
a = array.array('h'); a.frombytes(open('$1', 'rb').read())
x = a[$3::$2]
R = 48000
loud = [i for i, v in enumerate(x) if abs(v) > 8000]
b0, b1 = loud[0], loud[-1]
rms = lambda s: round((sum(v * v for v in s) / max(1, len(s))) ** 0.5)
print(rms(x[b0 - 3 * R // 2:b0 - R // 2]), rms(x[b0 + R // 10:b1 - R // 10]), rms(x[b1 + R // 5:b1 + 7 * R // 10]),
      rms(x[b1 + 4 * R:b1 + 9 * R // 2]), sum(1 for i in range(1, len(x)) if abs(x[i]) >= 32767 and abs(x[i - 1]) >= 32767),
      max(x), min(x))"; }
nf() { echo "$1" | cut -d' ' -f$2; }
# startrms dump channels channel -> the RMS 20..350 ms after the first sound (the ring's ~340 ms filled at the start)
startrms() { python3 -c "
import array
a = array.array('h'); a.frombytes(open('$1', 'rb').read())
x = a[$3::$2]
s = [i for i, v in enumerate(x) if v != 0][0]
q = x[s + 960:s + 16800]
print(round((sum(v * v for v in q) / max(1, len(q))) ** 0.5))"; }
# a: Normalize off (the default): the sound as it is
T=$(newroot t24)
cp "$MEDIA/norm.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/off run "$T" "0:0" "560:quit" "" "$T/root/media/norm.mkv")
r=$(normstat $T/off.2ch 2 0)
expect "[ $rc = 0 ] && near $(nf "$r" 1) 463 12 && near $(nf "$r" 2) 11585 150 && near $(nf "$r" 3) 463 12 && near $(nf "$r" 4) 463 12" "Normalize off: unchanged (quiet RMS 463, the burst 11585): $r"
expect "grep -q '^AudioNormalize=0$' $T/root/mpc-hc.ini && grep -q '^AudioMaxNormFactor=400$' $T/root/mpc-hc.ini && grep -q '^AudioNormalizeRecover=1$' $T/root/mpc-hc.ini" "MPC-HC's defaults saved: AudioNormalize=0, AudioMaxNormFactor=400, AudioNormalizeRecover=1"
# b: on, Max amplification 400 %, Regain on: the quiet tone 4x; the burst brought to full scale, not clipped (its block's
# peak lowers the gain at once to 2x); after it the gain rises again (+6 %/s)
T=$(newroot t24b)
cp "$MEDIA/norm.mkv" "$T/root/media/"
ini "$T" "AudioNormalize=1"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/on run "$T" "0:0" "560:quit" "" "$T/root/media/norm.mkv")
r=$(normstat $T/on.2ch 2 0)
expect "[ $rc = 0 ] && near $(nf "$r" 1) 1852 40" "Normalize on: the quiet tone raised to the max amplification, 4x (RMS $(nf "$r" 1), 4 x 463 = 1852)"
r0=$(startrms $T/on.2ch 2 0)
expect "near $r0 1852 40" "... from the first block of the file: 20..350 ms after the sound starts already 4x (RMS $r0)"
expect "near $(nf "$r" 2) 23170 400 && [ $(nf "$r" 5) = 0 ] && [ $(nf "$r" 6) -le 32767 ]" "the loud burst after it: up to full scale (RMS $(nf "$r" 2), a sine at 32767: 23170), not clipped ($(nf "$r" 5) flattened tops)"
expect "near $(nf "$r" 3) 926 30 && python3 -c 'import sys; sys.exit(0 if $(nf "$r" 4) > $(nf "$r" 3) * 1.08 else 1)'" "Regain volume on: after the burst 2x (RMS $(nf "$r" 3)), rising again 4 s later (RMS $(nf "$r" 4))"
nosan "$T"
# c: Regain volume off: the gain only goes down
T=$(newroot t24c)
cp "$MEDIA/norm.mkv" "$T/root/media/"
ini "$T" "AudioNormalize=1
AudioNormalizeRecover=0"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/nr run "$T" "0:0" "560:quit" "" "$T/root/media/norm.mkv")
r=$(normstat $T/nr.2ch 2 0)
expect "[ $rc = 0 ] && near $(nf "$r" 1) 1852 40 && near $(nf "$r" 3) 926 30 && near $(nf "$r" 4) $(nf "$r" 3) 10" "Regain volume off: 4x, then 2x after the burst and it stays there (RMS $(nf "$r" 3), then $(nf "$r" 4))"
# d: Max amplification 200 %; and 5000 in the file is held to 1000 (MPC-HC's range 100..1000)
T=$(newroot t24d)
cp "$MEDIA/norm.mkv" "$T/root/media/"
ini "$T" "AudioNormalize=1
AudioMaxNormFactor=200"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/m2 run "$T" "0:0" "300:quit" "" "$T/root/media/norm.mkv")
r=$(normstat $T/m2.2ch 2 0)
r0=$(startrms $T/m2.2ch 2 0)
expect "[ $rc = 0 ] && near $(nf "$r" 1) 926 25 && near $r0 926 25 && grep -q '^AudioNormalize=1$' $T/root/mpc-hc.ini && grep -q '^AudioMaxNormFactor=200$' $T/root/mpc-hc.ini" "Max amplification 200 %: the quiet tone 2x from the start (RMS $r0, then $(nf "$r" 1)); read and saved back"
ini "$T" "AudioMaxNormFactor=5000
AudioNormalizeRecover=0"
rc=$(run "$T" "0:0" "20:quit" "")
expect "grep -q '^AudioMaxNormFactor=1000$' $T/root/mpc-hc.ini && grep -q '^AudioNormalizeRecover=0$' $T/root/mpc-hc.ini" "AudioMaxNormFactor=5000 held to 1000; AudioNormalizeRecover=0 kept"
# e: surround: the 7.1 file's left side channel on the 8-channel port, one gain for all channels
T=$(newroot t24e)
cp "$MEDIA/norm71.mkv" "$T/root/media/"
ini "$T" "AudioOutput=1
AudioNormalize=1"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/s run "$T" "0:0" "330:quit" "" "$T/root/media/norm71.mkv")
r=$(normstat $T/s.8ch 8 4)
r0=$(startrms $T/s.8ch 8 4)
expect "near $r0 1852 40" "8-channel output: 4x from the first block too (RMS $r0)"
expect "[ ! -f $T/s.2ch ] || python3 -c \"import array, sys; a = array.array('h'); a.frombytes(open('$T/s.2ch', 'rb').read()); sys.exit(0 if not any(a) else 1)\"" "the film's start not played on the stereo port first (Surround kept for the new file, nothing dropped at the switch)"
expect "[ $rc = 0 ] && [ -f $T/s.8ch ] && near $(nf "$r" 1) 1852 40 && near $(nf "$r" 2) 23170 400 && [ $(nf "$r" 5) = 0 ] && near $(nf "$r" 3) 926 30" "8-channel output: Ls 4x, the burst to full scale unclipped, then 2x ($r)"
expect "python3 -c \"import sys; r = list(map(int, '\$(rms $T/s.8ch 8)'.split())); sys.exit(0 if max(r[:4] + r[5:]) < 50 else 1)\"" "the other seven channels stay silent"
nosan "$T"
# f: the page in each language (English, Spanish, Portuguese (Brazil), French, German): Options, Audio Switcher (7 down
# the tree), R1 to Normalize, Cross: on (Max amplification and Regain volume enabled); R1, Right x2: 420 %; R1, Cross:
# Regain off; OK. Then on again with a file playing: it applies at once.
for L in 0 1 2 3 4; do
	T=$(newroot t24f_$L)
	ini "$T" "Language=$L"
	P="20:cmd options"
	f=30; for i in $(seq 1 7); do P="$P;$f:key down"; f=$((f + 3)); done
	P="$P;60:key r1;66:key enter;72:key r1;75:key right;78:key right;84:key r1;90:key enter;100:key menu;120:quit"
	rc=$(MPCPS5_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0" "$P" "58,96")
	expect "[ $rc = 0 ] && grep -q '^AudioNormalize=1$' $T/root/mpc-hc.ini && grep -q '^AudioMaxNormFactor=420$' $T/root/mpc-hc.ini && grep -q '^AudioNormalizeRecover=0$' $T/root/mpc-hc.ini" "language $L: Normalize on, 420 %, Regain volume off, OK: saved"
	expect "! grep -q 'Normalize\|amplification\|Regain' $T/missing.txt 2>/dev/null || [ $L = 0 ]" "language $L: the three texts translated"
	nosan "$T"
done
T=$(newroot t24g)
cp "$MEDIA/norm.mkv" "$T/root/media/"
P="5:cmd options"
f=8; for i in $(seq 1 7); do P="$P;$f:key down"; f=$((f + 3)); done
P="$P;29:key r1;32:key enter;35:key menu;300:quit"
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/live run "$T" "0:0" "$P" "" "$T/root/media/norm.mkv")
# where it took effect: the block the player logs (its time in the stream; the dump's first sound is the stream's
# start). In 50 ms windows: as it is (463) before that, 4x (1852) after it up to the burst -- or, when the pad's way
# through Options took so long that it came at the burst, the burst brought to full scale (Normalize at work), not
# left as it is (11585). (The dump is in the stream's order: what was already queued is before that block.)
on=$(sed -n 's/.*\[player\] Normalize on from \([0-9.]*\) s of the stream.*/\1/p' $T/root/logs/boot.log | head -1)
expect "[ $rc = 0 ] && [ -n \"$on\" ] && python3 -c \"
import array, sys
a = array.array('h'); a.frombytes(open('$T/live.2ch', 'rb').read())
x = a[0::2]
R = 48000
s = [i for i, v in enumerate(x) if v != 0][0]
b0 = [i for i, v in enumerate(x) if abs(v) > 8000][0]
b1 = [i for i, v in enumerate(x) if abs(v) > 8000][-1]
on = s + int($on * R)
rms = lambda q: (sum(v * v for v in q) / max(1, len(q))) ** 0.5
before = [rms(x[i:i + 2400]) for i in range(s, min(on, b0) - 2400, 2400)]
after = [rms(x[i:i + 2400]) for i in range(on + 2400, b0 - 7200, 2400)]  # not the burst's block: its peak lowers the gain
burst = rms(x[b0 + R // 10:b1 - R // 10])
ok = len(before) >= 2 and all(abs(r - 463) < 15 for r in before)
ok = ok and (all(abs(r - 1852) < 40 for r in after) if after else abs(burst - 23170) < 400)
print([round(r) for r in before], [round(r) for r in after], round(burst)); sys.exit(0 if ok else 1)\" >$T/live.txt" "turned on while a file plays: as it is up to the block it took effect at ($on s), 4x from there on"
nosan "$T"
fi

if want 25; then
echo "== 25. 1.4: disc drives (the PS5's own, USB): data discs, DVD-Video, Blu-ray; encrypted discs refused, nothing decrypted"
# disc images in the drives (MPCPS5_HOST_DISC: cd0 the console's own, cd1... USB; a missing file: an empty drive)
log() { grep -q -- "$2" "$1/root/logs/boot.log"; }
# a: the Open dialog: the drives with their discs' names; a data disc (UDF) listed; a file on it plays and seeks
T=$(newroot t25)
ini "$T" "LastDir=disc://cd1/Filmes"
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd.iso,$MEDIA/data-udf.iso,$WORK/no-disc.iso run "$T" "0:0" "30:cmd quickopen;60:key enter;150:state;160:seek 4;230:state;250:quit" "55,140,185")
expect "[ $rc = 0 ] && log $T '\[ui\] file browser: Disc drive (TEST_DVD) (disc://cd0)' && log $T '\[ui\] file browser: USB disc drive (MPC DATA) (disc://cd1)' && log $T '\[ui\] file browser: USB disc drive (No disc) (disc://cd2)'" "the Open dialog's navigation pane: Disc drive (TEST_DVD), USB disc drive (MPC DATA), USB disc drive (No disc)"
expect "log $T '\[disc\] cd1: \"MPC DATA\", UDF, ' && log $T '\[ui\] file browser at disc://cd1/Filmes: chapters.mkv$'" "the data disc read (UDF) and its folder listed: chapters.mkv"
expect "log $T '\[disc\] opened disc://cd1/Filmes/chapters.mkv' && [ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00140.ppm 960 400 red >/dev/null" "Cross: it plays from the disc (red)"
expect "near \$(field $T 2 2) \$(at $T 3.83 160 230) 1 && $CHECK $T/dump/flip00185.ppm 960 400 green >/dev/null" "it seeks: 4 s, green"
nosan "$T"
# b: ISO 9660 + Joliet: the long, accented name as Joliet keeps it
T=$(newroot t25b)
rc=$(MPCPS5_HOST_DISC=$MEDIA/data-joliet.iso run "$T" "0:0" "30:open disc://cd0/Ação - um nome longo demais para o ISO 9660.mp4;100:state;110:open disc://cd0/Filmes/chapters.mkv;170:seek 7;230:state;240:quit" "95,225")
expect "[ $rc = 0 ] && log $T '\[disc\] cd0: \"MPC_JOLIET\", ISO 9660 + Joliet, ' && [ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00095.ppm 960 400 red >/dev/null" "ISO 9660 + Joliet: the file with a long, accented name plays (red)"
expect "near \$(field $T 2 2) 7.9 1 && $CHECK $T/dump/flip00225.ppm 960 400 blue >/dev/null" "another file, seeking to 7 s: blue"
nosan "$T"
# c: a DVD-Video: File > Open Disc plays the longest title (2 of 2) with its chapters and audio tracks; Next goes to the next
# chapter (early: within a second of the next chapter Next skips it, as MPC-HC), the Portuguese track, a seek,
# Navigate > Titles: title 1 (white)
T=$(newroot t25c)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd.iso run "$T" "0:0" "30:cmd disc0;58:state;60:cmd next;130:state;180:cmd 2002;220:state;230:seek 6.5;300:state;310:cmd title1;390:state;400:quit" "55,125,295,385")
expect "[ $rc = 0 ] && log $T 'DVD-Video mpcdisc:cd0:[0-9]*: 2 title(s), playing title 2 (9.0 s), the longest' && log $T 'disc title 2 of 2: 3 chapter(s); #1 audio eng, #2 audio por'" "File > Open Disc: the DVD's main title (the longest, 2 of 2): 3 chapters, audio English and Portuguese"
expect "[ \"\$(field $T 1 1)\" = 2 ] && near \$(field $T 1 3) 9 0.1 && [ \"\$(field $T 1 5)\" = 1 ] && $CHECK $T/dump/flip00055.ppm 960 400 red >/dev/null" "it plays: 9 s, the first audio track, red"
expect "near \$(field $T 2 2) \$(at $T 3.23 60 130) 1.3 && $CHECK $T/dump/flip00125.ppm 960 400 green >/dev/null" "Next: chapter 2 (3 s, green)"
expect "[ \"\$(field $T 3 5)\" = 2 ]" "Play > Audio Track: the Portuguese one"
expect "near \$(field $T 4 2) 7.6 1.2 && $CHECK $T/dump/flip00295.ppm 960 400 blue >/dev/null" "a seek to 6.5 s: blue"
expect "log $T 'playing title 1 (3.0 s)' && near \$(field $T 5 3) 3 0.1 && $CHECK $T/dump/flip00385.ppm 960 400 255 255 255 12 >/dev/null" "Navigate > Titles > Title 1: the other title (3 s, white)"
nosan "$T"
# d: a Blu-ray (BDMV): libbluray reads it through the sector reader; its main title, chapters, the tracks' languages from
# the playlist; Next, a seek
T=$(newroot t25d)
rc=$(MPCPS5_HOST_DISC=$MEDIA/bd.iso run "$T" "0:0" "30:cmd disc0;58:state;60:cmd next;130:state;180:seek 6.5;250:state;260:quit" "55,125,245")
expect "[ $rc = 0 ] && log $T '\[disc\] cd0: \"TEST_BD\", UDF, .*: Blu-ray$' && log $T 'Blu-ray title 1 of 1: playlist 00000.mpls, 9.0 s, 3 chapters' && log $T 'disc title 1 of 1: 3 chapter(s); #1 audio eng, #2 audio por'" "File > Open Disc: the Blu-ray's title: 3 chapters, audio English and Portuguese (from the playlist)"
expect "[ \"\$(field $T 1 1)\" = 2 ] && near \$(field $T 1 3) 9 0.1 && $CHECK $T/dump/flip00055.ppm 960 400 red >/dev/null" "it plays: 9 s, red"
expect "near \$(field $T 2 2) \$(at $T 3.23 60 130) 1.3 && $CHECK $T/dump/flip00125.ppm 960 400 green >/dev/null" "Next: chapter 2 (green)"
expect "near \$(field $T 3 2) 7.6 1.2 && $CHECK $T/dump/flip00245.ppm 960 400 blue >/dev/null" "a seek to 6.5 s: blue"
nosan "$T"
# e: encrypted discs: a DVD whose packs are marked scrambled (CSS) and a Blu-ray with AACS: recognised, refused with the
# message; nothing opened; the disc libraries can't load anything at run time (no libdvdcss / libaacs / libbdplus)
T=$(newroot t25e)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd-css.iso,$MEDIA/bd-aacs.iso run "$T" "0:0" "30:cmd disc0;60:key enter;80:cmd disc1;110:key enter;130:quit" "55")
expect "[ $rc = 0 ] && log $T '\[disc\] cd0: \"CSS_DVD\", UDF, .*: encrypted - CSS' && log $T '\[disc\] cd1: \"AACS_BD\", UDF, .*: encrypted - AACS'" "recognised: the DVD (scrambled packs: CSS), the Blu-ray (AACS)"
expect "[ \$(grep -c '\[ui\] message: This disc is encrypted (a commercial film) and can.t be played here' $T/root/logs/boot.log) = 2 ] && ! log $T '\[player\] opened'" "both refused with the message; nothing played"
expect "! nm -u $PWD/build/deps/prefix-host/lib/libdvdread.a $PWD/build/deps/prefix-host/lib/libdvdnav.a $PWD/build/deps/prefix-host/lib/libbluray.a $PWD/build/deps/prefix-host/lib/libudfread.a 2>/dev/null | grep -qw 'dlopen\|dvdcss_open\|aacs_open'" "the disc libraries refer to no dlopen, libdvdcss or libaacs"
nosan "$T"
# f: the disc taken out while it plays: stopped, and said so; put back: read again
T=$(newroot t25f)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd.iso run "$T" "0:0" "30:cmd disc0;100:state;110:eject cd0;150:state;160:key enter;170:insert cd0;220:quit" "")
expect "[ $rc = 0 ] && [ \"\$(field $T 1 1)\" = 2 ] && log $T '\[ui\] the disc of dvd://cd0 was taken out: stopped' && log $T '\[ui\] message: The disc was taken out: playback stopped.' && [ \"\$(field $T 2 1)\" = 0 ]" "taken out while playing: playback stopped, the message shown"
expect "[ \$(grep -c '\[disc\] cd0: \"TEST_DVD\"' $T/root/logs/boot.log) = 2 ]" "put back: read again"
nosan "$T"
# g: an empty drive: "No disc"
T=$(newroot t25g)
ini "$T" "LastDir=disc://cd0"
rc=$(MPCPS5_HOST_DISC=$WORK/no-disc.iso run "$T" "0:0" "30:cmd quickopen;50:key escape;60:open dvd://cd0;90:key enter;100:quit" "")
expect "[ $rc = 0 ] && log $T '\[ui\] file browser at disc://cd0: No disc' && log $T '\[ui\] message: Failed to open \"cd0\":' && log $T '\[player\] failed: No disc'" "an empty drive: the Open dialog says No disc; playing it: No disc"
nosan "$T"
# h: an early seek back (0.5 s, a second in): a DVD title's first audio packet after the seek has no timestamp; the
# clock went back to where it was before the seek (1.5 s). The same on a Blu-ray title and a file
for m in dvd bd file; do
T=$(newroot t25h-$m)
if [ $m = file ]; then
	rc=$(run "$T" "0:0" "30:open $MEDIA/chapters.mkv;90:state;92:seek 0.5;93:state;130:state;140:quit" "")
else
	rc=$(MPCPS5_HOST_DISC=$MEDIA/$m.iso run "$T" "0:0" "30:cmd disc0;90:state;92:seek 0.5;93:state;130:state;140:quit" "")
fi
expect "[ $rc = 0 ] && [ \"\$(field $T 1 1)\" = 2 ] && near \$(field $T 2 2) 0.5 0.3 && near \$(field $T 3 2) \$(at $T 0.5 92 130) 0.5" "$m: a seek back to 0.5 s a second in: it plays on from 0.5 s"
nosan "$T"
done
fi

if want 26; then
echo "== 26. 1.4 review: damaged and hostile discs, a drive that fails or is slow, encrypted films as files, ISO 9660 DVDs"
DT="env ASAN_OPTIONS=detect_leaks=0 $PWD/build/host/mpc-disc-test"
log() { grep -q -- "$2" "$1/root/logs/boot.log"; }
# a: one byte of VIDEO_TS.IFO changed (a scratch): libdvdread freed a command table twice on that error path
r=$($DT title $MEDIA/dvd-damaged.iso dvd://cd0 2>&1)
expect "! echo \"\$r\" | grep -q AddressSanitizer && echo \"\$r\" | grep -q '^open 1 '" "a damaged IFO: no double free (libdvdread patched), the titles read from the backup"
T=$(newroot t26)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd-damaged.iso run "$T" "0:0" "30:open dvd://cd0;90:key enter;100:quit" "")
expect "[ $rc = 0 ] && log $T '\\[ui\\] message: Failed to open'" "... and the app says it can't open it, and goes on"
nosan "$T"
# b: root directories inside themselves (64 MiB each): read in part, quickly, in little memory; the other drive still
# watched; quitting doesn't wait
r1=$($DT mount $MEDIA/loop1.iso 5 300 2>&1); r2=$($DT mount $MEDIA/loop2.iso 5 300 2>&1)
expect "echo \"\$r1\" | grep -q '^mounted 1 kind 2 cut 1 ' && echo \"\$r2\" | grep -q '^mounted 1 kind 2 cut 1 '" "a directory in itself (one record over 32 sectors; two records): read in part in under 5 s and 300 MiB, Stop() at once ($(echo $r1 | grep -o 'in [0-9.]* s.*'); $(echo $r2 | grep -o 'in [0-9.]* s.*'))"
T=$(newroot t26b)
rc=$(MPCPS5_HOST_DISC=$MEDIA/loop2.iso,$MEDIA/dvd.iso run "$T" "0:0" "60:quit" "")
expect "[ $rc = 0 ] && log $T 'cd0: its file system was read in part' && log $T 'cd1: \"TEST_DVD\"'" "in the app: the hostile disc read in part (logged), the other drive's disc read too, quit at once"
nosan "$T"
# c: the drive fails, slowly, while the read-ahead runs: Stop() returns (a disc was let go of under the drives' lock)
expect "$DT stop-failing $MEDIA/data-big.iso disc://cd0/big.bin >/dev/null 2>&1" "the drive failing during read-ahead: Stop() returns at once"
# d: a slow drive (1.5 s a read): a read gives up as soon as it is cancelled; a part that can't be read isn't tried again
expect "$DT cancel $MEDIA/data-big.iso disc://cd0/big.bin >/dev/null 2>&1" "a read on a slow drive gives up at once when cancelled"
expect "$DT negcache $MEDIA/data-big.iso disc://cd0/big.bin >/dev/null 2>&1" "an unreadable part is read once (kept as unreadable: read-ahead doesn't retry it)"
T=$(newroot t26d)
sec=$($DT sector $MEDIA/data-udf.iso disc://cd0/Filmes/chapters.mkv 2>/dev/null)
rc=$(MPCPS5_HOST_DISC=$MEDIA/data-udf.iso MPCPS5_HOST_DISC_FAULT=slow=2500,from=$sec run "$T" "0:0" "30:open disc://cd0/Filmes/chapters.mkv;60:cmd close;61:state;100:quit" "")
expect "[ $rc = 0 ] && python3 -c \"
import re, sys
t = {int(m.group(2)): float(m.group(1)) for m in re.finditer(r'\\[ *([0-9.]+)\\] \\[test\\] frame ([0-9]+):', open('$T/root/logs/boot.log').read())}
print(t[61] - t[60]); sys.exit(0 if t[61] - t[60] < 0.6 else 1)\" >$T/close.txt" "File > Close while the drive takes 2.5 s a read: the window waits $(cat $T/close.txt 2>/dev/null) s"
nosan "$T"
# e: an encrypted disc's films as files (File > Open File, Recent Files): refused with the message too
r=$($DT open $MEDIA/dvd-css.iso disc://cd0/VIDEO_TS/VTS_01_1.VOB 2>&1); r2=$($DT open $MEDIA/bd-aacs.iso disc://cd0/BDMV/STREAM/00000.m2ts 2>&1)
expect "echo \"\$r\" | grep -q '^This disc is encrypted' && echo \"\$r2\" | grep -q '^This disc is encrypted'" "VTS_01_1.VOB of the CSS DVD, 00000.m2ts of the AACS Blu-ray: refused as files"
T=$(newroot t26e)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd-css.iso run "$T" "0:0" "30:open disc://cd0/VIDEO_TS/VTS_01_1.VOB;60:key enter;70:quit" "")
expect "[ $rc = 0 ] && log $T '\\[ui\\] message: This disc is encrypted (a commercial film)' && ! log $T '\\[player\\] opened'" "... in the app: the message, nothing played"
nosan "$T"
expect "$DT swap $MEDIA/dvd.iso dvd://cd0 >/dev/null 2>&1" "a disc swapped between finding its title and opening it: the new one isn't read as the old"
# f: title VOBs that don't read (a scratched disc, a drive that won't): "can't be read", not "encrypted"
sec=$($DT sector $MEDIA/dvd.iso disc://cd0/VIDEO_TS/VTS_01_1.VOB 2>/dev/null)
r=$(MPCPS5_HOST_DISC_FAULT=fail=1,from=$sec $DT mount $MEDIA/dvd.iso 5 300 2>&1)
expect "echo \"\$r\" | grep -q \"kind 6 cut 0 damaged 1 protection ''\"" "unreadable title VOBs: unreadable (scratched?), not called encrypted"
T=$(newroot t26f)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd.iso MPCPS5_HOST_DISC_FAULT=fail=1,from=$sec run "$T" "0:0" "30:cmd disc0;31:open dvd://cd0;60:key enter;70:quit" "")
expect "[ $rc = 0 ] && log $T \"\\[ui\\] message: .*This disc can't be read: it is scratched or damaged, or it is an encrypted film\\.\"" "... and the app says so"
nosan "$T"
# g: a DVD-Video on an ISO 9660-only disc (no UDF): libdvdread reads it as a folder of files through the player
T=$(newroot t26g)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd-iso.iso run "$T" "0:0" "30:cmd disc0;90:state;100:quit" "85")
expect "[ $rc = 0 ] && log $T 'cd0: \"ISO_DVD\", ISO 9660, .*: DVD-Video' && log $T 'disc title 2 of 2: 3 chapter(s)' && [ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00085.ppm 960 400 red >/dev/null" "an ISO 9660-only DVD-Video plays its main title with its chapters"
nosan "$T"
# h: a fragmented UDF file (its runs checked block by block); ISO 9660: a file in two extents (flag 0x80), and A.TXT;1 /
# A.TXT;2 as two files
expect "$DT read $MEDIA/frag.iso disc://cd0/FRAG.BIN $MEDIA/red.mp4 >/dev/null 2>&1" "a fragmented UDF file (blocks 0-9, 12- in line, 10-11 elsewhere) reads as itself"
r=$($DT list $MEDIA/multi.iso disc://cd0 2>&1)
expect "[ \"\$(echo \"\$r\" | grep -c '^A.TXT ')\" = 2 ] && echo \"\$r\" | grep -q '^BIG.BIN 10340$' && $DT read $MEDIA/multi.iso disc://cd0/BIG.BIN $MEDIA/multi.iso.big >/dev/null 2>&1" "ISO 9660: A.TXT;1 and A.TXT;2 two files, BIG.BIN's two extents one file (read whole)"
fi

if want 27; then
echo "== 27. 1.4 re-check: a drive that doesn't say the disc's size, deep and looping trees, // paths, UDF budgets, files in many pieces, a hung drive"
DT="env ASAN_OPTIONS=detect_leaks=0 $PWD/build/host/mpc-disc-test"
log() { grep -q -- "$2" "$1/root/logs/boot.log"; }
# a: no size from the drive (5S2CD's DVD structure / TOC / DIOCGMEDIASIZE all failing): the discs read all the same
NS="env MPCPS5_HOST_DISC_FAULT=nosize=1"
r=$($NS $DT mount $MEDIA/dvd.iso 5 300 2>&1; $NS $DT mount $MEDIA/dvd-iso.iso 5 300 2>&1; $NS $DT mount $MEDIA/data-big.iso 5 300 2>&1; $NS $DT mount $MEDIA/data-bigiso.iso 5 300 2>&1)
expect "[ \"\$(echo \"\$r\" | grep -c '^mounted 1 kind 3 ')\" = 2 ] && [ \"\$(echo \"\$r\" | grep -c '^mounted 1 kind 2 ')\" = 2 ]" "no size from the drive: the DVD-Videos (UDF, ISO 9660-only) and the data discs (UDF, Joliet) are read"
r=$($NS $DT title $MEDIA/dvd.iso dvd://cd0 2>&1; $NS $DT title $MEDIA/dvd-iso.iso dvd://cd0 2>&1)
expect "[ \"\$(echo \"\$r\" | grep -c '^ffmpeg 0 packets 783$')\" = 2 ] && $NS $DT read $MEDIA/data-big.iso disc://cd0/big.bin $MEDIA/data-big.ref >/dev/null 2>&1 && $NS $DT read $MEDIA/data-bigiso.iso disc://cd0/big.bin $MEDIA/data-big.ref >/dev/null 2>&1" "... the titles play to their end, the files read whole"
T=$(newroot t27a)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd.iso MPCPS5_HOST_DISC_FAULT=nosize=1 run "$T" "0:0" "30:cmd disc0;90:state;100:quit" "85")
expect "[ $rc = 0 ] && log $T \"the drive doesn't say the disc's size\" && log $T 'disc title 2 of 2: 3 chapter(s)' && [ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00085.ppm 960 400 red >/dev/null" "... in the app: File > Open Disc plays the DVD's main title"
nosan "$T"
# b: a branch too deep (a/d1/.../d26) or inside itself (ISO 9660: A is the root): passed over, the rest read
r=$($DT list $MEDIA/deep-udf.iso disc://cd0/z 2>&1; $DT list $MEDIA/deep-iso.iso disc://cd0/z 2>&1; $DT list $MEDIA/isoloop.iso disc://cd0/Z 2>&1)
expect "[ \"\$(echo \"\$r\" | grep -ci '^after.txt ')\" = 3 ]" "a too deep branch (UDF, Joliet), a directory inside itself (ISO 9660): only that branch passed over, z/after.txt listed"
# c: an encrypted disc's film with // or ./ in its path: refused as well
r=$($DT open $MEDIA/dvd-css.iso disc://cd0//VIDEO_TS/VTS_01_1.VOB 2>&1; $DT open $MEDIA/dvd-css.iso disc://cd0/VIDEO_TS//VTS_01_1.VOB 2>&1; $DT open $MEDIA/dvd-css.iso disc://cd0/./video_ts/VTS_01_1.VOB 2>&1)
expect "[ \"\$(echo \"\$r\" | grep -c '^This disc is encrypted')\" = 3 ]" "disc://cd0//VIDEO_TS/..., .../VIDEO_TS//..., ./video_ts/...: refused on the CSS DVD"
# d: UDF directories within the budgets: one over 4 MiB passed over (its sibling read), directories inside themselves
# stop at 32 MiB read
r=$($DT list $MEDIA/udfbig.iso disc://cd0/Z 2>&1; $DT list $MEDIA/udfbig.iso disc://cd0/D 2>&1)
expect "echo \"\$r\" | grep -q '^AFTER.TXT ' && ! echo \"\$r\" | grep -q '^A 0 dir'" "a UDF directory of 4.8 MB: passed over (not read), its sibling Z read"
r=$(DISC_TEST_LOG=1 $DT mount $MEDIA/udfloop.iso 5 300 2>&1)
expect "echo \"\$r\" | grep -q '^mounted 1 kind 2 cut 1 ' && echo \"\$r\" | grep -q 'entries, 327[0-9][0-9] KiB of directories'" "UDF directories inside themselves (800 KB each): read up to 32 MiB, in under 5 s and 300 MiB ($(echo $r | grep -o 'in [0-9.]* s, stop.*'))"
# e: a UDF file of 60000 blocks each in an extent of its own: mapped once, read whole quickly
r=$($DT scan $MEDIA/manyext.iso disc://cd0/MANY.BIN 5 2>&1)
expect "echo \"\$r\" | grep -q \"^read 122880000 of 122880000 bytes in .*, 'A' 61440000, 'B' 61440000\$\"" "a UDF file in 60000 pieces: read whole, in order, in under 5 s ($(echo $r | grep -o 'in [0-9.]* s'))"
# f: File > Load Subtitle on a slow drive (2.5 s a read): the UI waits 1 s at most here (5 s in the app); a drive that
# doesn't answer (a FIFO no one writes to): Stop() waits 2 s, not forever
expect "$DT ui-read $MEDIA/data-big.iso disc://cd0/big.bin >/dev/null 2>&1" "a disc file read from the UI thread on a slow drive: given up in time"
rm -f "$WORK/hung.fifo" && mkfifo "$WORK/hung.fifo"
expect "timeout 20 $DT stop-hung $WORK/hung.fifo >/dev/null 2>&1" "a drive that doesn't answer: Stop() (the app's exit) returns in 2 s"
rm -f "$WORK/hung.fifo"
# g: a DVD whose main title's PGC can't be read (UDF; ISO 9660-only): libdvdnav (patched) refuses to play it instead
# of following a NULL PGC; the app says the disc can't be read
r=$($DT title $MEDIA/dvd-nullpgc.iso dvd://cd0 2>&1; $DT title $MEDIA/dvd-nullpgc-iso.iso dvd://cd0 2>&1)
expect "! echo \"\$r\" | grep -q AddressSanitizer && [ \"\$(echo \"\$r\" | grep -c '^ffmpeg -[0-9]* packets 0$')\" = 2 ]" "a DVD-Video whose main title's PGC can't be read (UDF, ISO 9660-only): no crash, FFmpeg's open refused"
T=$(newroot t27g)
rc=$(MPCPS5_HOST_DISC=$MEDIA/dvd-nullpgc.iso run "$T" "0:0" "30:cmd disc0;60:key enter;70:quit" "")
expect "[ $rc = 0 ] && grep -a -A1 '\\[ui\\] message: Failed to open \"NULLPGC\"' $T/root/logs/boot.log | grep -q \"This disc can't be read: it is scratched or damaged, or it is an encrypted film\\.\" && ! log $T '\\[player\\] opened'" "... in the app: the damaged-disc message"
nosan "$T"
# h: an allocation extent descriptor pointing at itself (BDMV/index.bdmv): libbluray's open of the disc ends
r=$(timeout 30 $DT title $MEDIA/aedloop.iso bluray://cd0 2>&1)
expect "echo \"\$r\" | grep -q '^open 0 '" "UDF allocation extents in a loop (libudfread patched): the Blu-ray open ends, refused"
# i: the size: a DVD's physical end, or the medium's size when larger (a recordable DVD); the TOC's; the medium's
expect "$DT size - >/dev/null 2>&1" "the disc's size from the drive's answers: the larger of the DVD structure's and the medium's"
fi

if want 28; then
echo "== 28. 1.5 (Sulfurico-One64, #1): Options and the right stick bindable, speed and Options keys, the touchpad, moving dialogs"
log() { grep -q -- "$2" "$1/root/logs/boot.log"; }
# a: Options given "Options: Keys": pressed and let go it opens Options at Keys; held half a second it still opens
# the menu bar (nobody locks themselves out)
T=$(newroot t28a)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyOptions=OptionsKeys"
rc=$(run "$T" "0:0;40:$OPTIONS;44:0;100:$OPTIONS;160:0" "90:key escape;200:quit" "60,150" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && log $T '\[ui\] Options: OptionsKeys' && log $T '\[ui\] Options at \"Keys\"'" "Options = Options: Keys: a press opens Options at the Keys page"
expect "$REGION $T/dump/flip00060.ppm 790 330 1400 360 255 255 255 2 5000 >/dev/null" "... the Keys page drawn (its list)"
expect "log $T '\[ui\] Options held: the menu bar' && $REGION $T/dump/flip00150.ppm 10 70 240 580 240 240 240 6 50000 >/dev/null" "held half a second: the menu bar (File open) all the same"
nosan "$T"
# b: an old mpc-hc.ini whose [Keys] hasn't the new rows: their defaults (Options the menu bar, the right stick's left /
# right 1 s jumps), kept when saved; the right stick left held repeats, right pushed once jumps once
T=$(newroot t28b)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyL2=SubDelayDown"
rc=$(run "$T" "0:0;60:0,128,128,0,128;100:0,128,128,128,128;130:0,128,128,255,128;133:0,128,128,128,128;150:$OPTIONS;152:0" "50:seek 6;55:state;120:state;145:state;175:key escape;180:quit" "170" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && grep -q '^KeyOptions=MenuBar$' $T/root/mpc-hc.ini && grep -q '^KeyStickLeft=JumpBackSmall$' $T/root/mpc-hc.ini && grep -q '^KeyStickRight=JumpForwardSmall$' $T/root/mpc-hc.ini && grep -q '^KeyL2=SubDelayDown$' $T/root/mpc-hc.ini" "an old [Keys]: the new rows at their defaults (saved), L2 as it was"
expect "log $T '\[ui\] Right stick left: JumpBackSmall' && python3 -c \"import sys; a, b = float('\$(field $T 1 2)'), float('\$(field $T 2 2)'); sys.exit(0 if a - b > 2.5 else 1)\"" "the right stick left held: 1 s jumps, repeating ($(field $T 1 2) s -> $(field $T 2 2) s)"
expect "log $T '\[ui\] Right stick right: JumpForwardSmall' && python3 -c \"import sys; a, b = float('\$(field $T 2 2)'), float('\$(field $T 3 2)'); sys.exit(0 if 0.5 < b - a < 2.0 else 1)\"" "the right stick right pushed once: one 1 s jump"
expect "$REGION $T/dump/flip00170.ppm 10 70 240 580 240 240 240 6 50000 >/dev/null" "Options (its default): the menu bar at once"
nosan "$T"
# c: the speed: the right stick left / right as Slower / Faster, L3 as Normal speed: MPC-HC's steps (half, twice) and
# its OSD ("Speed: 2.00x"), as Play > Increase / Decrease / Reset Rate; under 0.125x it pauses
T=$(newroot t28c)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyStickLeft=Slower
KeyStickRight=Faster
KeyL3=NormalSpeed"
P="0:0;40:0,128,128,255,128;42:0,128,128,128,128;50:0,128,128,255,128;52:0,128,128,128,128;60:2;62:0"
f=70; for i in 1 2 3 4; do P="$P;$f:0,128,128,0,128;$((f + 2)):0,128,128,128,128"; f=$((f + 8)); done
rc=$(run "$T" "$P" "58:state;68:state;110:state;120:cmd faster;125:state;130:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && log $T '\[osd\] Speed: 2.00x' && log $T '\[osd\] Speed: 4.00x' && log $T '\[ui\] Right stick right: Faster'" "Faster: 2x, 4x (MPC-HC's OSD)"
expect "log $T '\[ui\] L3: NormalSpeed' && log $T '\[osd\] Speed: 1.00x' && log $T '\[osd\] Speed: 0.50x' && log $T '\[osd\] Speed: 0.12x'" "Normal speed: 1x; Slower: 0.5x ... 0.125x"
expect "[ \"\$(field $T 3 1)\" = 3 ] && log $T '\[osd\] Speed: 0.25x'" "Slower under 0.125x: paused (as MPC-HC)"
expect "[ \"\$(field $T 4 1)\" = 2 ]" "Play > Increase Rate (the menu's command) plays again, at 0.25x"
nosan "$T"
# d: the touchpad: a finger moves the pointer (to the corner, then onto File), its click clicks there: the File menu;
# with the touchpad off (Options > Player > Keys) the same strokes do nothing
PAD=$(python3 - <<'PY'
steps = ["0:0"]
f = 40
for k in range(3):
    for i in range(16):
        steps.append("%d:0,128,128,128,128,%d,%d" % (f, 1800 - i * 100, 1000 - i * 60)); f += 1
    steps.append("%d:0" % f); f += 3
x, y = 500, 500
steps.append("%d:0,128,128,128,128,%d,%d" % (f, x, y)); f += 1
for i in range(10):
    x += 2; y += 4
    steps.append("%d:0,128,128,128,128,%d,%d" % (f, x, y)); f += 1
steps.append("%d:100000,128,128,128,128,%d,%d" % (f + 5, x, y))
steps.append("%d:0,128,128,128,128,%d,%d" % (f + 10, x, y))
steps.append("%d:0" % (f + 12))
print(";".join(steps))
PY
)
T=$(newroot t28d)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "$PAD" "200:quit" "150" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && $REGION $T/dump/flip00150.ppm 10 70 240 580 240 240 240 6 50000 >/dev/null" "the touchpad moved the pointer onto File, its click opened the menu"
T=$(newroot t28d2)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "TouchpadPointer=0"
rc=$(run "$T" "$PAD" "200:quit" "150" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && ! $REGION $T/dump/flip00150.ppm 10 70 240 580 240 240 240 6 50000 >/dev/null && grep -q '^TouchpadPointer=0$' $T/root/mpc-hc.ini" "the touchpad off: nothing moved or clicked"
nosan "$T"
# e: Options dragged by its title bar (Cross held, the stick): it moves, stops at the screen's edge, opens there next
# time (mpc-hc.ini); its controls still work where it is (Show OSD clicked off, OK)
T=$(newroot t28e)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0;60:$CROSS;62:$CROSS,0,128;100:$CROSS,128,128;102:0" "40:cmd options;50:mouse 700,187;120:mouse 609,348;125:click;140:key menu;160:cmd options;190:key escape;200:quit" "110,180" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && log $T '\[ui\] \"Options\" moved to 0,' && grep -q '^OptionsX=0$' $T/root/mpc-hc.ini" "dragged left by its title bar: it stops at the screen's edge (kept in mpc-hc.ini)"
expect "$REGION $T/dump/flip00110.ppm 0 200 40 800 240 240 240 6 2000 >/dev/null && $REGION $T/dump/flip00180.ppm 0 200 40 800 240 240 240 6 2000 >/dev/null" "... drawn there, and opened there again"
expect "grep -q '^ShowOSD=0$' $T/root/mpc-hc.ini" "... its controls work where it is (Show OSD clicked off, OK)"
nosan "$T"
# f: the touchpad pressed while the pointer is hidden (where it was hidden: on File): the first press only shows it;
# the next one clicks
TP="0,128,128,128,128,900,500"; TPD="100000,128,128,128,128,900,500"
T=$(newroot t28f)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0;300:$TP;303:$TPD;306:$TP;309:0;340:$TP;343:$TPD;346:$TP;349:0" "30:mouse 25,50;390:quit" "330,370" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && ! $REGION $T/dump/flip00330.ppm 10 70 240 580 240 240 240 6 50000 >/dev/null" "the pointer hidden: a touchpad press only shows it (File, where it was hidden, not clicked)"
expect "$REGION $T/dump/flip00370.ppm 10 70 240 580 240 240 240 6 50000 >/dev/null" "... the next press clicks there: the File menu"
nosan "$T"
# g: a finger rolling a little as it presses the touchpad (28 units, quickly) doesn't move the pointer off a small
# button (Options' OK: closed); a longer stroke while pressed still drags (Options by its title bar)
T=$(newroot t28g)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0;60:$TP;63:$TPD;64:100000,128,128,128,128,900,528;67:0,128,128,128,128,900,528;70:0" "40:cmd options;50:mouse 1132,884;100:quit" "55,90" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && $REGION $T/dump/flip00055.ppm 500 600 700 800 255 255 255 6 30000 >/dev/null && ! $REGION $T/dump/flip00090.ppm 500 600 700 800 255 255 255 6 30000 >/dev/null" "a touchpad click on OK with the finger rolling: OK clicked (Options closed)"
T=$(newroot t28g2)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
P="0:0;60:$TP;63:$TPD"; for i in $(seq 1 10); do P="$P;$((63 + i)):100000,128,128,128,128,$((900 + 20 * i)),$((500 + 10 * i))"; done
rc=$(run "$T" "$P;80:0,128,128,128,128,1100,600;82:0" "40:cmd options;50:mouse 700,187;100:key escape;110:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && log $T '\[ui\] \"Options\" moved to'" "... a stroke while pressed on its title bar drags Options"
nosan "$T"
# h: a window opened over Options while its title bar is held (About): Options doesn't stay stuck to the pointer
T=$(newroot t28h)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0;60:$CROSS;90:0;100:0,255,128;115:0" "40:cmd options;50:mouse 700,187;70:cmd about;95:key escape;130:click;150:quit" "125" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && log $T 'cmd about' && $REGION $T/dump/flip00125.ppm 500 600 700 800 255 255 255 6 30000 >/dev/null && ! log $T '\[ui\] \"Options\" moved to'" "a window opened while Options' title bar is held: Options doesn't follow the pointer afterwards"
nosan "$T"
# i: Options (given Options: Keys) held while a window opens (About): no menu bar over it
T=$(newroot t28i)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeyOptions=OptionsKeys"
rc=$(run "$T" "0:0;40:$OPTIONS;140:0" "50:cmd about;160:key escape;170:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && log $T 'cmd about' && ! log $T 'Options held: the menu bar'" "Options held while a window opens: no menu bar over it"
nosan "$T"
# j: Faster from Stopped plays (MPC-HC's SetPlayingRate: not running, it plays)
T=$(newroot t28j)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0" "40:cmd stop;50:state;60:cmd faster;70:state;80:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \"\$(field $T 1 1)\" = 4 ] && [ \"\$(field $T 2 1)\" = 2 ] && log $T '\[osd\] Speed: 2.00x'" "Faster when stopped: it plays, at 2x"
nosan "$T"
# k: audio without any timestamp (nopts.vob: the tone starts with the green picture, at 2 s): it plays from the
# start, in step with the picture (when the tone is heard vs when the position passes 2 s; 1.5's first fix dropped
# the first 32 frames, so the audio played 0.77 s early)
T=$(newroot t28k)
SC=""; for f in $(seq 30 3 240); do SC="$SC$f:state;"; done
rc=$(MPCPS5_HOST_AUDIO_DUMP=$T/a run "$T" "0:0" "${SC}250:quit" "" "$MEDIA/nopts.vob")
avs() { python3 - "$1" <<'PY'
import sys, re, array
T = sys.argv[1]
log = open(T + '/root/logs/boot.log', errors='replace').read().splitlines()
t_open, pts = None, []
for l in log:
    m = re.match(r'\[\s*([0-9.]+)\] \[audio\] sceAudioOutOpen', l)
    if m and t_open is None:
        t_open = float(m.group(1))
    m = re.match(r'\[\s*([0-9.]+)\] \[test\] state 2 pos ([0-9.]+)', l)
    if m:
        pts.append((float(m.group(1)), float(m.group(2))))
# the wall time the position passed 2 s
w2 = next(w0 + (w1 - w0) * (2 - p0) / (p1 - p0) for (w0, p0), (w1, p1) in zip(pts, pts[1:]) if p0 < 2 <= p1)
a = array.array('h')
a.frombytes(open(T + '/a.2ch', 'rb').read())
L = a[0::2]
on = next(i for i in range(0, len(L) - 480, 480) if sum(x * x for x in L[i:i + 480]) / 480 > 1000 ** 2)
print('%+.3f' % (t_open + on / 48000 - w2))
PY
}
expect "[ $rc = 0 ] && python3 -c \"import sys; sys.exit(0 if -0.35 <= \$(avs $T) <= 0.35 else 1)\"" "audio with no timestamps: in step with the picture"
nosan "$T"
fi

if want 29; then
echo "== 29. 1.5.1 (#3): the eboot imports only what a title may load (no libkernel_sys / libkernel_web); 1.5.2: its own C++ runtime, the Makefile's dependencies"
SDKLIB=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}/target/lib
if [ -f build/app/mpc-pie.elf ] && [ -d "$SDKLIB" ]; then
	# 1.6: the GPU build (RADV) also loads AGC's two modules, as the Makefile's check allows
	AGC=""
	grep -q 'libvulkan_radeon\.ps5\.a(' build/app/mpc-pie.trace 2>/dev/null && AGC="$SDKLIB/libSceAgc.so $SDKLIB/libSceAgcDriver.so"
	python3 tools/check_imports.py build/app/mpc-pie.elf "$SDKLIB" build/app/stubs/libSceVideodec2.so $AGC >"$WORK/imports.txt" 2>&1
	rc=$?
	expect "[ $rc = 0 ] && grep -q '^check_imports: ok' $WORK/imports.txt && ! grep -q 'libkernel_sys\|libkernel_web' $WORK/imports.txt" "the app's PIE (build/app/mpc-pie.elf): only allowed modules and symbols"
	# 1.5.2: the eboot's C++ runtime is its own: its __gxx_personality_v0 defined (not imported), every CIE's
	# personality slot relocated to it, and __eh_frame_hdr_start = .eh_frame_hdr (libunwind finds the FDEs by it)
	if command -v llvm-dwarfdump-18 >/dev/null && command -v llvm-readelf-18 >/dev/null; then
		P=build/app/mpc-pie.elf
		pers=$(llvm-readelf-18 -sW $P | awk '$NF=="__gxx_personality_v0" && $2 != "0000000000000000" {print $2}' | sed 's/^0*//' | head -1)
		slots=$(llvm-dwarfdump-18 --eh-frame $P 2>/dev/null | awk '/Personality Address/ {print $3}' | sed 's/^0*//' | sort -u)
		hdr=$(llvm-readelf-18 -SW $P | awk '$2==".eh_frame_hdr" {print $4}' | sed 's/^0*//')
		ok=1
		[ -n "$pers" ] && [ -n "$slots" ] || ok=0
		for s in $slots; do llvm-readelf-18 -rW $P | grep -qi "^0*$s .*R_X86_64_RELATIVE *0*$pers\$" || ok=0; done
		llvm-readelf-18 -sW $P | awk '$NF=="__eh_frame_hdr_start" {print $2}' | grep -qi "0*$hdr\$" || ok=0
		expect "[ $ok = 1 ] && grep -q '^check_imports: ok' $WORK/imports.txt && ! grep -q 'the C++ runtime' $WORK/imports.txt" "the eboot's C++ runtime is its own: __gxx_personality_v0 at 0x$pers, every CIE's personality slot ($(echo $slots | tr ' ' ,)) relocated to it, __eh_frame_hdr_start = .eh_frame_hdr"
		# 1.5.2: the console's operator new throws std::bad_alloc when an allocation fails (1.5.1's trapped: ud2 right
		# after malloc's null, SIGILL, so the disc reader's catch (const std::bad_alloc&) could never run): a __cxa_throw
		# of std::bad_alloc's type in app_cpp_runtime.o, and the eboot's own __cxa_throw
		O=build/app/app_cpp_runtime.o
		ok=1
		llvm-objdump-18 -dr $O | grep -q '__cxa_throw' || ok=0
		llvm-objdump-18 -dr $O | grep -q '_ZTISt9bad_alloc' || ok=0
		llvm-readelf-18 -sW $P | awk '$NF=="__cxa_throw" && $7!="UND"' | grep -q . || ok=0
		expect "[ $ok = 1 ]" "operator new on the console: a failed allocation throws std::bad_alloc (no trap), the eboot's own __cxa_throw"
		# 1.5.2: the CRT shaders inherited from the emulator ports (never called by the player, their licences not in
		# the README) are out of the eboot
		expect "! llvm-readelf-18 -sW $P | grep -q '_ZN6ps5crt'" "no CRT shader code (ps5crt) in the eboot: the player never drew with it"
	fi
else
	echo "  (skipped: build/app/mpc-pie.elf or the SDK isn't there -- make ps5 first)"
fi
# the check itself: a PIE calling statfs (as 1.5 did) is refused, naming the symbol and libkernel_sys
if [ -d "$SDKLIB" ] && command -v ld.lld >/dev/null && command -v clang >/dev/null; then
	mkdir -p "$WORK/imp"
	printf 'int statfs(const char*, void*);\nint _start(void) { return statfs("/", 0); }\n' >"$WORK/imp/bad.c"
	clang -fPIC -c -o "$WORK/imp/bad.o" "$WORK/imp/bad.c" &&
		ld.lld -pie --no-dynamic-linker -e _start -o "$WORK/imp/bad.elf" "$WORK/imp/bad.o" --as-needed "$SDKLIB/libkernel.so" "$SDKLIB/libkernel_sys.so"
	python3 tools/check_imports.py "$WORK/imp/bad.elf" "$SDKLIB" >"$WORK/imp/out.txt" 2>&1
	rc=$?
	expect "[ $rc = 1 ] && grep -q 'NEEDED libkernel_sys.sprx: not a module the app may load' $WORK/imp/out.txt && grep -q 'import statfs: not in an allowed module (only in libkernel_sys.so)' $WORK/imp/out.txt" "a PIE importing statfs (1.5's crash): refused, naming libkernel_sys"
	# 1.5.2: a PIE taking the C++ personality routine from the system (the eboot up to 1.5.1, Stella PS5 1.0's crash
	# starting a game): refused
	printf 'extern char __gxx_personality_v0;\nvoid* p = &__gxx_personality_v0;\nint _start(void) { return p != 0; }\n' >"$WORK/imp/cxx.c"
	clang -fPIC -c -o "$WORK/imp/cxx.o" "$WORK/imp/cxx.c" &&
		ld.lld -pie --no-dynamic-linker -e _start -o "$WORK/imp/cxx.elf" "$WORK/imp/cxx.o" --as-needed "$SDKLIB/libkernel.so" "$SDKLIB/libSceLibcInternal.so"
	python3 tools/check_imports.py "$WORK/imp/cxx.elf" "$SDKLIB" >"$WORK/imp/cxx.txt" 2>&1
	rc=$?
	expect "[ $rc = 1 ] && grep -q 'import __gxx_personality_v0: the C++ runtime .* must be the eboot.s own' $WORK/imp/cxx.txt" "a PIE importing the C++ personality routine from the system: refused"
	# and one importing __dynamic_cast (1.5.1 did: the dialogs' dynamic_casts ran on the console's copy): refused too
	printf 'extern char __dynamic_cast;\nvoid* p = &__dynamic_cast;\nint _start(void) { return p != 0; }\n' >"$WORK/imp/dyn.c"
	clang -fPIC -c -o "$WORK/imp/dyn.o" "$WORK/imp/dyn.c" &&
		ld.lld -pie --no-dynamic-linker -e _start -o "$WORK/imp/dyn.elf" "$WORK/imp/dyn.o" --as-needed "$SDKLIB/libkernel.so" "$SDKLIB/libSceLibcInternal.so"
	python3 tools/check_imports.py "$WORK/imp/dyn.elf" "$SDKLIB" >"$WORK/imp/dyn.txt" 2>&1
	rc=$?
	expect "[ $rc = 1 ] && grep -q 'import __dynamic_cast: the C++ runtime .* must be the eboot.s own' $WORK/imp/dyn.txt" "a PIE importing __dynamic_cast from the system: refused"
else
	echo "  (skipped: no SDK, ld.lld or clang for the check's own test)"
fi
# 1.5.2: the objects follow what they are built from (make -n -W: what would be rebuilt, nothing built): mpc_app.o the CA
# list it carries, mpc_res.o the fonts, every object the flags (VERSION...); up to 1.5.1 the first two were recipe-less
# pattern rules, which add no prerequisite
make -n -W assets/data/cacert.pem build/host/port/player/mpc_app.o >"$WORK/mk1.txt" 2>&1
make -n -W assets/fonts/Roboto-Regular.ttf build/host/port/player/mpc_res.o >"$WORK/mk2.txt" 2>&1
make -n -W build/host/flags.txt build/host/port/player/mpc_ui.o >"$WORK/mk3.txt" 2>&1
expect "grep -q -- '-o build/host/port/player/mpc_app.o player/mpc_app.cpp' $WORK/mk1.txt && grep -q -- '-o build/host/port/player/mpc_res.o player/mpc_res.cpp' $WORK/mk2.txt && grep -q -- '-o build/host/port/player/mpc_ui.o player/mpc_ui.cpp' $WORK/mk3.txt" "the Makefile: mpc_app.o rebuilt for a new CA list, mpc_res.o for a new font, every object for new flags"
fi

if want 30; then
echo "== 30. 1.5.2: bitmap subtitles kept within a budget; a font's huge glyph box; a screenshot's name; a huge cover; passwords off the screen"
# a: an MKV with a Blu-ray (PGS) track, a 1000x200 picture every second for 60 s (800 KB each, 48 MB in all): with a
# 4 MiB budget (the console's is 256 MiB) the pictures read ahead at the start are kept (all within 30 s), a seek
# to 50 s lets go those of the lines that ended 30 s before it, and a seek back to 5 s those that start 30 s after it;
# the line at 50 s still shows
T=$(newroot t30)
python3 tests/make_pgs.py $T/subs.sup 60 1.0 0.98 1000 200
ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=25:d=61 -c:v libx264 -preset ultrafast -pix_fmt yuv420p $T/v.mp4 &&
	ffmpeg -v error -i $T/v.mp4 -i $T/subs.sup -map 0 -map 1 -c copy $T/root/media/pgs.mkv
rc=$(MPCPS5_HOST_SUB_BUDGET_MB=4 run "$T" "0:0" "40:state;60:seek 50;150:state;160:seek 5;220:state;230:quit" "140" "$T/root/media/pgs.mkv")
expect "[ $rc = 0 ] && grep -q 'opened .*pgs.mkv' $T/root/logs/boot.log && grep -q '\[subs\] bitmap subtitles over 4 MiB ([0-9]* MiB), all within 30 s' $T/root/logs/boot.log" "PGS: over the budget at the start, every picture within 30 s of the newest: kept"
expect "grep -q '\[subs\] bitmap subtitles over 4 MiB ([0-9]* MiB): [0-9]* picture(s) of lines over 30 s from 5[0-9]\.[0-9] s let go' $T/root/logs/boot.log" "a seek 50 s on: the pictures of the lines that ended long before let go"
expect "grep -q '\[subs\] bitmap subtitles over 4 MiB ([0-9]* MiB): [0-9]* picture(s) of lines over 30 s from [0-9]\.[0-9] s let go' $T/root/logs/boot.log" "a seek back to 5 s: those of the lines far ahead let go too (decoded again when the film gets there)"
expect "$REGION $T/dump/flip00140.ppm 500 700 1420 977 255 255 255 30 2000 >/dev/null" "the line at 50 s drawn (a white box at the bottom)"
nosan "$T"
# b: a font in the fonts folder whose 'A' says its box is 65535 units wide and high, at 16 units per em: not drawn
# (its bitmap would be gigabytes), the app runs on; its line height and advances (DejaVu's, in units of 1/16 em: 128
# times too big) are held to a few em, or each subtitle line's picture is hundreds of MB (the app stalled for minutes)
T=$(newroot t30b)
mkdir -p $T/root/fonts
python3 tests/make_test_fonts.py hugebox $T/root/fonts /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
printf '1\r\n00:00:00,000 --> 00:00:09,000\r\nAAA\r\n' >$T/root/media/movie.srt
cp "$MEDIA/grey.mp4" "$T/root/media/movie.mp4"
ini "$T" "SubFont=bf-hugebox.ttf"
rc=$(run "$T" "0:0" "60:state;70:quit" "60" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && ! grep -q 'runtime error\|AddressSanitizer\|SEGV\|allocation' $T/out.txt && grep -q '\[fonts\] glyph [0-9]* of bf-hugebox.ttf: its box ([0-9]*x[0-9]* at [0-9]* px) is far bigger than the font: not drawn' $T/root/logs/boot.log" "a glyph whose box is far bigger than the font: not drawn, no crash"
expect "grep -q '\[fonts\] bf-hugebox.ttf says its lines are [0-9]* px high at [0-9]* px: taken as' $T/root/logs/boot.log && [ \"\$(field $T 1 1)\" = 2 ]" "its lines far taller than its size: taken at a few em, the film plays on"
# c: File > Save Image of a file named ..%2F..%2Fescaped.mp4 (FileName decodes %2F): the picture stays in screenshots/,
# under a name that doesn't start with a dot (the glob below skips hidden files)
T=$(newroot t30c)
cp "$MEDIA/red.mp4" "$T/root/media/..%2F..%2Fescaped.mp4"
rc=$(run "$T" "0:0" "60:cmd saveimage;80:quit" "" "$T/root/media/..%2F..%2Fescaped.mp4")
expect "[ $rc = 0 ] && ls $T/root/screenshots/*escaped_snapshot_*.png >/dev/null 2>&1 && [ -z \"\$(find $T -name '*escaped_snapshot*' -not -path '*/screenshots/*')\" ]" "Save Image of ..%2F..%2Fescaped.mp4: the PNG in screenshots/, nowhere above it"
# d: an MP3 whose cover says 8200 x 8200 (a small PNG that would take 2 x 256 MiB): not decoded, the logo instead
T=$(newroot t30d)
ffmpeg -v error -f lavfi -i color=c=blue:size=8200x8200 -frames:v 1 $T/big.png &&
	ffmpeg -v error -f lavfi -i sine=frequency=660:sample_rate=44100:duration=6 -i $T/big.png -map 0 -map 1 -c:a libmp3lame \
		-b:a 128k -c:v copy -disposition:v attached_pic $T/root/media/bigcover.mp3
rc=$(run "$T" "0:0" "40:state;50:quit" "35" "$T/root/media/bigcover.mp3")
expect "[ $rc = 0 ] && [ \"\$(field $T 1 1)\" = 2 ] && grep -q \"the cover picture ([0-9]* bytes) can't be read, or is over 8192 pixels a side\" $T/root/logs/boot.log && ! $CHECK $T/dump/flip00035.ppm 960 500 blue >/dev/null" "a cover over 8192 pixels a side: not decoded, the file plays (the logo shown)"
nosan "$T"
# e: an http:// address with a password in the Recent Files: the Open dialog shows it without the password (1.5.1
# hid it in smb:// addresses only), and OK opens it with its password (the log shows that one as ***)
T=$(newroot t30e)
printf '[Settings]\n[Recent File List]\nFile1=http://bob:s3cret@127.0.0.1:9/x.mp4\n' >$T/root/mpc-hc.ini
rc=$(MPCPS5_HOST_DRAWN=$T/drawn.txt run "$T" "0:0" "30:cmd open;60:key menu;120:quit" "")
touch $T/drawn.txt
expect "[ $rc = 0 ] && grep -qx 'http://bob@127.0.0.1:9/x.mp4' $T/drawn.txt && ! grep -q s3cret $T/drawn.txt" "an http:// password in the Recent Files: shown without it in the Open dialog"
expect "grep -q '\[ui\] open http://bob:\*\*\*@127.0.0.1:9/x.mp4' $T/root/logs/boot.log" "OK opens the address with its password"
nosan "$T"
fi

if want 31; then
echo "== 31. 1.5.2 review: a file's name without its address's user and password; threads the console refuses; .sup and VobSub files"
log() { grep -q -- "$2" "$1/root/logs/boot.log"; }
# a: an address with nothing past its host (http://bob:s3cret@127.0.0.1:9/) was named "bob:s3cret@127.0.0.1:9" by
# FileName: in the title bar, the failure message, the notification and boot.log. And smb://'s "Not a file on a share"
# message showed the whole address. Neither shows the password now (every text drawn is recorded on the host).
T=$(newroot t31a)
rc=$(MPCPS5_HOST_DRAWN=$T/drawn.txt run "$T" "0:0" "20:open http://bob:s3cret@127.0.0.1:9/;80:key enter;100:open smb://bob:s3cret@127.0.0.1:9/share;180:key enter;200:quit" "")
touch $T/drawn.txt
expect "[ $rc = 0 ] && grep -q '127.0.0.1:9' $T/drawn.txt && grep -q 'Not a file on a share: smb://bob@127.0.0.1:9/share' $T/drawn.txt && ! grep -q s3cret $T/drawn.txt" "a failed http:// address and an smb:// one with no file: their passwords never drawn"
expect "! grep -q s3cret $T/root/logs/boot.log && ! grep -q s3cret $T/out.txt && grep -q 'Failed to open \"127.0.0.1:9\"' $T/root/logs/boot.log" "nor in boot.log or the notification (the name is the host)"
nosan "$T"
# b: threads the console refuses (MPCPS5_HOST_REFUSE_THREADS stands for it): each place gives up what needed one,
# says so, and the app runs on (1.5.1: std::terminate, the app gone). b1: the file's opener and the notification
T=$(newroot t31b1)
cp "$MEDIA/red.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_REFUSE_THREADS=opener,notify run "$T" "0:0" "60:key enter;80:quit" "" "$T/root/media/red.mp4")
expect "[ $rc = 0 ] && log $T '\[player\] a new thread refused: opener: ' && log $T 'failed: The console refused a new thread' && log $T '\[notify\] a new thread refused: notify: .*: sent from the caller' && grep -q '\[host\] notification: .*could not open red.mp4' $T/out.txt" "the opener refused: the file fails with a message, the notification sent by the caller"
nosan "$T"
# b2: the video thread (the demuxer already running): the file fails, the demuxer stops with it
T=$(newroot t31b2)
cp "$MEDIA/red.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_REFUSE_THREADS=video run "$T" "0:0" "60:key enter;80:quit" "" "$T/root/media/red.mp4")
expect "[ $rc = 0 ] && log $T '\[player\] a new thread refused: video: ' && log $T 'failed: The console refused a new thread'" "the video thread refused: the file fails, the app quits cleanly"
nosan "$T"
# b3: the audio output, the disc watcher and the tone mapping's workers: no sound, no disc, the caller tone maps all
T=$(newroot t31b3)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_REFUSE_THREADS=audio-out,disc-watch,tonemap run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "[ $rc = 0 ] && log $T '\[audio\] a new thread refused: audio-out: .*: no sound' && log $T '\[disc\] a new thread refused: disc-watch: .*: the disc drives aren.t watched' && log $T 'a new thread refused: tonemap: .* (worker 1): its bands done by the caller' && log $T '\[tonemap\] 0 worker threads started'" "the audio output, the disc watcher, the tone mapping workers refused: logged"
expect "grey_in $T/dump/flip00045.ppm 480 520 168 179 && grey_in $T/dump/flip00045.ppm 1440 520 248 255" "the film plays, tone mapped whole by the caller (100 and 1000 nits as in group 14)"
nosan "$T"
# b4: a disc's reading thread: its sectors read straight from the drive, the DVD plays; b5: the thread of an audio
# track chosen later: that track off, the film plays on
T=$(newroot t31b4)
rc=$(MPCPS5_HOST_REFUSE_THREADS=disc-io,audio-track MPCPS5_HOST_DISC=$MEDIA/dvd.iso run "$T" "0:0" "30:cmd disc0;58:state;60:cmd 2002;120:state;130:quit" "55")
expect "[ $rc = 0 ] && log $T 'cd0: a new thread refused: disc-io: .*: read straight from the drive' && [ \"\$(field $T 1 1)\" = 2 ] && $CHECK $T/dump/flip00055.ppm 960 400 red >/dev/null" "the disc's reading thread refused: read straight from the drive, the DVD plays (red)"
expect "log $T 'a new thread refused: audio-track: .*: audio track [0-9]* off' && [ \"\$(field $T 2 1)\" = 2 ] && [ \"\$(field $T 2 5)\" = -1 ]" "an audio track's thread refused: that track off, the film plays on"
nosan "$T"
# c: a Blu-ray .sup beside the film (60 lines of 1000x200, a picture a second): up to 1.5.1 it went through the text
# conversion and never loaded. Its packets are kept, each line's pictures decoded when it shows; with a 1 MiB budget,
# a seek to 50 s lets the pictures of the first lines (shown at the start) go, a seek back to 1.3 s those far ahead,
# and the line at 1.2 s shows again, decoded anew from its packets
T=$(newroot t31c)
python3 tests/make_pgs.py $T/root/media/film.sup 60 1.0 0.98 1000 200
ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=25:d=61 -c:v libx264 -preset ultrafast -pix_fmt yuv420p $T/root/media/film.mp4
rc=$(MPCPS5_HOST_SUB_BUDGET_MB=1 run "$T" "0:0" "30:state;40:seek 50;100:state;110:seek 1.3;170:state;180:quit" "95,125" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && log $T 'subtitles loaded: .*film.sup (bitmaps: 60 line(s), [0-9]* KB of packets kept)'" "film.sup beside film.mp4: loaded, 60 lines"
expect "$REGION $T/dump/flip00095.ppm 500 700 1420 977 255 255 255 30 2000 >/dev/null && $REGION $T/dump/flip00125.ppm 500 700 1420 977 255 255 255 30 2000 >/dev/null" "its line at 50 s and, after a seek back, the one at 1.2 s (let go meanwhile) drawn"
expect "grep -q 'bitmap subtitles over 1 MiB ([0-9]* MiB): [0-9]* picture(s) of lines over 30 s from 5[0-9]\.[0-9] s let go' $T/root/logs/boot.log && grep -q 'bitmap subtitles over 1 MiB ([0-9]* MiB): [0-9]* picture(s) of lines over 30 s from [0-9]\.[0-9] s let go' $T/root/logs/boot.log" "the budget held: pictures far from the line shown let go (decoded again from the packets when needed)"
nosan "$T"
# d: a VobSub pair (film.idx + film.sub, made from a PGS file by FFmpeg): read with FFmpeg's VobSub reader, the line
# at 1 s drawn
T=$(newroot t31d)
python3 tests/make_pgs.py $T/vs.sup 6 1.0 0.98 1000 200
python3 tests/make_vobsub.py $T/vs.sup $T/root/media/film 2>/dev/null
cp "$MEDIA/grey.mp4" "$T/root/media/film.mp4"
rc=$(run "$T" "0:0" "60:state;70:quit" "60" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && log $T 'subtitles loaded: .*film.idx (bitmaps: 6 line(s)' && $REGION $T/dump/flip00060.ppm 500 700 1420 977 255 255 255 60 2000 >/dev/null" "film.sub with its film.idx beside the film: VobSub loaded (6 lines), the line at 1 s drawn"
nosan "$T"
# e (review): after a refused audio track's thread (astream -1), choosing tracks again: 1.5.2's first build stopped
# reading streams[-1] (a heap overflow; the same when a file's first audio decoder didn't open)
T=$(newroot t31e)
rc=$(MPCPS5_HOST_REFUSE_THREADS=audio-track MPCPS5_HOST_DISC=$MEDIA/dvd.iso run "$T" "0:0" "30:cmd disc0;60:cmd 2002;90:cmd 2003;100:cmd 2002;150:state;160:quit" "")
expect "[ $rc = 0 ] && [ \"\$(field $T 1 1)\" = 2 ]" "audio tracks chosen again after one turned off: no write before the streams, the film plays"
nosan "$T"
# f (review): a .sup of 48 KB whose one line is an 8000 x 8000 picture on an 8000 x 8000 screen: not made (~1 GB, a few
# frames a second; on the console an allocation that fails), the film plays
T=$(newroot t31f)
python3 tests/make_pgs.py $T/root/media/film.sup 1 1.0 3.0 8000 8000 8000 8000
cp "$MEDIA/grey.mp4" "$T/root/media/film.mp4"
rc=$(run "$T" "0:0" "60:state;70:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && log $T 'a bitmap subtitle picture of 8000x8000 (on a 8000x8000 picture): over the bounds, not shown' && [ \"\$(field $T 1 1)\" = 2 ]" "an 8000 x 8000 subtitle picture: not made, the film plays"
nosan "$T"
# g (review): a .sup of 30,000 tiny lines: read up to 20,000 lines, quickly (each line's insertion scanned the whole
# list back: O(n^2), 106 s for 100,000 lines in the opener, which Close waited for); a quit while it is read stops it
T=$(newroot t31g)
python3 tests/make_pgs.py $T/root/media/film.sup 30000 0.01 0.005 2 2
cp "$MEDIA/grey.mp4" "$T/root/media/film.mp4"
rc=$(run "$T" "0:0" "200:state;210:quit" "" "$T/root/media/film.mp4")
secs=$(python3 -c "
import re, sys
t = {}
for l in open('$T/root/logs/boot.log'):
    m = re.match(r'\[ *([0-9.]+)\] \[player\] (opened|subtitles loaded)', l)
    if m and m.group(2) not in t:
        t[m.group(2)] = float(m.group(1))
print(round(t['subtitles loaded'] - t['opened'], 1) if len(t) == 2 else 999)")
expect "[ $rc = 0 ] && log $T 'subtitles loaded: .*film.sup (bitmaps: 20000 line(s), .*cut at .*: over 128 MB or 20000 lines' && python3 -c 'import sys; sys.exit(0 if $secs < 15 else 1)'" "30,000 tiny lines: 20,000 read, in $secs s"
nosan "$T"
rm -f $T/root/logs/boot.log
rc=$(run "$T" "0:0" "10:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && log $T 'subtitles: .*film.sup: reading stopped, the file closed'" "a quit while they are read (~2 s here): the reading stops, Close doesn't wait for it"
# h (review 3): a PGS track in an MKV, a 0.1 s line every 0.5 s, the video at 5 frames a second (the demuxer reads a
# minute -- ~120 lines -- ahead): after a seek back, each replayed line is stored again mid-list, open-ended, and its
# clearing set must end it. b33e45c looked for the open line among the last 64 of the list (all later lines read
# ahead): the replayed line never ended and stayed on screen (50 of 50 frames with the box; 8-9 without the seek)
T=$(newroot t31h)
python3 tests/make_pgs.py $T/s.sup 240 0.5 0.1 400 100
ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=5:d=121 -c:v libx264 -preset ultrafast -pix_fmt yuv420p $T/v.mp4 &&
	ffmpeg -v error -i $T/v.mp4 -i $T/s.sup -map 0 -map 1 -c copy $T/root/media/pgs5.mkv
D=$(seq -s, 200 4 396)
rc=$(run "$T" "0:0" "150:state;160:seek 1.0;400:quit" "$D" "$T/root/media/pgs5.mkv")
boxed=0
for f in $(seq 200 4 396); do
	$REGION $(printf "$T/dump/flip%05d.ppm" $f) 500 700 1420 977 255 255 255 30 300 >/dev/null 2>&1 && boxed=$((boxed + 1))
done
rm -f $T/dump/*.ppm
expect "[ $rc = 0 ] && [ $boxed -gt 0 ] && [ $boxed -lt 30 ]" "after a seek back, the replayed lines end again: the box on $boxed of 50 frames (a 0.1 s line every 0.5 s)"
nosan "$T"
# i (1.6, the audit's PGS item): normal display sets that use their epoch's palette and object (make_pgs.py epochs: a
# palette update at 2.2 s, white -> red; a new object in the epoch's colour 2, green, at 4.2 s; a second epoch at 8.2 s,
# a 200x100 box, colour 1 blue and 2 yellow). An external .sup's line is decoded when it shows: up to 1.5.4 on the file's
# one decoder, left by the line decoded before -- after 8.2 s's, the line at 2.2 s was a 200x100 red box and 4.2 s's
# yellow. Each line now has a decoder of its own with its epoch replayed first. Paused, seeks to 8.6, 2.6 and 4.6 s
T=$(newroot t31i)
python3 tests/make_pgs.py $T/root/media/film.sup epochs
ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=25:d=12 -c:v libx264 -preset ultrafast -pix_fmt yuv420p $T/root/media/film.mp4
rc=$(run "$T" "0:0" "20:cmd pause;30:seek 8.6;90:state;100:seek 2.6;160:state;170:seek 4.6;230:state;240:quit" "85,155,225" "$T/root/media/film.mp4")
pgs_far="1100 850 1380 926" # the 1000x200 box's lower right (a 200x100 one, at its corner or in the middle, isn't there)
pgs_box="500 700 1420 977"
expect "[ $rc = 0 ] && log $T 'subtitles loaded: .*film.sup (bitmaps: 4 line(s)' && $REGION $T/dump/flip00085.ppm $pgs_box 0 0 255 40 500 >/dev/null && ! $REGION $T/dump/flip00085.ppm $pgs_far 0 0 255 40 100 >/dev/null" "the second epoch's line at 8.6 s: the small blue box"
expect "$REGION $T/dump/flip00155.ppm $pgs_far 255 0 0 40 2000 >/dev/null" "then 2.6 s: the palette update drawn on its own epoch's object (the 1000x200 box, red)"
expect "$REGION $T/dump/flip00225.ppm $pgs_box 0 255 0 40 2000 >/dev/null && ! $REGION $T/dump/flip00225.ppm $pgs_box 255 255 0 40 100 >/dev/null" "then 4.6 s: the new object in its epoch's colour 2 (green, not the later epoch's yellow)"
nosan "$T"
# j (1.6, the same in an MKV): the lines are decoded as the demuxer reads them (a minute ahead: the whole film here,
# in order, right); after a seek back the packets read again went to the decoder left at the second epoch, and each
# line replayed replaced the one decoded in order (1.5.4: a 200x100 red box at 2.5 s, yellow at 4.5 s; a keyframe
# every 0.2 s). At a seek the PGS decoder is made again and given nothing until a display set that begins an epoch
# (a new decoder alone made 4.2 s's object see-through: 2.2 s's palette update left colour 1 only): the lines decoded
# in order stay
T=$(newroot t31j)
python3 tests/make_pgs.py $T/s.sup epochs
ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=25:d=12 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 5 $T/v.mp4 &&
	ffmpeg -v error -i $T/v.mp4 -i $T/s.sup -map 0 -map 1 -c copy $T/root/media/pgs.mkv
rc=$(run "$T" "0:0" "120:state;130:cmd pause;140:seek 2.5;200:state;210:seek 4.5;270:state;280:quit" "195,265" "$T/root/media/pgs.mkv")
expect "[ $rc = 0 ] && $REGION $T/dump/flip00195.ppm $pgs_far 255 0 0 40 2000 >/dev/null" "after a seek back to 2.5 s: the palette update on the 1000x200 box (red), as decoded in order"
expect "$REGION $T/dump/flip00265.ppm $pgs_box 0 255 0 40 2000 >/dev/null && ! $REGION $T/dump/flip00265.ppm $pgs_box 255 255 0 40 100 >/dev/null" "then 4.5 s: the new object green (not the later epoch's yellow)"
nosan "$T"
# k (review): a line cleared by a normal display set, after a seek forward inside it (make_pgs.py clearing: a white box
# 10-20 s, another line at 25 s). Paused, a seek to 15 s (the demuxer had read to ~12 s: the box open-ended), then to
# 22 s: the clearing set at 20 s is read while the track waits for an epoch -- 1.6's first build passed it over and the
# box stayed until 25 s. A display set with no object goes through. k: MKV (a display set a packet); l: M2TS (a segment
# a packet: the clearing set's end segment goes through with it)
for v in k:mkv l:m2ts; do
	T=$(newroot t31${v%%:*})
	ext=${v##*:}
	python3 tests/make_pgs.py $T/s.sup clearing
	ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=25:d=30 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 5 $T/v.mp4
	if [ $ext = mkv ]; then
		ffmpeg -v error -copyts -i $T/v.mp4 -i $T/s.sup -map 0 -map 1 -c copy $T/root/media/clear.mkv
	else
		ffmpeg -v error -copyts -i $T/v.mp4 -i $T/s.sup -map 0 -map 1 -c copy -f mpegts -mpegts_m2ts_mode 1 $T/root/media/clear.m2ts
	fi
	rc=$(run "$T" "0:0" "20:cmd pause;30:seek 15;90:state;100:seek 22;160:state;170:quit" "85,155" "$T/root/media/clear.$ext")
	expect "[ $rc = 0 ] && $REGION $T/dump/flip00085.ppm $pgs_box 255 255 255 30 2000 >/dev/null" "$ext: at 15 s, inside the line: the white box"
	expect "! $REGION $T/dump/flip00155.ppm $pgs_box 255 255 255 30 2000 >/dev/null" "$ext: then 22 s, past its clearing set at 20 s: no box"
	nosan "$T"
done
# m (review): one epoch start (the palette) and 999 normal lines with new objects and no palette (make_pgs.py longepoch,
# ~6,000 packets); the line at 90.2 s is drawn: its epoch's palette replayed from ~6,000 packets back (1.6's first
# build replayed the last 4,096: no palette, the line never drawn)
T=$(newroot t31m)
python3 tests/make_pgs.py $T/root/media/film.sup longepoch
ffmpeg -v error -f lavfi -i color=c=gray:s=320x180:r=25:d=93 -c:v libx264 -preset ultrafast -pix_fmt yuv420p $T/root/media/film.mp4
rc=$(run "$T" "0:0" "20:cmd pause;30:seek 91;90:state;100:quit" "85" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && log $T 'subtitles loaded: .*film.sup (bitmaps: 1000 line(s)' && $REGION $T/dump/flip00085.ppm $pgs_box 255 255 255 30 2000 >/dev/null" "a long epoch: the line at 90.2 s drawn with the palette of its start (~6,000 packets before)"
nosan "$T"
fi

if want 32; then
echo "== 32. 1.5.3 (Sulfurico-One64, #3): Square's own action with the pointer, the fullscreen controls when the pointer is over them, a held jump's pictures"
log() { grep -q -- "$2" "$1/root/logs/boot.log"; }
count() { grep -c -- "$2" "$1/root/logs/boot.log"; }
# a: Square given another action (Next audio track) does it with the pointer showing (moved just before) as without it
# (3 s still); over Options (a dialog) it stays the right-click there; R3 held with it still shows the statistics; the
# window's right-click is still the touchpad pressed with two fingers
T=$(newroot t32a)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeySquare=NextAudio"
TP2="100000,128,128,128,128,900,500,1100,500"
P="0:0;40:0,200,128;46:0;50:$SQUARE;53:0;300:$SQUARE;303:0;340:0,200,128;346:0;360:$SQUARE;363:0"
P="$P;400:0,60,128;406:0;410:$R3;413:8004;416:0;440:0,200,128;446:0;450:$TP2;454:0"
rc=$(run "$T" "$P" "330:cmd options;380:key escape;470:key escape;480:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \$(count $T '\[ui\] Square: NextAudio') = 2 ] && [ \$(count $T '\[osd\] Audio: ') -ge 2 ]" "Square as Next audio track: with the pointer showing and without it, the track changes ($(count $T '\[ui\] Square: NextAudio') times)"
# (test5) the wall time in boot.log between the first lines matching $2 and $3, seconds (its [  12345.678] stamps): a
# check of where a film is after it was opened that holds on a loaded machine (the frames come slower than 60 a second)
since() { python3 - "$1/root/logs/boot.log" "$2" "$3" <<'PY'
import re, sys
t = {}
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.match(r'\[\s*([0-9.]+)\]', line)
    for k, pat in ((2, sys.argv[2]), (3, sys.argv[3])):
        if m and k not in t and re.search(pat, line):
            t[k] = float(m.group(1))
print('%.3f' % (t[3] - t[2]) if 2 in t and 3 in t else 'nan')
PY
}
expect "log $T '\[ui\] Square: Statistics'" "R3 held with Square (the pointer showing): the statistics, as before"
expect "[ \$(count $T '\[ui\] right-click at') = 1 ] && [ \$(line $T 'right-click at') -gt \$(line $T 'Square: Statistics') ]" "no right-click from Square in the window (in Options: Options' own, nothing); the touchpad pressed with two fingers: the window's right-click"
nosan "$T"
# b: Square at its default: the right-click with the pointer showing, the context menu without it (as up to 1.5.2)
T=$(newroot t32b)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
rc=$(run "$T" "0:0;40:0,200,128;46:0;50:$SQUARE;53:0;300:$SQUARE;303:0" "70:key escape;320:key escape;340:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \$(count $T '\[ui\] right-click at') = 1 ] && [ \$(count $T '\[ui\] Square: ContextMenu') = 1 ]" "Square at its default: the right-click with the pointer, the context menu without it"
nosan "$T"
# the films for c..k: a test pattern under noise, 1920x1080 at 15 frames a second, 100 s, a keyframe every 10 s (an
# exact seek decodes up to 150 pictures first: ~0.25-0.5 s here, as a 4K film read from a hard disk on the console),
# with a subtitle line at the bottom the whole time; and 100 s of sound alone
[ -f "$MEDIA/gop10.mp4" ] || ffmpeg -v error -y -f lavfi -i testsrc2=s=1920x1080:r=15:d=100 -vf noise=alls=12:allf=t+u \
	-c:v libx264 -preset veryfast -crf 26 -g 150 -keyint_min 150 -sc_threshold 0 -pix_fmt yuv420p "$MEDIA/gop10.mp4"
[ -f "$MEDIA/tone100.m4a" ] || ffmpeg -v error -y -f lavfi -i sine=frequency=500:sample_rate=48000:duration=100 -c:a aac "$MEDIA/tone100.m4a"
film() {
	cp "$MEDIA/gop10.mp4" "$1/root/media/film.mp4"
	printf '1\r\n00:00:00,500 --> 00:01:40,000\r\nThe subtitles the bars would cover\r\n' >"$1/root/media/film.srt"
}
# the bars (the seek bar's and the toolbar's COLOR_BTNFACE) drawn between x0 and x1 at the bottom
bars() { $REGION "$1" "$2" 985 "$3" 1040 240 240 240 6 $(( ($3 - $2) * 20 )) >/dev/null; }
# c: Options > Fullscreen "Show when the pointer is over them": the pointer moving in the middle, then inside Options
# over the picture: no bars (up to 1.5.2 any movement brought them, over the subtitles being styled); at the bottom:
# the bars; it leaves: still there for a second, then gone; the seek bar dragged off the band keeps them; nothing open:
# shown
T=$(newroot t32c)
film "$T"
ini "$T" "FullScreen=1
HideFullscreenControlsPolicy=1"
S="100:cmd options;110:mouse 900,500;115:mouse 950,520;120:mouse 1000,560;150:key escape;160:mouse 960,1075;180:mouse 960,500"
S="$S;280:mouse 960,995;400:cmd close;440:quit"
rc=$(run "$T" "0:0;60:0,200,128;66:0;290:$CROSS;292:$CROSS,128,0;330:$CROSS;340:0" "$S" "80,125,170,190,260,330,425" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && ! bars $T/dump/flip00080.ppm 0 1920 && ! bars $T/dump/flip00125.ppm 0 1920" "over-them mode: the pointer moving over the picture, then inside Options: no bars"
expect "bars $T/dump/flip00170.ppm 0 1920 && log $T 'fullscreen: the pointer over the controls (960,1075): shown'" "... the pointer at the bottom: the bars"
expect "bars $T/dump/flip00190.ppm 0 1920 && ! bars $T/dump/flip00260.ppm 0 1920 && log $T 'fullscreen: the pointer left the controls: hidden'" "... it leaves: a second more, then hidden"
expect "bars $T/dump/flip00330.ppm 0 1920 && log $T 'seek bar let go'" "... the seek bar dragged up off the bars: they stay while it is held"
expect "bars $T/dump/flip00425.ppm 0 1920 && grep -q '^HideFullscreenControlsPolicy=1$' $T/root/mpc-hc.ini" "... nothing open: shown; the choice kept in mpc-hc.ini"
nosan "$T"
# d: Options moved to the bottom left, over the bars' band: the pointer over it there brings no bars; beside it, they show
T=$(newroot t32d)
film "$T"
ini "$T" "FullScreen=1
HideFullscreenControlsPolicy=1
OptionsX=0
OptionsY=1079"
rc=$(run "$T" "0:0" "40:cmd options;50:mouse 100,1070;80:mouse 1850,1075;100:key escape;110:quit" "70,95" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && ! bars $T/dump/flip00070.ppm 1100 1920 && bars $T/dump/flip00095.ppm 1100 1920" "over-them mode, Options over the band: the pointer on Options there, no bars; beside it, the bars"
nosan "$T"
# e: an mpc-hc.ini without the new key (1.5.2's): "when the pointer moves", as before -- moving inside Options brings
# the bars; saved with HideFullscreenControlsPolicy=2
T=$(newroot t32e)
film "$T"
ini "$T" "FullScreen=1"
rc=$(run "$T" "0:0;60:0,200,128;66:0" "100:cmd options;110:mouse 900,500;115:mouse 950,520;120:mouse 1000,560;150:key escape;160:quit" "80,125" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && bars $T/dump/flip00080.ppm 0 1920 && bars $T/dump/flip00125.ppm 0 1920 && grep -q '^HideFullscreenControlsPolicy=2$' $T/root/mpc-hc.ini" "1.5.2's mpc-hc.ini: the bars when the pointer moves (Options too), as before; the default saved"
nosan "$T"
# the held jumps: "<button> let go after S s: N jump(s), K keyframe seek(s), P picture(s) shown; exact seek to X"
held() { grep "let go after" "$1/root/logs/boot.log" | sed -n "${2}p" | sed -E 's/.* after ([0-9.]+) s: ([0-9]+) jump\(s\), ([0-9]+) keyframe seek\(s\), ([0-9]+) picture\(s\) shown; exact seek to ([0-9.]+)/\1 \2 \3 \4 \5/'; }
hf() { held "$1" "$2" | cut -d' ' -f"$3"; }
# pictures while held, a few a second (the seek bar's line has no jumps: its fields one to the left)
# (1.5 a second: a host-speed check, loose for a loaded machine -- 1.5.2 showed none; third review: 2.48 under load)
often() { python3 -c "import sys; s, p = float(sys.argv[1]), float(sys.argv[2]); sys.exit(0 if p >= 3 and p / s >= 1.5 else 1)" "${1:-1}" "${2:-0}"; }
distinct() { python3 - "$@" <<'PY'
import hashlib, sys
# the picture's middle band (not the OSD's corner, not the subtitle at the bottom)
print(len({hashlib.md5(open(f, 'rb').read()[-1920 * 3 * 800:-1920 * 3 * 300]).hexdigest() for f in sys.argv[1:]}))
PY
}
# f: paused at 5 s, D-pad Right (5 s jumps) held: pictures while held (several a second, the screen changing), the
# position counted from the jumps, an exact seek to it when let go; then Left pressed once: exactly 5 s back (a
# keyframe seek would land on the keyframe before it, up to 10 s back)
T=$(newroot t32f)
film "$T"
D=$(seq -s, 115 3 136)
rc=$(run "$T" "0:0;100:$RIGHT;135:0;300:$LEFT;303:0" "30:cmd pause;40:seek 5;80:state;260:state;360:state;370:quit" "$D" "$T/root/media/film.mp4")
N=$(hf $T 1 2); X=$(hf $T 1 5); DIST=$(distinct $T/dump/flip*.ppm); P1=$(field $T 1 2); P2=$(field $T 2 2); P3=$(field $T 3 2)
expect "[ $rc = 0 ] && often $(hf $T 1 1) $(hf $T 1 4) && [ $DIST -ge 3 ]" "D-pad Right held (paused): $(hf $T 1 4) pictures in $(hf $T 1 1) s, the screen changed $DIST times in 8 frames (1.5.2: each repeat dropped the seek still decoding)"
expect "[ -n \"$N\" ] && near $X \$(python3 -c 'print(min(100, $P1 + 5 * ($N + 1)))') 0.01 && [ \"\$(field $T 2 1)\" = 3 ] && near $P2 $X 0.07" "... let go: an exact seek to where the jumps added up to ($X s: $P1 + 5 x $((${N:-0} + 1))), paused there ($P2 s)"
expect "[ -n \"$P2\" ] && near $P3 \$(python3 -c 'print($P2 - 5)') 0.1" "a single press: exact as before ($P2 s -> $P3 s; a keyframe seek: up to 10 s back)"
rm -f $T/dump/*.ppm
nosan "$T"
# g: playing, the right stick held right (1 s jumps): pictures while held; it plays on from where they added up to
T=$(newroot t32g)
film "$T"
rc=$(run "$T" "0:0;100:0,128,128,255,128;160:0,128,128,128,128" "60:seek 5;175:state;185:quit" "" "$T/root/media/film.mp4")
X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && log $T 'Right stick right held: jumping by keyframes' && often $(hf $T 1 1) $(hf $T 1 4) && [ \"\$(field $T 1 1)\" = 2 ] && python3 -c 'import sys; sys.exit(0 if $X <= $P1 <= $X + 1.5 else 1)'" "the right stick held (playing): $(hf $T 1 4) pictures in $(hf $T 1 1) s; plays on from $X s ($P1 s)"
nosan "$T"
# h: L2 held with D-pad Left (20 s jumps back), playing from 90 s: the combination held, by keyframes, to the start at most
T=$(newroot t32h)
film "$T"
rc=$(run "$T" "0:0;100:$L2;105:$((L2 + LEFT));150:$L2;155:0" "60:seek 90;170:state;180:quit" "" "$T/root/media/film.mp4")
N=$(hf $T 1 2); X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && log $T 'D-pad Left: JumpBackLarge' && [ -n \"$N\" ] && near $X \$(python3 -c 'print(max(0, 90 - 20 * ($N + 1)))') 0.01 && python3 -c 'import sys; sys.exit(0 if $X <= $P1 <= $X + 1.5 else 1)'" "L2 + D-pad Left held: 20 s jumps back to $X s, playing on from there ($P1 s)"
nosan "$T"
# i: sound alone: held, it jumps (nothing to wait for), exact when let go
T=$(newroot t32i)
cp "$MEDIA/tone100.m4a" "$T/root/media/"
rc=$(run "$T" "0:0;100:$RIGHT;150:0" "30:cmd pause;40:seek 5;200:state;210:quit" "" "$T/root/media/tone100.m4a")
N=$(hf $T 1 2); X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$N\" ] && [ \"$(hf $T 1 3)\" -ge 1 ] && near $X \$(python3 -c 'print(min(100, 5 + 5 * ($N + 1)))') 0.01 && near $P1 $X 0.05" "sound alone: held, $N jump(s) by $(hf $T 1 3) seek(s); let go at $X s ($P1 s)"
nosan "$T"
# j: held past the end (the last seek finds nothing to show), then held back: the pictures come again (a seek that never
# shows one doesn't stop the jumps)
T=$(newroot t32j)
film "$T"
rc=$(run "$T" "0:0;100:$RIGHT;170:0;220:$LEFT;270:0" "30:cmd pause;40:seek 80;320:state;330:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && [ \"$(hf $T 1 5)\" = 100.00 ] && [ -n \"$(hf $T 2 4)\" ] && [ $(hf $T 2 4) -ge 2 ] && near \$(field $T 1 2) $(hf $T 2 5) 0.07" "held past the end, then back: $(hf $T 2 4) pictures on the way back, at $(hf $T 2 5) s"
nosan "$T"
# k: the seek bar dragged (Cross held on it, the stick): its keyframe seeks one at a time too (each pointer step dropped
# the last one); let go, exact. (From 5 s to ~38 s there are four keyframes to show: 1.5.2 showed one)
T=$(newroot t32k)
film "$T"
D=$(seq -s, 110 5 155)
rc=$(run "$T" "0:0;100:$CROSS;102:$CROSS,200,128;160:$CROSS;165:0" "30:cmd pause;40:seek 5;90:mouse 200,995;220:state;230:quit" "$D" "$T/root/media/film.mp4")
SB=$(grep 'seek bar let go after' $T/root/logs/boot.log | sed -E 's/.* after ([0-9.]+) s: ([0-9]+) keyframe seek\(s\), ([0-9]+) picture\(s\) shown; exact seek to ([0-9.]+)/\1 \2 \3 \4/')
DIST=$(distinct $T/dump/flip*.ppm)
P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$SB\" ] && often $(echo $SB | cut -d' ' -f1) $(echo $SB | cut -d' ' -f3) && [ $DIST -ge 2 ] && near $P1 $(echo $SB | cut -d' ' -f4) 0.07" "the seek bar dragged (paused): $(echo $SB | cut -d' ' -f3) pictures in $(echo $SB | cut -d' ' -f1) s, the screen changed $DIST times in 10 frames; let go, exact at $(echo $SB | cut -d' ' -f4) s ($P1 s)"
rm -f $T/dump/*.ppm
nosan "$T"
# --- the review of 2c61437 ---
# l: a jump pressed as the next file opens and held: it starts from where that file is (0 s), not from the last file's
# target (up to 2c61437 the press, ignored while the file opened, left the last file's button and target: "jumping by
# keyframes from 55.00" in the new file, let go at its end)
T=$(newroot t32l)
film "$T"
cp "$MEDIA/gop10.mp4" "$T/root/media/film2.mp4"
rc=$(run "$T" "0:0;80:$RIGHT;83:0;120:$RIGHT;170:0" "30:cmd pause;40:seek 50;60:state;100:state;120:open $T/root/media/film2.mp4;230:quit" "" "$T/root/media/film.mp4")
FROM=$(grep 'held: jumping by keyframes from' $T/root/logs/boot.log | tail -1 | sed -E 's/.* from ([0-9.]+)$/\1/')
expect "[ $rc = 0 ] && near \$(field $T 2 2) 54.97 0.1 && [ -n \"$FROM\" ] && python3 -c 'import sys; sys.exit(0 if $FROM <= 5.5 else 1)'" "a jump held from the next file's opening: from that file's start (${FROM:-none} s), not the last file's 55 s"
nosan "$T"
# m: the seek bar held when a window opens over it (About): the drag ends there with an exact seek where it was; the
# moves over the window don't move it (up to 2c61437 they asked 0 s -- x = -1 -- and the release, going to the window,
# left the drag on); n: the same with the volume (it went to 0)
T=$(newroot t32m)
film "$T"
rc=$(run "$T" "0:0;100:$CROSS;102:$CROSS,200,128;130:$CROSS;150:$CROSS,200,128;180:$CROSS;200:0" "30:cmd pause;40:seek 5;90:mouse 200,995;135:cmd about;220:key escape;250:state;260:quit" "" "$T/root/media/film.mp4")
X=$(grep 'seek bar a window opened over it after' $T/root/logs/boot.log | sed -E 's/.* exact seek to ([0-9.]+)$/\1/'); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$X\" ] && python3 -c 'import sys; sys.exit(0 if $X > 10 else 1)' && near $P1 $X 0.07" "the seek bar held as About opens: the drag ends at ${X:-?} s, exact ($P1 s), not at 0 s"
nosan "$T"
T=$(newroot t32n)
film "$T"
# (test5: the moves start once About is open -- before it, the held stick moves the volume by the wall clock's repeat,
# more on a loaded machine; what is checked is that the moves over About leave the volume alone)
rc=$(run "$T" "0:0;100:$CROSS;108:$CROSS,60,128;110:$CROSS;150:$CROSS,60,128;180:$CROSS;200:0" "30:cmd pause;90:mouse 1890,1025;105:cmd about;220:key escape;260:quit" "" "$T/root/media/film.mp4")
VOL=$(sed -n 's/^Volume=//p' $T/root/mpc-hc.ini)
expect "[ $rc = 0 ] && [ -n \"$VOL\" ] && [ \"$VOL\" -gt 20 ]" "the volume held as About opens: the moves over it leave the volume alone (Volume=$VOL)"
nosan "$T"
# o: Square given another action with the touchpad as the pointer turned off: with the pointer showing it stays the
# right-click (the only one left; up to 2c61437 there was none), without it does its action
T=$(newroot t32o)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
ini "$T" "KeySquare=NextAudio
TouchpadPointer=0"
rc=$(run "$T" "0:0;40:0,200,128;46:0;50:$SQUARE;53:0;300:$SQUARE;303:0" "70:key escape;320:quit" "" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \$(count $T '\[ui\] right-click at') = 1 ] && [ \$(count $T '\[ui\] Square: NextAudio') = 1 ]" "Square as Next audio track, the touchpad not the pointer: the right-click with the pointer showing, its action without"
nosan "$T"
# p: a slow source (every seek takes 2.5 s here: MPCPS5_HOST_SLOW_SEEK): held, the seeks being done are waited for, so
# pictures still come; up to 2c61437 a 2 s cap sent the next one anyway, dropping each, and none came
T=$(newroot t32p)
film "$T"
rc=$(MPCPS5_HOST_SLOW_SEEK=2500 run "$T" "0:0;100:0,128,128,255,128;520:0,128,128,128,128" "30:cmd pause;40:seek 5;1000:state;1010:quit" "" "$T/root/media/film.mp4")
X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$X\" ] && [ \"$(hf $T 1 4)\" -ge 2 ] && near $P1 $X 0.07" "every seek 2.5 s: held $(hf $T 1 1) s, $(hf $T 1 4) pictures shown, $(hf $T 1 3) keyframe seeks; let go, exact at ${X:-?} s ($P1 s)"
nosan "$T"
# r: a held jump ends when a window opens over it (About): its exact seek then, not when the button is let go after
T=$(newroot t32r)
film "$T"
rc=$(run "$T" "0:0;100:$RIGHT;250:0" "30:cmd pause;40:seek 5;150:cmd about;200:key escape;300:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && [ -n \"\$(line $T 'let go after')\" ] && [ \$(line $T 'let go after') -gt \$(line $T 'cmd about') ] && [ \$(line $T 'let go after') -lt \$(line $T 'key escape') ]" "a held jump ends (exact seek) as About opens over it"
nosan "$T"
# q: Square given another action, in a menu with the pointer showing: still the right-click there -- on View >
# Statistics it runs it, as Windows' menus do (were Square its key in a menu too, it would close the menu instead)
T=$(newroot t32q)
film "$T"
ini "$T" "KeySquare=NextAudio"
rc=$(run "$T" "0:0;70:$SQUARE;73:0" "30:cmd pause;40:key menu;45:key right;55:mouse 150,217;100:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && grep -q '^ShowStatistics=1$' $T/root/mpc-hc.ini && ! log $T 'Square: NextAudio'" "Square (another action) in a menu, the pointer showing: the menu's right-click (View > Statistics run)"
nosan "$T"
# --- the second review ---
# s: a seek whose picture can never come -- the press's exact seek past the end of a video track (5 s) with 400 s of
# sound after it (no video packet will come, and paused the queues fill, 64 MiB, before the end). Held, the next seeks
# go once the sound read has run 10 s past its first packet and the last video packet, paused (d8f2f0b waited for that
# picture all hold long) and playing (6ec13df's "a second without reading" never came while playing: the sound drains)
SHORT=$WORK/t32s-short.mkv
trap 'rm -f "$SHORT"' EXIT # (77 MB: not left behind by a run cut short; interrupted, the run stops)
trap 'rm -f "$SHORT"; exit 130' INT TERM
ffmpeg -v error -y -f lavfi -i testsrc=s=320x180:r=25:d=5 -f lavfi -i sine=frequency=300:sample_rate=48000:duration=400 \
	-ac 2 -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -c:a pcm_s16le "$SHORT"
T=$(newroot t32s)
rc=$(run "$T" "0:0;100:$RIGHT;300:0" "30:cmd pause;40:seek 2;400:state;410:quit" "" "$SHORT")
X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$X\" ] && [ \"$(hf $T 1 3)\" -ge 2 ] && near $P1 $X 0.05" "an exact seek past the video's end (no picture can come), paused: held, $(hf $T 1 3) keyframe seeks follow; let go at ${X:-?} s ($P1 s)"
nosan "$T"
T=$(newroot t32s2)
rc=$(run "$T" "0:0;100:$RIGHT;300:0" "40:seek 2;320:state;330:quit" "" "$SHORT")
X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$X\" ] && [ \"$(hf $T 1 3)\" -ge 2 ] && [ \"\$(field $T 1 1)\" = 2 ] && python3 -c 'import sys; sys.exit(0 if $X <= ${P1:-0} <= $X + 2 else 1)'" "... playing: held, $(hf $T 1 3) keyframe seeks follow; plays on from ${X:-?} s ($P1 s)"
rm -f "$SHORT"
trap - EXIT INT TERM
nosan "$T"
# t: the seek bar held as the file ends and the next one in the folder opens: the drag doesn't go on into it (d8f2f0b
# ended it at the next Tick, seeking the new file to the old position if it was already playing)
T=$(newroot t32t)
cp "$MEDIA/chapters.mkv" "$T/root/media/a.mkv"
cp "$MEDIA/red.mp4" "$T/root/media/b.mp4"
ini "$T" "AfterPlayback=1"
rc=$(run "$T" "0:0;40:$CROSS;200:0" "30:mouse 1900,995;230:state;240:quit" "" "$T/root/media/a.mkv")
OPEN=$(line $T 'open .*b.mp4')
EL=$(since $T 'open .*b.mp4' '\[test\] state')
expect "[ $rc = 0 ] && [ -n \"$OPEN\" ] && ! sed -n \"${OPEN:-1},\\\$p\" $T/root/logs/boot.log | grep -q 'seek bar' && [ \"\$(field $T 1 1)\" = 2 ] && python3 -c 'import sys; p = $(field $T 1 2); sys.exit(0 if p <= $EL + 0.3 and p < 5 else 1)'" "the seek bar held over the file's end: the next file plays from its start ($(field $T 1 2) s, $EL s after it opened), the drag not carried into it"
nosan "$T"
# u / v: Stop, or File > Close, in the middle of a held jump (let go just after): no exact seek to the hold's target
# afterwards (2c61437: Stop kept the hold, and its release seeked there; fixed in 9cb73ae); closed: nothing left
T=$(newroot t32u)
film "$T"
rc=$(run "$T" "0:0;100:$RIGHT;152:0" "30:cmd pause;40:seek 5;150:cmd stop;200:state;210:quit" "" "$T/root/media/film.mp4")
P1=$(field $T 1 2)
expect "[ $rc = 0 ] && log $T 'held: jumping by keyframes' && ! log $T 'let go after' && [ \"\$(field $T 1 1)\" = 4 ] && python3 -c 'import sys; sys.exit(0 if ${P1:-99} < 10 else 1)'" "Stop while a jump is held: stopped, no exact seek to the hold's target after it ($P1 s: a repeat in the frame after Stop may still jump 5 s from 0, as a press)"
nosan "$T"
T=$(newroot t32v)
film "$T"
rc=$(run "$T" "0:0;100:$RIGHT;152:0" "30:cmd pause;40:seek 5;150:cmd close;200:state;210:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && log $T 'held: jumping by keyframes' && ! log $T 'let go after' && [ \"\$(field $T 1 1)\" = 0 ]" "File > Close while a jump is held: closed, nothing left to seek"
nosan "$T"
# w: File > Close while the seek bar is held: the drag ends with the file (no seek, nothing stuck: a later move and the
# release do nothing; then a new file plays from its start)
T=$(newroot t32w)
film "$T"
cp "$MEDIA/red.mp4" "$T/root/media/b.mp4"
rc=$(run "$T" "0:0;100:$CROSS;102:$CROSS,200,128;130:$CROSS;150:$CROSS,200,128;160:$CROSS;180:0" "30:cmd pause;40:seek 5;90:mouse 200,995;140:cmd close;200:open $T/root/media/b.mp4;260:state;270:quit" "" "$T/root/media/film.mp4")
EL=$(since $T 'open .*b.mp4' '\[test\] state')
expect "[ $rc = 0 ] && ! sed -n \"\$(line $T 'cmd close'),\\\$p\" $T/root/logs/boot.log | grep -q 'seek bar' && [ \"\$(field $T 1 1)\" = 2 ] && python3 -c 'import sys; p = $(field $T 1 2); sys.exit(0 if p <= $EL + 0.3 and p < 5 else 1)'" "File > Close while the seek bar is held: the drag ends with it; the next file plays from its start ($(field $T 1 2) s)"
nosan "$T"
# x: Stop while the seek bar is held: the drag ends with it (no seek on the release: stopped at the start)
T=$(newroot t32x)
film "$T"
rc=$(run "$T" "0:0;100:$CROSS;102:$CROSS,200,128;130:$CROSS;150:$CROSS,200,128;160:$CROSS;180:0" "30:cmd pause;40:seek 5;90:mouse 200,995;140:cmd stop;220:state;230:quit" "" "$T/root/media/film.mp4")
expect "[ $rc = 0 ] && ! sed -n \"\$(line $T 'cmd stop'),\\\$p\" $T/root/logs/boot.log | grep -q 'seek bar' && [ \"\$(field $T 1 1)\" = 4 ] && near \$(field $T 1 2) 0 0.05" "Stop while the seek bar is held: the drag ends with it, stopped at the start"
nosan "$T"
# y: a read that blocks after a failed one (after each seek here the first read fails and the next takes 2.5 s:
# MPCPS5_HOST_SEEK_READ_FAULT, a share retrying): that seek is still being worked on and isn't dropped -- 6ec13df counted
# a second from the failure, read or not, and dropped every one (63 keyframe seeks, no picture)
T=$(newroot t32y)
film "$T"
rc=$(MPCPS5_HOST_SEEK_READ_FAULT=2500 run "$T" "0:0;100:0,128,128,255,128;520:0,128,128,128,128" "30:cmd pause;40:seek 5;1000:state;1010:quit" "" "$T/root/media/film.mp4")
X=$(hf $T 1 5); P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n \"$X\" ] && [ \"$(hf $T 1 4)\" -ge 2 ] && near $P1 $X 0.07" "a failed read then a 2.5 s one after each seek: held $(hf $T 1 1) s, $(hf $T 1 4) pictures shown, $(hf $T 1 3) keyframe seeks; let go, exact at ${X:-?} s ($P1 s)"
nosan "$T"
fi

if want 33; then
echo "== 33. 1.5.4 (#3): Restart App after an update: the helper opens the app again once it has ended"
stop_helpers
ASK="python3 $PWD/tests/ask_helper.py $MPCPS5_HELPER_PORT"
# h33 dir [VAR=value...]: the helper alone, with short waits (WAIT33: 3 s for the app to end, SETTLE33: 1 s after);
# its log: dir/hroot/logs/helper.log, what it called on the "system": dir/helper.txt
h33() {
	local d=$1
	shift
	rm -f "$d/hroot/logs/helper.log" # (the last helper's "listening" isn't this one's)
	env MPCPS5_HOST_RELAUNCH_WAIT_MS=${WAIT33-3000} MPCPS5_HOST_RELAUNCH_SETTLE_MS=${SETTLE33-1000} "$@" MPCPS5_PS5_ROOT=$d/hroot \
		ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$d/helper.txt" 2>&1 &
	H33=$!
	waitfor "$d/hroot/logs/helper.log" "listening" || sleep 1
}
stoph33() { kill $H33 2>/dev/null; wait $H33 2>/dev/null; }
hlog() { grep -q -- "$2" "$1/hroot/logs/helper.log"; }
# until the helper's log says it (up to 10 s)
hwait() { for i in $(seq 1 100); do hlog "$1" "$2" && return 0; sleep 0.1; done; return 1; }
launches() { grep -c '\[host\] sceSystemServiceLaunchApp(PPSA99015' "$1"; }
notice() { grep -q "\[host\] notification: $2" "$1"; }
EN_NOTICE="Media Player Classic PS5 couldn't open again by itself: open it from the home screen to use the new version."
DE_NOTICE="Media Player Classic PS5 konnte sich nicht selbst wieder öffnen: Öffne es über den Startbildschirm, um die neue Version zu verwenden."
# the seconds between the first lines matching $2 and $3 (either file's: their stamps are the same clock)
gap() { python3 - "$1" "$2" "$3" "$4" <<'PY'
import re, sys
def at(f, pat):
    for l in open(f, encoding="utf-8", errors="replace"):
        m = re.match(r"\[ *([0-9.]+)\] (.*)", l)
        if m and re.search(pat, m.group(2)):
            return float(m.group(1))
    return None
a, b = at(sys.argv[1], sys.argv[2]), at(sys.argv[3], sys.argv[4])
print("none" if a is None or b is None else "%.3f" % (b - a))
PY
}
ge() { python3 -c "import sys; sys.exit(0 if '$1' != 'none' and float('$1') >= float('$2') else 1)"; }
# the update for a..b: the payload installs the app and stays as its helper (as from the autoload); a release 9.9 on a
# test server, the daily check due
T=$(newroot t33a)
SRV=$T/srv; mkdir -p "$SRV" "$T/appmeta/PPSA99015"; echo old >"$T/appmeta/PPSA99015/icon0.png"
SPORT=$((18000 + RANDOM % 1900))
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRV33=$!
openssl genpkey -algorithm ed25519 -out $T/release.pem 2>/dev/null
openssl genpkey -algorithm ed25519 -out $T/other.pem 2>/dev/null
PUBHEX=$(openssl pkey -in $T/release.pem -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')
inst33() { # [VAR=value...]: the payload, staying as the helper
	rm -f $T/root/logs/installer.log
	env MPCPS5_HOST_RELAUNCH_WAIT_MS=3000 MPCPS5_HOST_RELAUNCH_SETTLE_MS=1000 "$@" MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$T/homebrew \
		MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst.txt" 2>&1 &
	I33=$!
	for i in $(seq 1 100); do grep -q 'listening on 127.0.0.1' $T/root/logs/installer.log 2>/dev/null && break; sleep 0.1; done
}
u33run() { MPCPS5_HOST_RELEASE_KEY=$PUBHEX MPCPS5_UPDATE_API=http://127.0.0.1:$SPORT/latest.json MPCPS5_PS5_APPMETA=$T/appmeta run "$@"; }
upd33() { # [settings]: the daily check offers 9.9, Yes installs it, Yes restarts
	release v9.9 99.000.000
	echo 0 >"$T/root/update-check.txt"
	rm -f $T/root/logs/boot.log $T/root/mpc-hc.ini $T/root/update-pending.txt
	[ -n "${1:-}" ] && ini "$T" "$1"
	u33run "$T" "0:0" "200:key enter;450:key enter;520:key enter;560:quit" ""
}
# a: the whole update; Restart: the helper takes it, the app ends the normal way, and once it has ended (and the
# settle) the title is opened, once
inst33
rc=$(upd33)
for i in $(seq 1 50); do grep -q '\[host\] sceSystemServiceLaunchApp(PPSA99015' $T/inst.txt && break; sleep 0.1; done
sleep 1.5
IL=$T/root/logs/installer.log
expect "[ $rc = 0 ] && grep -q '\[update\] 9.9 installed (in .*): offering the restart' $T/root/logs/boot.log && grep -q 'restart: the helper opens the app again once it has ended' $T/root/logs/boot.log && grep -q '\[boot\] exit 0' $T/root/logs/boot.log" "the update installed; Yes to the restart: the helper said yes, the app ended the normal way"
expect "grep -q 'sceSystemServiceLoadExec(exit)' $T/out.txt && ! grep -q 'sceSystemServiceLoadExec(.*eboot' $T/out.txt && ! grep -q 'Close Media Player Classic PS5 and open it again' $T/root/logs/boot.log" "... no LoadExec of the eboot, no \"close it and open it again\""
G=$(gap $T/root/logs/boot.log '\[boot\] exit 0' $IL 'relaunch: sceSystemServiceLaunchApp\(PPSA99015')
expect "grep -q 'relaunch: pid [0-9]* asks to be opened again once it has ended (language en)' $IL && grep -q 'relaunch: pid [0-9]* ended' $IL && [ \$(launches $T/inst.txt) = 1 ] && ge $G 1.0 && grep -q 'relaunch: PPSA99015 opened again' $IL" "the helper (the payload, as from the autoload) opened the title once, $G s after the app ended (the 1 s settle after it)"
expect "grep -q 'sceSystemServiceLaunchApp(PPSA99015, argv\[0\] null, ctx size 32 user 0x10000001 opt 0 crash 0 check 0)' $T/inst.txt && grep -q 'relaunch: libSceUserService.sprx is loaded' $IL && grep -q 'sceUserServiceGetForegroundUser -> 0, user 0x10000001' $IL" "... for the user in front, with the 32-byte launch context, no argument"
expect "[ -z \"\$(find $T -name app.db -o -name appinfo.db)\" ] && ! grep -q 'runtime error\|AddressSanitizer' $T/inst.txt" "nothing wrote an app.db / appinfo.db; the payload: no sanitizer reports"
nosan "$T"
kill $I33 2>/dev/null; wait $I33 2>/dev/null
# n: the app in German (Language=4) sends "de"; the system refusing, the helper's notice is German
inst33 MPCPS5_HOST_LAUNCH_RC=80940010
rc=$(upd33 "Language=4")
for i in $(seq 1 100); do grep -q 'relaunch failed' $IL && break; sleep 0.1; done
sleep 0.3
expect "[ $rc = 0 ] && grep -q 'relaunch: pid [0-9]* asks to be opened again once it has ended (language de)' $IL && grep -q 'relaunch failed: the system didn.t open it (80940010); notice in de' $IL && notice $T/inst.txt \"$DE_NOTICE\"" "the app in German sends \"de\": the system refused the launch, the notice is German"
expect "grep -q 'App jetzt neu starten, um ihn zu verwenden? (Die Anwendungsinformationen auf dem Startbildschirm zeigen möglicherweise weiterhin eine ältere Version.)' $T/root/logs/boot.log" "... the restart question in German, App Information included"
nosan "$T"
kill $I33 2>/dev/null; wait $I33 2>/dev/null
# o: a 1.5.3 helper (it closes the connection on a request it doesn't know): 1.5.3's message, the app stays
python3 tests/fake_helper.py $MPCPS5_HELPER_PORT old >$T/fake.txt 2>&1 &
FH=$!
listening $MPCPS5_HELPER_PORT
rc=$(upd33)
kill $FH 2>/dev/null; wait $FH 2>/dev/null
expect "[ $rc = 0 ] && grep -q 'restart: not by the helper (the helper is older than 1.5.4' $T/root/logs/boot.log && grep -q '\[ui\] message: Close Media Player Classic PS5 and open it again to use the new version.' $T/root/logs/boot.log && grep -q '\[test\] frame 560: quit' $T/root/logs/boot.log" "an older helper: \"close it and open it again\" (1.5.3's message), the app goes on"
nosan "$T"
kill $SRV33 2>/dev/null; wait $SRV33 2>/dev/null
# b: the asking process never ends (a stand-in: a sleeping process asks for it, raw): after the wait (3 s here), no
# launch (never two copies), the notice; c: a second request while one waits is refused; once it is over, asked again
T=$(newroot t33b)
h33 "$T"
sleep 60 &
P=$!
A1=$($ASK 8 $P PPSA99015 en); A2=$($ASK 8 $P PPSA99015 en)
hwait "$T" 'relaunch failed'
expect "[ \"$A1\" = 'ret 0 ok' ] && hlog $T 'relaunch failed: it was still running after the wait: not opened (never two copies); notice in en' && notice $T/helper.txt \"$EN_NOTICE\" && [ \$(launches $T/helper.txt) = 0 ]" "the asking process still runs after the wait: not opened, the notice (English)"
expect "[ \"$A2\" = 'ret -1 a relaunch is already waiting' ]" "a second request while one waits: refused (${A2})"
sleep 0.5 # (the notice is sent, then the waiting one is over)
A3=$($ASK 8 $P PPSA99015 en)
kill $P; wait $P 2>/dev/null
hwait "$T" 'relaunch: PPSA99015 opened again'
expect "[ \"$A3\" = 'ret 0 ok' ] && [ \$(launches $T/helper.txt) = 1 ]" "over, asked again: taken; the process ends: opened"
stoph33
# d: refused: pid 0 / -1, another title, a process that isn't this title's (gone, or another title's)
h33 "$T"
sleep 60 &
P=$!
R1=$($ASK 8 0 PPSA99015 en); R2=$($ASK 8 -1 PPSA99015 en); R3=$($ASK 8 $P PPSA99013 en)
sleep 30 & Q=$!; kill $Q; wait $Q 2>/dev/null
R4=$($ASK 8 $Q PPSA99015 en)
expect "[ \"$R1\" = 'ret -1 bad pid' ] && [ \"$R2\" = 'ret -1 bad pid' ] && [ \"$R3\" = 'ret -1 not this title' ] && [ \"$R4\" = \"ret -1 the asking process isn't Media Player Classic PS5\" ] && [ \$(grep -c 'asks to be opened again' $T/hroot/logs/helper.log) = 0 ]" "refused: pid 0 and -1 (bad pid), another title id, a pid that isn't running (no thread started)"
stoph33
h33 "$T" MPCPS5_HOST_JB_TITLE=PPSA99013
R5=$($ASK 8 $P PPSA99015 en)
expect "[ \"$R5\" = 'ret -1 asked by PPSA99013' ] && [ \$(grep -c 'asks to be opened again' $T/hroot/logs/helper.log) = 0 ]" "refused: asked by another title's process"
stoph33
kill $P; wait $P 2>/dev/null
expect "! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "the helper: no sanitizer reports"
# e: a copy opened meanwhile (in the process list during the settle, 2 s here): not opened again, no notice
T=$(newroot t33e)
: >$T/pids
sleep 60 &
P=$!
echo $P >$T/pids
SETTLE33=2000 h33 "$T" MPCPS5_HOST_TITLE_PIDS_FILE=$T/pids
A1=$($ASK 8 $P PPSA99015 en)
kill $P; wait $P 2>/dev/null
hwait "$T" 'relaunch: pid [0-9]* ended'
sleep 60 &
Q=$!
echo $Q >$T/pids
hwait "$T" 'opened meanwhile'
sleep 0.5
expect "[ \"$A1\" = 'ret 0 ok' ] && hlog $T \"PPSA99015 was opened meanwhile (pid $Q): not opened again\" && [ \$(launches $T/helper.txt) = 0 ] && ! grep -q notification: $T/helper.txt" "a copy opened while the helper waits: not opened again, no notice"
stoph33; kill $Q; wait $Q 2>/dev/null
# k: a copy appearing right before the launch (as the helper asks for the user): the list read again then sees it
: >$T/pids
sleep 60 & Q=$!
sleep 60 & P=$!
h33 "$T" MPCPS5_HOST_TITLE_PIDS_FILE=$T/pids MPCPS5_HOST_LAUNCH_COPY_PID=$Q
A1=$($ASK 8 $P PPSA99015 en)
kill $P; wait $P 2>/dev/null
hwait "$T" 'nothing to do'
expect "[ \"$A1\" = 'ret 0 ok' ] && hlog $T \"PPSA99015 runs already (pid $Q): not opened again\" && [ \$(launches $T/helper.txt) = 0 ] && ! grep -q notification: $T/helper.txt" "a copy opened right before the launch: seen, not opened again"
stoph33; kill $Q; wait $Q 2>/dev/null
# j: the process list can't be read (the file named, missing): nothing is decided by it -- no launch; at the wait's end
# (2 s), failed with the notice
WAIT33=2000 h33 "$T" MPCPS5_HOST_TITLE_PIDS_FILE=$T/missing
sleep 60 & P=$!
A1=$($ASK 8 $P PPSA99015 en)
kill $P; wait $P 2>/dev/null
hwait "$T" 'relaunch failed'
G=$(gap $T/hroot/logs/helper.log 'asks to be opened again' $T/hroot/logs/helper.log 'relaunch failed')
expect "[ \"$A1\" = 'ret 0 ok' ] && hlog $T \"relaunch failed: the process list couldn't be read: whether it ended is unknown\" && ge $G 1.95 && [ \$(launches $T/helper.txt) = 0 ] && notice $T/helper.txt \"$EN_NOTICE\"" "the process list unreadable: no launch; failed at the wait's end ($G s), the notice"
stoph33
expect "! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "the helper: no sanitizer reports"
# f / g / h: the system refusing the launch; nobody signed in; the launch calls not found: failed, the notice
# (each: the setting | what the log says | launch calls made | what it stands for)
for how in "MPCPS5_HOST_LAUNCH_RC=80940010|the system didn.t open it (80940010)|1|the system refusing the launch" \
	"MPCPS5_HOST_NO_USER=1|no foreground user|0|nobody signed in" \
	"MPCPS5_HOST_LAUNCH_UNRESOLVED=1|the system.s launch calls couldn.t be found|0|the launch calls not found"; do
	IFS='|' read -r SET WHY CALLS WHAT <<<"$how"
	T=$(newroot t33f)
	h33 "$T" "$SET"
	sleep 60 & P=$!
	A1=$($ASK 8 $P PPSA99015 en)
	kill $P; wait $P 2>/dev/null
	hwait "$T" 'relaunch failed'
	expect "[ \"$A1\" = 'ret 0 ok' ] && hlog $T \"relaunch failed: $WHY; notice in en\" && notice $T/helper.txt \"$EN_NOTICE\" && [ \$(launches $T/helper.txt) = $CALLS ]" "$WHAT ($SET): failed, not opened, the notice"
	stoph33
done
expect "hlog $T 'relaunch: sceUserServiceInitialize not found (0)' && hlog $T 'relaunch: sceSystemServiceLaunchApp not found (0)' && ! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "... the calls looked up and refused one by one; no sanitizer reports"
# i: no thread for the wait (MPCPS5_HOST_REFUSE_THREADS=relaunch): answered (refused), a second request answered the
# same, the unlock still served
T=$(newroot t33i)
h33 "$T" MPCPS5_HOST_REFUSE_THREADS=relaunch
sleep 60 & P=$!
A1=$($ASK 8 $P PPSA99015 en); A2=$($ASK 8 $P PPSA99015 en); U=$($ASK 5 $P)
expect "[ \"$A1\" = 'ret -1 no thread for it' ] && [ \"$A2\" = 'ret -1 no thread for it' ] && [ \"$U\" = 'ret 0 ok' ] && hlog $T 'letting it out'" "no thread for the wait: refused, a second request too (nothing left waiting), the unlock still served"
stoph33; kill $P; wait $P 2>/dev/null
# l: the kernel R/W lock: each section held 0.25 s (MPCPS5_HOST_KERNEL_RW_MS; 5 sections stay inside the
# helper's 2 s answer deadline even if the unlock waits for all of them -- the mutex isn't fair); an unlock asked while the relaunch looks up
# its calls is never served during one (the sections never overlap) and comes after the first lookup's end (final
# review: not required before the last one -- the mutex isn't fair, so it may wait past several)
T=$(newroot t33l)
SETTLE33=200 h33 "$T" MPCPS5_HOST_KERNEL_RW_MS=250
sleep 60 & P=$!
sleep 60 & Q=$!
A1=$($ASK 8 $P PPSA99015 en)
kill $P; wait $P 2>/dev/null
hwait "$T" 'kernel R/W: kernel_dynlib_handle libSceUserService.sprx: begins'
sleep 0.2
U=$($ASK 5 $Q)
hwait "$T" 'relaunch: PPSA99015 opened again'
RW=$(python3 - $T/hroot/logs/helper.log <<'PY'
import re, sys
open_, order, ok = None, [], True
for l in open(sys.argv[1], encoding="utf-8"):
    m = re.search(r"kernel R/W: (.*): (begins|ends)$", l)
    if not m:
        continue
    what, ev = m.groups()
    if ev == "begins":
        ok = ok and open_ is None
        open_ = what
    else:
        ok = ok and open_ == what
        open_ = None
        order.append(what)
lookups = [i for i, w in enumerate(order) if w.startswith("kernel_dynlib")]
u = order.index("unlock") if "unlock" in order else -1
print("ok" if ok and open_ is None and lookups and u > lookups[0] else "bad", len(order), u)
PY
)
expect "[ \"$A1\" = 'ret 0 ok' ] && [ \"$U\" = 'ret 0 ok' ] && [ \"\${RW%% *}\" = ok ] && [ \$(launches $T/helper.txt) = 1 ]" "an unlock during the lookup: served between two kernel R/W sections, none overlapping ($RW); then opened"
stoph33; kill $Q; wait $Q 2>/dev/null
# m: the notice's language: arbitrary text in msg2 -> English (and the text goes nowhere), "de" -> German
T=$(newroot t33m)
h33 "$T" MPCPS5_HOST_LAUNCH_UNRESOLVED=1
sleep 60 & P=$!
A1=$($ASK 8 $P PPSA99015 'xx %s%n <script> ZZQ')
kill $P; wait $P 2>/dev/null
hwait "$T" 'relaunch failed'
expect "[ \"$A1\" = 'ret 0 ok' ] && hlog $T '(language other)' && hlog $T 'notice in other' && notice $T/helper.txt \"$EN_NOTICE\" && ! grep -q ZZQ $T/helper.txt $T/hroot/logs/helper.log" "any other text as the language: the English notice; the text itself logged nowhere"
sleep 60 & P=$!
A1=$($ASK 8 $P PPSA99015 de)
kill $P; wait $P 2>/dev/null
for i in $(seq 1 100); do [ "$(grep -c 'relaunch failed' $T/hroot/logs/helper.log)" = 2 ] && break; sleep 0.1; done
expect "[ \"$A1\" = 'ret 0 ok' ] && notice $T/helper.txt \"$DE_NOTICE\"" "\"de\": the German notice"
stoph33
expect "! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "the helper: no sanitizer reports"
# p: App Information's version isn't written: no app.db / appinfo.db, no sceAppInstUtil in what is built
BINS="$BIN $HELPER $INSTALLER"
for f in build/ps5/MediaPlayerClassicPS5.elf build/ps5/MediaPlayerClassicPS5-helper.elf build/app/mpc-pie.elf; do [ -f $f ] && BINS="$BINS $f"; done
expect "! strings $BINS | grep -q -e 'app\.db' -e 'appinfo\.db' -e sceAppInstUtil" "nothing built names app.db, appinfo.db or sceAppInstUtil ($(echo $BINS | wc -w) binaries)"
stop_helpers
fi

if want 34; then
echo "== 34. 1.6: the video drawn by the GPU (ps5-slang-gpu on lavapipe): every pixel format against the CPU path, HDR10 / HLG tone mapping, 4K 10-bit HDR at 24 / 30 / 60 fps, the window over the video, the fall-back"
R3=4 # (the pad bit again: group 33 keeps the helper's answers in R1..R5)
GM=$MEDIA/gpu
[ -f "$GM/x_320x180_1.5.24fps.yuv" ] || bash tests/make_gpu_media.sh "$GM" >/dev/null
COMPARE="python3 $PWD/tests/compare_ppm.py"
# paused on the same picture (an exact seek), dumped: once on the CPU (1.5.4's path), once on the GPU
SHOT="30:cmd pause;32:seek 0.5;60:state;62:quit"
SHOT_AT=55
# (4K: the CPU path's picture after the seek can take longer than 23 frames with two cores; dumped later there)
SHOT4K="30:cmd pause;32:seek 0.5;100:state;102:quit"
twice() { # name file ini -> $WORK/t34-<name>-{cpu,gpu}
	local n=$1 f=$2 i=$3
	for g in 0 1; do
		local t shot=$SHOT at=$SHOT_AT
		# (only the CPU run waits longer: the GPU's picture is there by frame 55, and its run must stay well inside the
		# harness's 180 s on a loaded machine)
		case $n in 4k*) [ $g = 0 ] && shot=$SHOT4K at=95 ;; esac
		t=$(newroot t34-$n-$g)
		ini "$t" "$i"
		cp "$f" "$t/root/media/"
		MPCPS5_HOST_GPU=$g run "$t" "0:0" "$shot" "$at" "$t/root/media/$(basename "$f")" >/dev/null
		[ -f "$t/dump/flip000$at.ppm" ] && mv "$t/dump/flip000$at.ppm" "$t/dump/shot.ppm"
	done
}
# (from row 130: above it, the OSD's box (x 10-234, y 76-128) is timed by the wall clock)
same() { $COMPARE $WORK/t34-$1-0/dump/shot.ppm $WORK/t34-$1-1/dump/shot.ppm 0 130 1920 976 $2 $3; }
# a: every pixel format a decoder gives, against libswscale's bicubic (the GPU's resizer set to bicubic too)
for f in yuv420p yuv422p yuv444p yuv410p yuv420p10le yuv422p10le yuv444p12le yuv420p16le nv12 nv21 gray gray10le gbrp gbrp10le bgra rgba 0rgb; do
	twice $f "$GM/fmt_$f.nut" "VideoResizer=1"
	T=$WORK/t34-$f-1
	r=$(same $f 2 12)
	expect "same $f 2 12 >/dev/null && state $T 1 | grep -q ' gpu 1 \[GPU: upload' && ! grep -q 'drawn on the CPU' $T/root/logs/boot.log" "$f: drawn by the GPU, as the CPU path draws it ($r)"
	nosan "$T"
done
twice yuvj420p "$GM/fmt_yuvj420p.mkv" "VideoResizer=1"
expect "same yuvj420p 2 12 >/dev/null && ! grep -q 'drawn on the CPU' $WORK/t34-yuvj420p-1/root/logs/boot.log" "yuvj420p (MJPEG, full range): the same ($(same yuvj420p 2 12))"
# b: what the GPU path doesn't take is drawn on the CPU, logged once
for f in rgb24 yuyv422 pal8; do
	twice $f "$GM/fmt_$f.nut" "VideoResizer=1"
	expect "same $f 0.2 2 >/dev/null && grep -q 'picture is drawn on the CPU' $WORK/t34-$f-1/root/logs/boot.log" "$f: not a GPU format: drawn on the CPU, said once in the log ($(same $f 0.2 2))"
done
# c: HDR10 and HLG (group 14's pictures and expectations) on the GPU, the console decoder's stand-in too
T=$(newroot t34c)
cp "$MEDIA/hdr_pq.mkv" "$MEDIA/hdr_hlg.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:open $T/root/media/hdr_hlg.mkv;110:state;120:quit" "45,105" "$T/root/media/hdr_pq.mkv")
expect "[ $rc = 0 ] && state $T 1 | grep -q ' gpu 1 '" "exit code 0, the GPU drew"
expect "grey_in $T/dump/flip00045.ppm 480 520 168 179 && grey_in $T/dump/flip00045.ppm 1440 520 248 255" "PQ, BT.2390: 100 nits ~174, 1000 nits white (as the CPU path)"
expect "grey_in $T/dump/flip00105.ppm 480 520 125 138 && grey_in $T/dump/flip00105.ppm 1440 520 205 235" "HLG: its OOTF then the curve (as the CPU path)"
nosan "$T"
T=$(newroot t34c2)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_GPU=1 MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "state $T 1 | grep -q ' hw 1 gpu 1 ' && grey_in $T/dump/flip00045.ppm 480 520 168 179 && grey_in $T/dump/flip00045.ppm 1440 520 248 255" "the console decoder's 10-bit pictures (its stand-in) on the GPU"
expect "grep -q 'pictures handed out in place (16 buffers' $T/root/logs/boot.log && grep -q 'KiB at .* imported (the GPU reads the pictures there)' $T/root/logs/boot.log && grep -q 'pictures read where it wrote them (no copy)' $T/root/logs/boot.log" "... read where the decoder wrote them: its buffers imported once, no CPU copy (16 buffers)"
# the same 4K HDR10 film from the console decoder's stand-in, on the GPU: read in place, and copied once -- the same
# pixels. (Against the CPU path: the 640x360 film after a seek, j below. At 4K lavapipe runs the window at about one frame
# a second and the stand-in's pictures after a paused seek aren't the same ones from run to run.) Pictures decoded
# while paused are copied; MPCPS5_HOST_INPLACE_PAUSED=1 hands them out in place all the same, so the picture compared
# is one read in place (the state line says so). Below the OSD (y 66 of 540).
for ip in 0 1; do
	t=$(newroot t34-hw4k-$ip)
	ini "$t" "VideoResizer=1"
	cp "$GM/hdr4k_30.mkv" "$t/root/media/"
	MPCPS5_HOST_INPLACE_PAUSED=1 MPCPS5_HOST_INPLACE=$ip MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 MPCPS5_HOST_GPU_OUT=960x540 run "$t" "0:0" "30:cmd pause;32:seek 0.5;150:state;152:quit" "145" "$t/root/media/hdr4k_30.mkv" >/dev/null
done
r=$($COMPARE $WORK/t34-hw4k-0/dump/flip00145.ppm $WORK/t34-hw4k-1/dump/flip00145.ppm 0 66 960 540 0.2 2)
expect "$COMPARE $WORK/t34-hw4k-0/dump/flip00145.ppm $WORK/t34-hw4k-1/dump/flip00145.ppm 0 66 960 540 0.2 2 >/dev/null && state $WORK/t34-hw4k-1 1 | grep -q ' hw 1 gpu 1 .*shown picture: in place' && state $WORK/t34-hw4k-0 1 | grep -q 'shown picture: copied' && grep -q 'pictures handed out in place' $WORK/t34-hw4k-1/root/logs/boot.log && ! grep -q 'pictures handed out in place' $WORK/t34-hw4k-0/root/logs/boot.log && ! grep -q 'out of picture buffers' $WORK/t34-hw4k-1/root/logs/boot.log" "4K HDR10 from the console decoder: read in place, the same picture as copied ($r), the decoder never short of buffers"
T=$(newroot t34c3)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
ini "$T" "HdrToneMapping=0"
rc=$(MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "grey_in $T/dump/flip00045.ppm 480 520 124 136 && grey_in $T/dump/flip00045.ppm 1440 520 186 198" "tone mapping Off: the PQ signal as it comes"
T=$(newroot t34c4)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
ini "$T" "HdrToneMapping=2
HdrPeak=150"
rc=$(MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:quit" "45" "$T/root/media/hdr_pq.mkv")
expect "grey_in $T/dump/flip00045.ppm 1440 520 240 255" "Hable, white at 150 nits"
# d: 4K HEVC Main 10 HDR10 at 24, 30, 60 fps and HLG: the GPU's picture against the CPU's, each tone mapping
for fps in 24 30 60; do
	twice 4k$fps "$GM/hdr4k_$fps.mkv" "VideoResizer=1"
	expect "same 4k$fps 2.5 16 >/dev/null && state $WORK/t34-4k$fps-1 1 | grep -q ' gpu 1 '" "3840x2160 10-bit HDR10, $fps fps, BT.2390: the GPU's as the CPU's ($(same 4k$fps 2.5 16))"
done
for m in 0 2 3; do
	twice 4ktm$m "$GM/hdr4k_30.mkv" "VideoResizer=1
HdrToneMapping=$m"
	expect "same 4ktm$m 2.5 16 >/dev/null" "4K HDR10, tone mapping $m (0 off, 2 Hable, 3 Reinhard): the same ($(same 4ktm$m 2.5 16))"
done
twice 4khlg "$GM/hlg4k_30.mkv" "VideoResizer=1"
expect "same 4khlg 2.5 16 >/dev/null" "4K HLG: the same ($(same 4khlg 2.5 16))"
# e: the window over the video: subtitles, the statistics (R3 + Square), the File menu; the GPU's screen against the CPU's
for g in 0 1; do
	T=$(newroot t34e-$g)
	cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
	printf '1\r\n00:00:00,000 --> 00:00:09,000\r\nSubtitles over the GPU picture\r\n' >"$T/root/media/movie.srt"
	MPCPS5_HOST_GPU=$g run "$T" "0:0;40:$((R3 + SQUARE));42:0;70:$OPTIONS;72:0" "26:state;30:cmd pause;32:seek 1;50:state;90:quit" "55,85" "$T/root/media/movie.mp4" >/dev/null
done
# (the statistics' own text differs: the GPU's times, not "scale": rows 130 and below)
r1=$($COMPARE $WORK/t34e-0/dump/flip00085.ppm $WORK/t34e-1/dump/flip00085.ppm 0 130 1920 1080 0.5 2)
expect "$COMPARE $WORK/t34e-0/dump/flip00085.ppm $WORK/t34e-1/dump/flip00085.ppm 0 130 1920 1080 0.5 2 >/dev/null && $COMPARE $WORK/t34e-0/dump/flip00085.ppm $WORK/t34e-1/dump/flip00085.ppm 0 0 250 1080 0.2 1 >/dev/null" "the File menu, the subtitles and the window over the GPU's picture: the CPU's screen ($r1)"
r2=$($COMPARE $WORK/t34e-0/dump/flip00055.ppm $WORK/t34e-1/dump/flip00055.ppm 0 200 1920 1080 0.5 2)
expect "$COMPARE $WORK/t34e-0/dump/flip00055.ppm $WORK/t34e-1/dump/flip00055.ppm 0 200 1920 1080 0.5 2 >/dev/null" "the subtitles over it: the same ($r2)"
expect "$REGION $WORK/t34e-1/dump/flip00055.ppm 20 80 900 110 224 224 224 40 300 >/dev/null" "the statistics shown over the GPU's picture (R3 + Square)"
expect "state $WORK/t34e-1 1 | grep -q 'GPU: upload [0-9.]* ms (one copy), draw [0-9.]* ms, present [0-9.]* ms'" "... with the GPU path's times: $(state $WORK/t34e-1 1 | sed 's/.*\[\(GPU[^]]*\)\].*/\1/')"
nosan "$WORK/t34e-1"
# f: the fall-back: no Vulkan driver; a start that died setting the GPU up; the setting
T=$(newroot t34f1)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
rc=$(VK_ICD_FILENAMES=/nonexistent.json VK_DRIVER_FILES=/nonexistent.json MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'renderer: CPU (the GPU couldn.t be set up' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 0 ' && $CHECK $T/dump/flip00045.ppm 960 400 red >/dev/null" "no Vulkan driver: the CPU draws (logged: $(grep -m1 '\[gpu\]' $T/root/logs/boot.log | cut -c14-90))"
expect "[ ! -e $T/root/logs/gpu-starting ]" "... the start guard's marker removed"
T=$(newroot t34f2)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
mkdir -p "$T/root/logs"
printf 'attempts 0\nworks 1\nbuild test\n' >"$T/root/logs/gpu-attempts"
: >"$T/root/logs/gpu-starting"
rc=$(MPCPS5_HOST_GPU_BUILD_ID=test MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'CPU (a previous GPU start failed' $T/root/logs/boot.log && state $T 1 | grep -q ' hw 1 gpu 0 ' && grep -q '^attempts 4' $T/root/logs/gpu-attempts" "one interrupted GPU start: software rendering immediately, hardware decoding retained"
rc=$(MPCPS5_HOST_GPU_BUILD_ID=test MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "grep -q 'CPU (a previous GPU start failed' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 0 ' && grep -q '^attempts 4' $T/root/logs/gpu-attempts" "... later launches keep recovery without restarting the four-variant cycle"
# A deliberately changed driver compatibility revision permits one new attempt.
rc=$(MPCPS5_HOST_GPU_BUILD_ID=new-driver MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "grep -q 'driver revision changed' $T/root/logs/boot.log && grep -q 'GPU start: variant 1 works' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 1 ' && grep -q '^build new-driver' $T/root/logs/gpu-attempts" "a changed driver revision gets one new attempt, a displayed frame records success"
rc=$(MPCPS5_HOST_GPU_BUILD_ID=new-driver MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "grep -q 'GPU start: the kept variant 1' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 1 '" "... the working GPU is kept for later launches"
nosan "$T"
T=$(newroot t34f8)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
ini "$T" "GpuStart=1"
rc=$(MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'renderer: CPU (the setting (Options > Output: GPU start: CPU only))' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 0 ' && $CHECK $T/dump/flip00045.ppm 960 400 red >/dev/null" "Options > Output > GPU start: CPU only: the GPU isn't started"
nosan "$T"
T=$(newroot t34f3)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
ini "$T" "VideoRenderer=1"
rc=$(MPCPS5_HOST_GPU=1 run "$T" "0:0" "50:state;60:quit" "45" "$T/root/media/movie.mp4")
expect "grep -q 'renderer: CPU (the setting' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 0 ' && grep -q '^VideoRenderer=1' $T/root/mpc-hc.ini" "Options > Output > Software renderer: the CPU, kept in mpc-hc.ini"
# h (review M1): pictures whose plane images the GPU can't make are drawn on the CPU, not left black
T=$(newroot t34h)
cp "$GM/fmt_yuv420p.nut" "$T/root/media/"
ini "$T" "VideoResizer=1"
PS5GPU_TEST_VIDEO_IMAGE_FAIL=3 MPCPS5_HOST_GPU=1 run "$T" "0:0" "$SHOT" "55" "$T/root/media/fmt_yuv420p.nut" >/dev/null
expect "grep -q 'not taken: no memory for its plane images' $T/root/logs/boot.log && grep -q 'picture is drawn on the CPU' $T/root/logs/boot.log && $COMPARE $WORK/t34-yuv420p-0/dump/shot.ppm $T/dump/flip00055.ppm 0 66 1920 976 0.2 2 >/dev/null" "the GPU can't make a picture's images: refused, the CPU draws it (the same as 1.5.4's)"
# i (review M2, M-A): a decoder that holds 10 / 3 of its buffers (its pipeline and reordering), 150 4K HDR10 pictures:
# never short of buffers, no picture lost at the end; as many handed out in place as its buffers leave room for, the
# others copied into the same two planes (the GPU's plane images made once for the film, never swapped)
for h in 10 3; do
	T=$(newroot t34i-$h)
	ini "$T" "VideoResizer=1"
	cp "$GM/hdr4k_30_5s.mkv" "$T/root/media/"
	MPCPS5_HOST_HWDEC_HOLD=$h MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 MPCPS5_HOST_GPU_OUT=320x180 MAXFLIPS=3000 run "$T" "0:0" "60:state;418:state;420:quit" "" "$T/root/media/hdr4k_30_5s.mkv" >/dev/null
	CL=$(grep -o 'closed: .*' $T/root/logs/boot.log | head -1)
	IP=$(echo "$CL" | sed -n 's/.*(\([0-9]*\) in place).*/\1/p')
	SW=$(grep -c 'video planes: .*3840x2160' $T/root/logs/boot.log)
	MIN=$([ $h = 10 ] && echo 45 || echo 120)
	expect "state $T 1 | grep -q ' hw 1 gpu 1 ' && ! grep -q 'out of picture buffers\|handing the video to FFmpeg\|no picture buffer free' $T/root/logs/boot.log && echo '$CL' | grep -q 'closed: 150 access units, 150 pictures (.*), 0 errors'" "a decoder holding $h buffers: never short of them, all 150 pictures out ($CL)"
	expect "[ \"${IP:-0}\" -ge $MIN ] && [ $SW = 1 ]" "... at least $MIN in place (${IP:-0}), the plane images made once ($SW)"
	nosan "$T"
done
# j (review L1): an HDR10 film's peak from the stream's SEI on the console decoder's pictures (the container has none)
T=$(newroot t34j)
cp "$GM/hdr4000.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "40:state;42:quit" "35" "$T/root/media/hdr4000.mkv")
expect "grep -q 'the stream.s mastering display: up to 4000 nits' $T/root/logs/boot.log && grep -q 'MaxCLL 4000' $T/root/logs/boot.log && grep -q 'film peak 4000 nits' $T/root/logs/boot.log" "the console decoder's pictures carry the stream's HDR metadata: tone mapped from 4000 nits"
for g in 0 1; do
	t=$(newroot t34j-$g)
	ini "$t" "VideoResizer=1"
	cp "$GM/hdr4000.mkv" "$t/root/media/"
	MPCPS5_HOST_INPLACE_PAUSED=1 MPCPS5_HOST_HWDEC=$g MPCPS5_HOST_GPU=$g run "$t" "0:0" "$SHOT" "55" "$t/root/media/hdr4000.mkv" >/dev/null
done
r=$($COMPARE $WORK/t34j-0/dump/flip00055.ppm $WORK/t34j-1/dump/flip00055.ppm 0 130 1920 976 2.5 16)
expect "$COMPARE $WORK/t34j-0/dump/flip00055.ppm $WORK/t34j-1/dump/flip00055.ppm 0 130 1920 976 2.5 16 >/dev/null && state $WORK/t34j-1 1 | grep -q ' hw 1 gpu 1 .*shown picture: in place'" "... read in place after a seek, as FFmpeg's pictures on the CPU path are ($r)"
# k (review L2): a start that died at its first picture read in place: the next copies them, the one after tries again
T=$(newroot t34k)
cp "$MEDIA/hdr_pq.mkv" "$T/root/media/"
mkdir -p "$T/root/logs" && : >"$T/root/logs/gpu-inplace-starting"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "40:state;42:quit" "35" "$T/root/media/hdr_pq.mkv")
expect "grep -q \"didn't get past its first picture read in place: this one copies them\" $T/root/logs/boot.log && ! grep -q 'pictures handed out in place' $T/root/logs/boot.log && [ ! -e $T/root/logs/gpu-inplace-starting ]" "a start that died at its first in-place picture: the next copies them (the marker gone)"
# (final review 2) with each frame left in flight until the next Present, as on the console's display
# (PS5GPU_TEST_OFFSCREEN_IN_FLIGHT): the marker goes only once the frame that read the first in-place picture is done
# (two Presents after it was taken), and at the exit the picture the last frame read is let go before its import is
rc=$(PS5GPU_TEST_OFFSCREEN_IN_FLIGHT=1 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "40:state;42:quit" "35" "$T/root/media/hdr_pq.mkv")
GL=$(grep -o 'in-place start guard cleared: present [0-9]*, the first in-place picture taken after present [0-9]*' $T/root/logs/boot.log | head -1)
GN=$(echo "$GL" | sed -n 's/.*cleared: present \([0-9]*\),.*/\1/p'); GP=$(echo "$GL" | sed -n 's/.*after present \([0-9]*\)$/\1/p')
expect "grep -q 'pictures handed out in place' $T/root/logs/boot.log && [ ! -e $T/root/logs/gpu-inplace-starting ] && [ -n '$GN' ] && [ ${GN:-0} -ge \$((${GP:-0} + 2)) ] && state $T 1 | grep -q 'shown picture: in place'" "... the start after it reads them in place again; the marker removed once the frame that read the first one is done ($GL)"
expect "[ \$(grep -c 'KiB at .* imported' $T/root/logs/boot.log) -ge 1 ] && [ \$(grep -c 'KiB at .* imported' $T/root/logs/boot.log) = \$(grep -c 'picture buffers let go (their import released)' $T/root/logs/boot.log) ] && ! grep -q 'only forgotten' $T/root/logs/boot.log" "... at the exit, the last frame still in flight: every import of the decoder's buffers released before the GPU shuts down"
# an orderly exit right after the first pictures: the marker never stays behind
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "14:quit" "" "$T/root/media/hdr_pq.mkv")
expect "[ $rc = 0 ] && [ ! -e $T/root/logs/gpu-inplace-starting ]" "an exit soon after the start: no marker left (gpu::Stop clears it)"
# (review M-B) the marker is written before the import itself: a start that ends there leaves it for the next
rc=$(MPCPS5_HOST_DIE_AT_IMPORT=1 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "40:state;42:quit" "35" "$T/root/media/hdr_pq.mkv")
expect "[ $rc = 3 ] && grep -q 'the app ends at the import' $T/root/logs/boot.log && [ -e $T/root/logs/gpu-inplace-starting ]" "a start that ends in the import itself: the marker already there (exit $rc)"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "40:state;42:quit" "35" "$T/root/media/hdr_pq.mkv")
expect "grep -q \"didn't get past its first picture read in place: this one copies them\" $T/root/logs/boot.log && ! grep -q 'KiB at .* imported' $T/root/logs/boot.log" "... the next start copies the pictures (no import)"
# l (review L3): the GPU lost while playing: the CPU and the scan-out buffers take over, the picture goes on
T=$(newroot t34l)
cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
rc=$(MPCPS5_HOST_GPU_FAIL_AT=40 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "30:state;80:state;90:quit" "85" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q 'three presents failed in a row' $T/root/logs/boot.log && state $T 1 | grep -q ' gpu 1 ' && state $T 2 | grep -q ' gpu 0 ' && $CHECK $T/dump/flip00085.ppm 960 400 red >/dev/null" "the GPU lost while playing: the CPU draws on, into the scan-out buffers (no wait of seconds)"
expect "grep -q 'KiB at .* imported' $T/root/logs/boot.log && grep -q 'picture buffers let go (their import released)' $T/root/logs/boot.log && ! grep -q 'only forgotten' $T/root/logs/boot.log" "... the open decoder's import released while the GPU was still there"
nosan "$T"

# g: Options > Output shows the renderer and the resizer
T=$(newroot t34g)
ini "$T" "KeyOptions=OptionsOutput"
rc=$(MPCPS5_HOST_GPU=1 run "$T" "0:0;30:$OPTIONS;32:0" "60:quit" "50")
expect "[ $rc = 0 ] && grep -q '^VideoResizer=0' $T/root/mpc-hc.ini && grep -q '^VideoRenderer=0' $T/root/mpc-hc.ini" "Options > Output (opened by Options: KeyOptions=OptionsOutput); the settings written"
# (the page's look: checked by eye, $T/dump/flip00050.ppm)
nosan "$T"
fi

if want 35; then
echo "== 35. 1.6 test3: the console decoder at the start (one line, on or off and why), VP9 on it, 8K on the CPU (no freeze), raw .yuv and .y4m"
GM=$MEDIA/gpu
[ -f "$GM/x_320x180_1.5.24fps.yuv" ] || bash tests/make_gpu_media.sh "$GM" >/dev/null
COMPARE="python3 $PWD/tests/compare_ppm.py"
SHOT="30:cmd pause;32:seek 0.5;60:state;62:quit"
# a: the start's lines: the console decoder on (its library loaded, every function bound, the queues made) or off, and
# why; the imports' report (no table in the host build: tools/embed_imports.py writes it into the console's eboot)
T=$(newroot t35a1)
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "20:quit" "")
expect "grep -q '\[hwdec\] console decoder: on (H.264, HEVC 8 / 10-bit, VP9 8 / 10-bit, up to 4K)' $T/root/logs/boot.log && grep -q 'VP9 compute queue: ready' $T/root/logs/boot.log && grep -q '\[imports\] no table in this build' $T/root/logs/boot.log" "the console decoder on at the start, in one line (VP9's own queue made too)"
T=$(newroot t35a2)
rc=$(run "$T" "0:0" "20:quit" "")
expect "grep -q \"\[hwdec\] console decoder: off (its library didn't load: 0x80020063)\" $T/root/logs/boot.log" "... off, and why"
# b: VP9 8-bit (profile 0) and 10-bit (profile 2) on the console decoder's stand-in, drawn by the GPU: every picture
# out, the same picture as FFmpeg's on the CPU path after a paused seek (the 10-bit one read in place)
for b in 8 10; do
	for g in 0 1; do
		t=$(newroot t35b$b-$g)
		ini "$t" "VideoResizer=1"
		cp "$GM/vp9_$b.webm" "$t/root/media/"
		MPCPS5_HOST_INPLACE_PAUSED=1 MPCPS5_HOST_HWDEC=$g MPCPS5_HOST_GPU=$g run "$t" "0:0" "$SHOT" "55" "$t/root/media/vp9_$b.webm" >/dev/null
	done
	T=$WORK/t35b$b-1
	r=$($COMPARE $WORK/t35b$b-0/dump/flip00055.ppm $T/dump/flip00055.ppm 0 130 1920 976 2.5 16)
	expect "grep -q 'video decoder: VP9 640x360 (console decoder)' $T/root/logs/boot.log && state $T 1 | grep -q ' hw 1 gpu 1 ' && $COMPARE $WORK/t35b$b-0/dump/flip00055.ppm $T/dump/flip00055.ppm 0 130 1920 976 2.5 16 >/dev/null" "VP9 $b-bit on the console decoder, drawn by the GPU: FFmpeg's picture ($r)"
	nosan "$T"
	t=$(newroot t35bp$b)
	cp "$GM/vp9_$b.webm" "$t/root/media/"
	MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$t" "0:0" "150:state;152:quit" "" "$t/root/media/vp9_$b.webm" >/dev/null
	expect "grep -q 'closed: 50 access units, 50 pictures (.*), 0 errors' $t/root/logs/boot.log" "... played to its end: all 50 pictures ($(grep -o 'closed: [0-9]* access units, [0-9]* pictures' $t/root/logs/boot.log))"
done
# c: a VP9 the console's decoder refuses: FFmpeg, said why
T=$(newroot t35c)
cp "$GM/vp9_8.webm" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC_NO_VP9=1 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=1 run "$T" "0:0" "40:state;42:quit" "" "$T/root/media/vp9_8.webm")
expect "[ $rc = 0 ] && grep -q 'video decoder: FFmpeg (the decoder couldn.t be created)' $T/root/logs/boot.log && state $T 1 | grep -q 'picture 1 .* hw 0 gpu 1'" "VP9 the console's decoder refuses: FFmpeg plays it"
# c2 (test12; test11's PS5 log): a 720x1280 phone video -- the console's decoder fails its memory query for pictures
# under 1920x1088 (the stand-in does with MPCPS5_HOST_HWDEC_SMALL_FAILS=1): the decoder is made with room for 1920x1088
# and plays it; its pictures are copied right (red at the middle)
T=$(newroot t35c2)
cp "$GM/portrait.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC_SMALL_FAILS=1 MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "60:state;62:quit" "55" "$T/root/media/portrait.mp4")
expect "[ $rc = 0 ] && grep -q '720x1280: the decoder is made with room for 1920x1280 (smaller pictures fail its memory query)' $T/root/logs/boot.log && grep -q 'video decoder: H.264 720x1280 (console decoder)' $T/root/logs/boot.log && ! grep -q 'memory query 0x811d0200' $T/root/logs/boot.log && state $T 1 | grep -q 'picture 1 .* hw 1 ' && $CHECK $T/dump/flip00055.ppm 960 540 red >/dev/null" "a 720x1280 phone video on the console's decoder: made with room for 1920x1088 (the memory query fails below it), plays (red)"
nosan "$T"
# d: 8K HEVC 10-bit (test2: the app froze). (test12, after VLC for PS5) the pictures come from the console's direct memory
# (some 12 GiB), so 8K opens; only with direct memory short too (200 MiB: another app holding it) is it refused, with
# the honest message, at once; the app goes on
for g in 0 1; do
	T=$(newroot t35d$g)
	cp "$GM/hevc8k_10.mp4" "$T/root/media/"
	t0=$(date +%s)
	rc=$(MPCPS5_HOST_DIRECT_FREE_MB=200 MPCPS5_HOST_HEAP_MB=360 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=$g run "$T" "0:0;40:$CROSS;42:0" "60:state;62:quit" "" "$T/root/media/hevc8k_10.mp4")
	dt=$(( $(date +%s) - t0 ))
	expect "[ $rc = 0 ] && grep -q \"failed: This 7680x4320 10-bit video can't be played here: the console's decoder takes up to 4K, and decoding it on the CPU needs about [0-9]* MiB of memory, more than the PS5 leaves the app (200 MiB). Even with the memory, the CPU would show it at about 30 pictures a second.\" $T/root/logs/boot.log && grep -q 'bigger than 4K' $T/root/logs/boot.log && [ $dt -lt 90 ]" "8K HEVC 10-bit with direct memory short (200 MiB), GPU $g: not opened, the message says why; the app answers (${dt} s)"
done
# e: the same 8K with the console's direct memory (12276 MiB free, as the console said): opened, 8 frame threads, the
# pictures from direct memory (the bigmem allocator: blocks mapped, none left to FFmpeg's own), said on the OSD; a quit
# while it decodes ends the app at once
for g in 0 1; do
	T=$(newroot t35e$g)
	cp "$GM/hevc8k_10.mp4" "$T/root/media/"
	t0=$(date +%s)
	# (lavapipe shrinking 8K pictures takes seconds a frame at 960x540: the GPU run's output is 320x180)
	rc=$(MPCPS5_HOST_HEAP_MB=360 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_GPU=$g MPCPS5_HOST_GPU_OUT=320x180 run "$T" "0:0" "90:state;92:quit" "" "$T/root/media/hevc8k_10.mp4")
	dt=$(( $(date +%s) - t0 ))
	BM=$(grep -o 'pictures: [0-9]* blocks mapped, [0-9]* taken from the free list, [0-9]* left to FFmpeg' $T/root/logs/boot.log | head -1)
	expect "[ $rc = 0 ] && grep -q '7680x4320 10-bit, beyond the console.s decoder (4K): FFmpeg on the CPU, its pictures from direct memory (12276 MiB free)' $T/root/logs/boot.log && ! grep -q '7680x4320 10-bit on the CPU: ' $T/root/logs/boot.log && grep -q '\[osd\] 7680x4320 on the CPU (the console.s decoder takes up to 4K): it may play slowly.' $T/root/logs/boot.log && grep -q '\[player\] closed' $T/root/logs/boot.log && state $T 1 | grep -q 'video 0 .* picture 1' && echo '$BM' | grep -q 'pictures: [1-9][0-9]* blocks mapped, .* 0 left to FFmpeg' && [ $dt -lt 120 ]" "8K on the CPU with the memory, GPU $g: opened with 8 frame threads, its pictures from direct memory ($BM), a picture shown; quit while decoding: closed at once (${dt} s)"
	nosan "$T"
done
# (test12) 8K with sound for 3 s (90 pictures): Auto decoding turns fast when the CPU can't keep up (the decoder slowed
# too); Full never does; Fast starts so. The bigmem allocator's blocks come back from its free list (not mapped again)
for m in 0 1 2; do
	T=$(newroot t35e3-$m)
	cp "$GM/hevc8k_10_3s.mp4" "$T/root/media/"
	ini "$T" "DecodeMode=$m"
	rc=$(MPCPS5_HOST_SLOW_DECODE=40,10 MPCPS5_HOST_GPU=1 MPCPS5_HOST_GPU_OUT=320x180 MAXFLIPS=4000 run "$T" "0:0" "600:state;602:quit" "" "$T/root/media/hevc8k_10_3s.mp4")
	BM=$(grep -o 'pictures: [0-9]* blocks mapped, [0-9]* taken from the free list, [0-9]* left to FFmpeg' $T/root/logs/boot.log | head -1)
	case $m in
	0) W="grep -q \"FFmpeg can't keep up (.*): faster decoding, level 1 (no loop filter)\" $T/root/logs/boot.log && grep -q '\[osd\] Fast decoding: softer picture so this video keeps up.' $T/root/logs/boot.log && ! grep -q 'decoding: fast (Options' $T/root/logs/boot.log"; D="Auto: faster decoding once it can't keep up, said on the OSD" ;;
	1) W="grep -q 'decoding: fast (Options > Playback): no loop filter, FFmpeg.s fast flag, from the start' $T/root/logs/boot.log && ! grep -q 'faster decoding, level 1' $T/root/logs/boot.log"; D="Fast: fast from the start (no step to level 1)" ;;
	2) W="! grep -q \"can't keep up\\|decoding: fast\" $T/root/logs/boot.log"; D="Full: never faster" ;;
	esac
	expect "[ $rc = 0 ] && $W && state $T 1 | grep -q 'video 0 audio 1 .* picture 1' && echo '$BM' | grep -q 'pictures: [1-9][0-9]* blocks mapped, [1-9][0-9]* taken from the free list, 0 left to FFmpeg'" "8K with sound, Decoding mode $m -- $D; pictures from direct memory, reused ($BM)"
	nosan "$T"
done
# f: raw .yuv: no size inside; with its size in its name it opens as raw video; .y4m opens
T=$(newroot t35f1)
cp "$GM/BodeMuseum_PQ.yuv" "$T/root/media/"
rc=$(run "$T" "0:0;30:$CROSS;32:0" "40:quit" "" "$T/root/media/BodeMuseum_PQ.yuv")
expect "grep -q 'failed: Raw .yuv files have no picture size or format inside; convert it to .mp4 or .mkv (or .y4m)' $T/root/logs/boot.log" "a raw .yuv without its size: the message says what to do"
T=$(newroot t35f2)
cp "$GM/raw_320x180_10fps.yuv" "$GM/clip.y4m" "$T/root/media/"
rc=$(run "$T" "0:0" "20:state;30:open $T/root/media/clip.y4m;50:state;60:quit" "" "$T/root/media/raw_320x180_10fps.yuv")
expect "grep -q 'raw .yuv: 320x180 yuv420p at 10 fps, from its name' $T/root/logs/boot.log && state $T 1 | grep -q 'video 0 .* picture 1' && state $T 2 | grep -q 'video 0 .* picture 1' && grep -q 'opened .*clip.y4m' $T/root/logs/boot.log" "... with its size in its name: opened as raw video; a .y4m opens"
nosan "$T"
T=$(newroot t35f3)
cp "$GM/raw_320x180_23.976fps.yuv" "$T/root/media/"
rc=$(run "$T" "0:0" "20:state;30:quit" "" "$T/root/media/raw_320x180_23.976fps.yuv")
expect "grep -q 'raw .yuv: 320x180 yuv420p at 23.976 fps, from its name' $T/root/logs/boot.log" "... a fractional rate in its name (23.976fps)"
for n in "clip_320x180.60fps.yuv:60" "x_320x180_1.5.24fps.yuv:5.24"; do
	T=$(newroot t35f4)
	cp "$GM/${n%%:*}" "$T/root/media/"
	rc=$(run "$T" "0:0" "20:quit" "" "$T/root/media/${n%%:*}")
	expect "grep -q 'raw .yuv: 320x180 yuv420p at ${n##*:} fps, from its name' $T/root/logs/boot.log" "... ${n%%:*}: ${n##*:} fps"
done
# g (test4 review): a decoder slower than real time for the film's first 8 s (MPCPS5_HOST_SLOW_DECODE): FFmpeg decodes
# faster a step at a time, measured at the decoder; the video catches up by keyframes; the sound never runs short; once
# the load is gone, the steps come back down to full quality
T=$(newroot t35g)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_SLOW_DECODE=80,6 MAXFLIPS=4000 run "$T" "0:0" "120:state;400:state;1740:state;1742:quit" "" "$T/root/media/slow28.mp4")
U1=$(state $T 1 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p'); U2=$(state $T 2 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p')
expect "grep -q \"FFmpeg can't keep up (.*): faster decoding, level 1 (no loop filter)\" $T/root/logs/boot.log && grep -q \"FFmpeg can't keep up (.*): faster decoding, level 2\" $T/root/logs/boot.log" "a decoder slower than real time: faster decoding, level 1 then 2 ($(grep -c "can't keep up" $T/root/logs/boot.log) steps)"
expect "grep -q 'FFmpeg keeps up again: faster decoding, level 1' $T/root/logs/boot.log && grep -q 'FFmpeg keeps up again: faster decoding, level 0 (full quality)' $T/root/logs/boot.log" "... the load gone: back down a step at a time to full quality"
expect "[ -n '$U1' ] && [ -n '$U2' ] && [ \$(( ${U2:-0} - ${U1:-0} )) -le 5 ]" "... the sound never ran short meanwhile (underruns $U1 -> $U2 between 2 s and 6.7 s)"
expect "grep -q 'the video is .* behind: left out up to the next keyframe' $T/root/logs/boot.log && grep -q 'packets left out; decoding on from a keyframe (the decoder flushed)' $T/root/logs/boot.log" "... more than a second behind: the video goes on from the next keyframe, its decoder flushed"
# (and none of it when the decoder keeps up: the same film, not slowed)
T=$(newroot t35g2)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(run "$T" "0:0" "600:state;602:quit" "" "$T/root/media/slow28.mp4")
expect "[ $rc = 0 ] && ! grep -q \"left out\\|can't keep up\" $T/root/logs/boot.log && state $T 1 | grep -q 'video 0 audio 1'" "... the same film decoded in time: nothing left out, no faster decoding"
# (test6 review) the demuxer never leaves packets out; with the sound short it reads on past its limit, up to a cap
# (MPCPS5_HOST_DEMUX_MB=1: 1 MiB "full", 1.5 MiB the cap; peak: what the demuxer held at most, at the close)
peak_ok() { local k; k=$(sed -n 's/.*demuxer: at most \([0-9]*\) KiB of packets queued.*/\1/p' $1/root/logs/boot.log | tail -1); [ -n "$k" ] && [ "$k" -le $((1536 + 512)) ]; }
peak() { sed -n 's/.*demuxer: at most \([0-9]*\) KiB of packets queued.*/\1/p' $1/root/logs/boot.log | tail -1; }
# real starvation: a decoder far too slow for 10 s, at the small cap: the sound plays on (no new underruns), the video
# catches up by the video thread's keyframe skip, nothing left out by the demuxer, the queues under the cap
T=$(newroot t35g3)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_DEMUX_MB=1 MPCPS5_HOST_SLOW_DECODE=400,10 MAXFLIPS=4000 run "$T" "0:0" "180:state;600:state;602:quit" "" "$T/root/media/slow28.mp4")
U1=$(state $T 1 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p'); U2=$(state $T 2 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p')
expect "[ $rc = 0 ] && ! grep -q 'by the demuxer' $T/root/logs/boot.log && grep -q 'the video is .* behind: left out up to the next keyframe' $T/root/logs/boot.log && [ -n '$U1' ] && [ \$(( ${U2:-0} - ${U1:-0} )) -le 5 ] && peak_ok $T" "a decoder far too slow at a small cap: the sound plays on (underruns ${U1:-?} -> ${U2:-?}), the video catches up by the video thread's skip, nothing left out by the demuxer, at most $(peak $T) KiB queued (cap 1536)"
nosan "$T"
# ... the video thread kept from skipping too: the demuxer reads on to the cap and waits there (no packet left out)
T=$(newroot t35g4)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_DEMUX_MB=1 MPCPS5_HOST_NO_VSKIP=1 MPCPS5_HOST_SLOW_DECODE=400,10 MAXFLIPS=4000 run "$T" "0:0" "600:state;602:quit" "" "$T/root/media/slow28.mp4")
expect "[ $rc = 0 ] && grep -q 'the sound is short behind a full video queue: reading on (up to 1 MiB queued)' $T/root/logs/boot.log && ! grep -q 'left out' $T/root/logs/boot.log && peak_ok $T" "... the video thread kept from skipping: the demuxer reads on for the sound up to the cap and waits there (at most $(peak $T) KiB queued), nothing left out"
nosan "$T"
# (test7 review) ... and with the heap short: the cap is what the heap leaves past the pictures' estimate and a margin,
# never under the normal limit (40 MiB of heap: none past the 1 MiB limit)
T=$(newroot t35g5)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_HEAP_MB=40 MPCPS5_HOST_DEMUX_MB=1 MPCPS5_HOST_NO_VSKIP=1 MPCPS5_HOST_SLOW_DECODE=400,10 MAXFLIPS=4000 run "$T" "0:0" "600:state;602:quit" "" "$T/root/media/slow28.mp4")
K=$(peak $T)
expect "[ $rc = 0 ] && [ \$(grep -c 'the sound is short behind a full video queue: the heap leaves no room past 1 MiB (40 MiB free, about [0-9]* MiB kept for pictures)' $T/root/logs/boot.log) = 1 ] && ! grep -q 'reading on' $T/root/logs/boot.log && [ -n '$K' ] && [ ${K:-9999} -le $((1024 + 512)) ]" "... the heap short (40 MiB): no reading on past the normal limit, said once (at most ${K:-?} KiB queued)"
nosan "$T"
# (test7 review) the console's decoder slower than real time (its host stand-in slowed): the same keyframe skip, the
# decoder reset there as after a seek; the sound plays on
T=$(newroot t35g6)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_SLOW_DECODE=80,6 MAXFLIPS=4000 run "$T" "0:0" "120:state;400:state;402:quit" "" "$T/root/media/slow28.mp4")
U1=$(state $T 1 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p'); U2=$(state $T 2 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p')
expect "[ $rc = 0 ] && state $T 2 | grep -q ' hw 1 ' && grep -q 'the video is .* behind: left out up to the next keyframe' $T/root/logs/boot.log && grep -q 'packets left out; decoding on from a keyframe (the console.s decoder reset)' $T/root/logs/boot.log && [ -n '$U1' ] && [ \$(( ${U2:-0} - ${U1:-0} )) -le 5 ]" "the console's decoder slower than real time: it catches up by keyframes, reset there; the sound plays on (underruns ${U1:-?} -> ${U2:-?})"
nosan "$T"
# (test7 review) a long GOP on the CPU (5 s, 300 packets, at 60 fps), the decoder slower than real time for 12 s: the
# skip goes to the queued keyframe all the same; the sound plays on; once the decoder is fast again the picture is with
# the clock
T=$(newroot t35g7)
cp "$GM/long_gop60.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_SLOW_DECODE=40,12 MAXFLIPS=4000 run "$T" "0:0" "120:state;600:state;1320:state;1322:quit" "" "$T/root/media/long_gop60.mp4")
U1=$(state $T 1 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p'); U2=$(state $T 2 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p')
P3=$(field $T 3 2); I3=$(state $T 3 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p')
expect "[ $rc = 0 ] && grep -q 'the video is .* behind: left out up to the next keyframe' $T/root/logs/boot.log && grep -q 'packets left out; decoding on from a keyframe (the decoder flushed)' $T/root/logs/boot.log && [ -n '$U1' ] && [ \$(( ${U2:-0} - ${U1:-0} )) -le 5 ] && python3 -c 'import sys; sys.exit(0 if abs(${P3:-0} - ${I3:--99}) < 1.0 else 1)'" "a 5 s GOP at 60 fps, the decoder too slow for 12 s: the skip to the queued keyframe (packets $(grep -o '[0-9]* packets left out' $T/root/logs/boot.log | head -1)); the sound plays on (underruns ${U1:-?} -> ${U2:-?}); then the picture with the clock (${I3:-?} s at ${P3:-?} s)"
nosan "$T"
# an exact seek into a 10 s GOP at 60 fps (test5 re-review: the demuxer left out the packets the decoder needed to
# reach the target, and the picture froze), with FFmpeg and the console's decoder (the host stand-in)
# (test8 review) the clock and the sound wait for the seek's first picture (the target's) and go on from it together:
# while the old picture is up the position stands at the target; the first picture is the target's; no catching up
# needed after it (no keyframe skip); nothing read on. (test10 review: the stand-in decodes about 170 pictures a second
# here, so its target is 4.5 s -- 270 pictures from the keyframe -- to be there well within the hold's 3 s)
for hw in 0 1; do
	T=$(newroot t35gop$hw)
	cp "$GM/gop600.mkv" "$T/root/media/"
	if [ $hw = 1 ]; then TG=4.5; else TG=9.5; fi
	SC="60:seek $TG;66:state;240:state;330:state;420:state;510:state;520:quit"
	if [ $hw = 1 ]; then
		rc=$(MPCPS5_HOST_HWDEC=1 MAXFLIPS=4000 run "$T" "0:0" "$SC" "" "$T/root/media/gop600.mkv")
	else
		rc=$(MAXFLIPS=4000 run "$T" "0:0" "$SC" "" "$T/root/media/gop600.mkv")
	fi
	SS=$(for i in 1 2 3 4 5; do state $T $i | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'; done | tr '\n' ' ')
	PP=$(for i in 1 2 3 4 5; do field $T $i 2; done | tr '\n' ' ')
	FP=$(sed -n 's/.*exact seek: its first picture (\([0-9.]*\) s) after \([0-9.]*\) s.*/\1/p' $T/root/logs/boot.log | head -1)
	HW=$(sed -n 's/.*exact seek: its first picture ([0-9.]* s) after \([0-9.]*\) s.*/\1/p' $T/root/logs/boot.log | head -1)
	expect "[ $rc = 0 ] && [ -n '$FP' ] && python3 -c 'import sys; sys.exit(0 if abs(${FP:-0} - $TG) <= 0.1 and ${HW:-99} <= 3.0 else 1)' && ! grep -q 'left out up to the next keyframe\|reading on\|latest one decoded' $T/root/logs/boot.log && python3 -c 'import sys; s = [float(x) for x in \"$SS\".split()]; p = [float(x) for x in \"$PP\".split()]; ok = len(s) == 5 == len(p) and all(abs(b - $TG) < 0.06 for a, b in zip(s, p) if a < $TG - 0.1) and any(a >= $TG - 0.1 and abs(a - b) < 0.3 for a, b in zip(s, p)) and any(a >= $TG - 0.1 and b > a + 0.5 for a, b in zip(s, s[1:])) and abs(s[4] - p[4]) < 1.0; sys.exit(0 if ok else 1)'" "an exact seek into a 10 s GOP at 60 fps (hw $hw): its first picture ${FP:-?} s, the target's (after ${HW:-?} s, the position held at $TG meanwhile), then picture and clock together; no keyframe skip, nothing read on (pictures $SS; clock $PP)"
	nosan "$T"
done
# ... paused while it waits, and sought again (exactly) before its first picture: the second target's picture, the
# position there while paused, then on together once played
T=$(newroot t35gopp)
cp "$GM/gop600.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 MAXFLIPS=4000 run "$T" "0:0" "60:seek 9.5;66:cmd pause;72:seek 12;300:state;310:cmd play;480:state;490:quit" "" "$T/root/media/gop600.mkv")
FP=$(sed -n 's/.*exact seek: its first picture (\([0-9.]*\) s) after.*/\1/p' $T/root/logs/boot.log | tr '\n' ' ')
S1=$(state $T 1 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'); P1=$(field $T 1 2); S2=$(state $T 2 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'); P2=$(field $T 2 2)
expect "[ $rc = 0 ] && [ \"\$(field $T 1 1)\" = 3 ] && python3 -c 'import sys; f = \"$FP\".split(); sys.exit(0 if len(f) == 1 and abs(float(f[0]) - 12) <= 0.1 and abs(${P1:-0} - 12) < 0.06 and abs(${S1:-0} - 12) <= 0.1 and ${P2:-0} > 12.5 and abs(${P2:-0} - ${S2:-0}) < 1.0 else 1)'" "... paused while it waits, then sought again: one first picture, 12 s's (${FP:-?}), the position 12 s while paused (${P1:-?}), then together once played (${S2:-?} s at ${P2:-?} s)"
nosan "$T"
# (test10 review) the hold's 3 s bound: it lets go where the decoder is -- the latest picture decoded before the target,
# the clock and the sound from its time (early, never late; nothing left out, no flush) -- or, with no picture decoded
# yet (a slow source), the sound at the target and the video catching up by the keyframe skip. Each case ends with
# picture and clock together, the picture moving:
#   dec100 / dec30  the decoder at 100 / 30 ms a picture up to 10.6 s, seek to 9.5
#   read2700        the seek's first read failing, the next one taking 2.7 s (the test9 re-review's freeze: none now)
#   read4000        the same taking 4 s: no picture at all by 3 s
#   nokey           seek to 15 with no keyframe after it (the next is the file's end), the decoder at 30 ms up to 12 s
#   to3             seek to 3 with the decoder at 30 ms up to 10.6 s: early, never at the next keyframe (10 s)
# (test11 review) a latest picture more than 1 s before the target is not shown (the sound would go back a GOP with
# it, then jump forward at the keyframe skip): those cases take the "no picture" release; the "near" case (a decoder
# slowed to 100 ms a picture only from 9 s on: the latest picture about 9.4 s, within 1 s of 9.5) shows it
for c in "dec100:MPCPS5_HOST_SLOW_DECODE=100,10.6:9.5:420:600:none" "dec30:MPCPS5_HOST_SLOW_DECODE=30,10.6:9.5:360:480:none" \
	"read2700:MPCPS5_HOST_SEEK_READ_FAULT=2700:9.5:300:420:none" "read4000:MPCPS5_HOST_SEEK_READ_FAULT=4000:9.5:420:600:none" \
	"nokey:MPCPS5_HOST_SLOW_DECODE=30,12:15:300:420:none" "to3:MPCPS5_HOST_SLOW_DECODE=30,10.6:3:600:800:none" \
	"near:MPCPS5_HOST_SLOW_DECODE=100,9.8,9.0:9.5:420:600:latest"; do
	IFS=: read -r name env tg st1 st2 how <<<"$c"
	T=$(newroot t35gb$name)
	cp "$GM/gop600.mkv" "$T/root/media/"
	rc=$(export "$env"; MAXFLIPS=4000 run "$T" "0:0" "60:seek $tg;$st1:state;$st2:state;$((st2 + 2)):quit" "" "$T/root/media/gop600.mkv")
	HB=$(sed -n 's/.*exact seek: .* after \([0-9.]*\) s;.*/\1/p' $T/root/logs/boot.log | head -1)
	FP=$(sed -n 's/.*exact seek: its first picture (\([0-9.]*\) s) after.*/\1/p' $T/root/logs/boot.log | head -1)
	I1=$(state $T 1 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'); I2=$(state $T 2 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'); P2=$(field $T 2 2)
	case $how in
	latest) W="grep -q 'no picture at the target in 3 s: on from the latest one decoded' $T/root/logs/boot.log && python3 -c 'import sys; sys.exit(0 if $tg - 1.0 <= ${FP:-99} < $tg else 1)'" ;;
	# (the picture at the first state is the target's region, or still the one from before the seek -- about 1 s --
	# never a new one more than 1 s before the target)
	none) W="grep -q 'exact seek: no picture in 3 s after' $T/root/logs/boot.log && ! grep -q 'latest one decoded' $T/root/logs/boot.log && python3 -c 'import sys; sys.exit(0 if ${I1:--99} >= $tg - 1.0 or ${I1:--99} < 1.5 else 1)'" ;;
	*) W="true" ;;
	esac
	expect "[ $rc = 0 ] && [ -n '$HB' ] && $W && [ \"\$(field $T 2 1)\" = 2 ] && python3 -c 'import sys; sys.exit(0 if ${HB:-99} <= 3.2 and ${I2:--99} > ${I1:--99} + 1 and abs(${P2:-0} - ${I2:--99}) < 0.5 else 1)'" "the hold's bound ($name): let go after ${HB:-?} s$([ -n "$FP" ] && echo " at ${FP} s"), then together and moving (${I1:-?} s; ${I2:-?} s at ${P2:-?} s)"
	nosan "$T"
done
# (test10 review) no underruns counted between files: a file closed, 3 s with none open, then another
T=$(newroot t35gu)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MAXFLIPS=4000 run "$T" "0:0" "120:state;122:cmd close;300:open $T/root/media/slow28.mp4;420:state;422:quit" "" "$T/root/media/slow28.mp4")
U1=$(state $T 1 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p'); U2=$(state $T 2 | sed -n 's/.* underruns \([0-9]*\) .*/\1/p')
expect "[ $rc = 0 ] && [ -n '$U1' ] && [ -n '$U2' ] && [ \$(( ${U2:-999} - ${U1:-0} )) -le 10 ]" "no underruns counted while no file plays (${U1:-?} before the close, ${U2:-?} 2 s into the next file)"
nosan "$T"
# (test9 review) an exact seek past the end of the video track (5 s of video, 300 s of sound) at a 1 MiB limit: no
# picture can come; let go at once (it waited the whole bound: the sound held, the demuxer stopped at its limit)
T=$(newroot t35gopv)
cp "$GM/short_video.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_DEMUX_MB=1 MAXFLIPS=4000 run "$T" "0:0" "60:seek 60;180:state;182:quit" "" "$T/root/media/short_video.mkv")
HV=$(sed -n 's/.*exact seek: no picture can come (the video track ended before the target) after \([0-9.]*\) s.*/\1/p' $T/root/logs/boot.log | head -1)
P1=$(field $T 1 2)
expect "[ $rc = 0 ] && [ -n '$HV' ] && python3 -c 'import sys; sys.exit(0 if ${HV:-99} < 1.0 and ${P1:-0} > 60.5 else 1)'" "an exact seek past the end of the video track: let go after ${HV:-?} s (under 1), the sound on (${P1:-?} s)"
nosan "$T"
# (test9 review) the console's decoder giving up during the hold (MPCPS5_HOST_HWDEC_FAIL: every access unit from the
# 35th fails -- after the seek, on the way to its target -- and after five resets FFmpeg takes over at a later keyframe): no
# stuck state, held at most 3 s, the sound starting with that picture (not before it), then picture and clock together
T=$(newroot t35gopf)
cp "$GM/slow28.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_FAIL=35 MAXFLIPS=4000 run "$T" "0:0" "30:seek 12.9;150:state;330:state;340:quit" "" "$T/root/media/slow28.mp4")
HF=$(sed -n 's/.*exact seek: .* after \([0-9.]*\) s;.*/\1/p' $T/root/logs/boot.log | head -1)
P1=$(field $T 1 2); I1=$(state $T 1 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'); P2=$(field $T 2 2); I2=$(state $T 2 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p')
SK=$(line $T 'frame 30: seek'); HD=$(line $T 'video decoder: FFmpeg from now on')
expect "[ $rc = 0 ] && [ -n '$HD' ] && [ ${HD:-0} -gt ${SK:-99999} ] && [ \"\$(field $T 2 1)\" = 2 ] && [ -n '$HF' ] && python3 -c 'import sys; sys.exit(0 if ${HF:-99} <= 3.1 and ${I2:--99} > ${I1:--99} and abs(${P2:-0} - ${I2:--99}) < 0.5 else 1)'" "the console's decoder giving up during an exact seek's hold: held ${HF:-?} s (at most 3), FFmpeg on, then together (${I1:-?} s at ${P1:-?} s; ${I2:-?} s at ${P2:-?} s)"
nosan "$T"
# ... and nothing read on when nothing starves: a 60 fps film with FLAC sound (100 ms packets), and a 60 s film whose
# sound ends at 5 s (at the 1 MiB limit the demuxer is still reading when the sound is over); the picture plays on
for f in flac60.mkv short_sound60.mkv; do
	T=$(newroot t35g-$f)
	cp "$GM/$f" "$T/root/media/"
	rc=$(MPCPS5_HOST_DEMUX_MB=1 run "$T" "0:0" "300:state;900:state;902:quit" "" "$T/root/media/$f")
	S1=$(state $T 1 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p'); S2=$(state $T 2 | sed -n 's/.*; its time \(-\{0,1\}[0-9.]*\)$/\1/p')
	expect "[ $rc = 0 ] && ! grep -q 'left out\|reading on' $T/root/logs/boot.log && python3 -c 'import sys; sys.exit(0 if ${S2:-0} > ${S1:-0} + 5 else 1)' && peak_ok $T" "$f at the 1 MiB limit: nothing left out or read on, the picture plays on (${S1:-?} s, then ${S2:-?} s), at most $(peak $T) KiB queued"
done
SZ=$(stat -c %s "$GM/short_sound60.mkv")
expect "[ $SZ -gt \$((4 << 20)) ] && state $WORK/t35g-short_sound60.mkv 2 | grep -Eq 'pos ([6-9]|[1-5][0-9])\.' && peak_ok $WORK/t35g-short_sound60.mkv" "... short_sound60.mkv ($((SZ >> 20)) MiB): at the state past the sound's end (5 s) and still reading the file (at most 1.5 MiB of it queued)"
nosan "$T"
# h (test4 review): a 4K film on the CPU with the heap a console start showed (360 MiB): FFmpeg keeps its frame threads
# (test5 review) up to 4K the frame threads the heap takes: 8 with plenty, 3 with 600 MiB (24.9 MB pictures: 6
# references + 3 being decoded + 6 queued + 2 drawn in 0.7 x 600 MiB), none (slice threads, 3 queued) with 360 MiB;
# never refused
# (test12) a 4K 10-bit picture is 25 MiB: from direct memory, whose free run is the budget (MPCPS5_HOST_DIRECT_FREE_MB;
# 90% of it): 8 threads with 12 GiB, 3 with 470 MiB, none with 300 MiB
for mb in 12276 470 300; do
	T=$(newroot t35h$mb)
	cp "$GM/hdr4k_30.mkv" "$T/root/media/"
	MPCPS5_HOST_DIRECT_FREE_MB=$mb MPCPS5_HOST_HEAP_MB=360 MPCPS5_HOST_GPU=1 MPCPS5_HOST_GPU_OUT=320x180 run "$T" "0:0" "60:state;62:quit" "" "$T/root/media/hdr4k_30.mkv" >/dev/null
done
expect "! grep -q '3840x2160 10-bit on the CPU' $WORK/t35h12276/root/logs/boot.log && grep -q '3840x2160 10-bit on the CPU: 3 frame threads (about [0-9]* MiB of 470 MiB free of direct memory)' $WORK/t35h470/root/logs/boot.log && grep -q '3840x2160 10-bit on the CPU: about [0-9]* MiB of 300 MiB free of direct memory; no frame threads, 3 pictures queued' $WORK/t35h300/root/logs/boot.log && state $WORK/t35h300 1 | grep -q 'picture 1' && ! grep -q 'player\] failed' $WORK/t35h300/root/logs/boot.log" "4K on the CPU: 8 frame threads with 12 GiB of direct memory, 3 with 470 MiB, none with 300 MiB (still played)"
# (test6 review) past its own estimate's budget the 4K path opens, and the log says so
expect "grep -q 'no frame threads, 3 pictures queued (warning: past the share kept for pictures; it may run short of memory)' $WORK/t35h300/root/logs/boot.log && ! grep -q 'warning' $WORK/t35h470/root/logs/boot.log" "... 300 MiB: past the budget (about $(grep -o 'about [0-9]* MiB of 300' $WORK/t35h300/root/logs/boot.log | head -1)): opened with a warning in the log"
# (test6 review) the console's decoder gives up (MPCPS5_HOST_HWDEC_FAIL=1: it refuses the video): FFmpeg takes over with the threads
# the heap took at the open (600 MiB: 3; 360 MiB: none, 3 pictures queued), not 8
for mb in 470 300; do
	T=$(newroot t35hf$mb)
	cp "$GM/hdr4k_30_k5.mkv" "$T/root/media/"
	MPCPS5_HOST_DIRECT_FREE_MB=$mb MPCPS5_HOST_HEAP_MB=360 MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_FAIL=1 MPCPS5_HOST_GPU=1 MPCPS5_HOST_GPU_OUT=320x180 MAXFLIPS=4000 run "$T" "0:0" "240:state;242:quit" "" "$T/root/media/hdr4k_30_k5.mkv" >/dev/null
done
expect "grep -q 'video decoder: HEVC 3840x2160 (console decoder)' $WORK/t35hf470/root/logs/boot.log && grep -q \"3840x2160 10-bit on the CPU should the console's decoder give up: 3 frame threads\" $WORK/t35hf470/root/logs/boot.log && grep -q 'FFmpeg with 3 frame threads$' $WORK/t35hf470/root/logs/boot.log && grep -q \"3840x2160 10-bit on the CPU should the console's decoder give up: about [0-9]* MiB of 300 MiB free of direct memory; no frame threads\" $WORK/t35hf300/root/logs/boot.log && grep -q 'FFmpeg with 8 slice threads, 3 pictures queued' $WORK/t35hf300/root/logs/boot.log && state $WORK/t35hf300 1 | grep -q 'picture 1'" "the console's decoder giving up on 4K 10-bit: FFmpeg takes over with 3 frame threads at 470 MiB of direct memory, none (3 pictures queued) at 300 MiB"
nosan "$T"
# i (test4 review): VP9 with blocks showing no frame alone (superframes split): its pictures keep their times whether
# the decoder gives a picture for such a block (as VLC found the console does) or none (as FFmpeg); 12-bit to FFmpeg
for mode in one none; do
	for g in 0 1; do
		t=$(newroot t35i$mode-$g)
		ini "$t" "VideoResizer=1"
		cp "$GM/vp9_split.nut" "$t/root/media/"
		MPCPS5_HOST_VP9_HIDDEN=$mode MPCPS5_HOST_INPLACE_PAUSED=1 MPCPS5_HOST_HWDEC=$g MPCPS5_HOST_GPU=$g run "$t" "0:0" "$SHOT" "55" "$t/root/media/vp9_split.nut" >/dev/null
	done
	T=$WORK/t35i$mode-1
	r=$($COMPARE $WORK/t35i$mode-0/dump/flip00055.ppm $T/dump/flip00055.ppm 0 130 1920 976 2.5 16)
	expect "grep -q 'video decoder: VP9 640x360 (console decoder)' $T/root/logs/boot.log && $COMPARE $WORK/t35i$mode-0/dump/flip00055.ppm $T/dump/flip00055.ppm 0 130 1920 976 2.5 16 >/dev/null" "VP9 with lone hidden blocks, the decoder giving $mode picture for them: the picture FFmpeg shows at that time ($r)"
done
expect "grep -q 'VP9: [1-9][0-9]* blocks gave no picture, .* dropped: 0)' $WORK/t35inone-1/root/logs/boot.log && grep -q 'VP9: 0 blocks gave no picture, .* dropped: [1-9][0-9]*)' $WORK/t35ione-1/root/logs/boot.log" "... (none: such blocks get no time of their own; one: they give the picture that is dropped: $(grep -o 'VP9: [0-9]* blocks.*' $WORK/t35inone-1/root/logs/boot.log | cut -c1-40) / $(grep -o 'dropped: [0-9]*' $WORK/t35ione-1/root/logs/boot.log))"
T=$(newroot t35i12)
cp "$GM/vp9_12.webm" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "30:state;32:quit" "" "$T/root/media/vp9_12.webm")
expect "grep -q 'video decoder: FFmpeg (VP9 12-bit)' $T/root/logs/boot.log && state $T 1 | grep -q 'picture 1'" "VP9 12-bit: FFmpeg (the console's decoder takes 10)"
# j (test4 review): the import table the console's eboot carries, read by the app: a synthetic one naming the host's own
# words (two bound, one left at 0), and a malformed one (a slot past the image's end)
B=$BIN
AV=$(nm $B | awk '$3=="mpcps5_crash_anchor"{print $1}'); SV=$(nm $B | awk '$3=="mpcps5_test_slots"{print $1}')
SPAN=$(readelf -lW $B | python3 -c "import sys; print('%x' % max(int(p[2], 16) + int(p[5], 16) for p in (l.split() for l in sys.stdin) if p and p[0] == 'LOAD'))")
printf 'MPCIMP03\n%s\n%s\n%x libTest.sprx bound_one\n%x libTest.sprx unbound_one\n%x libTest.sprx bound_two\n' $AV $SPAN $((0x$SV)) $((0x$SV + 8)) $((0x$SV + 16)) >$WORK/imports-ok.txt
printf 'MPCIMP03\n%s\n%s\n%x libTest.sprx bound_one\n%x libTest.sprx far_away\n' $AV $SPAN $((0x$SV)) $((0x$SPAN + 64)) >$WORK/imports-bad.txt
printf 'MPCIMP03\n%s\n%s\n%x libTest.sprx misaligned\n' $AV $SPAN $((0x$SV + 4)) >$WORK/imports-odd.txt
T=$(newroot t35j)
rc=$(MPCPS5_HOST_IMPORT_TABLE=$WORK/imports-ok.txt run "$T" "0:0" "10:quit" "")
expect "grep -q '\[imports\] bound by the console: libTest.sprx 2/3' $T/root/logs/boot.log && grep -q '\[imports\] 1 left at 0: unbound_one' $T/root/logs/boot.log" "the import table read at the start: per module, and the names left at 0"
rc=$(MPCPS5_HOST_IMPORT_TABLE=$WORK/imports-bad.txt run "$T" "0:0" "10:quit" "")
expect "[ $rc = 0 ] && grep -q '\[imports\] table malformed' $T/root/logs/boot.log" "... a malformed one: said, not read further, no fault"
rc=$(MPCPS5_HOST_IMPORT_TABLE=$WORK/imports-odd.txt run "$T" "0:0" "10:quit" "")
expect "[ $rc = 0 ] && grep -q '\[imports\] table malformed' $T/root/logs/boot.log" "... a misaligned slot: the same"
# (test5 review) the crash handler's pieces, called directly (its signals are the sanitizer's on the host)
rc=$(MPCPS5_HOST_CRASH_SELFTEST=1 run "$T" "0:0" "10:quit" "")
expect "grep -q '\[crash\] self-test: ok' $T/root/logs/boot.log && grep -q '^\[crash\] self-test: written through the handler.s own fd' $T/root/logs/boot.log" "the crash handler's line building, safe reads (an unmapped page too), address naming, and its own fd (made while logging) writing into boot.log"
nosan "$T"
fi

echo
if want 36; then
echo "== 36. Dark mode: controller selection, Apply/OK/Cancel, restart, menus, video and subtitles"
T=$(newroot t36)
# The theme is the last Player control; the existing controls keep their original tab order.
TO_THEME=""
for frame in 20 24 28 32 36 40 44 48 52; do TO_THEME="$TO_THEME;$frame:key r1"; done
S="10:cmd options$TO_THEME;56:key right;60:key r1;64:key r1;68:key r1;72:key enter;90:key escape;110:key menu;120:key escape;130:quit"
rc=$(run "$T" "0:0" "$S" "58,80,100,115")
expect "[ $rc = 0 ]" "controller-driven Apply exits normally"
expect "$CHECK $T/dump/flip00058.ppm 960 600 240 240 240 2 >/dev/null" "choosing Dark waits for Apply"
expect "$CHECK $T/dump/flip00080.ppm 960 600 32 32 32 2 >/dev/null" "Apply repaints the open Options dialog"
expect "$CHECK $T/dump/flip00100.ppm 900 10 28 28 28 2 >/dev/null && $CHECK $T/dump/flip00100.ppm 900 1025 32 32 32 2 >/dev/null" "caption and toolbar use Dark"
expect "$CHECK $T/dump/flip00115.ppm 30 584 40 40 40 2 >/dev/null" "File menu uses Dark"
expect "grep -q '^DarkMode=1$' $T/root/mpc-hc.ini" "Apply persists Dark"
nosan "$T"
rc=$(run "$T" "0:0" "20:quit" "15")
expect "[ $rc = 0 ] && grep -q '\[ui\] theme: Dark' $T/root/logs/boot.log && $CHECK $T/dump/flip00015.ppm 900 10 28 28 28 2 >/dev/null" "a new process restores Dark"
# Cancel a pending change to Light.
rc=$(run "$T" "0:0" "10:cmd options$TO_THEME;56:key left;65:key escape;90:quit" "80")
expect "[ $rc = 0 ] && grep -q '^DarkMode=1$' $T/root/mpc-hc.ini && $CHECK $T/dump/flip00080.ppm 900 10 28 28 28 2 >/dev/null" "Cancel leaves the applied theme and saved preference intact"
# OK applies Light and closes the sheet (one tab from Theme to OK).
rc=$(run "$T" "0:0" "10:cmd options$TO_THEME;56:key left;60:key r1;65:key enter;90:quit" "80")
expect "[ $rc = 0 ] && grep -q '^DarkMode=0$' $T/root/mpc-hc.ini && $CHECK $T/dump/flip00080.ppm 900 10 255 255 255 2 >/dev/null" "OK restores Light and saves it"
nosan "$T"
# A malformed new setting must not affect old preferences or prevent startup.
printf '[Settings]\nDarkMode=1junk\nVolume=73\n' >"$T/root/mpc-hc.ini"
rc=$(run "$T" "0:0" "20:quit" "15")
expect "[ $rc = 0 ] && grep -q '\[ui\] theme: Light' $T/root/logs/boot.log && grep -q '^Volume=73$' $T/root/mpc-hc.ini" "invalid theme falls back to Light and preserves volume"
for renderer in 0 1; do
	T=$(newroot t36video-$renderer)
	cp "$MEDIA/red.mp4" "$T/root/media/movie.mp4"
	printf '1\n00:00:00,500 --> 00:00:05,000\nTheme-independent subtitles\n' >"$T/root/media/movie.srt"
	printf '[Settings]\nDarkMode=1\n' >"$T/root/mpc-hc.ini"
	rc=$(MPCPS5_HOST_GPU=$renderer run "$T" "0:0" "100:state;110:quit" "90" "$T/root/media/movie.mp4")
	expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 960 400 red >/dev/null" "renderer $renderer: the video keeps its original red color"
	expect "$REGION $T/dump/flip00090.ppm 500 700 1420 960 255 255 255 30 2000 >/dev/null" "renderer $renderer: subtitles keep their white text"
	expect "$CHECK $T/dump/flip00090.ppm 900 1025 32 32 32 2 >/dev/null" "renderer $renderer: dark controls over video"
	if [ "$renderer" = 1 ]; then
		expect "grep -q '\[gpu\] video: drawn by the GPU' $T/root/logs/boot.log" "GPU renderer really initialized"
	fi
	nosan "$T"
done
fi

echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
