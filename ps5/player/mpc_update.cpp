// Media Player Classic PS5: the updater's setup (mpc_update.h). FFmpeg's avio does the HTTPS GETs (redirects
// followed, the certificate checked against the CA list the app writes to <root>/cacert.pem).
//
// SPDX-License-Identifier: MIT

#include "mpc_update.h"

#include "mpc_i18n.h"

#include "OrbisPaths.h"
#include "ProsperoNotify.h"
#include "ProsperoUpdate.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#ifndef MPCPS5_UPDATE_REPO
#define MPCPS5_UPDATE_REPO "MisterTemaki/MediaPlayerClassicPS5"
#endif

namespace mpcupdate
{
namespace
{
std::string EnvOr(const char* name, const char* fallback)
{
	const char* v = getenv(name);
	return v && *v ? v : fallback;
}

int StatusOf(int err)
{
	switch (err)
	{
		case AVERROR_HTTP_BAD_REQUEST: return 400;
		case AVERROR_HTTP_UNAUTHORIZED: return 401;
		case AVERROR_HTTP_FORBIDDEN: return 403;
		case AVERROR_HTTP_NOT_FOUND: return 404;
		case AVERROR_HTTP_TOO_MANY_REQUESTS: return 429;
		case AVERROR_HTTP_OTHER_4XX: return 400;
		case AVERROR_HTTP_SERVER_ERROR: return 500;
		default: return -1;
	}
}

// GET url into out (at most max bytes): 200, the HTTP error, -1 (no connection...) or -2 (too big).
int AvioGet(const std::string& url, std::vector<uint8_t>& out, size_t max, std::atomic<size_t>* progress)
{
	out.clear();
	AVDictionary* opts = nullptr;
	av_dict_set(&opts, "user_agent", "MediaPlayerClassicPS5/" MPCPS5_PS5_VERSION, 0);
	av_dict_set(&opts, "headers", "Accept: application/vnd.github+json, */*\r\n", 0);
	av_dict_set(&opts, "rw_timeout", "20000000", 0);
	// GitHub only over https, its certificate checked against the CA list the app writes at start: without that
	// list nothing is fetched (an update must not come from a server that couldn't be checked)
	const std::string ca = OrbisRoot() + "/cacert.pem";
#ifdef __PROSPERO__
	const bool test_server = false;
#else
	const bool test_server = getenv("MPCPS5_UPDATE_API") != nullptr; // the host tests' local server (http://)
#endif
	if (url.compare(0, 8, "https://") != 0 && !test_server)
	{
		OrbisLog("[update] GET %s: not https, refused", url.c_str());
		return -1;
	}
	if (!OrbisIsFile(ca))
	{
		OrbisLog("[update] GET %s: no CA list (%s), refused", url.c_str(), ca.c_str());
		return -1;
	}
	av_dict_set(&opts, "ca_file", ca.c_str(), 0);
	av_dict_set(&opts, "tls_verify", "1", 0);
	AVIOContext* io = nullptr;
	const int r = avio_open2(&io, url.c_str(), AVIO_FLAG_READ, nullptr, &opts);
	av_dict_free(&opts);
	if (r < 0)
	{
		char e[128];
		av_strerror(r, e, sizeof(e));
		OrbisLog("[update] GET %s: %s", url.c_str(), e);
		return StatusOf(r);
	}
	std::vector<uint8_t> buf(1 << 16);
	int status = 200;
	for (;;)
	{
		const int n = avio_read(io, buf.data(), int(buf.size()));
		if (n == AVERROR_EOF || n == 0)
			break;
		if (n < 0)
		{
			char e[128];
			av_strerror(n, e, sizeof(e));
			OrbisLog("[update] GET %s: read failed after %zu bytes: %s", url.c_str(), out.size(), e);
			status = -1;
			break;
		}
		if (out.size() + size_t(n) > max)
		{
			status = -2;
			break;
		}
		out.insert(out.end(), buf.begin(), buf.begin() + n);
		if (progress)
			progress->store(out.size(), std::memory_order_relaxed);
	}
	avio_closep(&io);
	if (status != 200)
		out.clear();
	return status;
}
} // namespace

AppPlace g_place;

std::string PendingPath()
{
	return OrbisRoot() + "/update-pending.txt";
}

std::string EbootPath()
{
	// the eboot the console runs: /app0 is the app's own view of its folder (the console)
	if (OrbisIsFile("/app0/eboot.bin"))
		return "/app0/eboot.bin";
	return (g_place.mount.empty() ? g_place.dir : g_place.mount) + "/eboot.bin";
}

const AppPlace& Place()
{
	return g_place;
}

void NoteInstalled(const std::string& version)
{
	if (FILE* f = fopen(PendingPath().c_str(), "w"))
	{
		fprintf(f, "%s\n%s\n0\n", version.c_str(), g_place.dir.c_str());
		fclose(f);
	}
}

AfterUpdate CheckAfterUpdate()
{
	AfterUpdate a;
	FILE* f = fopen(PendingPath().c_str(), "r");
	if (!f)
		return a;
	char want[64] = "", dir[512] = "";
	int shown = 0;
	const bool read = fgets(want, sizeof(want), f) && fgets(dir, sizeof(dir), f);
	if (read && fscanf(f, "%d", &shown) != 1)
		shown = 0;
	fclose(f);
	auto chomp = [](char* t) { t[strcspn(t, "\r\n")] = 0; };
	chomp(want);
	chomp(dir);
	if (!read || !*want)
	{
		remove(PendingPath().c_str());
		return a;
	}
	a.pending = true;
	a.want = want;
	a.dir = dir;
	a.ok = update::CompareVersions(MPCPS5_PS5_VERSION, want) >= 0;
	a.shown = shown + 1;
	if (a.ok || a.shown >= 3)
		remove(PendingPath().c_str()); // seen running (or said three times: not again)
	else if (FILE* w = fopen(PendingPath().c_str(), "w"))
	{
		fprintf(w, "%s\n%s\n%d\n", want, dir, a.shown);
		fclose(w);
	}
	OrbisLog("[update] after the update to %s (in %s): this is %s%s", want, dir, MPCPS5_PS5_VERSION,
		a.ok ? ": the new version runs" : ": still the old version");
	return a;
}

void Init()
{
	avformat_network_init();
	update::Config c;
	c.repo = MPCPS5_UPDATE_REPO;
	c.title_id = MPCPS5_TITLE_ID;
	c.version = MPCPS5_PS5_VERSION;
	c.app_name = "Media Player Classic PS5";
	// where the console runs the app from (ShadowMount+'s mount of its folder): the update goes there
	g_place = FindAppPlace(MPCPS5_TITLE_ID, EnvOr("MPCPS5_PS5_HOMEBREW", "/data/homebrew") + "/" MPCPS5_TITLE_ID);
	OrbisLog("[update] %s", g_place.log.c_str());
	c.app_dir = g_place.dir;
	if (!g_place.writable)
		c.cant_install = g_place.reason;
	c.appmeta_dir = EnvOr("MPCPS5_PS5_APPMETA", "/user/appmeta") + "/" MPCPS5_TITLE_ID;
	c.api_url = EnvOr("MPCPS5_UPDATE_API", "");
	c.release_key = MPCPS5_RELEASE_KEY; // keys/release-ed25519.pub at build time ("" when the build has none)
#ifndef __PROSPERO__
	if (const char* k = getenv("MPCPS5_HOST_RELEASE_KEY")) // the host tests' own key
		c.release_key = k;
#endif
	update::Init(c, AvioGet); // also puts back an update cut off half way
	if (!update::StartupProblem().empty())
	{
		ProsperoNotify("%s", mpc::T(update::StartupProblem().c_str()));
		ProsperoNotifyFlush();
	}
	update::SetOnNewer([](const std::string& v) {
		ProsperoNotify(mpc::T("Media Player Classic PS5 %s is available: Help, Check for updates."), v.c_str());
		ProsperoNotifyFlush();
	});
}

void RefreshPlace()
{
	if (g_place.from_helper)
		return;
	g_place = FindAppPlace(MPCPS5_TITLE_ID, EnvOr("MPCPS5_PS5_HOMEBREW", "/data/homebrew") + "/" MPCPS5_TITLE_ID);
	OrbisLog("[update] looked again: %s", g_place.log.c_str());
	update::SetInstallPlace(g_place.dir, g_place.writable ? "" : g_place.reason);
}

bool DailyCheckDue()
{
	const std::string stamp = OrbisRoot() + "/update-check.txt";
	long long last = 0;
	if (FILE* f = fopen(stamp.c_str(), "r"))
	{
		if (fscanf(f, "%lld", &last) != 1)
			last = 0;
		fclose(f);
	}
	const long long now = (long long)time(nullptr);
	if (now - last < 24 * 3600 && now >= last)
		return false;
	if (FILE* f = fopen(stamp.c_str(), "w"))
	{
		fprintf(f, "%lld\n", now);
		fclose(f);
	}
	return true;
}
} // namespace mpcupdate
