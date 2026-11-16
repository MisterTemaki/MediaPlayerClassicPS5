# Settings backup (1.6.3)

[Issue #10](https://github.com/MisterTemaki/MediaPlayerClassicPS5/issues/10): a console that has to be reset to
factory settings loses `/data/mpcps5`, and with it everything set up in the app. 1.6.3 keeps a copy of the
settings on a USB drive and brings it back.

## Where

**View > Options > Miscellaneous > Backup.**

- **Back up automatically to:** **Off** (the default), or one of the connected drives, named as the file browser
  names them (**USB Drive 0 (/mnt/usb0)**, **Extended Storage 0 (/mnt/ext0)**...). Choose a drive and press **OK**
  or **Apply**: the backup is written at once, to `<drive>/mpcps5/backup/mpc-hc.ini`, and again every time the
  settings change (Options, the volume, a new favorite, a file's position...). Nothing is written when the settings
  are the same as the backup's.
- **Back up now:** applies the pages, writes the backup and says where (or why it couldn't).
- The line under the button says when the backup on that drive was written, or that there is none yet, and why
  the last attempt failed if it did.
- **Import...:** takes the newest backup on the connected drives, says its date and place, and asks first. **Yes**
  replaces every setting and list with the backup's, and the drive it came from becomes the one backed up to.

## What is in a backup

Everything `mpc-hc.ini` holds: Options (Player, Playback, Output, Fullscreen, Subtitles and their Default Style,
Audio Switcher, Miscellaneous), the controller's Keys, the theme and language, recent files, file positions,
favorites and network locations.

**Not in a backup:**

- **Network passwords.** They are in the encrypted store, whose key never leaves the console
  ([saved network passwords](NETWORK-PASSWORDS.md)). After an import, the network locations are there; type each
  password again when it is asked for. A backup has no password in it, and an older settings file with passwords
  in its addresses has them dropped when imported, never used.
- **The fonts folder** (`/data/mpcps5/fonts`): copy back the fonts you had added. A Default Style font that isn't
  there is replaced by the default one.

## Notes

- With **Off** (a new installation, or after **Reset**), the app never writes a backup, so a backup on a drive stays
  as it was until it is imported.
- A drive that isn't connected keeps its place in the list as "(not connected)"; backups resume when it is back.
- The backup is written beside the old one and then renamed over it: a backup interrupted halfway (the drive pulled
  out, the console switched off) leaves the previous one whole.
- The video renderer and HDR output are read when the app starts: after an import they apply from the next start.
