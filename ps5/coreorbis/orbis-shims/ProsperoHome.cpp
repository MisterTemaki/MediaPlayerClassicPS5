// Media Player Classic PS5 1.7: the home screen's category in param.json (ProsperoHome.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoHome.h"

#include "OrbisPaths.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
bool ReadText(const std::string& path, std::string& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.assign(65536, '\0');
	out.resize(fread(&out[0], 1, out.size(), f));
	const bool whole = feof(f) != 0;
	fclose(f);
	return whole;
}

// written beside it, synced, then renamed over it (never half a param.json)
bool WriteText(const std::string& path, const std::string& text)
{
	const std::string part = path + ".part";
	const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
		return false;
	size_t off = 0;
	while (off < text.size())
	{
		const ssize_t n = write(fd, text.data() + off, text.size() - off);
		if (n <= 0)
		{
			close(fd);
			unlink(part.c_str());
			return false;
		}
		off += size_t(n);
	}
	// (1.7.1, audit D10) closed whatever fsync said (up to 1.7.0 a failed fsync left the descriptor open)
	const bool synced = fsync(fd) == 0;
	const bool closed = close(fd) == 0;
	if (!synced || !closed || rename(part.c_str(), path.c_str()) != 0)
	{
		unlink(part.c_str());
		return false;
	}
	return true;
}

// the value param.json gives applicationCategoryType ("" when it has none, or not a number)
std::string CategoryOf(const std::string& param)
{
	const std::string key = "\"applicationCategoryType\"";
	const size_t k = param.find(key);
	size_t a = k == std::string::npos ? k : param.find(':', k + key.size());
	if (a == std::string::npos)
		return "";
	a++;
	while (a < param.size() && (param[a] == ' ' || param[a] == '\t'))
		a++;
	size_t b = a;
	while (b < param.size() && isdigit(static_cast<unsigned char>(param[b])))
		b++;
	return param.substr(a, b - a);
}

// 1 written, 0 already so, -1 not (missing: -2; no category in it to change: -3)
int ApplyTo(const std::string& path, int category, std::string* error)
{
	std::string text;
	if (!ReadText(path, text))
		return -2;
	if (CategoryOf(text).empty())
		return -3; // (review L10: not reported as done)
	const std::string want = WithHomeCategory(text, category);
	if (want == text)
		return 0;
	if (!WriteText(path, want))
	{
		OrbisLog("[home] %s couldn't be written", path.c_str());
		if (error)
			*error = "the app's param.json couldn't be written";
		return -1;
	}
	return 1;
}
} // namespace

int HomeCategory()
{
	int category = 0;
	if (FILE* f = fopen((OrbisRoot() + "/mpc-hc.ini").c_str(), "r"))
	{
		char line[512];
		while (fgets(line, sizeof(line), f))
			if (strncmp(line, "HomeScreenCategory=", 19) == 0)
				category = atoi(line + 19) == 1 ? 1 : 0;
		fclose(f);
	}
	return category;
}

std::string WithHomeCategory(const std::string& param, int category)
{
	const std::string key = "\"applicationCategoryType\"";
	const size_t k = param.find(key);
	if (k == std::string::npos)
		return param;
	size_t a = param.find(':', k + key.size());
	if (a == std::string::npos)
		return param;
	a++;
	while (a < param.size() && (param[a] == ' ' || param[a] == '\t'))
		a++;
	size_t b = a;
	while (b < param.size() && isdigit(static_cast<unsigned char>(param[b])))
		b++;
	if (b == a)
		return param;
	return param.substr(0, a) + (category == 1 ? "0" : "65536") + param.substr(b);
}

int ApplyHomeCategory(const std::string& dir, const std::string& appmeta, int category, std::string* error)
{
	const int app = ApplyTo(dir + "/sce_sys/param.json", category, error);
	if (app == -2)
	{
		OrbisLog("[home] %s/sce_sys/param.json isn't there", dir.c_str());
		if (error)
			*error = "the app's param.json isn't there";
		return -1;
	}
	if (app == -3)
	{
		OrbisLog("[home] %s/sce_sys/param.json has no applicationCategoryType number", dir.c_str());
		if (error)
			*error = "the app's param.json has no category to change";
		return -1;
	}
	if (app < 0)
		return -1;
	const int meta = appmeta.empty() ? 0 : ApplyTo(appmeta + "/param.json", category, nullptr); // (as SyncAppMeta: best effort)
	// (-3: no category in the home screen's copy: left as it is, as one that isn't there)
	OrbisLog("[home] %s: %s (the home screen's copy: %s)", category == 1 ? "Games" : "Media",
		app == 1 ? "param.json written" : "param.json already says so",
		meta == 1 ? "written" : meta == 0 ? "the same" : meta == -2 || meta == -3 ? "not there" : "not written");
	return app == 1 || meta == 1 ? 1 : 0;
}
