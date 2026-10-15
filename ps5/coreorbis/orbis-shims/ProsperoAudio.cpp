// Media Player Classic PS5: sound through libSceAudioOut.
//
// The port opens the main output on the system user (255), S16 stereo, 48 kHz, 256-frame grains, and a
// thread feeds it: sceAudioOutOutput blocks until the previous grain was consumed, so the thread runs at
// exactly the console's audio clock. The player's audio thread pushes into a single-producer
// single-consumer ring; when the ring runs dry the thread plays silence (counted in Underruns) instead of
// blocking. For a player the output thread also: pauses at once (silence, the ring kept), drops what is
// queued on a seek (Clear), counts the frames it really played (the A/V clock) and applies the volume at
// the last moment, so the volume slider answers without the ring's delay.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoAudio.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"
#include "ProsperoSce.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace ps5audio
{
namespace
{
struct State
{
	int handle = -1;
	std::thread thread;
	std::atomic<bool> quit{false};
	alignas(64) std::atomic<uint32_t> head{0}; // written by the producer (frames, wraps)
	alignas(64) std::atomic<uint32_t> tail{0}; // written by the audio thread
	std::atomic<uint64_t> underruns{0};
	std::atomic<uint64_t> played{0}; // frames of the ring played (not the silence)
	std::atomic<bool> paused{false};
	std::atomic<uint32_t> clear_req{0}, clear_ack{0};
	std::atomic<int> gain{65536}; // 16.16
	int16_t ring[kCapacity * 2];
};
State* g = nullptr;

void Run()
{
	void* alt = crashlog::ArmThread(); // a crash here is reported too
	alignas(64) int16_t grain[kGrain * 2];
	while (!g->quit.load(std::memory_order_relaxed))
	{
		const uint32_t req = g->clear_req.load(std::memory_order_acquire);
		if (req != g->clear_ack.load(std::memory_order_relaxed))
		{
			g->tail.store(g->head.load(std::memory_order_acquire), std::memory_order_release);
			g->clear_ack.store(req, std::memory_order_release);
		}
		const uint32_t tail = g->tail.load(std::memory_order_relaxed);
		const uint32_t head = g->head.load(std::memory_order_acquire);
		const uint32_t avail = head - tail;
		if (g->paused.load(std::memory_order_relaxed))
			memset(grain, 0, sizeof(grain));
		else if (avail >= uint32_t(kGrain))
		{
			const int gain = g->gain.load(std::memory_order_relaxed);
			for (int i = 0; i < kGrain; i++)
			{
				const uint32_t pos = (tail + i) % kCapacity;
				if (gain == 65536)
				{
					grain[i * 2] = g->ring[pos * 2];
					grain[i * 2 + 1] = g->ring[pos * 2 + 1];
				}
				else
				{
					grain[i * 2] = int16_t((int32_t(g->ring[pos * 2]) * gain) >> 16);
					grain[i * 2 + 1] = int16_t((int32_t(g->ring[pos * 2 + 1]) * gain) >> 16);
				}
			}
			g->tail.store(tail + kGrain, std::memory_order_release);
			g->played.fetch_add(kGrain, std::memory_order_relaxed);
		}
		else
		{
			memset(grain, 0, sizeof(grain));
			g->underruns.fetch_add(1, std::memory_order_relaxed);
		}
		if (sceAudioOutOutput(g->handle, grain) < 0)
			usleep(kGrain * 1000000 / kRate); // an error returns at once: don't drain the ring at CPU speed
	}
	crashlog::DisarmThread(alt);
}
} // namespace

bool Available()
{
	return g != nullptr;
}

bool Init()
{
	if (g)
		return true;
	const int r = sceAudioOutInit();
	OrbisLog("[audio] sceAudioOutInit -> %x", r);
	const int handle = sceAudioOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain,
		kRate, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
	OrbisLog("[audio] sceAudioOutOpen(48 kHz, S16 stereo, %d) -> %d", kGrain, handle);
	if (handle <= 0)
		return false;
	g = new State();
	g->handle = handle;
	g->thread = std::thread(Run);
	return true;
}

void Shutdown()
{
	if (!g)
		return;
	g->quit.store(true);
	if (g->thread.joinable())
		g->thread.join();
	sceAudioOutClose(g->handle);
	OrbisLog("[audio] closed, %llu underruns", (unsigned long long)g->underruns.load());
	delete g;
	g = nullptr;
}

int Free()
{
	if (!g)
		return kCapacity;
	return kCapacity - int(g->head.load(std::memory_order_relaxed) - g->tail.load(std::memory_order_acquire));
}

int Queued()
{
	return kCapacity - Free();
}

int Push(const int16_t* stereo, int frames)
{
	if (!g || frames <= 0)
		return 0;
	const int n = frames < Free() ? frames : Free();
	const uint32_t head = g->head.load(std::memory_order_relaxed);
	for (int i = 0; i < n; i++)
	{
		const uint32_t pos = (head + i) % kCapacity;
		g->ring[pos * 2] = stereo[i * 2];
		g->ring[pos * 2 + 1] = stereo[i * 2 + 1];
	}
	g->head.store(head + n, std::memory_order_release);
	return n;
}

uint64_t Underruns()
{
	return g ? g->underruns.load(std::memory_order_relaxed) : 0;
}

void SetPaused(bool paused)
{
	if (g)
		g->paused.store(paused);
}

void Clear()
{
	if (!g)
		return;
	const uint32_t req = g->clear_req.fetch_add(1) + 1;
	// the output thread takes it at its next grain (5.3 ms); paused or not it keeps running
	for (int i = 0; i < 200 && g->clear_ack.load(std::memory_order_acquire) != req; i++)
		usleep(1000);
}

uint64_t Played()
{
	return g ? g->played.load(std::memory_order_relaxed) : 0;
}

void SetGain(double gain)
{
	if (!g)
		return;
	if (gain < 0)
		gain = 0;
	if (gain > 4)
		gain = 4;
	g->gain.store(int(gain * 65536 + 0.5));
}
} // namespace ps5audio
