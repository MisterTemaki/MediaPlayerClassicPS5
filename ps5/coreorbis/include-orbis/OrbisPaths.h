// Media Player Classic PS5: the /data/mpcps5 folder layout and the boot log (orbis-shims/orbis_paths.cpp).
//
//   /data/mpcps5/media        a place for videos and music on the console's own storage
//   /data/mpcps5/fonts        extra .ttf/.otf fonts (subtitles in scripts Selawik and Roboto don't cover)
//   /data/mpcps5/screenshots  File > Save Image
//   /data/mpcps5/logs         boot.log (this run) and boot.prev.log (the run before); installer.log, helper.log
//   /data/mpcps5/mpc-hc.ini   the settings
//   /data/mpcps5/cacert.pem   certificates for https:// streams (written by the app)
//
// Media is opened from anywhere the browser reaches: these folders, /data, USB drives (/mnt/usbN, /mnt/extN)
// and network shares.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#ifndef ORBIS_ROOT_DEFAULT
#define ORBIS_ROOT_DEFAULT "/data/mpcps5"
#endif

// Root folder (ORBIS_ROOT_DEFAULT; the host tests point it elsewhere with MPCPS5_PS5_ROOT).
const std::string& OrbisRoot();
// <root>/<sub>, created at boot by OrbisPathsInit.
std::string OrbisDir(const char* sub);
// Creates the folder tree. Returns false when the root itself can't be created (no /data access).
bool OrbisPathsInit();
// The media folder and the USB drives that exist right now.
std::vector<std::string> OrbisRomRoots();

bool OrbisIsDir(const std::string& path);
bool OrbisIsFile(const std::string& path);
bool OrbisMkdirs(const std::string& path);

// <root>/logs/<name>.log (the run before kept as <name>.prev.log): boot.log for the emulator, installer.log
// for the installer/helper payload. Every line also goes to stdout (once the log is open). Lines logged before the file is open
// (before the jailbreak shows /data to the app) are kept and written first.
void OrbisLogOpen(const char* name = "boot");
void OrbisLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void OrbisLogClose();
// Debug logs on or off: the "Debug logs" setting (debug_logs= in mpc-hc.ini, on unless it says 0).
// OrbisLogOpen reads it, so the app, the installer and the helper all follow it; while off, OrbisLog writes
// nothing (no file, no stdout) and the log files of earlier runs are left as they are.
void OrbisLogSetEnabled(bool on);
// Reads the setting again and applies it (the helper, which keeps running, calls this for each request).
void OrbisLogRefresh();
bool OrbisLogEnabled();
// The open log's file descriptor, for the crash handler's signal-safe write(); -1 before it is open.
int OrbisLogFd();
