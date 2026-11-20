## Media Player Classic PS5 1.7.0

**Video post-processing on the GPU**, suggested by **Cris Valente**: thank you, Cris! Also a choice of where the
home screen lists the app, and the fixes made for 1.6.4 (which wasn't released on its own): **Stop** keeps the
file's position, and an audit's fixes. Application version is **1.7.0**, content version is **01.070.000**, and the
title ID remains **PPSA99015**.

### Added: Shaders (View > Options > Output > Shaders, Play > Shaders)

A chain of compute passes between the decoder's picture and the screen, for the video only (never the window, the
menus or the subtitles), with the GPU renderer. **Off** is the default: the picture is drawn in one pass, as in 1.6.

- **Presets:**
  - **Performance:** FSR 1 enlarges the picture to the screen (AMD FidelityFX Super Resolution 1.0's EASU, with its
    RCAS sharpening at 75 %).
  - **Quality:** debanding and light deblocking first, then FSR 1 (sharpening 88 %).
  - **Experimental:** Quality, and motion smoothing.
  - **Custom:** each of them on or off, their strength, the scaler (FSR 1, Lanczos, bicubic or bilinear) and the
    sharpening.
- **Debanding** smooths the steps of gradients (skies, dark scenes) the way libplacebo and f3kdb do: where nearby
  pixels differ by less than a threshold, their average is taken, then a little grain hides what is left.
- **Deblocking** softens the 8×8 block edges of MPEG-2, MPEG-4 and early H.264 video, where the step between two
  blocks is smaller than the texture around it (H.264's own rule), and keeps a pixel next to a strong edge within
  its neighbours' range (deringing).
- **Motion smoothing (experimental)** makes pictures between the film's own for the screen's 60 a second: each
  picture's optical flow both ways, coarse to fine, then the picture at the display's time, with occluded areas taken
  from the picture that shows them. A cut (two pictures more different than the **Scene cut** setting) is never
  blended, and where the motion is beyond what the flow can follow the nearer picture is shown as it is, never a
  double image. It is used while playing; paused, stepped or seeking, the picture is shown as it is.
- **Colour controls:** brightness, contrast and saturation, with any preset but Off.
- **Play > Shaders** switches between the presets at once; **Shader Options...** opens the page.
- **View > Statistics** has a **Shaders** line with each pass's GPU time per frame (prep, motion smoothing, scale,
  final); the statistics over the video (R3 + Square) show their sum. A paused picture, and the repeats of a 24 fps
  film at 60 Hz, cost only the last pass.
- HDR10 and HLG films go through the passes in their own signal; tone mapping, or the native HDR10 output, comes
  after, as before.
- The passes use the GPU's half-precision arithmetic where it has it, and say so in the statistics.
- The design, the settings in `mpc-hc.ini` and what wasn't done (neural networks, FSR 2/3, 120 Hz) and why:
  [video post-processing](https://github.com/MisterTemaki/MediaPlayerClassicPS5/blob/v1.7.0/docs/POST-PROCESSING.md).

### Added: where the home screen lists the app (View > Options > Miscellaneous)

- **On the PS5's home screen, under: Media / Games.** 1.6.3 moved the app to **Media**; this puts it back under
  **Games** for whoever prefers it there (or keeps it under Media, the default).
- The choice is written into the app's `sce_sys/param.json` and the home screen's copy at once, at every start of
  the app, and by the installer ELF. The PS5 reads it when it adds the app to the home screen: delete the app from
  the home screen (Options on its card, Delete) and ShadowMount+ adds it again under the chosen one. Settings,
  favorites and file positions in `/data/mpcps5` stay; a `manual.lst` line must be put back.
- When the app's folder can't be written (an image, a read-only folder), Options says so and nothing changes.

### Fixed (from 1.6.4)

- **A file stopped with Stop opens again where it was stopped.** Stop goes back to the start of the file, as in
  MPC-HC, and up to 1.6.3 that start was the position saved when the file was closed or the app exited. Now the
  place where **Stop** was pressed is the position remembered, written to `mpc-hc.ini` at once. It stays the file's
  position until the file plays again or another place is chosen; **File > Close**, opening another file,
  **File > Exit** and **Favorites > Add to Favorites** after a Stop all keep it.
- **Settings are saved again when the saved network passwords can't be read** (after a copy of `/data/mpcps5` over
  FTP, for instance, or a damaged `credentials.enc`). Up to 1.6.3 no option, position, recent file or favorite was
  saved from then on. The encrypted passwords and their key are never written over.
- **Playlists:** an `.m3u` entry starting with a character from U+F000 to U+FFFF no longer loses it; an entry with
  `=` in its name is no longer left out (`key=value` lines are read in `.pls` files only, a file whose name ends with
  `.pls` or whose first line is `[playlist]`); a path written with backslashes (made on Windows) opens.
- A file position or a favorite's position in `mpc-hc.ini` that isn't a number of seconds is ignored instead of being
  used as a seek.
- A UTF-16 subtitle file with half a surrogate pair shows U+FFFD there instead of dropping the line.
- The CA list (`cacert.pem`, for `https://` and the updater) is written only when it differs, and whole.
- Error texts on the backup and disc threads use `strerror_r`; the SMB address parser wipes its copies of a password.

### Unchanged

- Without a preset, the picture is drawn exactly as in 1.6 (nothing of the passes is made).
- Stop still goes back to the start, and **Play** after a Stop still plays from the start.
- Backup, dark mode, saved network passwords, settings and favorites behave as in 1.6.3.

### Updating

- From 1.6.3 or an older version with the updater, choose **Help > Check for updates**, install the update and
  restart the app.
- For manual installation, use **MediaPlayerClassicPS5-v1.7.0.elf**, the complete installer, then open the app
  from the home screen. If an older installer ELF is in your autoload, replace it with this one.
- Settings and media remain in `/data/mpcps5`.

### Files

- [MediaPlayerClassicPS5-v1.7.0.elf](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.7.0/MediaPlayerClassicPS5-v1.7.0.elf): complete installer.
- [MediaPlayerClassicPS5-v1.7.0-PPSA99015.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.7.0/MediaPlayerClassicPS5-v1.7.0-PPSA99015.zip): application package for the in-app updater.
- [mpc-ps5-v1.7.0-src.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.7.0/mpc-ps5-v1.7.0-src.zip): source code, tests, documentation and third-party notices.

### Validation and limits

- (to be filled in with the final runs)

### Credits

- **Cris Valente**, for suggesting the post-processing, its presets and the timings to watch.
- AMD FidelityFX Super Resolution 1.0 (MIT), vendored unchanged in `ps5-slang-gpu/third_party/ffx-fsr1`.
- The debanding follows the approach of libplacebo and f3kdb, the motion smoothing's time approximation that of Super
  SloMo (Jiang et al., 2018); both reimplemented, no code copied.

Port by [MisterTemaki](https://github.com/MisterTemaki), with the PS5 layer of PS5SX2.
