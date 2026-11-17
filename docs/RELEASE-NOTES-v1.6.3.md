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

UPDATING-TILE-PLACEHOLDER

### Files

- [MediaPlayerClassicPS5-v1.6.3.elf](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.3/MediaPlayerClassicPS5-v1.6.3.elf): complete installer.
- [MediaPlayerClassicPS5-v1.6.3-PPSA99015.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.3/MediaPlayerClassicPS5-v1.6.3-PPSA99015.zip): application package for the in-app updater.
- [mpc-ps5-v1.6.3-src.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.3/mpc-ps5-v1.6.3-src.zip): source code, tests, documentation and third-party notices.
- The matching `.sha256` files and `SHA256SUMS.txt` contain download checksums.

### Validation and limits

VALIDATION-PLACEHOLDER

Port by [MisterTemaki](https://github.com/MisterTemaki).
