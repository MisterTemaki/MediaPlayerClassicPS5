# Validation: v1.6-gpu-hdr2

## Actual console observations

The supplied hdr1 logs were recorded on PS5 firmware 13.60 with kstuff. hdr2 uses the same player eboot, but rebuilds the installer with the corrected HDR-capable application profile. hdr2 itself still needs a console retest after installation.

| Observation in hdr1 logs | Result and limit |
| --- | --- |
| GPU startup | RADV PS5 selected on the first attempt; Vulkan output 3840x2160 at 59.940 Hz; color self-tests passed; recovery recorded attempts=0, works=1. |
| H.264 720x1280 | Hardware pipeline 4; 2,890 frames, all in place; zero decoder errors. |
| HEVC Main10 3840x2160, about 100 Mbps | Hardware pipeline 4; 1,797 access units decoded; zero decoder errors; 1,794 zero-copy and 3 copied frames. Decode time about 1 ms. Seven displayed frames dropped. |
| HDR request | VideoOut refused HDR with 0x80290003; SDR tone mapping remained active. The TV does not support HDR10. |
| 7680x4320 HEVC | CPU decoding with GPU rendering; about 46 fps and 583 dropped frames. 8K exceeds the supported hardware decoder envelope. |
| Exit | Exit status 0. |

These sessions contain pauses, seeks and control interactions. The 4K result is not a controlled performance benchmark or a guarantee of perfect playback. The aggregate audio underrun counter spans silent clips, 8K playback and interactions; it does not isolate an audio defect. VP9 hardware playback was not confirmed in these logs. HDMI HDR output, TV color accuracy and sustained 4K playback with audio remain unverified.

## Host checks already completed for the shared player

- 83 isolated ASan/UBSan checks: update/log/installer 36, startup recovery 14, decoder memory cache 26, VideoOut bridge 7.
- 15 HDR Vulkan checks, including PQ levels, HLG, UI white, refusal/confirmation, preference and shutdown. No Vulkan validation-layer errors in the HDR test.
- 130 existing GPU checks: unit 11, formats 18, UI 2, video 80, buffer ownership 14, allocation failure 5.

Total: 228 checks in those groups, plus build/link/import checks. Host Vulkan used software lavapipe and mocked VideoOut, not PS5 HDMI. The complete player integration suite was not rerun for this change. This is not a claim that all repository tests passed in hdr2.

## hdr2-specific checks

The metadata preflight rejected the old attribute=0 profile and accepted 0x62000000. The rebuilt installer is ELF64 x86-64 PIE, has an executable entry point, 16 KiB-aligned load segments and no Linux interpreter. All six embedded application files matched the intended package. The eboot SHA-256 is unchanged from hdr1:

`261d67734909ec6d5a58ba7fe7a46e78cc0157ff55422a66ed1b7df5105c07f4`

Installer ELF SHA-256:

`a1360a23704ae799c375dfab7a0c1b0386e76a9a4ae04dda786765c01250556d`

## Next console test

1. Install hdr2 through the ELF loader and launch the home-screen icon. Confirm GPU startup in View > Statistics and boot.log.
2. On an SDR TV, disable native HDR output in Options > Output, restart and test HDR-to-SDR tone mapping.
3. On an HDR10-capable TV with PS5 HDR enabled, enable native HDR output, restart, play a 3840x2160 HEVC Main10 PQ file and check both the TV's HDR indicator and VideoOut confirmation (`range 2`).
4. For performance, play a known clip without seeking or pausing for several minutes. Record codec, bit depth, bitrate, duration, renderer, decoder, dropped frames and audio behavior.
5. Test startup after an interrupted GPU launch and the explicit retry option separately. Existing successful logs do not prove every recovery case on hardware.

Raw console logs and test media are deliberately not distributed with these public sources.
