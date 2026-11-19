## Media Player Classic PS5 1.6.4

**Stop** no longer makes a file forget its position, and fixes from an audit of the app's code: settings are saved
again when the saved network passwords can't be read, and playlists made elsewhere open whole. Application version
is **1.6.4**, content version is **01.064.000**, and the title ID remains **PPSA99015**.

### Fixed

- **A file stopped with Stop opens again where it was stopped.** Stop goes back to the start of the file, as in
  MPC-HC, and up to 1.6.3 that start was the position saved when the file was closed or the app exited. With
  **Remember File position** on, the file then opened again from the beginning, while a file paused, or still
  playing, when the app exited opened again where it was left. Every earlier version behaved this way.
  - Now the place where **Stop** was pressed is the position remembered (MPC-HC also keeps the last position from
    before its Stop). It is written to `mpc-hc.ini` as soon as Stop is pressed, so it is kept even when the app is
    then closed from the PS5 home screen.
  - The stopped place stays the file's position until the file plays again (from the start, as Stop leaves it) or
    another place is chosen (Go To, a jump, the seek bar, a chapter). **File > Close**, opening another file,
    **File > Exit** and **Favorites > Add to Favorites** after a Stop all keep it.
- **Settings are saved again when the saved network passwords can't be read.** If `credentials.enc` was there
  but couldn't be read, every save of the settings failed, and only the log said so. That happens when the hidden
  `.credentials-device` folder is missing or no longer private, for example after `/data/mpcps5` was copied over FTP,
  or when the file is damaged. From then on no option, file position, recent file or favorite was saved again. Now
  the settings are saved; the encrypted passwords and their key are never written over, as before.
- **Playlists:**
  - an `.m3u` entry whose name starts with a character from U+F000 to U+FFFF (a fullwidth letter, halfwidth
    katakana) lost its first character, as if it were the byte order mark: now only the file's first line has one;
  - an `.m3u` entry with `=` in its name (or starting with `[`) was left out, read as a `.pls` key: `key=value`
    lines are now read in `.pls` files only;
  - a relative path written with backslashes (a list made on Windows, `Films\film.mkv`) now opens, as in MPC-HC.
- A file position or a favorite's position in `mpc-hc.ini` that isn't a number of seconds (`inf`, `nan`, a negative
  one, from a file edited by hand or a backup imported from a USB drive) is ignored instead of being used as a seek.
- A UTF-16 subtitle file with half a surrogate pair shows U+FFFD there instead of bytes that aren't UTF-8.
- The CA list (`cacert.pem`, for `https://` and the updater) is written only when it differs, and whole. It was
  rewritten in place at every start, and a write cut short left part of the list.
- Error texts on the backup and disc threads use `strerror_r` instead of `strerror`, which isn't thread-safe.
- An SMB address's password is wiped from memory once the address is done with, as the credential store does.

### Unchanged

- Stop still goes back to the start, and **Play** after a Stop still plays from the start.
- Positions are still forgotten in the first 5 seconds and near the end of a file, as before.
- Backup, dark mode, saved network passwords, settings and favorites behave as in 1.6.3, except that `mpc-hc.ini`
  (and the USB backup, when one is chosen) is now also written when Stop is pressed with **Remember File position**
  on.

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

- (to be filled in with the final runs)

Host tests use simulated PS5 calls and software Vulkan.

Port by [MisterTemaki](https://github.com/MisterTemaki).
