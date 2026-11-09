// SPDX-License-Identifier: MIT
#define MPC_HDR_TEST
#include "ProsperoHdr.cpp"
#include <cstdio>
static int open_result = 7, close_result = 0, submit_result = 0, range = 2, seen_handle = -1, submits = 0, failed = 0;
static uint64_t pixel_format;
static uint32_t seen_width, seen_height;
void OrbisLog(const char*, ...) {}
extern "C" {
int __real_sceVideoOutOpen(int, int, int, const void*) { return open_result; }
int __real_sceVideoOutClose(int) { return close_result; }
void sceVideoOutSetBufferAttribute2(SceVideoOutBufferAttribute2*, uint64_t f, uint32_t, uint32_t w, uint32_t h,
	uint64_t, uint32_t, uint64_t) { pixel_format = f; seen_width = w; seen_height = h; }
int sceVideoOutSubmitChangeBufferAttribute2(int handle, int, const void*, const void*)
{ seen_handle = handle; ++submits; return submit_result; }
int sceVideoOutGetOutputStatus(int handle, OutputStatus* status)
{ seen_handle = handle; status->dynamic_range = range; return 0; }
}
static void check(bool ok, const char* name) { printf("%s %s\n", ok ? "ok  " : "FAIL", name); failed += !ok; }
int main()
{
	check(ps5video::SetConsoleHdr(true, 3840, 2160) < 0 && submits == 0, "no HDR request before VideoOut opens");
	check(__wrap_sceVideoOutOpen(255, 0, 0, nullptr) == 7 && ps5video::SetConsoleHdr(true, 3840, 2160) == 0 &&
		seen_handle == 7 && pixel_format == UINT64_C(0x8100070400000000) && seen_width == 3840 && seen_height == 2160,
		"WSI handle receives the native BGR10A2 PQ/BT.2020 attribute at display resolution");
	check(ps5video::ConsoleOutputRange() == 2 && seen_handle == 7, "console output status can confirm actual HDR");
	submit_result = -12;
	check(ps5video::SetConsoleHdr(true, 3840, 2160) == -12, "driver propagates display refusal");
	submit_result = 0;
	check(ps5video::SetConsoleHdr(false, 3840, 2160) == 0 && pixel_format == UINT64_C(0x8000000000000000),
		"HDR shutdown restores WSI's original B8G8R8A8 SDR attribute");
	close_result = -1;
	__wrap_sceVideoOutClose(7);
	check(ps5video::ConsoleOutputRange() == 2, "failed close retains the valid output handle");
	close_result = 0;
	__wrap_sceVideoOutClose(7);
	check(ps5video::SetConsoleHdr(true, 3840, 2160) < 0 && ps5video::ConsoleOutputRange() < 0,
		"successful close clears the handle before another HDR request");
	return failed ? 1 : 0;
}
