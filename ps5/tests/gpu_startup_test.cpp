// SPDX-License-Identifier: MIT
// Real startup/recovery code; the Vulkan driver is replaced at its boundary.
#include "ProsperoVideo.cpp"
#include <filesystem>
#include <fstream>
#include <cstdarg>

static std::string root;
static bool driver_ok = true, armed_before_driver = false, fail_sync = false;
static int driver_calls = 0, released_queues = 0, failed = 0;
const std::string& OrbisRoot() { return root; }
void OrbisLog(const char*, ...) {}
void OrbisLogSync() {}
namespace crashlog {
bool ImportsBound(const char*, const char**) { return true; }
void Stage(Area, const char*) {}
void StageText(Area, const char*) {}
}
namespace mpc::hwdec {
bool QueuesExist() { return true; }
void ReleaseQueues(const char*) { ++released_queues; }
}
extern "C" void MpcHostGpuFlip(const uint8_t*, int, int, bool) {}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) { if (fail_sync) { errno = EIO; return -1; } return __real_fsync(fd); }
namespace ps5gpu {
static std::string marker;
void Configure(const Config&) {}
bool Init(int, int, bool, int, int)
{
	++driver_calls;
	int attempts, works;
	ps5video::ReadGpuAttempts(&attempts, &works);
	armed_before_driver = attempts == 4 && works == 0;
	return driver_ok;
}
bool GuardBegin(const std::string& path) { marker = path; std::ofstream(path).close(); return true; }
void GuardEnd() { if (!marker.empty()) unlink(marker.c_str()); marker.clear(); }
const char* DeviceName() { return "test driver"; }
int OutWidth() { return 1920; }
int OutHeight() { return 1080; }
bool VideoAvailable() { return true; }
}

static void check(bool ok, const char* name)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", name);
	failed += !ok;
}
static std::string state()
{
	std::ifstream file(ps5video::GpuAttemptsFile());
	return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	root = argv[1];
	std::filesystem::create_directories(root + "/logs");
	setenv("MPCPS5_HOST_GPU", "1", 1);
	setenv("MPCPS5_HOST_GPU_BUILD_ID", "test-driver", 1);
	using namespace ps5video;
	check(InitGpu() && driver_calls == 1 && armed_before_driver, "fresh start tries GPU with recovery already persisted");
	GpuReachedScreen();
	check(state().find("works 1") != std::string::npos, "first displayed frame records a working GPU");
	check(InitGpu() && driver_calls == 2 && armed_before_driver, "working startup still arms recovery before the driver");
	// Simulate process termination during Init, without executing shutdown/cleanup.
	check(!InitGpu() && driver_calls == 2, "one interrupted GPU start immediately falls back on the next launch");
	check(!InitGpu() && driver_calls == 2, "subsequent launches keep recovery instead of cycling GPU variants");
	check(released_queues == 0, "crash recovery preserves the hardware decoder queues");
	check(RetryGpuNextStart(), "explicit retry can be saved");
	driver_ok = false;
	check(!InitGpu() && driver_calls == 3, "explicit retry enters the driver once and handles a returned failure");
	driver_ok = true;
	check(!InitGpu() && driver_calls == 3, "a returned initialization failure also persists recovery");
	check(RetryGpuNextStart() && InitGpu() && driver_calls == 4, "a later explicit retry can restore GPU rendering");
	GpuReachedScreen();
	const auto before = state();
	fail_sync = true;
	check(!RetryGpuNextStart() && state() == before && !std::filesystem::exists(GpuAttemptsFile() + ".tmp"),
		"failed state sync retains the previous state and removes the temporary file");
	check(!InitGpu() && driver_calls == 4, "unwritable recovery state prevents an unguarded GPU start");
	fail_sync = false;
	std::ofstream(GpuAttemptsFile()) << "attempts 0\nworks 0\nbuild old-driver\n";
	std::ofstream(GpuMarker()).close();
	check(InitGpu() && driver_calls == 5, "deliberately changed driver revision permits one attempt after legacy failures");
	GpuReachedScreen();
	std::ofstream(root + "/mpc-hc.ini") << "GpuStart=1\n";
	check(!InitGpu() && driver_calls == 5, "explicit CPU-only preference is honored");
	return failed ? 1 : 0;
}
