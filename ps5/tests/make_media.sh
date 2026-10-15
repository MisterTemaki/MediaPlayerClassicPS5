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
# a playlist of two files
printf '#EXTM3U\nred.mp4\nchapters.mkv\n' >"$D/list.m3u"
echo "media in $D"
