#!/bin/bash
# Media Player Classic PS5 host tests: the real port code (main-boot, the PS5 layer, the player: FFmpeg, libsmb2,
# the MPC-HC window; the installer and the helper) on Linux, with the PS5 calls faked by host/sce_host.cpp. A
# scripted pad (and a script of commands, MPCPS5_HOST_SCRIPT) drives the app; flips are de-tiled to PPM and
# checked. The media are made by tests/make_media.sh with the host's ffmpeg.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/build/host/mpc-ps5-host
INSTALLER=$PWD/build/host/mpc-ps5-installer
HELPER=$PWD/build/host/mpc-ps5-helper
# ports nobody listens on, so the app's ordinary runs find no helper and no ELF loader
export MPCPS5_HELPER_PORT=$((20000 + RANDOM % 10000)) MPCPS5_ELFLDR_PORT=$((30000 + RANDOM % 10000)) MPCPS5_JB_NO_OTHERS=1
CHECK="python3 $PWD/tests/check_ppm.py"
REGION="python3 $PWD/tests/check_region.py"
WORK=${WORK:-$(mktemp -d)}
ONLY=${ONLY:-}
PASS=0
FAIL=0
MEDIA=$WORK/media
[ -f "$MEDIA/surround71.mkv" ] || bash tests/make_media.sh "$MEDIA" >/dev/null

# pad bits
CROSS=4000; CIRCLE=2000; TRIANGLE=1000; SQUARE=8000; UP=10; DOWN=40; LEFT=80; RIGHT=20; L1=400; R1=800; L2=100; R2=200
L3=2; R3=4; OPTIONS=8

ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
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
near() { python3 -c "import sys; sys.exit(0 if abs(float('$1') - float('$2')) <= float('$3') else 1)"; }

if want 1; then
echo "== 1. the window without a file: MPC-HC's chrome, the logo; File > Exit from the keyboard menu"
T=$(newroot t1)
# Options opens the File menu (keyboard), Up wraps to Exit, Cross runs it
rc=$(run "$T" "0:0;60:$OPTIONS;62:0;70:$UP;72:0;80:$CROSS;82:0" "" "40,75")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'Media Player Classic PS5 $(sed -n 's/^VERSION ?= //p' Makefile), FFmpeg' $T/root/logs/boot.log" "boot log names the player and FFmpeg"
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
# R1 at 60 (chapter 2: green), R1 at 140 (chapter 3: blue), L1 at 220 (back to green)
rc=$(run "$T" "0:0;60:$R1;62:0;140:$R1;142:0;220:$L1;222:0" "40:state;120:state;200:state;280:state;290:cmd 2001;330:state;340:quit" \
	"30,120,200,280" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(field $T 1 5)\" = 2 ]" "the default audio track (Portuguese, stream 2)"
expect "[ \"\$(field $T 1 6)\" = 3 ]" "the default subtitle track (ASS, stream 3)"
expect "$CHECK $T/dump/flip00030.ppm 960 300 red >/dev/null" "chapter 1: red"
expect "$CHECK $T/dump/flip00120.ppm 960 300 green >/dev/null" "R1: chapter 2, green"
expect "$CHECK $T/dump/flip00200.ppm 960 300 blue >/dev/null" "R1: chapter 3, blue"
expect "$CHECK $T/dump/flip00280.ppm 960 300 green >/dev/null" "L1: back to chapter 2, green"
expect "$REGION $T/dump/flip00030.ppm 500 700 1420 960 255 255 255 30 3000 >/dev/null" "the embedded subtitle drawn (white text at the bottom)"
expect "[ \"\$(field $T 5 5)\" = 1 ]" "Play > Audio Track > English: audio stream 1"
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
sleep 1
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
sleep 2
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

if want 13; then
echo "== 13. the PS5's video decoder (a stand-in on the host): H.264 / HEVC on it, seeking, handing back to FFmpeg"
T=$(newroot t13)
cp "$MEDIA/hevc10.mkv" "$MEDIA/red.mp4" "$MEDIA/av1.mkv" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "50:state;60:open $T/root/media/red.mp4;120:state;130:seek 4;190:state;200:open $T/root/media/av1.mkv;250:state;260:quit" \
	"45,115,185,245" "$T/root/media/hevc10.mkv")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'hardware decoding available' $T/root/logs/boot.log" "the decoder's library and compute queue made at start"
expect "grep -q 'video decoder: HEVC 640x360 (console decoder)' $T/root/logs/boot.log && state $T 1 | grep -q ' hw 1$'" "HEVC Main 10 on the console's decoder"
expect "grep -q 'pictures 640x368 (shown 640x360) 10-bit' $T/root/logs/boot.log" "10-bit pictures, the padding left out"
expect "$CHECK $T/dump/flip00045.ppm 960 400 255 0 255 12 >/dev/null" "its picture: magenta"
expect "grep -q 'video decoder: H.264 640x360 (console decoder)' $T/root/logs/boot.log && state $T 2 | grep -q ' hw 1$'" "H.264 on it too"
expect "$CHECK $T/dump/flip00115.ppm 960 400 red >/dev/null" "its picture: red"
expect "near \$(field $T 3 2) 4.5 1.2 && grep -q 'going on from a keyframe' $T/root/logs/boot.log" "a seek: on from the next keyframe"
expect "$CHECK $T/dump/flip00185.ppm 960 400 red >/dev/null" "the picture after the seek"
expect "grep -q 'video decoder: FFmpeg (not H.264 or HEVC)' $T/root/logs/boot.log && state $T 4 | grep -q ' hw 0$'" "AV1 stays with FFmpeg"
expect "$CHECK $T/dump/flip00245.ppm 960 400 255 255 0 60 >/dev/null" "its picture: yellow"
nosan "$T"
T=$(newroot t13b)
cp "$MEDIA/chapters.mkv" "$T/root/media/"
# chapters.mkv: a keyframe every second; red 0-3 s, green 3-6 s, blue 6-9 s
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_FAIL=20 run "$T" "0:0" "480:state;490:quit" "470" "$T/root/media/chapters.mkv")
expect "[ $rc = 0 ] && [ \$(grep -c 'stream error 0x811d0301: reset' $T/root/logs/boot.log) = 5 ]" "stream errors: reset, on from the next keyframe, five times"
expect "grep -q 'handing the video to FFmpeg: stream errors keep coming' $T/root/logs/boot.log && grep -q 'video decoder: FFmpeg from now on' $T/root/logs/boot.log" "then the video is handed to FFmpeg"
expect "state $T 1 | grep -q ' hw 0$' && near \$(field $T 1 2) 8.5 0.8 && $CHECK $T/dump/flip00470.ppm 960 400 blue >/dev/null" "it plays on to the end: the blue part"
nosan "$T"
T=$(newroot t13c)
cp "$MEDIA/red.mp4" "$T/root/media/"
rc=$(MPCPS5_HOST_HWDEC=1 MPCPS5_HOST_HWDEC_REFUSE=1 run "$T" "0:0" "60:state;70:quit" "55" "$T/root/media/red.mp4")
expect "grep -q \"video decoder: FFmpeg (the decoder couldn't be created)\" $T/root/logs/boot.log && $CHECK $T/dump/flip00055.ppm 960 400 red >/dev/null" "a decoder that can't be made: FFmpeg"
T=$(newroot t13d)
cp "$MEDIA/red.mp4" "$T/root/media/"
ini "$T" "HardwareDecoding=0"
rc=$(MPCPS5_HOST_HWDEC=1 run "$T" "0:0" "60:state;70:quit" "55" "$T/root/media/red.mp4")
expect "grep -q 'video decoder: FFmpeg (turned off in the options)' $T/root/logs/boot.log && state $T 1 | grep -q ' hw 0$'" "Options > Output > Hardware decoding off: FFmpeg"
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
expect "state $T 1 | grep -q ' hw 1$' && grey_in $T/dump/flip00045.ppm 480 520 168 179 && grey_in $T/dump/flip00045.ppm 1440 520 248 255" "the same from the console's decoder (10-bit pictures)"
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
release() { # tag contentVersion [bad]
	rm -rf "$T/rel" && mkdir -p "$T/rel" && cp -r "$T/homebrew/PPSA99015" "$T/rel/PPSA99015" && rm -f "$T/rel/PPSA99015/manifest.sha256" "$T/rel/PPSA99015/sce_sys/old.bin"
	echo "eboot $1" >"$T/rel/PPSA99015/eboot.bin"; echo "new art" >"$T/rel/PPSA99015/sce_sys/icon0.png"; echo extra >"$T/rel/PPSA99015/sce_sys/extra.bin"
	sed -i "s/\"contentVersion\": \"[0-9.]*\"/\"contentVersion\": \"$2\"/" "$T/rel/PPSA99015/sce_sys/param.json"
	python3 tools/make_release_zip.py "$T/rel/PPSA99015" PPSA99015 "$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip" >/dev/null
	local sha size
	sha=$(sha256sum "$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip" | cut -c1-64); size=$(stat -c %s "$SRV/MediaPlayerClassicPS5-$1-PPSA99015.zip")
	[ -n "${3:-}" ] && sha=0000${sha:4}
	printf '{"tag_name":"%s","name":"Media Player Classic PS5 %s","assets":[{"name":"MediaPlayerClassicPS5-%s.elf","size":1,"browser_download_url":"http://127.0.0.1:%s/x.elf"},{"name":"MediaPlayerClassicPS5-%s-PPSA99015.zip","size":%s,"digest":"sha256:%s","browser_download_url":"http://127.0.0.1:%s/MediaPlayerClassicPS5-%s-PPSA99015.zip"}]}' \
		"$1" "$1" "$1" $SPORT "$1" $size $sha $SPORT "$1" >"$SRV/latest.json"
}
u16run() { MPCPS5_UPDATE_API=http://127.0.0.1:$SPORT/latest.json MPCPS5_PS5_APPMETA=$T/appmeta run "$@"; }
cp "$T/homebrew/PPSA99015/eboot.bin" "$T/eboot.before"
# a: the daily check at start finds this version: nothing offered; Help > Check for updates says "the latest version"
release v1.1 01.010.000
rc=$(u16run "$T" "0:0" "150:cmd 601;250:key enter;300:quit" "")
expect "[ $rc = 0 ] && grep -q '\[update\] daily check' $T/root/logs/boot.log && [ -f $T/root/update-check.txt ]" "the daily check at start"
expect "[ \$(grep -c '\[update\] up to date (1.1)' $T/root/logs/boot.log) = 2 ]" "this version is the newest: up to date (at start, then from the Help menu)"
# b: a newer release whose download isn't what GitHub says: refused, nothing changed
echo gone >"$T/homebrew/PPSA99015/sce_sys/old.bin"
printf '%s *sce_sys/old.bin\n' "$(sha256sum <"$T/homebrew/PPSA99015/sce_sys/old.bin" | cut -c1-64)" >"$T/homebrew/PPSA99015/manifest.sha256"
release v9.9 99.000.000 bad
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "100:cmd 601;200:key enter;400:key enter;450:quit" "")
expect "grep -q '\[update\] 9.9 is available ([0-9]* bytes): offered' $T/root/logs/boot.log" "Help > Check for updates: 9.9 is available, offered"
expect "grep -q \"\[update\] failed: .*SHA-256 doesn't match\" $T/root/logs/boot.log && cmp -s $T/homebrew/PPSA99015/eboot.bin $T/eboot.before && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "Yes: the download isn't GitHub's file: refused, nothing changed"
# c: the right file: the daily check (a day later) offers it, Yes installs it, Yes restarts into it
release v9.9 99.000.000
echo 0 >"$T/root/update-check.txt"
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "200:key enter;450:key enter;520:key enter;560:quit" "300")
expect "grep -q '\[update\] 9.9 is available ([0-9]* bytes): offered' $T/root/logs/boot.log" "the daily check offers 9.9"
expect "grep -q '\[update\] 9.9 installed: offering the restart' $T/root/logs/boot.log" "Yes: downloaded, every file checked, installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99015/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $T/homebrew/PPSA99015/sce_sys/param.json" "the new eboot.bin and param.json in place"
expect "[ -f $T/homebrew/PPSA99015/sce_sys/extra.bin ] && [ ! -e $T/homebrew/PPSA99015/sce_sys/old.bin ] && grep -q 'new art' $T/appmeta/PPSA99015/icon0.png && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "a new file added, a gone one removed, the home screen's art refreshed"
expect "grep -q 'sceSystemServiceLoadExec($T/homebrew/PPSA99015/eboot.bin)' $T/out.txt" "Yes again: the app restarts into the new eboot.bin"
expect "grep -q 'is available: Help, Check for updates' $T/out.txt" "a notification says a new version is out"
nosan "$T"
# d: the 1.1 payload sent again (an autoload): the newer version is left alone
MPCPS5_PS5_ROOT=$T/root MPCPS5_PS5_HOMEBREW=$T/homebrew MPCPS5_PS5_APPMETA=$T/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >"$T/inst2.txt" 2>&1 &
for i in $(seq 1 80); do grep -q 'a newer version is installed' $T/inst2.txt 2>/dev/null && break; sleep 0.1; done
expect "grep -q 'a newer version is installed' $T/inst2.txt && grep -q 'eboot v9.9' $T/homebrew/PPSA99015/eboot.bin && grep -q 'new art' $T/appmeta/PPSA99015/icon0.png" "an older payload sent again leaves the update (and its art) alone"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/inst.txt $T/inst2.txt" "the payload: no sanitizer reports"
stop16
fi

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
