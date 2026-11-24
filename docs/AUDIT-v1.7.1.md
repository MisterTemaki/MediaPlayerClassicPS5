# Media Player Classic PS5 1.7.1: audit report

The audit behind 1.7.1: what was read, what was found, what was fixed and how each fix was checked, and what is
left. It started from issue #11 (a screen showing only the app's logo) and covered the whole app.

- Audited code: 1.7.0, commit `493c5da` (the published release).
- Fixed in: branch `mpc-171`, the commits listed in [Files changed](#7-files-changed).
- Method: five independent read-only audits of the 1.7.0 tree, each with its own scope and a coverage table. Every
  finding was then checked against the code before it was fixed. Each fix has a host test, run before and after the
  change where that was possible, and the whole suite runs again at the end.
- What this is not: nothing here ran on a PS5 yet. The console test of 1.7.1 comes before the release, and its
  results go into the release notes.

## 1. Inventory and architecture

| Part | What it is | Main files |
|---|---|---|
| Boot | Process start, jailbreak request to the helper, paths, log, video, audio, pad, then the app | `coreorbis/main-boot.cpp` |
| PS5 layer | Video out (GPU or CPU scan-out), HDR, audio ring, pad, toasts, crash handler, updater, installer, helper | `coreorbis/orbis-shims/*` |
| GPU library | Vulkan (RADV) renderer: window layer, video pass, HDR output, post-processing (vendored, MPC patches) | `ps5-slang-gpu/` |
| Player | FFmpeg demuxer, decoders, console decoder (`mpc_vdec`), clock, audio filter, subtitles (libass), tone mapping | `player/mpc_player.cpp`, `mpc_vdec.cpp`, `mpc_tonemap.cpp`, `mpc_ass.cpp` |
| Window | MPC-HC's window, menus, dialogs, file browser, OSD, settings, translations | `player/mpc_app.cpp`, `mpc_ui.cpp`, `mpc_dialogs.cpp`, `mpc_menu.cpp`, `mpc_font.cpp`, `mpc_settings.cpp`, `mpc_i18n.cpp` |
| Sources | SMB (libsmb2), discs (ISO 9660, UDF, DVD, Blu-ray), HTTP/HTTPS (FFmpeg) | `player/mpc_smb.cpp`, `mpc_disc.cpp` |
| Storage | Settings, encrypted network passwords, backups | `player/mpc_settings.cpp`, `mpc_credentials.cpp`, `mpc_backup.cpp` |
| Runtime | The PS5 C runtime layer (stdio wrap, memstream, locale, emutls), thread stacks, DNS fallback | `mitrt/`, `player/mpc_console.cpp` |
| Tests | Host build of the real code with simulated PS5 calls (ASan/UBSan), harnesses, the GPU library's tests | `tests/`, `host/`, `ps5-slang-gpu/tests/` |

Threads: one UI thread (input, Tick, paint, present, Open and Close); per file an opener, a demuxer, a video and an
audio thread; the decoder's copy helpers, the tone-mapping workers, the update worker, the backup thread, the
notification worker, and (1.7.1) the SMB listing threads and the GPU start watch.

## 2. Baseline

The 1.7.0 release's own checks, as published: full frontend suite 941 passed and 12 failed. The 12 are known and
fail the same way on 1.6.4: an SMB address check, and 11 comparisons of the GPU's 4K HDR pictures with the CPU
path's on the host's software Vulkan (Mesa lavapipe). The harnesses (credentials, theme, update, log, installer,
GPU start, the GPU library) passed.

During the audit, `make test` showed one more failure that the 1.7.1 work itself had caused: the GPU start harness
didn't link after the issue #11 changes (the start watch calls `ProsperoNotify`). It is fixed in `d67a7c8`.

## 3. Coverage

| Area | Files | State |
|---|---|---|
| Boot, video, GPU start | `main-boot.cpp`, `ProsperoVideo.cpp`, `ProsperoCrash.cpp`, `orbis_paths.cpp`, `ProsperoHdr.cpp`, `ProsperoInput.cpp`, `ProsperoAudio.cpp`, `ProsperoNotify.cpp`, `ProsperoThread.h` | Analyzed |
| GPU library | `ps5-slang-gpu/src/gpu.cpp` (Init, Shutdown, Present, swapchain, HDR, video, post-processing, waits), `common.cpp`, headers, the shaders MPC uses | Partially: the shelf (scene) code and the slang preset parser, which MPC never reaches, were only skimmed; `sha256.cpp` and the third-party presets were not read |
| Player | `mpc_player.cpp`, `mpc_vdec.cpp`, `mpc_bigmem.cpp`, `mpc_gpu.cpp`, `mpc_tonemap.cpp`, `mpc_ass.cpp` | Analyzed |
| Window | `mpc_app.cpp`, `mpc_ui.cpp`, `mpc_dialogs.cpp`, `mpc_menu.cpp`, `mpc_gdi.cpp`, `mpc_font.cpp`, `mpc_sfnt.cpp`, `mpc_res.cpp`, `mpc_settings.cpp` | Analyzed |
| Translations | `mpc_i18n.cpp` | Code analyzed; the table checked by script (formats, keys, accelerators, fields); the wording not reviewed |
| Network and storage | `mpc_smb.cpp`, `mpc_credentials.cpp`, `mpc_credentials_ui.cpp`, `mpc_backup.cpp`, `mpc_update.cpp`, `mpc_disc.cpp`, `mpc_console.cpp` | Analyzed (`mpc_settings.cpp`'s backup import, `LoadFile`, only partially) |
| Updater, installer, helper | `ProsperoUpdate.cpp`, `ProsperoInstall.cpp`, `ProsperoHome.cpp`, `ProsperoAppPlace*.cpp`, `ProsperoHelper.cpp`, `ProsperoJailbreak.cpp`, `installer_main.cpp` | Analyzed |
| Build and release tools | `Makefile`, `tools/build-deps.sh`, `make_release_zip.py`, `sign_release.sh`, `make_release_key.sh`, the check tools | Analyzed (the check tools lightly); `tools/patches/*` (third-party library patches) not analyzed |
| PS5 runtime | `mitrt/src`, `mitrt/platform/src`, emutls | Analyzed; compiler-rt's unchanged files, the RADV build tooling and the libc.prx tooling (MPC doesn't use it) not analyzed |
| Tests | `tests/run-tests.sh` | Partially: helpers and about half the groups read line by line, the rest searched for patterns (sleeps, timing bounds, defaults, negative checks) |
| Tests | the C++ harnesses, the Python helpers, the GPU library's runner | Analyzed; media generators and `gpu_bench.cpp` not analyzed |
| Host stand-ins | `host/sce_host.cpp` | Analyzed; `host/sce_host_vdec.cpp` partially |
| Third-party libraries | FFmpeg, libsmb2, libass, libbluray, mbedtls... | Not audited; libsmb2's and FFmpeg's sources were read where a finding depended on them (timeouts, TLS checks) |

## 4. Findings

Severity is the auditor's, after triage. State: **Fixed** (with the test that checks it), **Mitigated** (part of it
fixed, the rest in [residual risks](#10-residual-risks)), **Open** (not changed in 1.7.1), **Not a defect**.

### 4.1 Issue #11

| ID | Severity | Finding | State | Check |
|---|---|---|---|---|
| C2 | medium | Full screen (Triangle) was saved as "Launch files in fullscreen" (`FullScreen=`): after full screen on a film, the app started in full screen, nothing open: the logo alone | Fixed: the window's state isn't saved; `LaunchFullScreen=` opens a film in full screen, as MPC-HC | group 41 a, b, b2; group 32 |
| C3 | medium | Screens with the logo alone and nothing saying how to reach the menu (Minimal, full screen with music) | Fixed: the OSD names the menu's button (at the start too); the bars stay for a file without a picture | group 41 c, c2, c3, d, d2 |
| -- | -- | A start that never reached the screen left nothing to read (Debug logs off by default) | Fixed: `logs/start-pending`, then the next start writes boot.log; the GPU start watch (a toast after 20 s) | group 41 e, f, f2 |

### 4.2 Startup, video and the GPU (audit A)

| ID | Severity | Certainty | Location | Finding | State | Check |
|---|---|---|---|---|---|---|
| A1 | high | confirmed | `ProsperoVideo.cpp`: Present | The start guard ended at Init's own black present, before any frame was shown: a driver that hangs or dies in its first frames was tried again at every start | Fixed: the guard ends after 8 presents (or hands over to the in-place guard at the first import of the decoder's buffers); the watch covers the 8 | group 41 g, g2, g3; group 34 k |
| A2 | medium | confirmed | `gpu.cpp`: fences, acquire, WaitOneFlip | Every GPU wait is unbounded | Mitigated: the watch speaks during the start (20 s), the next start uses the CPU | group 41 g2 |
| A3 | medium | confirmed | `ProsperoVideo.cpp`: three failed presents | A GPU lost while running was saved as a failed start: every later start on the CPU, "a previous GPU start failed" | Fixed: after a start that worked, the next start tries the GPU again | group 41 h, h2; group 34 l |
| A4 | low | confirmed | `gpu.cpp`: Present | A failure after a successful acquire leaks the image and leaves the semaphore signalled | Open | -- |
| A5 | low | confirmed | HDR switching | Details of the HDR mode switch | Open | -- |
| A6 | low | confirmed | `host/` | The GPU-to-CPU scan-out hand-over isn't modelled on the host | Open (test gap) | -- |
| A7 | low | confirmed | `main-boot.cpp` | "Video failed to start" pointed to a boot.log that isn't written by default | Fixed | group 41 i |
| A8 | low | confirmed | `gpu.cpp`: MakeTarget | Surface capabilities read uninitialised on failure; render pass and framebuffers unchecked | Fixed (vendor patch) | code review; host display path not reachable |
| A9 | low | confirmed | `gpu.cpp`: UploadImage | The upload's fence result ignored; a lost device reported as wrong pictures | Fixed: checked, said as a lost device; the start guard and three failed presents still decide the fallback | code review |
| A10 | low | confirmed | `ui.slang` | The window layer is always nearest-sampled (cosmetic, at 1440p / 720p) | Open | -- |
| A11 | low | hypothesis | `gpu.cpp`: SavePipelineCache | The pipeline cache renamed without fsync | Fixed (vendor patch) | code review |

### 4.3 The player (audit B)

| ID | Severity | Certainty | Finding | State | Check |
|---|---|---|---|---|---|
| B1 | medium | confirmed path | Close or the next file waited for SMB requests that ignore the quit flag (15 s each); 11 SMB connections per film for its subtitles | Fixed: SMB requests are cancellable (below); one listing for the subtitles | group 42 a, d |
| B2 | medium | probable | The console decoder cut pictures to the size at the open after a size change | Fixed: FFmpeg takes the video from the next keyframe | group 42 e |
| B3 | medium | confirmed | "Ended" came before the last ~0.3 s were played (ring, queued pictures, filter tail) | Fixed | group 42 f |
| B4 | low | probable | The UI thread reads the stream list and tags while the demuxer can change them (chained Ogg, MPEG-TS) | Open | -- |
| B5 | low | probable | An audio track chosen while the file opens raced the opener (std::terminate) | Fixed (the race itself is rare: 1.7.0 didn't crash in the test) | group 42 g |
| B6 | low | probable | Timestamp jumps are never re-synchronised | Open | -- |
| B7 | low | probable | Mixed interlaced streams through the deinterlacer | Open | -- |
| B8 | low | confirmed (code) | A huge subtitle duration converted to uint32 (undefined) | Fixed, defensively: FFmpeg's subtitle decoders set the line's end themselves, so the conversion wasn't reached in the test | group 42 h checks no sanitizer report only |
| B9 | low | confirmed | The queues' byte counts read unlocked | Fixed (atomic) | build; suite |
| B10 | low | confirmed | A failed packet copy queued as the end mark | Fixed | code review (out of memory only) |
| B11 | low | hypothesis | A packet lost when the decoder answers EAGAIN | Open (unconfirmed) | -- |
| B12 | low | confirmed | File > Load Subtitle reads the whole file on the UI thread | Mitigated: 10 s at most from a share | -- |
| B13 | low | hypothesis | A demuxer error on rtsp:// retried forever | Open (unconfirmed) | -- |

### 4.4 The window (audit C)

| ID | Severity | Certainty | Finding | State | Check |
|---|---|---|---|---|---|
| C1 | high | confirmed | SMB listings and reads on the UI thread froze the app (file browser, Next/Previous, Open Directory, Play next) | Fixed: on their own threads, cancellable | group 42 b, c, c2 |
| C2, C3 | medium | confirmed | Issue #11 | Fixed | 4.1 |
| C4 | medium | confirmed | The file position was lost when the app was closed from the home screen | Fixed: written each minute while playing and on Pause | group 43 a |
| C5 | medium | probable | A subtitle cue of thousands of lines made a picture of gigabytes | Fixed: the lines that fit, no wider than the screen, bad_alloc caught | group 43 c |
| C6 | medium | confirmed | A failed settings save said nothing (an unreadable old credentials.ini blocks every save) | Fixed: said once, with the cause | group 43 b |
| C7 | medium | confirmed | Menus taller than the screen ran off it | Fixed: they scroll | group 43 d (screenshot checked) |
| C8 | low | confirmed (arithmetic) | At 200 %, the on-screen keyboard and the file browser are larger than the screen | Open | -- |
| C9 | low | confirmed | Opening a favorite skipped Open()'s resets (rate, playlist selection) | Fixed | code review |
| C10 | low | confirmed | Unbounded lists, favorite ids colliding with discs and titles, long lines split, atoi overflow | Mitigated: lists capped at load; line length and atoi left | code review |
| C11 | low | probable | FitText is quadratic (the browser's first frames cost about 1 s on the host with sanitizers) | Open | measured in group 42 b |
| C12 | low | confirmed | Text layout edge cases (long words, UTF-8 trimming, password stars per byte) | Open | -- |
| C13 | low | confirmed path | Events reach a closed dialog until the next Tick | Open | -- |
| C14 | low | confirmed (UB) | Times not checked before integer conversions | Fixed (Go To, Seek, FormatTime); chapter tables bounded by FormatTime | code review |
| C15 | low | confirmed | smb:// addresses cut at the first '@' when shown | Fixed | group 43 e |
| C16 | low | confirmed | Data (favorites, recent files) passed through the translation lookup | Fixed for those menus; `T()`'s unlocked read of the language left (benign) | code review |

### 4.5 Network, storage, update, install (audit D)

| ID | Severity | Certainty | Finding | State | Check |
|---|---|---|---|---|---|
| D1 | medium | confirmed | Close while a share stops answering froze the UI | Fixed (with B1) | group 42 a |
| D2 | medium | confirmed | Blocking SMB I/O on the UI thread | Fixed (with C1); the DNS lookup and File > Load Subtitle from a disc remain bounded waits | group 42 b, c |
| D3 | medium | confirmed | No release signing key in the tree: updates are checked by GitHub's SHA-256 only | Open: the maintainer's decision (see 10) | -- |
| D4 | low-medium | confirmed | The updater rewrote the home screen's art in place, deleted it on a failed write | Fixed: written beside it, then renamed | group 16 c |
| D5 | low | confirmed | "Nothing was changed" added to errors that ask for a repair | Fixed | group 16 b5 |
| D6 | low | confirmed | A failed build step left its output for the next make | Fixed (`.DELETE_ON_ERROR`) | -- |
| D7 | low | confirmed | `make dist` zips the working tree | Open (releases are made with `git archive`) | -- |
| D8 | low | confirmed | SMB names with "%XX" couldn't be opened | Fixed ('%' as %25) | group 42 c2 |
| D9 | low | confirmed | Check for updates during the start's check said nothing | Fixed | code review |
| D10 | low | confirmed | A file descriptor leaked when fsync failed | Fixed | code review |
| D11 | low | hypothesis | An app folder on a drive that refuses fsync can't be updated | Open (unconfirmed) | -- |
| D12 | low | confirmed | The helper's 2 s deadline also covers finding the app's place | Open | -- |
| D13 | low | hypothesis | The update download has no hard memory cap | Open | -- |
| D14 | low | confirmed, latent | DVD image reads return a partial count without moving | Open (no caller) | -- |
| D15 | low | confirmed | The updater's HTTPS path is never tested | Open (test gap) | -- |

### 4.6 Runtime and tests (audit E)

| ID | Severity | Certainty | Finding | State | Check |
|---|---|---|---|---|---|
| E1 | medium | probable | The DNS fallback holds one lock across the console's resolver (5 s, 2 retries) | Mitigated: no SMB lookup runs on the UI thread now; the serialisation stays | -- |
| E2 | medium | confirmed | Sanitizer reports of earlier runs in a test folder went unchecked | Fixed: `nosan` reads every run | full suite |
| E3 | medium | confirmed | `make test` didn't run the runtime layer's own tests | Fixed (`test-mitrt`) | `make test` |
| E4 | low | confirmed | The DNS fallback answered for any failure, a service name as port 0 | Fixed: not for a bad service, socket type, family, flags or no memory (review R4), and a numeric port | PS5 build only (compiled out on the host) |
| E5 | low | hypothesis | The pthread wrap changes the caller's attribute object | Open (no caller) | -- |
| E6-E9 | low | confirmed | Test timing, missing timeouts, checks that pass without data, dead steps | Open (test quality) | -- |
| E10, E11 | low | confirmed | TLS, fuzzers and some library modes not wired into tests | Open (test gaps) | -- |
| E12-E15 | low | mixed | Link guards, a host-dependent mitrt test, a memstream lock-order hypothesis, POSIX details | Open | -- |

### 4.7 The review of the fixes

An independent review of the 1.7.1 changes (read-only, against the pinned libsmb2 and FFmpeg) found no HIGH issue;
it confirmed the SMB request loop (no use-after-free or double callback in the paths traced) and the GPU start guard.
Its findings and what was done:

| ID | Severity | Finding | State |
|---|---|---|---|
| R1 | medium | libsmb2 keeps its contexts in a global list without a lock; 1.7.1 makes and destroys contexts on several threads | Fixed: one lock around `smb2_init_context` / `smb2_destroy_context` |
| R2 | medium | A folder still being read could be applied late (Open Directory after another file opened; Play next after the film was played again); a second Open Directory dropped | Fixed: dropped by Open and Close; the newest request wins; Play next checks the end still stands |
| R3 | medium | The B8 test couldn't fail: GCC's UBSan leaves out float-cast-overflow | The host build adds float-cast-overflow; the test's description corrected (see B8) |
| R4 | low | The E4 change sent fewer failures to the console's resolver; which code the system's libc gives in a title wasn't recorded | Changed to a deny list: every failure but a bad service, socket type, family, flags or no memory still goes to it |
| R5 | low | UI-thread joins can wait on a DNS lookup; a `.sup` from a share read on the UI thread had no limit | The `.sup` read: 20 s at most (`smb::DeadlineScope`); the DNS wait is a residual risk (E1) |
| R6 | low | The SMB backstop timed whole requests from their start | 30 s from the socket's last activity |
| R7 | low | libsmb2's fstat callback dereferences a reply a timeout or a destroy doesn't carry | No fstat after the open (its end-of-file is the size) |
| R8 | low | An address with a raw '%' kept by 1.7.0 didn't match the escaped listing | Next / Previous / Play next compare the decoded name; a remembered position under the old spelling isn't carried over |
| R9 | low | The subtitle search gave up when the folder can't be listed | Each name asked then, as 1.7.0 |
| R10 | low | Missing `nosan` checks; fixed timings in 41g3, 42a, 42c, 43a | Fixed |
| R11 | low | The position saved each minute does an fsync on the UI thread | Left: to watch on the console |

The full suite then showed two failures in group 34 k. The 8-present guard caught a crash in the first import of the
decoder's buffers, which the in-place guard is made for (the next start went to the CPU instead of copying the
pictures on the GPU): the GPU start now hands over to the in-place guard at that import. And the test read the
state line at the end of a 2 s film, which B3 now plays to its end (its last pictures are copies): it counts the
pictures read in place at the decoder's close.

## 5. The main fixes

**SMB requests that can be stopped (B1, C1, D1, D2).** libsmb2's sync calls wait in a loop of their own (a poll of a
second at a time, up to the 15 s timeout of each request) that nothing outside can stop. Every request now goes
through `smb::Run`: libsmb2's async call and the same loop polled every 0.1 s, which gives up as soon as the calling
thread's cancel flag is set (`smb::CancelScope`: the player's quit, the file browser's Cancel) or its deadline
passes. A request given up stays with libsmb2: its record is freed by its callback when that comes, from the
context's destroy, and a connection with a request given up is only destroyed, never used again. On top of that the
file browser, Next / Previous, Open Directory and "Play next file" list a share on threads of their own, and the
player looks for a film's subtitles with one listing instead of 11 connections.

**The GPU start guard (A1, A3).** The guard (a marker on disk while the driver is set up) ended at the first
successful present, which is Init's own black screen and only queues a frame. It now ends after 8 presents, so a
hang or crash in the first frames leaves the next start on the CPU. A GPU lost after a start that worked no longer
keeps every later start on the CPU.

**The end of a file (B3).** "Ended" is set once the queued pictures are shown and the console's ring is played (2 s
at most), and the audio filter's tail is flushed.

## 6. Evidence

The new tests (groups 41, 42, 43) were run against two other builds with the 1.7.1 test script (`MPC_HOST_BIN`):

- **The 1.7.0 host binary** (built from `493c5da` with the same sanitizers): 31 of the 67 checks fail, 36 pass (the
  negative checks and the `nosan` lines). For example: with the share stalled, Close came back after 26.15 s (1.7.1:
  1.07 s); the file browser at a share that doesn't answer took 18.30 s for 60 frames, Next on a stalled share 17.02 s
  (1.7.1: about 1 s each); a 2 s clip ended paused at 1.73 s (1.7.1: 2.00 s, its last picture shown); the app killed
  while playing had no position written (1.7.1: about 6 s); the issue #11 checks, the menu hint, start-pending, the
  scrolling menu, the save failure message, the subtitle cue cap and the '@' address all fail. Two checks pass on
  1.7.0 as well and don't prove their fix: B8 (see 4.3) and the B5 race (1.7.0 didn't crash in that run).
- **1.7.1 with A1 and A3 taken out** (the guard ended at the first present, a GPU lost while running keeping
  `attempts 4`): exactly the five A1 / A3 checks fail (41 g, g2, h), the other 33 pass.

## 7. Files changed

| Commit | Content |
|---|---|
| `fa539bb` | Issue #11: full screen not saved, LaunchFullScreen, the menu hint, start-pending, the GPU start watch |
| `1c7aa1c` | 1.7.1; A1, A3, A7 |
| `8f0c80c` | B1, C1, D1, D2, D8: SMB |
| `e4c5fc8` | B2, B3, B5, B8, B10 |
| `9117fb1` | C4, C5, C6, C7 |
| `5c50e09` | C10, C14, C15, C16 |
| `bce9cec` | A8, A9, D4, D5, D9, D10, E2, E3, E4 |
| `d67a7c8` | A11, B9, C9, D6; the GPU start harness |
| `b7ebffe` | The review's R1-R9 |
| `084d6f0` | The GPU start guard hands over to the in-place guard |
| `11177fb`, `1e76553`, `0ff04d1` | The review's R10; group 34 k; group 42 h's description |

Source files: `coreorbis/main-boot.cpp`, `coreorbis/include-orbis/OrbisPaths.h`, `coreorbis/orbis-shims/orbis_paths.cpp`,
`ProsperoVideo.cpp`, `ProsperoUpdate.cpp`, `ProsperoHome.cpp`; `player/mpc_app.cpp`, `mpc_dialogs.cpp`, `mpc_smb.cpp`,
`mpc_smb.h`, `mpc_player.cpp`, `mpc_vdec.cpp`, `mpc_menu.cpp`, `mpc_font.cpp`, `mpc_settings.cpp`, `mpc_settings.h`,
`mpc_console.cpp`, `mpc_i18n.cpp`, `mpc_gpu.cpp`; `coreorbis/orbis-shims/ProsperoVideo.h`; `ps5-slang-gpu/src/gpu.cpp` and `VENDOR.txt` (patch in
[patches/gpu-audit-171.diff](patches/gpu-audit-171.diff)); `Makefile`, `app/sce_sys/param.json`.
Tests: `tests/run-tests.sh` (groups 41, 42, 43, and 16, 32, 34), `tests/smb_stall.py` (new),
`tests/gpu_startup_test.cpp`.

## 8. Checks run

On the code of the first test build (`0ff04d1`), the host's GCC 13 with ASan, UBSan and float-cast-overflow (and
again on the second, `a49b4de`, with issue #12's change and its group 44: 1025 passed, the same 12 failed):

- **`make test`:** the frontend suite **1010 passed, 12 failed**. The 12 are the baseline's (an SMB address check and
  11 comparisons of the GPU's HDR pictures with the CPU path's on lavapipe), failing with the same values as on 1.7.0.
  New groups: 41 (38 checks), 42 (20), 43 (9). Every run's output is checked for sanitizer reports (E2): none.
- **Harnesses in `make test`:** update transaction, log password, install content, GPU start (14 checks; it didn't link
  before `d67a7c8`), decoder memory, console HDR, theme (43), credentials (36), credentials UI (31), and the runtime
  layer's own host and platform tests (new in `make test`, E3): all pass.
- **Fail-before:** section 6 (the 1.7.0 binary: 31 of 67 new checks fail; 1.7.1 without A1 and A3: those 5 fail).
- **Translations:** 762 texts (765 with #12), five languages, no format or key problem (`tests/check_i18n.py`).
- **Vendor check:** `ps5-slang-gpu` matches its VENDOR.txt (420 files).
- **PS5 build** (`make ps5`, the native GPU build and the complete installer): the app profile (content version
  01.071.000), imports, syscalls, the runtime's link check and the RADV link (338 Vulkan entry points) pass.
  `MediaPlayerClassicPS5.elf`: 21,485,712 bytes, SHA-256 `936525c173034f0ca83e446effc4e040db136934ec38c12e429aaa3a33b7db57`.
- **On screen** (host screenshots): the file browser at a share that doesn't answer ("Reading the network
  folder..."), the Minimal preset's "OPTIONS: the menu", the save failure message, a menu of 60 favorites scrolled to
  its last item.
- **One independent review** of the changes (4.7).
- **Not run:** anything on a PS5 (the console test comes before the release); the PS5-only DNS fallback (E4) on a
  console; TSan.

## 9. Findings not confirmed

B11 (a packet lost on EAGAIN), B13 (rtsp:// retried forever), D11 (a drive that refuses fsync), D13 (the download's
memory), E5 (the pthread wrap), E14 (memstream lock order) and A11's real effect are hypotheses: none was reproduced,
and none was changed except A11 (a fsync, harmless either way).

## 10. Residual risks

- **No release signature (D3).** The updater checks each file against GitHub's SHA-256 and the release's zip
  against GitHub's digest, over TLS with the certificate and host name checked. The Ed25519 signature check exists in
  the code but needs a key: `keys/release-ed25519.pub` in the build (made with `tools/make_release_key.sh`, the private
  key kept by the maintainer, never in the repository) and a `.sig` asset in each release. That is the maintainer's
  decision; nothing in 1.7.1 changes it.
- **GPU waits (A2).** A driver that never returns from a wait can't be stopped from inside the app. The start watch
  says what to do after 20 s and the next start uses the software renderer; after the start, a GPU that stops
  answering in a wait (not in a failed present) still holds the app.
- **DNS (E1).** Names the system's resolver can't find go through the console's resolver, one at a time (up to 5 s,
  2 retries each). No SMB lookup runs on the UI thread now, but closing a share's listing waits for a lookup in
  progress.
- **SMB on the UI thread, bounded.** File > Load Subtitle from a share (10 s at most) and a `.sup` / VobSub file from
  a share (each request up to 15 s).
- **Host fidelity.** The host suite runs the real code with simulated PS5 calls and software Vulkan: the console's
  driver, its decoder and its resolver are stand-ins. A6 and D15 are test gaps.
- Every finding marked Open above.

## 11. Instructions

- Host tests: `make host`, then `bash tests/run-tests.sh` (all groups) or `ONLY="41 42 43" bash tests/run-tests.sh`;
  `make test` runs the harnesses, the runtime layer's tests and the suite. A fail-before run:
  `MPC_HOST_BIN=<another build>/mpc-ps5-host ONLY=42 bash tests/run-tests.sh`.
- On a PS5, the checks that matter most for 1.7.1: the app starts in its window after a session in full screen;
  View > Presets > Minimal shows "OPTIONS: the menu"; a film from a share, then the share's server turned off: Close
  comes back at once; the file browser at a share that is off shows "Reading the network folder..." and Cancel works;
  a film closed from the home screen while playing opens again near where it was (Remember File position on).
