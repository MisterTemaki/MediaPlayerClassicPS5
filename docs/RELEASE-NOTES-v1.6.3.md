## Media Player Classic PS5 1.6.3

Settings backup to a USB drive, addressing [issue #10](https://github.com/MisterTemaki/MediaPlayerClassicPS5/issues/10),
and the app listed under **Media** on the home screen, addressing
[issue #6](https://github.com/MisterTemaki/MediaPlayerClassicPS5/issues/6).
Application version is **1.6.3**, content version is **01.063.000**, and the title ID remains **PPSA99015**.

### Added

- **View > Options > Miscellaneous > Backup.** Choose a drive under **Back up automatically to:** and the
  settings are copied to `<drive>/mpcps5/backup/mpc-hc.ini` at once, then again every time they change.
  **Back up now** writes it on request. **Import...** takes the newest backup on the connected drives and, after
  asking, replaces the settings with it: after a factory reset, or on another PS5.
- A backup holds everything `mpc-hc.ini` holds: Options, the controller's Keys, audio and subtitle settings,
  theme and language, recent files, file positions, favorites and network locations.
- Network passwords are not in backups (their key never leaves the console): type them again after an import.
  The fonts folder isn't copied either. See [settings backup](https://github.com/MisterTemaki/MediaPlayerClassicPS5/blob/v1.6.3/docs/SETTINGS-BACKUP.md).
- **A backup this installation didn't write is never replaced on its own** (the one from before a factory reset,
  another PS5's, another stick at the same `/mnt/usbN`, one that can't be read). The page says so; **Back up now**
  asks first, with **No** as the default. The backup is written by its own thread, so the picture never waits for
  the drive.

### Changed

- `sce_sys/param.json` declares the app as a media application: `applicationCategoryType` is `65536`
  ("Prospero Native Media App") instead of `0` (a native game). VLC for PS5 declares the same value.
- The build's preflight check rejects a `param.json` that is not a media application, and one whose
  `contentVersion` doesn't match the version being built.
- Playback, dark mode, saved network passwords, settings and favorites behave as in 1.6.2.

### Updating

- From 1.6.2 or an older version with the updater, choose **Help > Check for updates**, install the update and
  restart the app.
- For manual installation, use **MediaPlayerClassicPS5-v1.6.3.elf**, the complete installer, then open the app
  from the home screen. This is the only ELF asset for this release.
- If an older installer ELF is configured in autoload, replace that file with the new installer.
- Settings and media remain in `/data/mpcps5`.

- **Moving to Media:** the console decides the tab when ShadowMount+ registers the app. If the tile still shows
  under Games after the update and a full restart, uninstall it from the home screen and run ShadowMount+ again from
  the Payload Manager (if the app is listed in `/data/shadowmount/manual.lst`, put its line back first), as in the
  README's [Updates](https://github.com/MisterTemaki/MediaPlayerClassicPS5/blob/v1.6.3/README.md#updates). The
  settings in `/data/mpcps5` are not touched; making a backup first is a good idea.

### Files

- [MediaPlayerClassicPS5-v1.6.3.elf](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.3/MediaPlayerClassicPS5-v1.6.3.elf): complete installer.
- [MediaPlayerClassicPS5-v1.6.3-PPSA99015.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.3/MediaPlayerClassicPS5-v1.6.3-PPSA99015.zip): application package for the in-app updater.
- [mpc-ps5-v1.6.3-src.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.3/mpc-ps5-v1.6.3-src.zip): source code, tests, documentation and third-party notices.
- The matching `.sha256` files and `SHA256SUMS.txt` contain download checksums.

### Validation and limits

- **Not run on a PS5 before this release.** Please report how the Backup page, the import and the home-screen tab
  behave on your console (with **Debug logs** on, `/data/mpcps5/logs/boot.log`).
- **Full frontend suite: 842 checks passed.** The 12 checks that fail (groups 30 and 34: an SMB address
  check and the GPU HDR comparisons) fail the same way on a 1.6.2 build without these changes. The new group 37
  (34 checks) covers choosing a drive, unchanged settings not rewritten, a drive not connected, a backup from
  another installation kept and replaced only after a yes, Reset, import with Yes and No, a backup made on another
  port, passwords dropped, an unreadable backup and a file that isn't a backup.
- Credential core 36, real SMB UI 31 and theme harness 42 checks passed, and the update, log, installer, GPU,
  decoder and HDR harnesses pass. Host runs used Clang 18 with ASan and UBSan.
- The translation table (707 texts) passed validation in all five languages; the Backup page was checked on
  screen in English, Spanish, Portuguese, French and German.
- The native PS5 GPU build and complete installer passed import, runtime-link, syscall, RADV linkage and
  application-profile checks (`applicationCategoryType` 65536, content version 01.063.000).
- Two independent code reviews; their findings are fixed.

Host tests use simulated PS5 calls and software Vulkan.

Port by [MisterTemaki](https://github.com/MisterTemaki).
