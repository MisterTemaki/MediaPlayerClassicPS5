// Media Player Classic PS5: the player's window (CMainFrame) and its loop.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace mpc
{
// Runs until File > Exit (or the window's close button). first: a file or URL to open at once.
int RunApp(const std::string& first, bool have_data);
// "FFmpeg n7.1.1" for the boot log and the About box
std::string EngineVersion();
} // namespace mpc
