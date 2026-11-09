// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace ps5video {
int SetConsoleHdr(bool on, int width, int height);
int ConsoleOutputRange(); // 1 SDR, 2 HDR, negative unavailable
}
