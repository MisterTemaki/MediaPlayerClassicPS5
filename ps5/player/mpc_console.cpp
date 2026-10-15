// Media Player Classic PS5: console glue for the libraries -- thread stacks and name lookups.
//
// Thread stacks: a pthread made with the SDK's default attributes gets a small stack on the console (the
// emulator ports lost workers to it: ProsperoThread.h). FFmpeg's decoder threads (HEVC, H.264 frame threads),
// dav1d's workers, libswscale's slices, std::thread (libc++) all call pthread_create, so the eboot is linked
// with --wrap=pthread_create and every thread gets at least 2 MiB.
//
// Name lookups:
// The emulator ports found that, on the console, libc's getaddrinfo can fail inside a title while the network is
// up (the helper's HTTPS client falls back to libSceNet's resolver: frontend/fe_tlshttp.cpp in those ports).
// FFmpeg (tcp://, http://, https://, rtsp://...) and libsmb2 both call getaddrinfo, so the eboot is linked with
// --wrap=getaddrinfo and this does the same: libc first, then sceNetResolverStartNtoa for an IPv4 address.
// The answer is laid out as FreeBSD's getaddrinfo lays it out (the address inside the addrinfo's own block), so
// libc's freeaddrinfo frees it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#if !defined(__linux__)

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
int __real_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);

int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg)
{
	constexpr size_t kMinStack = 2u << 20;
	if (!attr)
	{
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setstacksize(&a, kMinStack);
		const int r = __real_pthread_create(t, &a, fn, arg);
		pthread_attr_destroy(&a);
		return r == 0 ? r : __real_pthread_create(t, nullptr, fn, arg);
	}
	size_t size = 0;
	if (pthread_attr_getstacksize(attr, &size) == 0 && size < kMinStack)
		pthread_attr_setstacksize(const_cast<pthread_attr_t*>(attr), kMinStack);
	return __real_pthread_create(t, attr, fn, arg);
}

int __real_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char* hostname, uint32_t* addr, int timeout_us, int retries, int flags);
int sceNetResolverDestroy(int resolver);

int __wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res)
{
	const int r = __real_getaddrinfo(node, service, hints, res);
	if (r == 0 || !node || !*node)
		return r;
	if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
		return r;
	if (hints && (hints->ai_flags & AI_NUMERICHOST))
		return r;
	static std::mutex mu;
	static int pool = -1;
	uint32_t addr = 0;
	{
		std::lock_guard<std::mutex> l(mu);
		if (pool < 0)
		{
			sceNetInit();
			pool = sceNetPoolCreate("mpcps5-dns", 16 * 1024, 0);
		}
		const int rs = pool >= 0 ? sceNetResolverCreate("mpcps5-dns", pool, 0) : -1;
		const int rc = rs >= 0 ? sceNetResolverStartNtoa(rs, node, &addr, 5 * 1000 * 1000, 2, 0) : -1;
		if (rs >= 0)
			sceNetResolverDestroy(rs);
		OrbisLog("[net] getaddrinfo(%s) -> %d; libSceNet's resolver -> %x (%u.%u.%u.%u)", node, r, unsigned(rc),
			addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, addr >> 24);
		if (rc < 0 || addr == 0)
			return r;
	}
	struct Block
	{
		struct addrinfo ai;
		struct sockaddr_in sin;
	};
	Block* b = static_cast<Block*>(calloc(1, sizeof(Block)));
	if (!b)
		return EAI_MEMORY;
	b->sin.sin_len = sizeof(b->sin);
	b->sin.sin_family = AF_INET;
	b->sin.sin_addr.s_addr = addr; // network order, as the resolver gives it
	b->sin.sin_port = htons(uint16_t(service ? atoi(service) : 0));
	b->ai.ai_family = AF_INET;
	b->ai.ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
	b->ai.ai_protocol = hints ? hints->ai_protocol : 0;
	b->ai.ai_addrlen = sizeof(b->sin);
	b->ai.ai_addr = reinterpret_cast<struct sockaddr*>(&b->sin);
	*res = &b->ai;
	return 0;
}
}

#endif
