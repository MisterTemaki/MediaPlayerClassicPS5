// Media Player Classic PS5: Help, Check for updates (MPC-HC's menu item), as the emulator ports do it from their
// settings: the app's newest GitHub release, checked and installed by ProsperoUpdate.h. The app does it itself
// (FFmpeg's HTTPS, which works in the app: the network streams use it), once the helper has let it write
// /data/homebrew/<TITLE_ID>.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace mpcupdate
{
// The updater's setup: this build's version and title, the repository, FFmpeg as the transport.
void Init();
// The daily check at start: true (and the time noted in <root>/update-check.txt) when the last one is a day old.
bool DailyCheckDue();
// "/data/homebrew/<TITLE_ID>/eboot.bin": what opens the new version.
std::string EbootPath();
} // namespace mpcupdate
