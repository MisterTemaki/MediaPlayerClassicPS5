# v1.6-gpu-hdr2 changes

This experimental snapshot is based on the supplied v1.6 test13 sources. The installer keeps title ID `PPSA99015` and internal application version `1.6`. The `gpu-hdr2` suffix identifies this distribution; it is not a new internal numeric version.

## Rendering and startup

- Link all RADV archive members with `--whole-archive`. Weak Vulkan dispatch references previously left implemented entry points out of the player. `ps5/tools/check_radv_link.py` checks the final link. The corrected build includes all 703 members and 338 implemented entry points.
- Use RADV's embedded zlib for the player, retaining the installer's own zlib. Include `GPU` in the build configuration stamp so switching GPU=0/1 invalidates stale objects.
- Persist and fsync startup recovery before entering the driver. The first presented frame records success. After an interrupted or failed GPU initialization, the next launch selects software rendering. `Options > Output > Retry GPU next start`, then Apply/OK, permits another attempt. Compatibility revision `radv-whole-v1` permits one attempt after this fix; ordinary rebuilds do not clear recovery.
- Vulkan scales, converts and presents video and composites the UI. MPC's GDI-style controls and subtitles are still rasterized into a bitmap on the CPU before GPU composition.

## Console decoding

Prefer the PS5 decoder for supported H.264, HEVC and VP9 streams up to 4K; keep FFmpeg CPU fallback. Query 20 decoder memory configurations before leaving the sandbox. If a later query is refused, reuse a successful cached result only for the matching codec/profile/level/bit depth/pipeline/queue and queried envelope. This does not provide hardware decoding for arbitrary codecs or 8K video.

## Optional native HDR10

- Use a typed 10-bit `A2R10G10B10_UNORM_PACK32` target and copy its packed bits into VideoOut buffers configured for BGR10A2, PQ and BT.2020. PQ video is retained; HLG is converted to PQ.
- Capture the WSI VideoOut handle through wrappers and request/check the output attributes through Sony's VideoOut API. Wait for GPU idle before transitions, with 0.4 s/1 s entry/exit stabilization.
- Keep SDR tone mapping after a refused HDR request; revert if HDR is not confirmed after 60 presents. Fail presentation if restoring the buffer format fails rather than sending pixels with a mismatched interpretation.
- Convert SDR UI/subtitle composition to PQ/BT.2020 with a 203-nit graphics white and adjusted alpha.
- `Options > Output` controls native HDR output, effective next launch (`HdrOutput`, default enabled). For an SDR TV, disable it and keep HDR-to-SDR tone mapping.
- hdr2 changes the application capability profile in `param.json` from `0` to `1644167168` (`0x62000000`). A build preflight checks it. The player eboot is byte-identical to hdr1; the installer was rebuilt with the corrected metadata.

Native HDR10 HDMI output remains unverified on a compatible PS5 display. Declaring the application profile does not make an SDR TV support HDR. Dolby Vision profile 5 without an HDR10 base is unsupported.

The profile follows [VLC's application metadata](https://github.com/ZiZc3/VLC/blob/main/ps5/sce_sys/param.json) and the hardware findings in [ProsperoLight's porting documentation](https://github.com/blackbearreloaded/ProsperoLight/blob/a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9/docs/PORTING.md). Decoder and VideoOut flow were compared with [VLC PS5](https://github.com/ZiZc3/VLC). Local GPU library patches are recorded in [patches/gpu-hdr.diff](patches/gpu-hdr.diff) and the vendor manifest. This is a modified vendor copy, not pristine upstream.

## Other retained fixes

- Preserve update rollback journals/backups when rollback is incomplete; treat failed commit cleanup/fsync as failure instead of reporting success.
- Reject ZIP paths containing control/NUL characters, reserved staging/backup names, duplicate/unlisted entries and self-manifests.
- Fail closed on file hash open/read/close errors; redact truncated URL credentials in logs.
- Handle an installer comparison source growing after stat safely.
- Correct the libudfread release URL without changing its checksum; search `libdata/pkgconfig` as well as `lib/pkgconfig`; install RADV's matching Vulkan headers.
- Fix host compiler/test portability issues (allocation-elision test, locale initializer and paths containing spaces).

The code audit is partial. Build success and the checks described in the validation report do not establish complete compatibility or an exhaustive source audit.
