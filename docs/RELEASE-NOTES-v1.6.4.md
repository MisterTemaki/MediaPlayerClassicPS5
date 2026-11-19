## Media Player Classic PS5 1.6.4

**Stop** no longer makes a file forget its position. Application version is **1.6.4**, content version is
**01.064.000**, and the title ID remains **PPSA99015**.

### Fixed

- **A file stopped with Stop opens again where it was stopped.** Stop goes back to the start of the file, as in
  MPC-HC, and up to 1.6.3 that start was the position saved when the file was closed or the app exited. With
  **Remember File position** on, the file then opened again from the beginning, while a file paused, or still
  playing, when the app exited opened again where it was left. Every earlier version behaved this way.
- Now the place where **Stop** was pressed is the position remembered. It is saved to `mpc-hc.ini` as soon as
  Stop is pressed, so it is kept even when the app is then closed from the PS5 home screen.
- The stopped place stays the file's position until the file plays again (from the start, as Stop leaves it) or
  another place is chosen (Go To, a jump, the seek bar). **File > Close**, opening another file and **File >
  Exit** after a Stop all keep it.

### Unchanged

- Stop still goes back to the start, and **Play** after a Stop still plays from the start.
- Positions are still forgotten in the first 5 seconds and near the end of a file, as before.
- Backup, dark mode, saved network passwords, settings and favorites behave as in 1.6.3.

### Updating

- From 1.6.3 or an older version with the updater, choose **Help > Check for updates**, install the update and
  restart the app.
- For manual installation, use **MediaPlayerClassicPS5-v1.6.4.elf**, the complete installer, then open the app
  from the home screen. This is the only ELF asset for this release.
- If an older installer ELF is configured in autoload, replace that file with the new installer.
- Settings and media remain in `/data/mpcps5`.

### Files

- [MediaPlayerClassicPS5-v1.6.4.elf](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.4/MediaPlayerClassicPS5-v1.6.4.elf): complete installer.
- [MediaPlayerClassicPS5-v1.6.4-PPSA99015.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.4/MediaPlayerClassicPS5-v1.6.4-PPSA99015.zip): application package for the in-app updater.
- [mpc-ps5-v1.6.4-src.zip](https://github.com/MisterTemaki/MediaPlayerClassicPS5/releases/download/v1.6.4/mpc-ps5-v1.6.4-src.zip): source code, tests, documentation and third-party notices.
- The matching `.sha256` files and `SHA256SUMS.txt` contain download checksums.

### Validation and limits

- The new group 38 of the frontend suite (12 checks) covers Stop and then Exit, opening the file again, the app
  ended without Exit after a Stop, Stop and Pause, a second Stop, Go To while stopped, Play after a Stop,
  **File > Close** and another file opened after a Stop, and Exit while playing. On a 1.6.3 build six of these
  checks fail, the ones that keep the stopped place.

Host tests use simulated PS5 calls and software Vulkan.

Port by [MisterTemaki](https://github.com/MisterTemaki).
