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
# debug logs are off by default (1.1.1); the tests read boot.log, so their default is on (group 18 checks the real one)
export MPCPS5_LOGS_DEFAULT=1
CHECK="python3 $PWD/tests/check_ppm.py"
REGION="python3 $PWD/tests/check_region.py"
WORK=${WORK:-$(mktemp -d)}
ONLY=${ONLY:-}
PASS=0
FAIL=0
MEDIA=$WORK/media
[ -f "$MEDIA/fonts.mkv" ] || bash tests/make_media.sh "$MEDIA" >/dev/null

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
expect "grep -q \"frame_format 2 with 2-byte samples isn't on the confirmed list yet\" $T/root/logs/boot.log" "a frame_format not confirmed on a console yet: said in the log"
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
# the release key of the tests (the app gets its public half through MPCPS5_HOST_RELEASE_KEY), and another one
openssl genpkey -algorithm ed25519 -out $T/release.pem 2>/dev/null
openssl genpkey -algorithm ed25519 -out $T/other.pem 2>/dev/null
PUBHEX=$(openssl pkey -in $T/release.pem -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')
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
u16run() { MPCPS5_HOST_RELEASE_KEY=$PUBHEX MPCPS5_UPDATE_API=http://127.0.0.1:$SPORT/latest.json MPCPS5_PS5_APPMETA=$T/appmeta run "$@"; }
cp "$T/homebrew/PPSA99015/eboot.bin" "$T/eboot.before"
# a: the daily check at start finds this version: nothing offered; Help > Check for updates says "the latest version"
release v1.3 01.030.000
rc=$(u16run "$T" "0:0" "150:cmd 601;250:key enter;300:quit" "")
expect "[ $rc = 0 ] && grep -q '\[update\] daily check' $T/root/logs/boot.log && [ -f $T/root/update-check.txt ]" "the daily check at start"
expect "[ \$(grep -c '\[update\] up to date (1.3)' $T/root/logs/boot.log) = 2 ]" "this version is the newest: up to date (at start, then from the Help menu)"
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
# c: the right file: the daily check (a day later) offers it, Yes installs it, Yes restarts into it
release v9.9 99.000.000
echo 0 >"$T/root/update-check.txt"
rm -f $T/root/logs/boot.log
rc=$(u16run "$T" "0:0" "200:key enter;450:key enter;520:key enter;560:quit" "300")
expect "grep -q '\[update\] 9.9 is available ([0-9]* bytes): offered' $T/root/logs/boot.log" "the daily check offers 9.9"
expect "grep -q '\[update\] 9.9 installed: offering the restart' $T/root/logs/boot.log && grep -q \"the release's Ed25519 signature checks out\" $T/root/logs/boot.log" "Yes: downloaded, signature and every file checked, installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99015/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $T/homebrew/PPSA99015/sce_sys/param.json" "the new eboot.bin and param.json in place"
expect "[ -f $T/homebrew/PPSA99015/sce_sys/extra.bin ] && [ ! -e $T/homebrew/PPSA99015/sce_sys/old.bin ] && grep -q 'new art' $T/appmeta/PPSA99015/icon0.png && [ -z \"\$(find $T/homebrew -name '*.new' -o -name '*.old.*' -o -name 'update.journal*')\" ]" "a new file added, a gone one removed, the home screen's art refreshed (no .new, .old or journal left)"
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
S="$S;320:cmd pause;325:cmd opendir;340:key escape;350:open $WORK/no-such-file.mkv;380:key enter;390:quit"
for L in 1 2 3 4; do
	T=$(newroot t17_$L)
	cp "$MEDIA/chapters.mkv" "$T/root/media/"
	ini "$T" "Language=$L"
	rc=$(MPCPS5_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0" "$S" "145,255,262" "$T/root/media/chapters.mkv")
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
	expect "state $T 1 | grep -q ' hw 0$' && $CHECK $T/dump/flip00075.ppm 960 400 red >/dev/null" "... and it plays on: red"
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
# R2's 20 s jump back
T=$(newroot t22f)
srt22 $T "Delay line"
ini "$T" "SubDelayInterval=200"
P="30:cmd options;40:key down;50:key r1"
f=55; for i in $(seq 1 11); do P="$P;$f:key down"; f=$((f + 3)); done
P="$P;95:key r1"
f=100; for i in $(seq 1 14); do P="$P;$f:key right"; f=$((f + 3)); done
P="$P;150:key menu;165:key r2;200:cmd options;210:key down;215:key r1;220:key r1;225:key r1;230:key enter;235:key menu;250:key r2;270:quit"
rc=$(run "$T" "0:0" "$P" "92,170" "$T/root/media/movie.mp4")
expect "[ $rc = 0 ] && grep -q '\[ui\] R2: SubDelayUp' $T/root/logs/boot.log && grep -q '\[osd\] Subtitle delay: 200 ms' $T/root/logs/boot.log" "Keys: R2 set to Subtitle delay +; R2 adds the interval (200 ms), the OSD says so"
expect "$REGION $T/dump/flip00170.ppm 150 65 800 140 224 224 224 20 150 >/dev/null" "... the OSD on the picture"
expect "$REGION $T/dump/flip00092.ppm 790 632 1400 656 204 232 255 4 8000 >/dev/null" "the Keys page drawn, R2's row selected (scrolled to)"
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
fi

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
