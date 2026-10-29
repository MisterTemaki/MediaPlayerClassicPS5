// Media Player Classic PS5: Help, Check for updates (MPC-HC's menu item), as the emulator ports do it from their
// settings: the app's newest GitHub release, checked and installed by ProsperoUpdate.h. The app does it itself
// (FFmpeg's HTTPS, which works in the app: the network streams use it), once the helper has let it write
// /data/homebrew/<TITLE_ID>.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "ProsperoAppPlace.h"

namespace mpcupdate
{
// The updater's setup: this build's version and title, the repository, FFmpeg as the transport.
void Init();
// The daily check at start: true (and the time noted in <root>/update-check.txt) when the last one is a day old.
bool DailyCheckDue();
// What opens the new version: the eboot the console runs (/app0/eboot.bin on the console; else through the mount
// ShadowMount+ made, else the folder the update went to).
std::string EbootPath();
// 1.5: where the app runs from and where an update goes (ProsperoAppPlace.h), found at Init.
const AppPlace& Place();
// 1.5.1: when the place at start didn't come from the helper (it was silent, or not there yet), look again now --
// before an update is offered -- so a helper that answers now is asked (#3)
void RefreshPlace();
// An update installed: noted (<root>/update-pending.txt) so that the next start can tell whether it runs the new
// version.
void NoteInstalled(const std::string& version);
// At start: an update noted and not yet seen running. ok: this is that version (or newer) and the note is gone; else
// the note stays for a few starts (shown counts them) and the app says what to do.
struct AfterUpdate
{
	bool pending = false;
	bool ok = false;
	std::string want, dir;
	int shown = 0;
};
AfterUpdate CheckAfterUpdate();
} // namespace mpcupdate
