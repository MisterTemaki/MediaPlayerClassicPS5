#!/bin/bash
# Media Player Classic PS5 host tests: the test media, made with the host's ffmpeg (small, a few seconds each).
#   make_media.sh <dir>
set -e
D=$1
mkdir -p "$D"
FF="ffmpeg -loglevel error -y"
# red: 640x360 H.264 + AAC 440 Hz, 6 s (the picture's colour is checked)
$FF -f lavfi -i color=c=red:size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 \
	-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 96k -shortest -movflags +faststart "$D/red.mp4"
# chapters.mkv: red 0-3 s, green 3-6 s, blue 6-9 s, three chapters; two audio tracks (English 440 Hz, Portuguese
# 880 Hz, Portuguese the default); an ASS subtitle track (Portuguese, default)
$FF -f lavfi -i color=c=red:size=640x360:rate=25:d=3 -f lavfi -i color=c=green:size=640x360:rate=25:d=3 \
	-f lavfi -i color=c=blue:size=640x360:rate=25:d=3 -filter_complex "[0][1][2]concat=n=3:v=1:a=0[v]" -map "[v]" \
	-c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 25 "$D/rgb.mp4"
cat >"$D/chap.txt" <<'EOF'
;FFMETADATA1
title=Chapter test
[CHAPTER]
TIMEBASE=1/1000
START=0
END=3000
title=Red
[CHAPTER]
TIMEBASE=1/1000
START=3000
END=6000
title=Green
[CHAPTER]
TIMEBASE=1/1000
START=6000
END=9000
title=Blue
EOF
cat >"$D/subs.ass" <<'EOF'
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.50,0:00:08.50,Default,,0,0,0,,{\b1}Legenda embutida{\b0}\NSegunda linha
EOF
$FF -f lavfi -i sine=frequency=440:sample_rate=48000 -t 9 "$D/a440.wav"
$FF -f lavfi -i sine=frequency=880:sample_rate=48000 -t 9 "$D/a880.wav"
$FF -i "$D/rgb.mp4" -i "$D/a440.wav" -i "$D/a880.wav" -i "$D/subs.ass" -i "$D/chap.txt" \
	-map 0:v -map 1:a -map 2:a -map 3:s -map_metadata 4 -map_chapters 4 -c:v copy -c:a aac -b:a 96k -c:s ass \
	-metadata:s:a:0 language=eng -metadata:s:a:1 language=por -metadata:s:s:0 language=por \
	-disposition:a:0 0 -disposition:a:1 default -disposition:s:0 default "$D/chapters.mkv"
rm -f "$D/rgb.mp4" "$D/a440.wav" "$D/a880.wav" "$D/chap.txt" "$D/subs.ass"
# song.mp3 with a cover (a blue square) and tags
$FF -f lavfi -i color=c=blue:size=300x300 -frames:v 1 "$D/cover.png"
$FF -f lavfi -i sine=frequency=660:sample_rate=44100:duration=6 -i "$D/cover.png" -map 0 -map 1 -c:a libmp3lame -b:a 128k \
	-c:v png -disposition:v attached_pic -metadata title="Test Song" -metadata artist="MPC PS5" "$D/song.mp3"
rm -f "$D/cover.png"
# interlaced.ts: MPEG-2, interlaced (top field first), green, 4 s
$FF -f lavfi -i color=c=green:size=720x576:rate=25 -t 4 -c:v mpeg2video -flags +ilme+ildct -top 1 -b:v 4M \
	-f mpegts "$D/interlaced.ts"
# hevc10.mkv: HEVC Main 10, magenta, 3 s (the 4K Blu-ray rips' codec, small here)
$FF -f lavfi -i color=c=magenta:size=640x360:rate=25 -t 3 -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le \
	-x265-params log-level=error "$D/hevc10.mkv"
# av1.mkv: AV1 (dav1d decodes it), yellow, 3 s
$FF -f lavfi -i color=c=yellow:size=640x360:rate=25 -t 3 -c:v libsvtav1 -preset 12 -pix_fmt yuv420p "$D/av1.mkv" 2>/dev/null
# hdr_pq.mkv: HEVC Main 10, HDR10 (BT.2020, PQ, mastering 1000 nits): the left half grey at 100 nits, the right half
# at 1000 nits; hdr_hlg.mkv: the same with HLG, grey at signal 0.5 (mpc_tonemap.h)
python3 - "$D" <<'PY'
import struct, sys
d = sys.argv[1]
def pq(n):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    y = (n / 10000.0) ** m1
    return ((c1 + c2 * y) / (1 + c3 * y)) ** m2
w, h = 640, 360
def frames(path, left, right):
    yl, yr = round(64 + 876 * left), round(64 + 876 * right)
    row = struct.pack('<%dH' % w, *([yl] * (w // 2) + [yr] * (w // 2)))
    chroma = struct.pack('<%dH' % (w // 2), *([512] * (w // 2)))
    with open(path, 'wb') as f:
        for _ in range(50):
            f.write(row * h)
            f.write(chroma * (h // 2) * 2)
frames(d + '/pq.yuv', pq(100), pq(1000))
frames(d + '/hlg.yuv', 0.5, 0.75)
PY
$FF -f rawvideo -pix_fmt yuv420p10le -s 640x360 -r 25 -i "$D/pq.yuv" -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le \
	-x265-params "log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400" \
	-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$D/hdr_pq.mkv"
$FF -f rawvideo -pix_fmt yuv420p10le -s 640x360 -r 25 -i "$D/hlg.yuv" -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le \
	-x265-params "log-level=error:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:range=limited" \
	-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc "$D/hdr_hlg.mkv"
rm -f "$D/pq.yuv" "$D/hlg.yuv"
# surround51.mkv: 5.1 AC-3, a 1 kHz tone in the centre channel only, 4 s (black picture)
$FF -f lavfi -i color=c=black:size=320x180:rate=25 -f lavfi -i sine=frequency=1000:sample_rate=48000 -t 4 \
	-filter_complex "[1:a]pan=5.1|FC=c0[a]" -map 0:v -map "[a]" -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
	-c:a ac3 -b:a 384k -shortest "$D/surround51.mkv"
# surround71.mkv: 7.1 PCM, a 500 Hz tone in the left side channel only, 4 s
$FF -f lavfi -i color=c=black:size=320x180:rate=25 -f lavfi -i sine=frequency=500:sample_rate=48000 -t 4 \
	-filter_complex "[1:a]pan=7.1|SL=c0[a]" -map 0:v -map "[a]" -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
	-c:a pcm_s16le -shortest "$D/surround71.mkv"
# loud.mkv: a 440 Hz tone at full scale (peaks at 32700), 4 s: with the audio boost it must clip, not wrap around
$FF -f lavfi -i color=c=black:size=320x180:rate=25 -f lavfi -i "aevalsrc=0.998*sin(2*PI*440*t):s=48000:c=stereo" -t 4 \
	-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a pcm_s16le -shortest "$D/loud.mkv"
# a playlist of two files
printf '#EXTM3U\nred.mp4\nchapters.mkv\n' >"$D/list.m3u"
# libass (group 20): grey.mp4, 640x360 grey, 6 s; styled.ass beside it: (a) green text at \pos(20,20) top left,
# (b) the "Big" style (48 px yellow, a 5 px blue outline) at the bottom centre, (c) red text turned 90 degrees (\frz90)
# in the middle, all from 0 to 5 s, in a 640x360 script
$FF -f lavfi -i color=c=gray:size=640x360:rate=25 -t 6 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$D/grey.mp4"
cat >"$D/styled.ass" <<'EOF'
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
Style: Big,Arial,48,&H0000FFFF,&H000000FF,&H00FF0000,&H00000000,-1,0,0,0,100,100,0,0,1,5,0,2,10,10,12,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\an7\pos(20,20)\c&H00FF00&\bord0\shad0\fs30}Top left
Dialogue: 0,0:00:00.00,0:00:05.00,Big,,0,0,0,,BIG STYLE
Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,{\an5\pos(320,180)\frz90\c&H0000FF&\bord0\shad0\fs30}ROTATED
EOF
# fonts.mkv: grey, an ASS track whose style asks for a font only the file has (attached: DejaVu Serif, or another
# TrueType font of the system's), magenta at the top centre; attached-font.txt: "file|family"
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf
[ -f "$FONT" ] || FONT=$(fc-list -f '%{file}\n' ':fontformat=TrueType' | grep -iv 'selawik\|roboto' | grep '\.ttf$' | head -1)
FAMILY=$(fc-scan --format '%{family[0]}' "$FONT")
echo "$(basename "$FONT")|$FAMILY" >"$D/attached-font.txt"
cat >"$D/fontsub.ass" <<EOF
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Attached,$FAMILY,40,&H00FF00FF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,8,10,10,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:05.00,Attached,,0,0,0,,Attached font
EOF
$FF -i "$D/grey.mp4" -i "$D/fontsub.ass" -map 0:v -map 1:s -c:v copy -c:s ass -attach "$FONT" \
	-metadata:s:t mimetype=application/x-truetype-font "$D/fonts.mkv"
rm -f "$D/fontsub.ass"
echo "media in $D"
