#!/bin/bash
# Media Player Classic PS5 1.6 host tests: the GPU renderer's test media, made with the host's ffmpeg (test patterns
# only, nothing downloaded).
#   fmt_<pixfmt>.nut   testsrc2, 320x180, 10 fps, 1 s, raw video in each pixel format a decoder can give (and three the
#                      GPU path doesn't take: rgb24, yuyv422, pal8); no container keeps raw p010le (the library's own
#                      tests and the console decoder's stand-in cover semi-planar 10-bit)
#   fmt_yuvj420p.mkv   MJPEG: yuvj420p, full range
#   hdr4k_<fps>.mkv    HEVC Main 10, 3840x2160 at 24, 30 and 60 fps, 1 s each, HDR10 (BT.2020, PQ, mastering 1000 nits,
#                      MaxCLL 1000): a test pattern stretched up to 1000 nits (hdr4k_30_5s.mkv: 5 s of it at 30 fps)
#   hlg4k_30.mkv       the same at 30 fps with HLG
#   hdr4000.mkv        HEVC Main 10, 640x360, HDR10 mastered at 4000 nits (MaxCLL 4000): the metadata only in the
#                      stream's SEI, none in the container
set -e
D=${1:?usage: make_gpu_media.sh <dir>}
mkdir -p "$D"
FF="ffmpeg -loglevel error -y"
for f in yuv420p yuv422p yuv444p yuv410p yuv420p10le yuv422p10le yuv444p12le yuv420p16le nv12 nv21 \
	gray gray10le gbrp gbrp10le bgra rgba 0rgb rgb24 yuyv422 pal8; do
	[ -f "$D/fmt_$f.nut" ] || $FF -f lavfi -i "testsrc2=s=320x180:r=10" -t 1 -pix_fmt $f -c:v rawvideo "$D/fmt_$f.nut"
done
[ -f "$D/fmt_yuvj420p.mkv" ] || $FF -f lavfi -i "testsrc2=s=320x180:r=10" -t 1 -pix_fmt yuvj420p -c:v mjpeg -q:v 2 \
	"$D/fmt_yuvj420p.mkv"
HDR10="colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400"
for fps in 24 30 60; do
	[ -f "$D/hdr4k_$fps.mkv" ] || $FF -f lavfi -i "testsrc2=s=3840x2160:r=$fps" -t 1 -vf "format=yuv420p10le" \
		-c:v libx265 -preset ultrafast -x265-params "log-level=error:$HDR10" \
		-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$D/hdr4k_$fps.mkv"
done
# (review M-A) 5 s of it, for the console decoder's buffers handed out in place over 150 pictures
[ -f "$D/hdr4k_30_5s.mkv" ] || $FF -f lavfi -i "testsrc2=s=3840x2160:r=30" -t 5 -vf "format=yuv420p10le" \
	-c:v libx265 -preset ultrafast -x265-params "log-level=error:$HDR10" \
	-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$D/hdr4k_30_5s.mkv"
# (test6 review) 2 s of it with a keyframe every 5 pictures: the console's decoder refusing it gives up within a few
[ -f "$D/hdr4k_30_k5.mkv" ] || $FF -f lavfi -i "testsrc2=s=3840x2160:r=30" -t 2 -vf "format=yuv420p10le" \
	-c:v libx265 -preset ultrafast -x265-params "log-level=error:keyint=5:min-keyint=5:scenecut=0:$HDR10" \
	-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$D/hdr4k_30_k5.mkv"
[ -f "$D/hlg4k_30.mkv" ] || $FF -f lavfi -i "testsrc2=s=3840x2160:r=30" -t 1 -vf "format=yuv420p10le" -c:v libx265 \
	-preset ultrafast -x265-params "log-level=error:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc" \
	-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc "$D/hlg4k_30.mkv"
[ -f "$D/hdr4000.mkv" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=25" -t 1 -vf "format=yuv420p10le" -c:v libx265 -preset ultrafast \
	-x265-params "log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50):max-cll=4000,400" \
	-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc "$D/hdr4000.mkv"
# test3: 8K HEVC Main 10 (half a second; the encoder held to one thread pool and frame, so it fits the host's memory),
# VP9 8-bit (profile 0) and 10-bit (profile 2) for the console decoder's VP9, raw .yuv (one named with its size, one
# not) and .y4m
[ -f "$D/hevc8k_10.mp4" ] || $FF -f lavfi -i "testsrc2=s=7680x4320:r=30" -t 0.5 -vf "format=yuv420p10le" -c:v libx265 \
	-preset ultrafast -x265-params "log-level=error:pools=1:frame-threads=1:rc-lookahead=0:bframes=0:crf=40:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc" \
	-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -tag:v hvc1 "$D/hevc8k_10.mp4"
[ -f "$D/vp9_8.webm" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=25" -t 2 -pix_fmt yuv420p -c:v libvpx-vp9 -profile:v 0 \
	-deadline realtime -cpu-used 8 -b:v 1M "$D/vp9_8.webm"
[ -f "$D/vp9_10.webm" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=25" -t 2 -pix_fmt yuv420p10le -c:v libvpx-vp9 -profile:v 2 \
	-deadline realtime -cpu-used 8 -b:v 1M "$D/vp9_10.webm"
[ -f "$D/raw_320x180_10fps.yuv" ] || $FF -f lavfi -i "testsrc2=s=320x180:r=10" -t 2 -pix_fmt yuv420p -f rawvideo \
	"$D/raw_320x180_10fps.yuv"
[ -f "$D/BodeMuseum_PQ.yuv" ] || cp "$D/raw_320x180_10fps.yuv" "$D/BodeMuseum_PQ.yuv"
[ -f "$D/clip.y4m" ] || $FF -f lavfi -i "testsrc2=s=320x180:r=10" -t 2 -pix_fmt yuv420p "$D/clip.y4m"
# test4: VP9 with blocks of a hidden frame alone (its superframes split), VP9 12-bit, a slow-decoding test film with
# sound and a keyframe each second, a raw .yuv with a fractional rate in its name
[ -f "$D/vp9_split.nut" ] || $FF -i "$D/vp9_8.webm" -c:v copy -bsf:v vp9_superframe_split -f nut "$D/vp9_split.nut" # (NUT keeps the split blocks: IVF and WebM put the superframes back)
[ -f "$D/vp9_12.webm" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=25" -t 1 -pix_fmt yuv420p12le -c:v libvpx-vp9 -profile:v 2 \
	-deadline realtime -cpu-used 8 -b:v 1M "$D/vp9_12.webm"
[ -f "$D/slow.mp4" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=25" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 20 \
	-c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -c:a aac -b:a 96k "$D/slow.mp4"
[ -f "$D/raw_320x180_23.976fps.yuv" ] || cp "$D/raw_320x180_10fps.yuv" "$D/raw_320x180_23.976fps.yuv"
# test5: the slow-decoding film made longer (28 s: six calm windows a step back), a 60 fps film with FLAC sound (100 ms
# packets) and a film whose sound ends after 5 of its 20 s, raw .yuv names with "fps" after a point
[ -f "$D/slow28.mp4" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=25" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 28 \
	-c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p -c:a aac -b:a 96k "$D/slow28.mp4"
[ -f "$D/flac60.mkv" ] || $FF -f lavfi -i "testsrc2=s=320x180:r=60" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 30 \
	-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a flac "$D/flac60.mkv"
# (test6 review: 60 s, more than the demuxer reads ahead at a 1 MiB limit, so the sound's end comes before the file's)
[ -f "$D/short_sound60.mkv" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=30" -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=5" \
	-t 60 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 96k "$D/short_sound60.mkv"
# (test7 review) 60 fps with 5 s between keyframes (300 packets: more than the 120 the skip looked at before)
[ -f "$D/long_gop60.mp4" ] || $FF -f lavfi -i "testsrc2=s=640x360:r=60" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 30 \
	-c:v libx264 -preset ultrafast -g 300 -keyint_min 300 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -b:a 96k "$D/long_gop60.mp4"
# (test9 review) 5 s of video, 300 s of sound (a concert file whose video track stops early)
[ -f "$D/short_video.mkv" ] || $FF -f lavfi -i "testsrc2=s=320x180:r=25:d=5" -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=300" \
	-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 96k "$D/short_video.mkv"
# (test12) 8K HEVC Main 10 with sound, 3 s (90 pictures: enough for the auto fast decoding's windows)
[ -f "$D/hevc8k_10_3s.mp4" ] || $FF -f lavfi -i "testsrc2=s=7680x4320:r=30" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 3 \
	-vf "format=yuv420p10le" -c:v libx265 -preset ultrafast \
	-x265-params "log-level=error:pools=1:frame-threads=1:rc-lookahead=0:bframes=0:crf=32" -tag:v hvc1 -c:a aac "$D/hevc8k_10_3s.mp4"
# (test12) a portrait phone video, 720x1280 H.264 (test11's PS5 log: the console's decoder failed its memory query)
[ -f "$D/portrait.mp4" ] || $FF -f lavfi -i "color=c=red:size=720x1280:rate=30" -t 3 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$D/portrait.mp4"
# an exact seek into a 10 s GOP at 60 fps (the test5 re-review's: the demuxer's leaving out froze the picture)
[ -f "$D/gop600.mkv" ] || $FF -f lavfi -i "testsrc2=s=1280x720:r=60" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 20 \
	-c:v libx264 -preset ultrafast -g 600 -pix_fmt yuv420p -c:a aac "$D/gop600.mkv"
[ -f "$D/clip_320x180.60fps.yuv" ] || cp "$D/raw_320x180_10fps.yuv" "$D/clip_320x180.60fps.yuv"
[ -f "$D/x_320x180_1.5.24fps.yuv" ] || cp "$D/raw_320x180_10fps.yuv" "$D/x_320x180_1.5.24fps.yuv"
