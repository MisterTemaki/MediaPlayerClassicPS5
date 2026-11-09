// SPDX-License-Identifier: GPL-3.0-or-later
// VideoOut's HDR10 framebuffer attribute, as used by VLC-PS5's patches/mesa/0001-videoout-hdr10.patch.
// Capture the WSI's handle at the import boundary, so Mesa remains pinned and unchanged.
#include "ProsperoHdr.h"
#if defined(__PROSPERO__) || defined(MPC_HDR_TEST)
#include "ProsperoSce.h"
#include "OrbisPaths.h"
#include <mutex>
#include <cstdint>

namespace {
std::mutex videoout_mutex;
int videoout_handle = -1;
struct OutputStatus {
	uint32_t resolution, dynamic_range;
	uint64_t refresh_rate, flags, reserved[3];
};
static_assert(sizeof(OutputStatus) == 48);
}
extern "C" {
int __real_sceVideoOutOpen(int, int, int, const void*);
int __real_sceVideoOutClose(int);
int sceVideoOutSubmitChangeBufferAttribute2(int, int, const void*, const void*);
int sceVideoOutGetOutputStatus(int, OutputStatus*);
int __wrap_sceVideoOutOpen(int user, int bus, int index, const void* p)
{
	std::lock_guard<std::mutex> lock(videoout_mutex);
	const int handle = __real_sceVideoOutOpen(user, bus, index, p);
	if (handle >= 0) videoout_handle = handle;
	return handle;
}
int __wrap_sceVideoOutClose(int handle)
{
	std::lock_guard<std::mutex> lock(videoout_mutex);
	const int rc = __real_sceVideoOutClose(handle);
	if (rc == 0 && videoout_handle == handle) videoout_handle = -1;
	return rc;
}
}
namespace ps5video {
int SetConsoleHdr(bool on, int width, int height)
{
	std::lock_guard<std::mutex> lock(videoout_mutex);
	if (videoout_handle < 0 || width <= 0 || height <= 0) return -1;
	SceVideoOutBufferAttribute2 attribute = {};
	sceVideoOutSetBufferAttribute2(&attribute, on ? UINT64_C(0x8100070400000000) : UINT64_C(0x8000000000000000),
		0, uint32_t(width), uint32_t(height), 0, 0, 0);
	const int rc = sceVideoOutSubmitChangeBufferAttribute2(videoout_handle, 0, &attribute, nullptr);
	OrbisLog("[hdr] VideoOut %s, %dx%d: %#x", on ? "HDR10/PQ 10-bit BT.2020" : "SDR", width, height, unsigned(rc));
	return rc;
}
int ConsoleOutputRange()
{
	std::lock_guard<std::mutex> lock(videoout_mutex);
	if (videoout_handle < 0) return -1;
	OutputStatus status = {};
	const int rc = sceVideoOutGetOutputStatus(videoout_handle, &status);
	OrbisLog("[hdr] output status: %#x, range %u, flags %#llx", unsigned(rc), status.dynamic_range,
		static_cast<unsigned long long>(status.flags));
	return rc == 0 ? int(status.dynamic_range) : -1;
}
}
#else
namespace ps5video {
int SetConsoleHdr(bool, int, int) { return -1; }
int ConsoleOutputRange() { return -1; }
}
#endif
