// SPDX-License-Identifier: MIT
#include "mpc_vdec.cpp"
#include <cstdio>

static bool full_access = false;
static int queries = 0, creates = 0, allocations = 0, failures = 0;
static VdecConfig created = {};
void OrbisLog(const char*, ...) {}
extern "C" {
int sceVideodec2QueryDecoderMemoryInfo(const VdecConfig* c, VdecMemory* m)
{
	++queries;
	if (full_access) return int(0x811d0200);
	m->cpu_memory_size = c->max_frame_width == 3840 ? 0x8000 : 0x4000;
	m->gpu_memory_size = m->cpu_gpu_memory_size = 0x4000;
	m->max_frame_buffer_size = 0x10000;
	m->frame_buffer_alignment = 0x10000;
	return 0;
}
int sceVideodec2CreateDecoder(const VdecConfig* c, const VdecMemory* m, void** d)
{
	created = *c;
	if (m->cpu_memory_size != uint64_t(c->max_frame_width == 3840 ? 0x8000 : 0x4000)) return -1;
	++creates;
	*d = reinterpret_cast<void*>(uintptr_t(3));
	return 0;
}
int sceVideodec2DeleteDecoder(void*) { return 0; }
size_t sceKernelGetDirectMemorySize() { return size_t(12) << 30; }
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t* p) { *p = 0; return 0; }
int sceKernelReleaseDirectMemory(int64_t, size_t) { return 0; }
int sceKernelMapDirectMemory(void** p, size_t n, int, int, int64_t, size_t a)
{
	const int rc = posix_memalign(p, a, n);
	if (!rc) ++allocations;
	return rc;
}
int sceKernelMapFlexibleMemory(void** p, size_t n, int, int)
{
	*p = malloc(n);
	if (*p) ++allocations;
	return *p ? 0 : -1;
}
int sceKernelMunmap(void* p, size_t) { free(p); --allocations; return 0; }
int sceKernelReleaseFlexibleMemory(void* p, size_t) { free(p); --allocations; return 0; }
}
static void check(bool ok, const char* name)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", name);
	failures += !ok;
}
int main()
{
	using namespace mpc::hwdec;
	g_queue = reinterpret_cast<void*>(uintptr_t(1));
	g_vp9_queue = reinterpret_cast<void*>(uintptr_t(2));
	PrepareMemoryQueries();
	check(queries == 20 && g_memory_queries.size() == 20 && creates == 0 && allocations == 0,
		"startup queries H.264, HEVC and both VP9 profiles without allocating decoders");
	full_access = true;
	for (const auto& q : g_memory_queries)
	{
		VdecConfig c = q.config;
		Decoder d;
		d.frame_count = 1;
		VdecMemory m;
		const int rc = BringUp(&d, c, m);
		check(rc == 0 && created.compute_queue == q.config.compute_queue &&
			created.max_frame_width == q.config.max_frame_width && created.max_frame_height == q.config.max_frame_height,
			"cached memory creates the exact decoder after full-access query is refused");
		FreeDecoder(&d);
	}
	VdecConfig portrait = g_memory_queries[0].config;
	portrait.max_frame_width = 1920;
	portrait.max_frame_height = 1280;
	VdecMemory m;
	check(CachedMemory(portrait, m) && portrait.max_frame_width == 3840 && portrait.max_frame_height == 2176,
		"portrait video uses a cached envelope large enough for both dimensions");
	VdecConfig wrong = g_memory_queries[0].config;
	wrong.profile = 110;
	check(!CachedMemory(wrong, m), "unsupported profile is never given another profile's memory");
	wrong = g_memory_queries[0].config;
	wrong.compute_queue = g_vp9_queue;
	check(!CachedMemory(wrong, m), "memory answers from another compute queue are never reused");
	wrong = g_memory_queries[0].config;
	wrong.max_frame_width = 7680;
	check(!CachedMemory(wrong, m), "8K cannot reuse a 4K memory answer");
	check(allocations == 0 && creates == 20, "decoder and access-unit allocations are released");
	return failures ? 1 : 0;
}
