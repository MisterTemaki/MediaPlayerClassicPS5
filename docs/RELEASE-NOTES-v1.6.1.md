## Media Player Classic PS5 1.6.1

Dark mode for the PS5 interface, with saved preferences and instant switching. The application version is
1.6.1, content version is 01.061.000, and the title ID remains PPSA99015.

### New

- **Dark mode:** choose **View > Options > Player > Theme > Dark**, then **Apply** or **OK**. Select **Light** to
  restore the original appearance. Existing settings use Light by default.
- **Immediate switching:** the window, toolbar, menus, playlist, file browser, dialogs and controls update without
  restarting the player. The choice is saved for the next launch. Cancel discards a choice that has not been applied.
- **Readable toolbar icons:** dark neutral glyphs are brightened for Dark, while colored icons and transparency
  retain their original values. Returning to Light restores the original toolbar pixels.
- **Independent video colors:** video, HDR processing, subtitles and custom subtitle colors retain their colors.

With a controller, use L1/R1 to move between controls, D-pad Left/Right to select the theme, and Cross on Apply or OK.

### Fixed

- Added missing Spanish, Brazilian Portuguese, French and German translations for four existing HDR/GPU labels.
  The GPU retry button and hint are checked against the actual font metrics in all five languages.
- Fixed spaces in the dependency archive command and runtime checksum path. `MPCPS5_DEPS_DIR` allows dependencies
  to be built in a separate cache without spaces.
- Fixed an unused constant stopping Linux builds with Clang and `-Werror`, preserving the console's crash handling.

### Updating

- **From 1.6.0:** use **Help > Check for updates**, install the update and restart the app. The update ZIP contains
  the native application, its metadata, artwork, runtime module and file manifest.
- For manual installation, run `MediaPlayerClassicPS5-v1.6.1.elf` with your existing homebrew setup, then open
  Media Player Classic PS5 from the home screen. The ELF includes the installer, helper and native player.
- If an older installer ELF is configured in autoload, replace that file with the new installer.
- Settings and media remain in `/data/mpcps5`. The new preference is `DarkMode=1` (Dark) or `DarkMode=0` (Light)
  in `mpc-hc.ini`.

### Files

- `MediaPlayerClassicPS5-v1.6.1.elf`: the complete installer. This is the only ELF download for this release.
- `MediaPlayerClassicPS5-v1.6.1-PPSA99015.zip`: the application update package used by Help > Check for updates.
- `mpc-ps5-v1.6.1-src.zip`: the source code, tests, third-party notices and documentation.
- The `.sha256` files and `SHA256SUMS.txt`: download checksums.

### Validation and limits

- Native PS5 GPU build and complete installer: passed, including application imports, runtime/link checks,
  RADV linkage and application profile validation.
- Theme harness: **41 checks passed**, including Apply/OK/Cancel, saved preferences, invalid values, toolbar
  cache invalidation, alpha/color preservation, text contrast and translated control widths.
- Selected frontend groups 1, 2, 9, 17 and 36: **60 checks passed**, covering the original Light interface,
  playback, subtitles, Options, languages and controller-driven theme changes. Video and subtitles were checked
  with software and Vulkan rendering. The language group was checked again after the final wording adjustment:
  **12 checks passed**.
- Vulkan HDR output regressions on Lavapipe: **15 checks passed**. The 46 original Light palette values were
  preserved, and the translation table passed validation for 658 texts.
- Host checks used Clang 18 with ASan and UBSan. The complete frontend suite was not rerun.
- Release packaging: all six embedded application files match the update ZIP byte for byte; the manifest and
  stored ZIP entries were verified. **14 package update checks passed** using the real updater in an isolated
  host directory, including version comparison, checksum rejection, file installation and transaction cleanup.

**This build has not been tested on a physical PS5.** Host tests use simulated PS5 APIs and software Vulkan.
The 1.6.0 console observations remain historical; native HDR10 output on an HDR10-capable TV still needs confirmation.
See the [dark mode details and host capture](https://github.com/MisterTemaki/MediaPlayerClassicPS5/blob/v1.6.1/docs/DARK-MODE.md).

Port by [MisterTemaki](https://github.com/MisterTemaki).
