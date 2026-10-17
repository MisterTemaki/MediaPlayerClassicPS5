// Media Player Classic PS5: HDR tone mapping on the CPU (mpc_tonemap.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_tonemap.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"

extern "C" {
#include <libavutil/mastering_display_metadata.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mpc
{
namespace tonemap
{
namespace
{
constexpr int kIn = 4096;   // input LUT: the 16-bit signal's top 12 bits
constexpr int kTone = 4096; // tone curve: sqrt(L / peak) steps
constexpr int kOut = 8192;  // output LUT: sqrt(linear) steps -> 8 bits

double PqEotf(double e) // signal 0..1 -> nits
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double p = std::pow(std::max(e, 0.0), 1.0 / m2);
	return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double PqInv(double nits) // nits -> signal 0..1
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
	return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

double HlgInvOetf(double e) // signal 0..1 -> scene light 0..1
{
	const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
	return e <= 0.5 ? e * e / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
}

double HableCurve(double x)
{
	const double A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
	return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

// The highlight curve, nits in -> nits out (white is the screen's 1.0).
double Curve(Mode mode, double L, double peak, double white)
{
	if (L <= 0)
		return 0;
	if (peak <= white)
		return L;
	switch (mode)
	{
		case Hable:
			return white * HableCurve(L / white * 2.0) / HableCurve(peak / white * 2.0);
		case Reinhard:
		{
			const double x = L / white, lw2 = (peak / white) * (peak / white);
			return white * x * (1 + x / lw2) / (1 + x);
		}
		default:
		{
			// BT.2390 EETF in the PQ domain, from [0, peak] to [0, white]
			const double src = PqInv(peak);
			const double e1 = PqInv(L) / src;
			const double max_lum = PqInv(white) / src;
			const double ks = 1.5 * max_lum - 0.5;
			double e2 = e1;
			if (e1 > ks && ks < 1)
			{
				const double t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
				e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * max_lum;
			}
			return PqEotf(std::min(e2, 1.0) * src);
		}
	}
}

struct Tables
{
	Params key;
	bool valid = false;
	float in[kIn + 1];      // signal -> linear nits (PQ), or scene light (HLG)
	float ratio[kTone + 1]; // tone curve as a gain on max(R, G, B), indexed by sqrt(L / top)
	double top = 1;         // the tone curve's input range, nits
	uint8_t out[kOut + 1];  // sqrt(linear) -> 8-bit, gamma 2.4
	float ootf[kTone + 1];  // HLG: 1000 * Ys^0.2, indexed by sqrt(Ys)
};

std::mutex g_mu;
Tables g_t;

bool Same(const Params& a, const Params& b)
{
	return a.mode == b.mode && a.transfer == b.transfer && a.src_peak == b.src_peak && a.white == b.white;
}

void Build(Tables& t, const Params& p)
{
	for (int i = 0; i <= kIn; i++)
	{
		const double e = double(i) / kIn;
		t.in[i] = float(p.transfer == Transfer::Hlg ? HlgInvOetf(e) : PqEotf(e));
	}
	// HLG: display light comes from the OOTF at 1000 nits (system gamma 1.2), applied per pixel
	t.top = p.transfer == Transfer::Hlg ? 1000.0 : std::max(p.src_peak, p.white);
	for (int i = 0; i <= kTone; i++)
	{
		const double s = double(i) / kTone;
		const double L = s * s * t.top;
		t.ratio[i] = L > 0 ? float(Curve(p.mode, L, t.top, p.white) / L / p.white) : float(1.0 / p.white);
	}
	for (int i = 0; i <= kTone; i++)
	{
		const double s = double(i) / kTone;
		t.ootf[i] = float(1000.0 * std::pow(s * s, 0.2));
	}
	for (int i = 0; i <= kOut; i++)
	{
		const double s = double(i) / kOut;
		const double lin = s * s;
		t.out[i] = uint8_t(std::lround(std::pow(lin, 1.0 / 2.4) * 255.0));
	}
	t.key = p;
	t.valid = true;
}

void Rows(const Tables& t, const uint16_t* src, int src_pitch, uint32_t* dst, int dst_pitch, int w, int y0, int y1)
{
	const bool hlg = t.key.transfer == Transfer::Hlg;
	// BT.2020 -> BT.709, linear light
	const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
	const float tone_scale = float(kTone) / float(std::sqrt(t.top));
	for (int y = y0; y < y1; y++)
	{
		const uint16_t* s = src + size_t(y) * src_pitch * 4;
		uint32_t* d = dst + size_t(y) * dst_pitch;
		for (int x = 0; x < w; x++, s += 4)
		{
			float r = t.in[s[0] >> 4], g = t.in[s[1] >> 4], b = t.in[s[2] >> 4];
			if (hlg)
			{
				// OOTF: 1000 nits, gamma 1.2 on the scene's luminance
				const float ys = 0.2627f * r + 0.6780f * g + 0.0593f * b;
				const int oi = ys > 0 ? int(std::sqrt(ys < 1 ? ys : 1.0f) * kTone) : 0;
				const float k = t.ootf[oi];
				r *= k;
				g *= k;
				b *= k;
			}
			float R = m[0] * r + m[1] * g + m[2] * b;
			float G = m[3] * r + m[4] * g + m[5] * b;
			float B = m[6] * r + m[7] * g + m[8] * b;
			R = R > 0 ? R : 0;
			G = G > 0 ? G : 0;
			B = B > 0 ? B : 0;
			const float mx = std::max(R, std::max(G, B));
			int ti = int(std::sqrt(mx) * tone_scale);
			ti = ti > kTone ? kTone : ti;
			const float k = t.ratio[ti];
			R *= k;
			G *= k;
			B *= k;
			auto enc = [&](float v) -> uint32_t {
				v = v < 1 ? v : 1;
				return t.out[int(std::sqrt(v) * kOut)];
			};
			d[x] = enc(R) | (enc(G) << 8) | (enc(B) << 16) | 0xff000000u;
		}
	}
}
} // namespace

Transfer TransferOf(const AVFrame* f)
{
	if (f->color_trc == AVCOL_TRC_SMPTE2084)
		return Transfer::Pq;
	if (f->color_trc == AVCOL_TRC_ARIB_STD_B67)
		return Transfer::Hlg;
	return Transfer::Sdr;
}

double SourcePeak(const AVFrame* f)
{
	if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL))
	{
		const auto* cll = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
		if (cll->MaxCLL >= 200 && cll->MaxCLL <= 10000)
			return cll->MaxCLL;
	}
	if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
	{
		const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
		if (md->has_luminance && md->max_luminance.den)
		{
			const double v = av_q2d(md->max_luminance);
			if (v >= 200 && v <= 10000)
				return v;
		}
	}
	return 1000;
}

namespace
{
// The workers that share a picture's rows with the caller: made once, at the first HDR picture, and kept for the
// rest of the run (a picture used to start and join its own threads: up to 300 thread starts a second at 60 fps).
// One picture at a time (Convert holds g_mu throughout); each picture is a new generation the workers wake for,
// each takes its band of rows, the last one done wakes the caller.
constexpr int kBands = 6; // the caller + 5 workers

struct Job
{
	const Tables* t = nullptr;
	const uint16_t* src = nullptr;
	int src_pitch = 0;
	uint32_t* dst = nullptr;
	int dst_pitch = 0, w = 0, h = 0;
};

class Pool
{
public:
	Pool()
	{
		for (int i = 1; i < kBands; i++)
			threads_.emplace_back([this, i] { Work(i); });
		started_ += kBands - 1;
		OrbisLog("[tonemap] %d worker threads started (kept for the run)", kBands - 1);
	}

	// Stops the workers and waits for them (none is in a picture then: Convert holds g_mu around Run).
	~Pool()
	{
		{
			std::lock_guard<std::mutex> l(m_);
			stop_ = true;
		}
		cv_.notify_all();
		for (std::thread& t : threads_)
			t.join();
		OrbisLog("[tonemap] %d worker threads stopped", int(threads_.size()));
	}

	void Run(const Job& job)
	{
		std::unique_lock<std::mutex> l(m_);
		job_ = job;
		left_ = kBands - 1;
		gen_++;
		l.unlock();
		cv_.notify_all();
		Rows(*job.t, job.src, job.src_pitch, job.dst, job.dst_pitch, job.w, 0, job.h / kBands);
		l.lock();
		done_cv_.wait(l, [this] { return left_ == 0; });
	}

	static int Started() { return started_.load(); }

private:
	void Work(int band)
	{
		void* alt = crashlog::ArmThread(); // a crash here is reported too
		uint64_t seen = 0;
		for (;;)
		{
			std::unique_lock<std::mutex> l(m_);
			cv_.wait(l, [&] { return stop_ || gen_ != seen; });
			if (stop_)
				break;
			seen = gen_;
			const Job job = job_;
			l.unlock();
			Rows(*job.t, job.src, job.src_pitch, job.dst, job.dst_pitch, job.w, job.h * band / kBands,
				job.h * (band + 1) / kBands);
			l.lock();
			if (--left_ == 0)
				done_cv_.notify_one();
		}
		crashlog::DisarmThread(alt);
	}

	std::mutex m_;
	std::condition_variable cv_, done_cv_;
	std::vector<std::thread> threads_;
	Job job_;
	uint64_t gen_ = 0;
	int left_ = 0;
	bool stop_ = false;
	static std::atomic<int> started_;
};
std::atomic<int> Pool::started_{0};
std::unique_ptr<Pool> g_pool; // under g_mu
} // namespace

void Convert(const uint16_t* src, int src_pitch, uint32_t* dst, int dst_pitch, int w, int h, const Params& p)
{
	// g_mu for the whole picture: the tables aren't rebuilt under a picture still being converted
	std::lock_guard<std::mutex> l(g_mu);
	if (!g_t.valid || !Same(g_t.key, p))
		Build(g_t, p);
	if (h < 360)
	{
		Rows(g_t, src, src_pitch, dst, dst_pitch, w, 0, h);
		return;
	}
	if (!g_pool)
		g_pool.reset(new Pool()); // kept until Shutdown(): its threads wait for the next picture
	Job job;
	job.t = &g_t;
	job.src = src;
	job.src_pitch = src_pitch;
	job.dst = dst;
	job.dst_pitch = dst_pitch;
	job.w = w;
	job.h = h;
	g_pool->Run(job);
}

int WorkerThreads()
{
	return Pool::Started();
}

void Shutdown()
{
	std::lock_guard<std::mutex> l(g_mu);
	g_pool.reset();
}
} // namespace tonemap
} // namespace mpc
