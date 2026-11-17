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
- **Back up now:** applies the pages, writes the backup and says where (or why it couldn't). When the drive holds
  another installation's backup (see below), it asks before replacing it.
- The line under the button says when the backup on that drive was written, or that there is none yet, and why
  the last attempt failed if it did.
- **Import...:** takes the newest backup on the connected drives, says its date and place, and asks first. **Yes**
  replaces every setting and list with the backup's, and the drive it came from becomes the one backed up to.

## Whose backup

`mpc-hc.ini` has a random `BackupId=`, made the first time the settings are saved and kept by **Reset** and by
**Import...**. A backup belongs to the installation with the same id. The automatic backup writes only where there
is no backup yet, or this installation's own. It never writes over a backup from before a factory reset, from
another PS5, on another stick that is now at the same `/mnt/usbN`, or one it can't read. The page says when the
chosen drive holds such a backup: import it, or press **Back up now** and say yes to replace it (the question has
**No** as its default).

After an import the backup is written again with this installation's id, so it is this one's from then on. If the
console that made it still writes to the same stick, it finds another installation's backup there and leaves it.

Going back to 1.6.2 and then forward again makes a new id (1.6.2 doesn't keep the line): the backup on the stick is
then another installation's, and **Back up now** asks before replacing it.

## What is in a backup

Everything `mpc-hc.ini` holds: Options (Player, Playback, Output, Fullscreen, Subtitles and their Default Style,
Audio Switcher, Miscellaneous), the controller's Keys, the theme and language, recent files, file positions,
favorites and network locations.

**Not in a backup:**

- **Network passwords.** They are in the encrypted store, whose key never leaves the console
  ([saved network passwords](NETWORK-PASSWORDS.md)). After an import, the network locations are there; type each
  password again when it is asked for. A backup has no password in it, and an older settings file with passwords
  in its addresses has them dropped when imported, never used. As with any change to the lists, a saved password
  whose network location, favorite or recent file isn't in the imported settings is removed from the store.
- **The fonts folder** (`/data/mpcps5/fonts`): copy back the fonts you had added. A Default Style font that isn't
  there is replaced by the default one.

## Notes

- With **Off** (a new installation, or after **Reset**), the app never writes a backup, so a backup on a drive stays
  as it was until it is imported.
- A drive that isn't connected keeps its place in the list as "(not connected)"; backups resume when a drive is at
  that place again, if its backup is this installation's (or it has none).
- The backup is written by its own thread: the picture never waits for the drive. Closing the app waits up to three
  seconds for the last one. **Back up now** stops (and says so) when the settings can't be saved first.
- The backup is written beside the old one and then renamed over it: a backup interrupted halfway (the drive pulled
  out, the console switched off) leaves the previous one whole. On a drive that won't rename over a file, the old
  backup is removed first; if the rename still fails, the new one stays as `mpc-hc.ini.tmp` and the page says so.
- The video renderer and HDR output are read when the app starts: after an import they apply from the next start.
