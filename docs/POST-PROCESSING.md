# Video post-processing on the GPU (1.7)

Suggested by Cris Valente. View > Options > Playback > **Shaders** turns on, for the video only (never the window, the
menus or the subtitles), a chain of compute passes between the decoder's picture and the screen:

```
decoder planes ──► 1 prep ──► src ring ──► 2 motion smoothing ──► 3 scale ──► 4 final pass ──► display (4K on a 4K TV)
 (Y'CbCr / RGB)    convert,   (RGB10A2,   (Experimental: flow,    (FSR 1      (RCAS sharpening,
                   deband,    by picture)  warp, occlusion)        EASU,       colour controls,
                   deblock                                         Lanczos,    tone mapping / HDR10
                                                                   bicubic,    out, under the UI)
                                                                   bilinear)
```

With **Shaders: Off** (the default) nothing of it is made: the picture is drawn in one pass as in 1.6 (`video.frag`).

## What runs where

- **Vulkan 1.0 through RADV**, one queue (the console's driver gives one): the compute passes are recorded in the
  frame's command buffer before the final render pass, separated by `vkCmdPipelineBarrier`
  (`SHADER_WRITE` → `SHADER_READ`, `COMPUTE_SHADER` → `COMPUTE_SHADER` / `FRAGMENT_SHADER`, images in `GENERAL` while
  written and `SHADER_READ_ONLY_OPTIMAL` while read). No async compute and no timeline semaphores: with one queue there
  is nothing to overlap, and the decoder already runs on threads of its own, ahead of the screen (the player's queue of
  decoded pictures). One frame is in flight, as before (Present waits for the previous one's fence).
- **Formats.** The pictures between passes are `A2B10G10R10_UNORM` (4 bytes a pixel, 10 bits a component): enough
  for 8-bit and 10-bit sources, half the memory and bandwidth of `R16G16B16A16_SFLOAT`. They hold R'G'B' in the
  stream's own transfer (gamma for SDR, the PQ or HLG signal for HDR) and primaries: perceptually even codes, which is
  what debanding, the scalers and FSR 1 want; linear light and the display's primaries only in the final pass. The
  motion vectors are `R16G16_SFLOAT`, the pyramid's luma `R16_SFLOAT`.
- **Half precision.** Each compute shader is built twice, with and without
  `GL_EXT_shader_explicit_arithmetic_types_float16`; the FP16 build is used when the device has `shaderFloat16`
  (`VK_KHR_shader_float16_int8`; RDNA 2 does, at twice the FP32 rate), the FP32 one otherwise. The log says which.
- **Descriptor sets.** One set layout per pass, one set per pass (written when its images change), the parameters
  in push constants (≤ 128 bytes): no uniform buffer to keep in step with the frame in flight.

## The passes

1. **prep** (one dispatch per new picture, at the picture's size): each output pixel reads its planes at their
   texel centres (no resampling but the chroma's, sited as the stream says), the codes normalised, the colour matrix,
   then
   - **deblocking / deringing** (light): across the 8×8 grid of MPEG-2 / MPEG-4 / H.264-era blocks, a step between two
     blocks smaller than the texture around it is smoothed (H.264's normal-strength filter rule: |p0−q0| < α,
     |p1−p0| < β, |q1−q0| < β); deringing limits a pixel next to a strong edge to the range of its neighbours on its
     own side;
   - **debanding**: the approach of libplacebo's and f3kdb's deband (reimplemented, not copied): a few iterations
     comparing the pixel with the average of four pixels at a random angle and growing radius; where they differ by
     less than a threshold the average is taken, then a little noise ("grain") hides the last steps.

   Written to the **src ring**: three pictures, keyed by the player's picture id.
2. **motion smoothing** (Experimental, off by default; the picture between two decoded ones at the display's
   refresh):
   - the **pyramid**: each picture's luma at ¼ of its size (at most 960 wide), then halved while that leaves 32 pixels
     or more (at most six levels: 960 to 30 for a 4K picture);
   - the **flow**, coarse to fine, both ways (N → N+1 and N+1 → N): at each level each pixel starts from the coarser
     level's vector, doubled, taken as the median of five around it (a stray vector doesn't spread); it is refined by
     a ±1-pixel search with a 5×5 patch's sum of absolute differences (with a small pull towards the starting vector,
     so flat areas keep it) and a parabola through the best cost and its neighbours (sub-pixel). Six levels reach 63
     pixels at the finest level, 252 of a 4K picture's. A pair's flows are made once;
   - the **warp** at the picture's size, at the display's time t between N and N+1: each pixel's vectors to N and to
     N+1 approximated from the two flows (`F(t→0) = −(1−t)·t·F01 + t²·F10`, `F(t→1) = (1−t)²·F01 − t·(1−t)·F10`), each
     picture sampled there, and blended by t and by **occlusion**: where a vector and the other way's vector at its
     end don't cancel (forward–backward consistency), that picture is left out;
   - **scene changes**: the mean difference of the two pictures' coarsest luma above the threshold set in Options:
     no blend, the nearer picture shown.

   It costs one picture of latency in what the GPU is given (the player already holds the next picture decoded), none
   in the sound. The warp is made again only when t changes.
3. **scale** (only when the picture's size on screen isn't its own): **FSR 1 EASU** (AMD FidelityFX Super Resolution
   1.0, MIT, vendored unchanged in `ps5-slang-gpu/third_party/ffx-fsr1`), **Lanczos** (3 lobes), **bicubic**
   (Catmull-Rom) or **bilinear**, to the picture's rectangle on the display.
4. **final** (a fragment shader in the frame's render pass, under the UI): **RCAS** (FSR 1's sharpening, the
   strength in Options) when FSR 1 scaled, **brightness, contrast and saturation**, then what `video.frag` does after
   its conversion: tone mapping (BT.2390, Hable, Reinhard: the settings of Options > Output) or HDR10 output.

Nothing is made again that hasn't changed: a picture is prepared once (again when the prep settings change while it
is shown), the scaler runs only when its input or the rectangle changes. A paused picture, and the repeats of a 24 fps
film at 60 Hz, cost the final pass alone.

## Timings

Each pass is bracketed by timestamp queries; View > Statistics (and the OSD statistics, R3 + Square) shows the GPU
milliseconds of prep, motion smoothing, scale and final per frame. On the console these are the numbers to watch:
a 60 Hz display leaves 16.7 ms for all of it, the UI included.

## Presets

| | prep | motion smoothing | scale | sharpening |
|---|---|---|---|---|
| Off | — (1.6's single pass) | — | — | — |
| Performance | — | — | FSR 1 | 0.5 |
| Quality | deband, deblock | — | FSR 1 | 0.25 |
| Experimental | deband, deblock | on | FSR 1 | 0.25 |
| Custom | as set | as set | as set | as set |

## Not done, and why

- **Neural networks** (RIFE for frames, Real-ESRGAN or SeedVR for detail): they need an inference engine and models of
  tens of megabytes, and the console's GPU can't run them for every picture at these sizes in real time.
- **FSR 2 / 3**: temporal upscalers made for games, which give them each pixel's motion vector and a jittered camera.
  A video has neither; nothing to gain.
- **120 Hz output**: the app's display mode is the console's 60 Hz one; whether a homebrew title can have 120 Hz isn't
  known. Motion smoothing makes 60 pictures a second.
