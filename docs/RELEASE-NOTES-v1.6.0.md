## Media Player Classic PS5 1.6.0

GPU rendering and console decoder startup fixes, with optional native HDR10 output. This release is based on the 1.6 test13 sources. The app's internal version remains 1.6 and its title ID remains PPSA99015.

### New

- **The GPU draws the video.** Vulkan scales and converts the decoder's pictures and lays the window, menus and subtitles over them. The controls are still drawn into a bitmap on the CPU before GPU composition.
- **Optional native HDR10 output.** On a compatible TV, the player requests 10-bit PQ/BT.2020 output. PQ video is preserved; HLG is converted to PQ. The interface uses a 203-nit graphics white. Options > Output controls it, from the next start.
- **SDR tone mapping remains available.** If HDR output is refused or not confirmed, the player returns to SDR. For an SDR TV, turn native HDR output off and restart.

### Fixed

- **Missing Vulkan functions in the player.** The RADV driver is now linked in full, with a build check for the implemented entry points.
- **Repeated GPU startup attempts.** Recovery is saved before entering the driver. After an interrupted or failed start, the next launch uses software rendering. Options > Output > Retry GPU next start allows another attempt.
- **Decoder memory queries after leaving the sandbox.** Successful queries made early are cached for matching H.264, HEVC and VP9 configurations up to 4K. Unsupported streams still use FFmpeg on the CPU.
- **The app's HDR capability profile.** hdr2 rebuilds the installer with attribute 0x62000000. Its player binary is unchanged from the tested hdr1 build.
- **Update recovery and build fixes.** Preserve incomplete rollback state, reject invalid update ZIP entries, fail closed on file hash errors, redact truncated URL credentials and fix dependency/header discovery.

### Updating

- **From 1.5.4:** choose Help > Check for updates, confirm the download and installation, then restart the app. Or send `MediaPlayerClassicPS5-v1.6.0.elf` to your payload loader (port 9021) and open the app from the home screen.
- Put the new ELF in your autoload in place of the older one so the matching helper runs.
- The in-app updater downloads `MediaPlayerClassicPS5-v1.6.0-PPSA99015.zip`, verifies GitHub's SHA-256 and the embedded file manifest, then installs the app. The ELF remains available for manual installation and for updating the helper.

### Files

- `MediaPlayerClassicPS5-v1.6.0.elf`: the installer and helper, with the native player embedded.
- `MediaPlayerClassicPS5-v1.6.0-PPSA99015.zip`: the app folder and file manifest, used by Help > Check for updates. You do not need to download it manually.
- `mpc-ps5-v1.6.0-src.zip`: the source, including third-party notices and build notes.
- The `.sha256` files and `SHA256SUMS.txt`: download checksums.

### Status

The shared hdr1 player was run on PS5 13.60 with kstuff. RADV opened on the first attempt. H.264 and 4K HEVC Main10 used the console decoder. The approximately 100 Mbps 4K clip decoded 1,797 frames with no decoder errors, including 1,794 zero-copy frames; seven displayed frames were dropped. Pauses and control interactions mean this was not a continuous performance benchmark. VP9 hardware playback was not confirmed in these logs. 8K used CPU decoding and dropped frames.

**hdr2 still needs a console retest after installation. Native HDR10 output has not been confirmed on an HDR10-capable TV.** The test TV does not support HDR10; VideoOut refused HDR and SDR tone mapping remained available.

228 targeted host checks passed for the shared player, plus build/link checks. Host Vulkan used software lavapipe and mocked VideoOut. The complete player integration suite was not rerun. These results do not guarantee perfect playback or support for every format.

If something does not work, please open an issue with Options > Miscellaneous > Debug logs enabled (off by default). The app log is `/data/mpcps5/logs/boot.log`; include `boot.prev.log` and the helper's `helper.log` or `installer.log` when relevant. Review logs for personal paths before posting them publicly.

The update package was extracted from the published ELF and checked byte for byte. Host compatibility checks used the real 1.5.4 and 1.6 updater code with the full package, including installation, checksum rejection and version comparison. These host checks do not replace a console update test.

Port by [MisterTemaki](https://github.com/MisterTemaki).
