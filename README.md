# Media Player Classic PS5

A port of **Media Player Classic - Home Cinema** (MPC-HC 1.7.13) to the PlayStation 5: MPC-HC's window,
menus, toolbar, seek bar, status bar, dialogs and keyboard behaviour, drawn as Windows 10 draws them, on a
native PS5 app that plays video and music from **USB drives, the console's storage, Windows/Samba network
shares (SMB) and the Internet (HTTP/HTTPS, HLS)**.

On Windows MPC-HC plays through DirectShow and LAV Filters, which are FFmpeg inside; here the playback engine
is **FFmpeg 7.1.1** itself (with **dav1d** for AV1), built for the console, and SMB shares are read with
**libsmb2**, the library VLC's SMB module uses.

PS5 port by [github.com/MisterTemaki](https://github.com/MisterTemaki), with the PS5 layer of
[PS5SX2](https://github.com/MisterTemaki) (video out, sound, pad, the installer and the sandbox helper).

![Media Player Classic PS5](docs/screenshots/window.png)

> **Status (1.2.3):** builds with the ps5-payload-dev SDK into a signed native app and passes 229 host tests
> (the same code on Linux, the PS5 calls faked: playback, subtitles, chapters, tracks, HTTP, SMB, the pointer,
> the dialogs, the on-screen keyboard, the installer and the helper; the PS5's video decoder through a stand-in,
> HDR tone mapping, 5.1/7.1 output). It has **not been run on a PS5 yet** -- please report what works and what
> doesn't (with `debug_logs=1` in `/data/mpcps5/mpc-hc.ini` the log goes to `/data/mpcps5/logs/boot.log`).

**1.1:** what VLC for PS5 ([ZiZc3/VLC](https://github.com/ZiZc3/VLC)) showed can be done on the console:
**hardware decoding** of H.264 and HEVC (8 and 10-bit, up to 4K) on the PS5's video decoder, **HDR10 and HLG
tone mapping** to the SDR picture, and **5.1 / 7.1 output** on the console's 8-channel port. See
[Hardware decoding, HDR and surround](#hardware-decoding-hdr-and-surround). And **Help > Check for updates**
works: the app updates itself from its GitHub releases, as Porpoise does (see [Updates](#updates)).

**1.2.3:**
- **Subtitles**: a character the chosen font hasn't got (Japanese, Chinese, Korean, symbols) comes from any loaded
  font that has it: the app's own, the MKV's attachments, or `/data/mpcps5/fonts` (a small patch to libass, which
  otherwise needs a system font service the console doesn't have). Put a font with the scripts you need in that
  folder (for example a Noto Sans CJK or IPAGothic file).
- **The window when nothing happens** (no file, paused or stopped, no dialog, menu, OSD or pointer, nothing pressed
  for 2 s) is drawn twice a second instead of at every refresh; any input, a picture or a playing file brings the
  full rate back at once.

**1.2.2:** fixes from a third review, and **ASS/SSA subtitles drawn by libass** as they are authored (positions,
styles, fonts, colours, outlines, rotation, karaoke and other effects; the MKV's attached fonts), see
[Subtitles](#subtitles):
- **Updates**:
  - nothing is installed while an earlier update isn't put back;
  - `update.journal` is written whole and must read whole (a header, the files, `end`), else it is left alone and
    the app asks for the payload;
  - each update keeps its old copies as `<name>.old.<transaction>`, with each old file's SHA-256 in the journal (a
    file already put back is recognised);
  - the folders are synced and a sync that fails puts the old version back;
  - the payload sent over an unfinished update repairs the app and clears what it left.
- **Signed releases (Ed25519)**: a build made with a release key (`keys/release-ed25519.pub`, see
  [Updates](#updates)) installs only a zip that comes with a valid `.sig` from that key, on top of GitHub's SHA-256.
- **Network passwords**: `mpc-hc.ini` and `credentials.ini` carry a generation number; a changed password is
  written as `credentials.ini.new` first and only replaces the old one once the new `mpc-hc.ini` is in place, so a
  failed or cut-off save never loses the old password. `mpc-hc.ini` is synced to the disk before it replaces the old
  one. When two saved addresses disagree, the network location's password wins, then the newest Recent File's.
- **The PS5's video decoder**: each `frame_format` number not yet confirmed on a console is logged with what was
  read from it (Sony's numbers aren't public; see the README's status).
- **Surround**: an 8-channel port the console refuses is asked for again (at most every 10 seconds) instead of only
  after a restart of the sound.

**1.2.1:** fixes from a second review:
- **Updates**: when an old file can't be put back after a failed install, the error says so, `update.journal` keeps
  that file and the app tries again at its next start (telling you, if it still can't, to send the payload again);
  the journal goes only once everything is back. Every folder touched (`sce_sys` too) is synced to the disk.
- **Network passwords**: `credentials.ini` is written first and checked (write, flush, sync, rename); if that fails
  `mpc-hc.ini` isn't replaced, so a password is never lost. It is only rewritten when its passwords change.
- **The PS5's video decoder**: the pictures must keep the first picture's layout and format number, and an 8-bit
  stream must come as 8-bit samples (a 10-bit one as 16-bit); anything else hands the video to FFmpeg.
- The HDR workers are stopped and joined when the app closes; the 8-channel audio port is asked for again after
  the sound is restarted.

**1.2:** fixes from a code review, nothing new to learn:
- **Audio boost** (up to +300%): a loud passage now clips at the 16-bit limits instead of wrapping around to the
  other sign (a loud crackle); the 8-channel output's handle is handed to the sound thread safely.
- **HDR tone mapping**: the five worker threads are made once and kept, instead of five new threads for every
  picture (up to 300 thread starts a second at 60 fps).
- **The PS5's video decoder**: each picture's layout and size are checked against the buffer the player gave the
  decoder before it is read; anything unexpected hands the video to FFmpeg (and `boot.log` says what came out).
- **Network passwords** are no longer kept in `mpc-hc.ini`: they go to `credentials.ini` (see
  [Settings and files](#settings-and-files)), an older `mpc-hc.ini` has them moved out at the first start, and a
  password in an address shows as `***` in every log line.
- **Updates**: only over HTTPS with the certificate checked, and only when GitHub gives the download's SHA-256; a
  file that can't be put in place puts the old version back, and an install cut off half way (the console turned
  off) is put back when the app next starts. See [Updates](#updates).

**1.1.1:** the interface in five languages -- English (the default), Spanish, Portuguese (Brazil), French and
German -- picked in **View > Options > Player > Language** (kept in `mpc-hc.ini`). Debug logs are now off by
default: `debug_logs=1` in `mpc-hc.ini` turns them on.

## Contents

- [Install](#install)
- [The window](#the-window)
- [Controls](#controls)
- [Opening media](#opening-media)
- [Subtitles](#subtitles)
- [Audio](#audio)
- [Hardware decoding, HDR and surround](#hardware-decoding-hdr-and-surround)
- [What plays, and the limits](#what-plays-and-the-limits)
- [Updates](#updates)
- [Settings and files](#settings-and-files)
- [Building](#building)
- [Credits and licences](#credits-and-licences)

## Install

1. Send `MediaPlayerClassicPS5-v1.2.3.elf` to the console's payload loader (port 9021, as with the other ports:
   the PS5 Payload Manager, or `nc <ps5-ip> 9021 < MediaPlayerClassicPS5-v1.2.3.elf`).
2. It installs the app (`/data/homebrew/PPSA99015`, with its icon) and a notification says so. Open
   **Media Player Classic PS5** from the home screen.
3. The payload keeps running as the helper that lets the app read `/data` and USB drives. After a reboot the
   app starts the helper itself (through the ELF loader); if it can't, it says so: send the payload again.

Title ID `PPSA99015`, data folder `/data/mpcps5`. Nothing of the emulator ports (Snes9x, Genesis Plus GX,
FBNeo, Mesen2, NES FCEUmm) is touched.

## The window

MPC-HC 1.7.13 maximized on Windows 10, at 150 % scale (Options > Miscellaneous changes it: 100-200 %):

- **Title bar** -- the file's name and "Media Player Classic Home Cinema"; the restore button toggles full
  screen, the close button exits.
- **Menu bar** -- File, View, Play, Navigate, Favorites, Help, with MPC-HC's items (mpc-hc.rc's own menus).
  What a console can't do (DVD menus, Direct3D options, shut down...) stays there, greyed, as MPC-HC greys
  what doesn't apply.
- **The video** -- MPC-HC's logo while nothing plays, the cover of a music file, the OSD in the corner
  (MPC-HC's colours and font size).
- **Seek bar, toolbar, volume, status bar** -- ported from MPC-HC's painting code (CPlayerSeekBar,
  CPlayerToolBar with toolbar.svg, CVolumeCtrl, CPlayerStatusBar): chapters marked on the seek bar, the time
  under the pointer in a tooltip, the stereo/mono picture in the status bar.
- **View > Information / Statistics / Playlist** -- the information bar, the statistics (frame rate, dropped
  frames, decode and scale times, buffers), the playlist docked at the bottom (the file playing in red).
- **Dialogs** -- Open, Go To, Properties (Details, MediaInfo), Options (Player, Playback, Output, Fullscreen,
  Subtitles, Default Style, Audio Switcher, Miscellaneous), About, Add Favorite, Organize Favorites, and the
  Windows "Open" dialog behind every Browse... button.

| | |
|---|---|
| ![Playing, with subtitles](docs/screenshots/playing.png) | ![The Open dialog](docs/screenshots/browse.png) |
| ![Options](docs/screenshots/options.png) | ![The on-screen keyboard](docs/screenshots/keyboard.png) |

*Screenshots from the host build (the same code on Linux) playing a generated test pattern.*

## Controls

The DualSense is MPC-HC's mouse and keyboard. Move the **left stick** and the pointer appears: then **Cross**
clicks and **Square** right-clicks (the context menu), as with a mouse; it hides after 3 seconds still. Without
the pointer the buttons are keys:

| Button | In the window | In a menu / dialog |
|---|---|---|
| Cross | Play / Pause (click, with the pointer) | Activate / OK (on an edit box: the keyboard) |
| Circle | Leave full screen | Cancel / close |
| Triangle | Full screen on/off | (file browser: up one folder) |
| Square | The context menu (right-click, with the pointer) | -- |
| Options | The menu bar (File...), as Alt | OK |
| D-pad Left / Right | Jump back / forward 5 s | Move between controls, change a value |
| D-pad Up / Down | Volume up / down | Move between controls, lists |
| L2 / R2 (+ Left/Right: 20 s / 1 s) | Jump back / forward 20 s | Page up / down in lists |
| L1 / R1 | Previous / next chapter, then file | Previous / next control |
| L3 | Go To... | -- |
| R3 | Mute (R3 + Square: renderer statistics) | -- |
| Right stick | Volume (the mouse wheel) | Scroll lists |

The menus work as Windows' keyboard menus: Up/Down choose, Right opens a submenu (or the next menu bar menu),
Left goes back, Cross runs the command. The playlist, when it has the focus (a click on it): Up/Down, Cross
plays, Circle gives the focus back.

### The on-screen keyboard

Every edit box (the Open dialog's address, Go To, a network password...) opens Windows' On-Screen Keyboard:
Cross types the key under the highlight, **Square** is backspace, **Triangle** a space, **L2/R2** shift,
**Options** is Done, Circle cancels. The top row types `http://`, `https://`, `smb://`, `www.`, `.com`,
`.br`, `/` and `:` in one press.

## Opening media

- **File > Quick Open File...** -- the "Open" dialog: the navigation pane (This PS5, Media, Internal storage,
  each USB drive, Network), the folder, File name, the types (Media files / All files), Open.
- **File > Open File...** -- MPC-HC's Open dialog: an address (a file or a URL; Recent Files in the list),
  Browse..., "Add to playlist without opening".
- **File > Open Directory...** -- every media file of a folder as the playlist.
- **File > Recent Files**, **Favorites** (Add to Favorites remembers the position).

Where:

- **USB drives** (`/mnt/usb0`..`/mnt/usb7`, exFAT or FAT32) and extended storage (`/mnt/ext0`, `/mnt/ext1`).
- **The console's storage**: `/data/mpcps5/media` (copy files there over FTP), or anywhere in `/data`.
- **Network shares (SMB 2/3)**: in the Open dialog, *Network > Add a network location...*: the PC's name or IP
  address, the share (optional: without it the server's shares are listed), user and password (empty: guest),
  a name. The location stays in the navigation pane. Addresses can also be typed:
  `smb://user:password@192.168.0.10/Videos/Movie.mkv` (once a password is kept, `smb://user@192.168.0.10/...`
  uses it). Windows 10/11 needs the share's user and password
  (guest access is off by default there); a NAS or Samba share may allow guest.
- **The Internet**: `http://` and `https://` (certificates checked against Mozilla's list), HLS (`.m3u8`),
  `rtmp://`, `rtsp://`, `udp://`. Type the address in File > Open File.
- **Playlists**: `.m3u`, `.m3u8`, `.pls`.

**Remember File position** is on (Options > Player): a file opens again where it was left (not near its start
or its end). **After Playback** (Play menu): do nothing, play the next file in the folder, or exit. Navigate >
Previous/Next goes by chapter, then by playlist item, then to the neighbouring file in the folder.

## Subtitles

- Embedded tracks (SRT, ASS/SSA, WebVTT, PGS from Blu-ray, VobSub from DVD, DVB) and files next to the video
  with the same name (`.srt`, `.ass`, `.ssa`, `.vtt`, `.sub`, `Movie.pt-BR.srt`...) are loaded at once; an
  external file wins, as in MPC-HC. File > Subtitles > Load Subtitles... adds another.
- Text files may be UTF-8, UTF-16 or ANSI: ANSI is read as **Windows-1252**, as MPC-HC reads it on a Western
  Windows (accents in Portuguese, Spanish, French... come out right).
- **ASS/SSA** (embedded in MKV, or `.ass`/`.ssa` files) is drawn by **libass**, as authored -- the way MPC-HC's
  own renderer draws it: positions (`\pos`, `\an`, margins), the script's styles, fonts and sizes, colours,
  borders and shadows, rotation, karaoke and the other timed effects, several lines at once, the script's
  PlayResX/Y scaled to the picture as shown (zoom, pan and the aspect ratio included). Fonts: the MKV's
  attached fonts, then the fonts in `/data/mpcps5/fonts`, then the app's own (Selawik, Selawik Bold, Roboto);
  a font the script names and nobody has is drawn in Selawik, and a character the font in use hasn't got comes
  from any of those fonts that has it (the closest in weight and slant; there is no system font lookup on the
  console, so the port patches libass to look through the fonts it has).
- Play > Subtitle Track: the tracks, on/off, the delay (+/- 100 ms; ASS/SSA too). Options > Subtitles / Default
  Style: size, bold, outline, the bottom margin, relative to the video or the window -- for the other formats
  (SRT, WebVTT, MicroDVD...): ASS/SSA keeps its own styles.
- Those other text formats get MPC-HC's default style: bold white text with a black outline and shadow, sized to
  the picture, with line breaks, italics (slanted) and top alignment. PGS, VobSub and DVB pictures are scaled
  with the video.
- Chinese, Japanese, Korean and other scripts: put a font (`.ttf`/`.otf`, for example Noto Sans CJK) in
  `/data/mpcps5/fonts` and start the player again; Selawik and Roboto cover Latin, Greek and Cyrillic (for
  ASS/SSA any of them fills in what the script's font lacks).

## Audio

- Play > Audio Track: the tracks (language and format), as MPC-HC's menu; the stream marked default first.
- Stereo by default: 5.1 and 7.1 are mixed down (libswresample's matrix, centre and surrounds at -3 dB). With
  Options > Output > Audio Renderer on **surround**, they go to the console's 8-channel output instead (see
  below). No bitstream (passthrough) of Dolby/DTS to an AV receiver: they are decoded to PCM.
- Volume: MPC-HC's slider and curve; Options > Audio Switcher: boost (up to +300 %) and the time shift.
- Playback rate (Play > Increase/Decrease Rate, 0.125x-8x) keeps the pitch (FFmpeg's atempo).

## Hardware decoding, HDR and surround

All three are in **Options > Output**, on by default except surround.

![Options > Output](docs/screenshots/output.png)

**Hardware decoding.** H.264 (8-bit 4:2:0) and HEVC (Main and Main 10, up to 3840x2176) go to the PS5's own
video decoder (libSceVideodec2) instead of the CPU, so 4K HEVC 10-bit films play in real time. This follows VLC
for PS5's `ps5vdec` module, which found on the console how to drive it: the library (sysmodule 207) and the
decoder's compute queue are made when the app starts, before it leaves its sandbox (after that both are
refused); the packets go in as Annex B with the parameter sets in front; after a seek decoding goes on from the
next keyframe (HEVC's RASL pictures left out); Dolby Vision's own layers are dropped, so a Dolby Vision film
plays as its HDR10 base layer. Everything else -- AV1, VP9, MPEG-2, VC-1, H.264 High 10 / 4:2:2, interlaced
video (deinterlaced with bwdif) -- stays with FFmpeg, and so does a video the decoder refuses or keeps failing
on (from the next keyframe, without stopping). View > Statistics says which decoder is working ("Decoder:
PS5 video decoder (hardware)" or "FFmpeg (software)"); `boot.log` has the details (`[hwdec]`).

**HDR.** The screen is SDR (the window, the menus and the video in it), so HDR10 (PQ) and HLG pictures are tone
mapped: scaled to the window as 16-bit R'G'B', then turned to linear light (PQ's curve, or HLG's with its
1000-nit OOTF), from BT.2020 to BT.709 colours, the highlights compressed from the film's peak (its MaxCLL or
mastering metadata, 1000 nits when it has neither) to the picture's white, and encoded for the screen
(gamma 2.4). Curves: **BT.2390** (the default), Hable and Reinhard, or Off (the signal as it comes: washed out).
"White" sets how bright the picture is: the nits shown as the screen's white (250 by default, 100-400).
This is done by the CPU on the scaled picture (1080p at most), in six threads (five workers made at the first HDR
picture and kept).

**Surround.** With Audio Renderer on surround, 5.1 and 7.1 tracks go to the console's 8-channel output
(L R C LFE Ls Rs Lb Rb); the console mixes it down itself for a stereo TV. 5.1 fills the first six, 7.1's side
channels go to Ls/Rs and its back channels to Lb/Rb. Stereo tracks stay on the stereo output.

## What plays, and the limits

Every format FFmpeg 7.1.1 decodes: MKV, MP4/MOV, AVI, TS/M2TS, WebM, FLV, OGG, MPEG-PS, WMV/ASF...; H.264,
HEVC (8 and 10 bit), AV1 (dav1d), VP9, VP8, MPEG-2, MPEG-4 ASP (DivX/Xvid), VC-1, ...; AAC, AC-3, E-AC-3,
DTS, TrueHD, FLAC, Opus, Vorbis, MP3, PCM, ...; interlaced video is deinterlaced (bwdif).

Honest limits:

- **The hardware decoder hasn't been tried on a console with this player yet** (it follows VLC for PS5, which
  plays 4K HEVC with it). Without it -- other codecs, or Hardware decoding off -- decoding is done by the CPU
  (FFmpeg's frame threads, AVX2): 1080p plays at full speed, 4K HEVC 10-bit at high bit rates may drop frames
  (View > Statistics shows the decode and scale times; R3 + Square shows them over the video).
- **HDR is tone mapped to SDR**, not sent to the TV as HDR (the screen is drawn in SDR). Dolby Vision profile
  5 (no HDR10 base layer) has wrong colours.
- The output is 1920x1080 at 60 Hz: 24 fps films have the usual 3:2 cadence.
- No DVD or Blu-ray discs or menus (the console's drive isn't readable this way), no DRM (Netflix, Prime...),
  no Chromecast/DLNA server.
- The Save Image command writes PNGs to `/data/mpcps5/screenshots`; Save a Copy, thumbnails and subtitle
  downloads are greyed.

## Updates

**Help > Check for updates** (MPC-HC's menu item), as
[Porpoise](https://github.com/elripalda/Porpoise-Dolphin-Emulator-for-PS5) updates itself:

- It asks GitHub for this repository's newest release
  (`api.github.com/repos/MisterTemaki/MediaPlayerClassicPS5/releases/latest`). With a newer one it says so and asks
  "Download it and install it now?". Yes downloads the release's `MediaPlayerClassicPS5-v<version>-PPSA99015.zip`
  (about 40 MB) with the progress on the screen, installs it, then offers to restart into the new version.
- Once a day, when the app starts, it looks by itself (a notification, and the same question). Nothing is
  installed unless you answer Yes.
- The app does it with FFmpeg's HTTPS (the one the network streams use), always with the certificate checked
  against Mozilla's list (`/data/mpcps5/cacert.pem`, written by the app at start): without that list, or over plain
  HTTP, nothing is fetched. Installing needs the app unlocked by the payload (it writes `/data/homebrew/PPSA99015/`).
- The zip is checked before anything is written: its size and SHA-256 against what GitHub gives (a release
  without GitHub's SHA-256 isn't installed), then every file against the zip's own list
  (`PPSA99015/manifest.sha256`). Each file is written beside the old one; then, with `update.journal` listing them,
  each old file becomes `<name>.old` and the new one takes its place, `eboot.bin` and the manifest last. If a file
  can't be put in place, every old one is put back; if the install is cut off half way (the console turned off),
  the app puts the old version back when it next starts. A file that can't be put back stays in the journal and
  is tried again at the next start (the app says so; sending the payload again repairs everything). Then the `.old` files go, files the new version no longer
  has are removed, and the home screen's copy of the art is refreshed. Settings and media aren't touched.
- **Signed releases.** A build made with a release key (`keys/release-ed25519.pub`, 64 hex digits) installs only
  a zip that comes with `<zip name>.sig`, its Ed25519 signature (RFC 8032) made with that key's private half, on top
  of GitHub's SHA-256: a release served by GitHub but not signed by the project's key isn't installed. Without
  `keys/release-ed25519.pub` the build checks GitHub's SHA-256 only, and its log says so. To set it up, once, on the
  machine that makes the releases:
  ```
  tools/make_release_key.sh ~/.mpcps5/release-ed25519.pem   # the private key (keep it secret, out of the repo)
  git add keys/release-ed25519.pub                          # the public key the app is built with
  make dist RELEASE_SIGNING_KEY=~/.mpcps5/release-ed25519.pem   # signs the release zip: attach the .sig too
  ```
  Losing the private key means the builds with its public key can't update themselves any more (send the payload
  of a new build instead).
- **Your autoload:** the updated app is newer than the ELF in your autoload. The older ELF sees that and leaves the
  app alone (its notification says so). Put the new ELF in your autoload when you can.

For a release, `make dist` builds the zip (`build/dist/MediaPlayerClassicPS5-v<version>-PPSA99015.zip`, the app
folder stored without compression, with its manifest). Attach it to the GitHub release tagged `v<version>`, next to
the ELF; `make UPDATE_REPO=owner/repo` points the app at another repository.

## Settings and files

```
/data/mpcps5/mpc-hc.ini      the settings (MPC-HC's names: RememberFilePos, Volume, Loop, AfterPlayback...),
                             Recent Files, file positions, Favorites, network locations (without passwords)
/data/mpcps5/credentials.ini the passwords of those network addresses (smb://user@server<TAB>password), with the
                             generation of mpc-hc.ini it goes with (credentials.ini.new while a save is half done)
/data/mpcps5/media           a place for media on the console's storage
/data/mpcps5/fonts           extra subtitle fonts
/data/mpcps5/screenshots     File > Save Image
/data/mpcps5/logs/boot.log   the log, only with debug_logs=1 in mpc-hc.ini (boot.prev.log: the run before;
                             installer.log, helper.log)
/data/mpcps5/update-check.txt  when the app last looked for an update
```

Network passwords are kept out of `mpc-hc.ini` (so that file can be shared when asking for help) and out of the
logs (`smb://user:***@server`): they are in `credentials.ini`, readable by its owner only, and only the passwords
of the addresses still saved are kept (removing a network location removes its password). It isn't encryption:
on a jailbroken console anything that can read `/data` can read it. `mpc-hc.ini` files of 1.1.1 and before have
their passwords moved out at the first start.

## Building

On Linux (Ubuntu 24.04 tested), with the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)
(v0.42+) in `/opt/ps5-payload-sdk`, clang/lld 18, nasm, meson, ninja, cmake, git, python3 (Pillow):

```
cd ps5
make deps          # FFmpeg n7.1.1, dav1d 1.5.1, libsmb2, Mbed TLS, zlib for the PS5 and the host (once)
make ps5           # build/ps5/MediaPlayerClassicPS5.elf
make test          # host builds (ASan/UBSan) + tests/run-tests.sh (needs ffmpeg and python3-impacket)
make dist          # build/dist/MediaPlayerClassicPS5-v1.2.3.elf, the release zip, mpc-ps5-v1.2.3-src.zip
make send PS5_HOST=192.168.0.20
```

The layout is the emulator ports': `eboot.bin` is the player, a native title (`player/` + the PS5 layer in
`coreorbis/`, linked by `proto/native`'s tools); the payload is the installer and the helper, with the app
folder inside (the eboot and the backgrounds compressed: the payload is about 12 MB). The host build runs
the same code on Linux (`host/sce_host.cpp` fakes the PS5 calls; `MPCPS5_HOST_SCRIPT` and
`MPCPS5_HOST_PAD` drive it, flips are saved as PPM). `tests/fuzz/fuzz_update.cpp` is a libFuzzer harness for the
updater's parsers (GitHub's JSON, the release zip and its manifest); the command is at its top.

Source map:

| File | What |
|---|---|
| `player/mpc_app.cpp` | the main window (CMainFrame): layout, painting, commands, input |
| `player/mpc_player.cpp` | the playback engine: FFmpeg demux/decode threads, A/V clock, subtitles |
| `player/mpc_vdec.cpp` | the PS5's video decoder (libSceVideodec2), after VLC for PS5's ps5vdec; `stubs/videodec2_stub.c` is its link stub |
| `player/mpc_tonemap.cpp` | HDR10 / HLG tone mapping (BT.2390, Hable, Reinhard) |
| `player/mpc_ui.cpp` | the window system: Windows 10's controls, dialogs, message boxes, the on-screen keyboard, the pad as mouse and keyboard |
| `player/mpc_menu.cpp` | themed menus and their tracking |
| `player/mpc_dialogs.cpp` | MPC-HC's dialogs (from mpc-hc.rc's templates) and the Open dialog |
| `player/mpc_gdi.cpp`, `mpc_font.cpp`, `mpc_res.cpp` | a small GDI, fonts (stb_truetype), MPC-HC's resources |
| `player/mpc_smb.cpp`, `mpc_console.cpp` | SMB through libsmb2; the console glue (thread stacks, name lookups) |
| `player/mpc_settings.cpp` | mpc-hc.ini, and credentials.ini (the network passwords) |

## Credits and licences

The port is **GPLv3** (`LICENSE`), as MPC-HC.

- **MPC-HC** 1.7.13 -- the interface, its painting code, menus, dialog templates and resources (toolbar.svg,
  the logo, the icon, the status pictures); GPLv3; (C) 2002-2017 the MPC-HC Team (Authors.txt), Gabest.
  <https://github.com/mpc-hc/mpc-hc>
- **FFmpeg** 7.1.1 -- the playback engine (LAV Filters' engine on Windows); built as GPLv3.
  <https://ffmpeg.org>
- **dav1d** 1.5.1 -- AV1; BSD-2-Clause; VideoLAN.
- **libsmb2** -- SMB2/3; LGPL-2.1; Ronnie Sahlberg. VLC reads SMB shares with it too.
- **VLC** -- the reference for the network side (SMB through libsmb2, streams through FFmpeg's protocols) and
  for the ANSI subtitle fallback.
- **VLC for PS5** ([ZiZc3/VLC](https://github.com/ZiZc3/VLC)) -- `player/mpc_vdec.cpp` is its `ps5vdec` module
  (the console's video decoder: its structures, memory, access-unit and picture handling, recovery rules)
  brought over to FFmpeg's packets and frames, GPL-2.0-or-later; and the idea of the 8-channel output and its
  channel order.
- **libass** 0.17.5 -- ASS/SSA subtitles; ISC; the libass authors (<https://github.com/libass/libass>). With
  **FreeType** 2.14.3 (dual FreeType Project Licence / GPLv2, used here under the FTL, which goes with GPLv3:
  portions of this software are copyright (C) The FreeType Project, www.freetype.org; all rights reserved), **HarfBuzz** 14.5.1 (MIT,
  the core only: no glib, ICU or cairo) and **FriBidi** 1.0.16 (LGPL-2.1), all static, built without fontconfig
  by `tools/build-deps.sh` (`tools/build-deps.sh subs` -- `make deps-subs` -- builds only these four).
- **Mbed TLS** 3.6.7 (Apache-2.0), **zlib**, **Mozilla's CA list** (MPL-2.0, via certifi).
- **Monocypher** 4.0.2 -- Ed25519 for the release signatures; BSD-2-Clause or CC0; Loup Vaillant and contributors.
  <https://github.com/LoupVaillant/Monocypher>
- **Selawik** (SIL OFL 1.1, Microsoft) -- the stand-in for Segoe UI; **Roboto** (Apache-2.0).
- **NanoSVG** (zlib licence), **stb_truetype / stb_image** (public domain / MIT).
- **PS5SX2** -- the PS5 layer (video out, sound, pad, crash log, installer, helper, native app tooling); MIT.
- **ps5-payload-dev SDK** -- John Tornblom.
