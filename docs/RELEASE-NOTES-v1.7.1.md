## Media Player Classic PS5 1.7.1

Fixes for **issue #11** (the app showing only its logo) and for what an audit of the whole app found. Application
version is **1.7.1**, content version is **01.071.000**, and the title ID remains **PPSA99015**.

> **If you had ticked View > Options > Fullscreen > "Launch files in fullscreen", tick it again.** It has its own
> line in `mpc-hc.ini` now (`LaunchFullScreen=`); 1.7.0's `FullScreen=` isn't read.

### Fixed: the screen with the logo alone (#11)

- **The app always starts in its window**, not in full screen, as MPC-HC does. Up to 1.7.0, full screen
  (Triangle, View > Full Screen) was saved as "Launch files in fullscreen": after a film watched in full screen the
  next start was in full screen too, with nothing open, so the screen showed only the logo.
- **Launch files in fullscreen** works as in MPC-HC: a film opens in full screen; the app itself starts in its window.
- In full screen, a file with no picture (music without a cover) keeps the bars at the bottom.
- With the menu bar hidden (View > Presets > Minimal), the OSD says which button opens the menu ("OPTIONS: the
  menu"), also when the app starts that way.
- A start that doesn't reach the screen is noticed by the next one, which writes `boot.log` even with Debug logs off
  and says why.
- The GPU start is watched: if the video still hasn't come up after 20 s, a notification says to close the app and
  open it again (the next start uses the software renderer).

### Issue #12: Games or Media, and the fonts in the backup

- **The fonts folder is in the settings backup** (View > Options > Miscellaneous > Backup): the fonts you added for
  subtitles are copied to `<drive>/mpcps5/backup/fonts` with the settings, and **Import...** copies back the ones
  missing from the fonts folder (a font of the same name already there is kept).
- **Listing the app under Games again** is the option 1.7.0 added: **View > Options > Miscellaneous > "On the PS5's
  home screen, under: Games"**. The page now says what it brings: under Games, the PS5 allows screenshots and video
  clips and counts the play time, as for a game. The choice is written into the app's `param.json`, which
  ShadowMount+ reads when it adds the app to the home screen: after changing it, delete the app from the home screen
  (OPTIONS on its tile > Delete) and ShadowMount+ adds it again where you chose. The complete installer writes the
  choice into the app it installs; after an in-app update the app writes it back when it starts (restart it before
  you re-add the tile). The choice is in the settings backup too, so an import brings it back.
- The play time itself is counted and kept by the PS5, not by the app: the app can't save it or bring it back after
  a reinstall or a factory reset.

### Fixed: network shares (SMB)

- **A share that stops answering no longer freezes the app.** Closing the film, opening another one or closing the
  app comes back at once. Up to 1.7.0 each of them waited for every request to the share to time out (15 s each).
- **The Open dialog reads a share in the background:** the list says "Reading the network folder..." while the app
  goes on (the picture, the pad, the menus), and Cancel, Back or another folder give the listing up. Up to 1.7.0 a NAS
  that was asleep froze the app before the dialog appeared, at every Open, when the last folder browsed was on it.
- **Next / Previous (R1 / L1), Open Directory and "Play next file"** read a share's folder in the background too.
- The subtitles next to a film on a share are found with one listing of its folder instead of 11 connections, and
  their names are matched without regard to case (`FILM.SRT` for `film.mkv`).
- File > Load Subtitle from a share waits 10 s at most (20 s for a `.sup`).
- A file whose name contains `%` opens.

### Fixed: playback

- **The end of a file is played to its end.** Up to 1.7.0 the last third of a second of every file (the pictures
  still queued, the sound in the console's audio buffer) was cut when the playlist moved on or the player paused at
  the end.
- A video whose picture size changes in the middle (clips of different sizes joined, a broadcast switching between
  720p and 1080) is no longer cut to its first size by the console's decoder: FFmpeg takes it from the next keyframe.
- An audio track chosen from a menu while a file is still opening is ignored instead of possibly ending the app.
- A subtitle line with an absurd duration, or a packet that can't be copied for lack of memory, no longer causes
  undefined behaviour or a false end of the video.

### Fixed: the window and settings

- **The file position is kept when the app is closed from the PS5's home screen.** It is written to `mpc-hc.ini`
  each minute while a film plays, and on Pause. Up to 1.7.0 it was written only on Stop, Close, Exit and a few other
  events, so the usual "PS button, Close Application" lost it.
- **Menus taller than the screen scroll** (a Blu-ray's many subtitle tracks, a long chapter list, many favorites;
  the context menu at 200 %): arrows above and below, the highlight kept in sight, the right stick scrolls.
- A settings save that fails is said once, with the cause (an unreadable password file of an older version,
  the network passwords, a full storage). Up to 1.7.0 only the log said it, and Debug logs are off by default.
- A subtitle cue of thousands of lines shows the lines that fit on the screen instead of making a picture of
  gigabytes.
- Opening a favorite resets the playing rate and the playlist's selection, as opening a file does.
- An smb:// address with `@` in a file name is shown whole; favorites and recent files are shown as they are, not
  looked up as texts to translate.
- Lists in `mpc-hc.ini` are capped when read (favorites 999, file positions 50, network locations 64), and the times
  typed in Go To are bounded.

### Fixed: startup, the GPU and updates

- A GPU start counts as working once frames have reached the screen (8 frames), not after the first black frame
  that only queues: a driver that stops in the first frames now leaves the next start on the software renderer.
- A GPU lost while playing no longer keeps every later start on the software renderer.
- "Video failed to start" points to `boot.log` only when it is written.
- The GPU library checks the display's set-up and its uploads, and syncs its pipeline cache before replacing it.
- The updater writes the home screen's art beside the old one before replacing it (a failed write keeps the old),
  doesn't add "Nothing was changed" to an error that asks for a repair, and answers Help > Check for updates asked
  while the start's own check runs.

### Known limits

- Updates are checked against GitHub's SHA-256, over TLS with the certificate checked; releases still carry no
  Ed25519 signature (the code for it is there, without a key).
- A GPU driver that never returns from a wait can't be stopped from inside the app: after 20 s at the start a
  notification says what to do, and the next start uses the software renderer. A wait that hangs later, while
  playing, still holds the app until it is closed from the home screen.
- The full list of what the audit found, fixed or left: [audit report](https://github.com/MisterTemaki/MediaPlayerClassicPS5/blob/v1.7.1/docs/AUDIT-v1.7.1.md).

### Updating

- From 1.7.0 or an older version with the updater, choose **Help > Check for updates**, install the update and
  restart the app.
- For manual installation, use **MediaPlayerClassicPS5-v1.7.1.elf**, the complete installer, then open the app
  from the home screen. If an older installer ELF is in your autoload, replace it with this one.
- Settings and media remain in `/data/mpcps5`. Tick "Launch files in fullscreen" again if you used it.

### Files

- [MediaPlayerClassicPS5-v1.7.1.elf](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.7.1/MediaPlayerClassicPS5-v1.7.1.elf): complete installer.
- [MediaPlayerClassicPS5-v1.7.1-PPSA99015.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.7.1/MediaPlayerClassicPS5-v1.7.1-PPSA99015.zip): application package for the in-app updater.
- [mpc-ps5-v1.7.1-src.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.7.1/mpc-ps5-v1.7.1-src.zip): source code, tests, documentation and third-party notices.

### Validation and limits

- **On a PS5** (firmware 13.60, a 4K TV at 59.94 Hz, ShadowMount+; the second test build, the same binary as this
  release):
  - The complete installer installed 1.7.1, and its helper served every start.
  - Every start used the GPU, and each one counted as working after its first frames.
  - The GPU read the console decoder's pictures in place (H.264 1080p and 4K, and 4K HEVC HDR10 with native HDR10
    output). The only exception was 6 of 786 pictures of the HDR10 film, which the decoder held back and which were
    copied.
  - The Experimental preset cost what it did on 1.7.0: 5.3 ms per frame for a 4K HDR10 film, 1.5 ms for a
    1920×800 one. No picture was dropped.
  - A file played to its last second.
  - On each of two films, the sound didn't run short in a minute of steady play.
  - An 8K HEVC film is beyond the console's decoder: it went to the CPU, dropped most of its pictures as its notices
    said, and didn't crash.
- **Full frontend suite on the tested code: 1025 checks passed.** The 12 that fail (an SMB address check and 11 comparisons of the GPU's
  4K HDR pictures with the CPU path's on the host's software Vulkan) fail the same way on 1.7.0. The new groups 41,
  42 and 43 (67 checks) cover issue #11, the GPU start guard, a share that stalls or never answers, the end of a file,
  the picture size change, the position kept when the app is killed, the scrolling menus and the rest; on the 1.7.0
  build, 31 of them fail. Group 44 covers the fonts in the backup (#12). Two checks were added to it after the PS5
  test (a font put in over FTP with nothing else changed, and Back up now on a fresh installation); groups 37 and 44
  then passed in full (53 checks).
- The harnesses pass (update, logs, installer, GPU start, decoder memory, HDR, theme, credentials), now with the PS5
  runtime layer's own tests. The suite runs with ASan, UBSan and float-cast-overflow, and every run is checked for
  their reports.
- The translation table (765 texts) passed validation in all five languages.
- The native PS5 build and complete installer passed import, runtime-link, syscall and RADV linkage checks.
- An audit in five parts and an independent review of the fixes (see the audit report).

Host tests use simulated PS5 calls and software Vulkan.

### Credits

- The reporter of **issue #11**, for the screenshot that showed the logo screen.
- **Sulfurico-One64** (**issue #12**), for the fonts in the backup and for asking about Games and Media.

Port by [MisterTemaki](https://github.com/MisterTemaki), with the PS5 layer of PS5SX2.
