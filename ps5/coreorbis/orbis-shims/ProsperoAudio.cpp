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
#include "ProsperoThread.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <thread>
#include <unistd.h>

namespace ps5audio
{
namespace
{
struct State
{
	int handle = -1;
	// the 8-channel port (S16, L R C LFE Ls Rs Lb Rb), opened on first use by SetChannels (the player's thread)
	// and read by the output thread: atomic, and stored before `channels` says 8 (release/acquire)
	std::atomic<int> handle8{-1};
	// when the 8-channel port was last asked for (SetChannels' thread only): a refusal is asked again, at most
	// every 10 s (a surround track opened later, the console's audio set up again), not at every track
	double tried8 = -1;
	std::atomic<int> channels{2}; // what the ring holds: 2 or 8
	std::thread thread;
	std::atomic<bool> quit{false};
	alignas(64) std::atomic<uint32_t> head{0}; // written by the producer (frames, wraps)
	alignas(64) std::atomic<uint32_t> tail{0}; // written by the audio thread
	std::atomic<uint64_t> underruns{0};
	std::atomic<uint64_t> played{0}; // frames of the ring played (not the silence)
	std::atomic<bool> paused{false};
	std::atomic<uint32_t> clear_req{0}, clear_ack{0};
	std::atomic<int> gain{65536}; // 16.16
	int16_t ring[kCapacity * 8]; // 8 slots per frame; stereo uses the first two
};
State* g = nullptr;

// A sample times the gain (16.16, up to 4.0): in 64 bits (32767 * 4.0 doesn't fit 32 bits once shifted in), then
// held to the 16-bit range, so a boosted loud passage clips instead of wrapping around to the other sign.
inline int16_t Amplify(int16_t s, int gain)
{
	const int64_t v = (int64_t(s) * gain) >> 16;
	return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void Run()
{
	void* alt = crashlog::ArmThread(); // a crash here is reported too
	alignas(64) int16_t grain[kGrain * 8];
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
		const int h8 = g->handle8.load(std::memory_order_acquire);
		const int ch = g->channels.load(std::memory_order_acquire) == 8 && h8 > 0 ? 8 : 2;
		if (g->paused.load(std::memory_order_relaxed))
			memset(grain, 0, sizeof(grain));
		else if (avail >= uint32_t(kGrain))
		{
			const int gain = g->gain.load(std::memory_order_relaxed);
			for (int i = 0; i < kGrain; i++)
			{
				const int16_t* src = &g->ring[((tail + i) % kCapacity) * 8];
				int16_t* dst = &grain[i * ch];
				for (int c = 0; c < ch; c++)
					dst[c] = gain == 65536 ? src[c] : Amplify(src[c], gain);
			}
			g->tail.store(tail + kGrain, std::memory_order_release);
			g->played.fetch_add(kGrain, std::memory_order_relaxed);
		}
		else
		{
			memset(grain, 0, sizeof(grain));
			g->underruns.fetch_add(1, std::memory_order_relaxed);
		}
		if (sceAudioOutOutput(ch == 8 ? h8 : g->handle, grain) < 0)
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
	try
	{
		g->thread = ps5::StartThread("audio-out", Run);
	}
	catch (const std::system_error& e)
	{
		// 1.5.2: no sound (as when the port can't be opened), the app runs on (up to 1.5.1: std::terminate)
		OrbisLog("[audio] a new thread refused: %s: no sound", e.what());
		sceAudioOutClose(handle);
		delete g;
		g = nullptr;
		return false;
	}
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
	if (g->handle8.load() > 0)
		sceAudioOutClose(g->handle8.load());
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

int Push(const int16_t* samples, int frames, int channels)
{
	if (!g || frames <= 0)
		return 0;
	const int n = frames < Free() ? frames : Free();
	const uint32_t head = g->head.load(std::memory_order_relaxed);
	for (int i = 0; i < n; i++)
	{
		int16_t* d = &g->ring[((head + i) % kCapacity) * 8];
		const int16_t* s = samples + size_t(i) * channels;
		for (int c = 0; c < 8; c++)
			d[c] = c < channels ? s[c] : 0;
	}
	g->head.store(head + n, std::memory_order_release);
	return n;
}

bool SetChannels(int channels)
{
	if (!g)
		return false;
	const int want = channels > 2 ? 8 : 2;
	if (want == 8 && g->handle8.load(std::memory_order_acquire) <= 0)
	{
		timespec ts = {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		const double now = ts.tv_sec + ts.tv_nsec / 1e9;
		if (g->tried8 < 0 || now - g->tried8 >= 10)
		{
			g->tried8 = now;
			const int h = sceAudioOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain, kRate,
				SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH);
			OrbisLog("[audio] sceAudioOutOpen(48 kHz, S16 8 channels, %d) -> %d", kGrain, h);
			if (h > 0)
				g->handle8.store(h, std::memory_order_release); // before `channels` below makes the thread use it
		}
		if (g->handle8.load(std::memory_order_acquire) <= 0)
			return false;
	}
	if (g->channels.load() != want)
	{
		Clear();
		g->channels.store(want, std::memory_order_release);
		OrbisLog("[audio] %s output", want == 8 ? "8-channel" : "stereo");
	}
	return true;
}

int Channels()
{
	return g ? g->channels.load() : 2;
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
