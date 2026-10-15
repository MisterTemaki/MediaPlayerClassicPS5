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

> **Status (1.1):** builds with the ps5-payload-dev SDK into a signed native app and passes 117 host tests
> (the same code on Linux, the PS5 calls faked: playback, subtitles, chapters, tracks, HTTP, SMB, the pointer,
> the dialogs, the on-screen keyboard, the installer and the helper; the PS5's video decoder through a stand-in,
> HDR tone mapping, 5.1/7.1 output). It has **not been run on a PS5 yet** -- please report what works and what
> doesn't (the log is in `/data/mpcps5/logs/boot.log`).

**1.1:** what VLC for PS5 ([ZiZc3/VLC](https://github.com/ZiZc3/VLC)) showed can be done on the console:
**hardware decoding** of H.264 and HEVC (8 and 10-bit, up to 4K) on the PS5's video decoder, **HDR10 and HLG
tone mapping** to the SDR picture, and **5.1 / 7.1 output** on the console's 8-channel port. See
[Hardware decoding, HDR and surround](#hardware-decoding-hdr-and-surround).

## Contents

- [Install](#install)
- [The window](#the-window)
- [Controls](#controls)
- [Opening media](#opening-media)
- [Subtitles](#subtitles)
- [Audio](#audio)
- [Hardware decoding, HDR and surround](#hardware-decoding-hdr-and-surround)
- [What plays, and the limits](#what-plays-and-the-limits)
- [Settings and files](#settings-and-files)
- [Building](#building)
- [Credits and licences](#credits-and-licences)

## Install

1. Send `MediaPlayerClassicPS5-v1.1.elf` to the console's payload loader (port 9021, as with the other ports:
   the PS5 Payload Manager, or `nc <ps5-ip> 9021 < MediaPlayerClassicPS5-v1.1.elf`).
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
  `smb://user:password@192.168.0.10/Videos/Movie.mkv`. Windows 10/11 needs the share's user and password
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
- Play > Subtitle Track: the tracks, on/off, the delay (+/- 100 ms). Options > Subtitles / Default Style:
  size, bold, outline, the bottom margin, relative to the video or the window.
- MPC-HC's default style: bold white text with a black outline and shadow, sized to the picture. ASS styling
  (fonts, colours, positions, karaoke) is not drawn: the text is, with line breaks, italics (slanted) and top
  alignment.
- Chinese, Japanese, Korean and other scripts: put a font (`.ttf`/`.otf`, for example Noto Sans CJK) in
  `/data/mpcps5/fonts` and start the player again; Selawik and Roboto cover Latin, Greek and Cyrillic.

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
This is done by the CPU on the scaled picture (1080p at most), in six threads.

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

## Settings and files

```
/data/mpcps5/mpc-hc.ini      the settings (MPC-HC's names: RememberFilePos, Volume, Loop, AfterPlayback...),
                             Recent Files, file positions, Favorites, network locations
/data/mpcps5/media           a place for media on the console's storage
/data/mpcps5/fonts           extra subtitle fonts
/data/mpcps5/screenshots     File > Save Image
/data/mpcps5/logs/boot.log   the log (boot.prev.log: the run before; installer.log, helper.log)
```

Network passwords are kept in `mpc-hc.ini` as part of the share's address (as Windows' saved network
credentials, but in plain text): don't share that file.

## Building

On Linux (Ubuntu 24.04 tested), with the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)
(v0.42+) in `/opt/ps5-payload-sdk`, clang/lld 18, nasm, meson, ninja, cmake, git, python3 (Pillow):

```
cd ps5
make deps          # FFmpeg n7.1.1, dav1d 1.5.1, libsmb2, Mbed TLS, zlib for the PS5 and the host (once)
make ps5           # build/ps5/MediaPlayerClassicPS5.elf
make test          # host builds (ASan/UBSan) + tests/run-tests.sh (needs ffmpeg and python3-impacket)
make dist          # build/dist/MediaPlayerClassicPS5-v1.1.elf + mpc-ps5-v1.1-src.zip
make send PS5_HOST=192.168.0.20
```

The layout is the emulator ports': `eboot.bin` is the player, a native title (`player/` + the PS5 layer in
`coreorbis/`, linked by `proto/native`'s tools); the payload is the installer and the helper, with the app
folder inside (the eboot and the backgrounds compressed: the payload is about 12 MB). The host build runs
the same code on Linux (`host/sce_host.cpp` fakes the PS5 calls; `MPCPS5_HOST_SCRIPT` and
`MPCPS5_HOST_PAD` drive it, flips are saved as PPM).

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
| `player/mpc_settings.cpp` | mpc-hc.ini |

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
- **Mbed TLS** 3.6.7 (Apache-2.0), **zlib**, **Mozilla's CA list** (MPL-2.0, via certifi).
- **Selawik** (SIL OFL 1.1, Microsoft) -- the stand-in for Segoe UI; **Roboto** (Apache-2.0).
- **NanoSVG** (zlib licence), **stb_truetype / stb_image** (public domain / MIT).
- **PS5SX2** -- the PS5 layer (video out, sound, pad, crash log, installer, helper, native app tooling); MIT.
- **ps5-payload-dev SDK** -- John Tornblom.
