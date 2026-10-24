// Media Player Classic PS5: the /data/mpcps5 folder layout and the boot log (include-orbis/OrbisPaths.h).
//
// stat(), not access(): PS5SX2 found (vk-285-46) that a sandboxed process can get EPERM from access() on
// files that exist, while stat() answers on both sides of a jailbreak. The same rule is kept here.
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace
{
std::mutex s_log_mutex;
FILE* s_log = nullptr;
bool s_enabled = true; // the "Debug logs" setting
std::string s_name;    // the log OrbisLogOpen opened (or would have), to open again when logs are turned on
bool s_rotated = false; // this run's log file was started (the one before kept as <name>.prev.log)

// The "Debug logs" setting, read here (not through mpc::Settings: the installer and the helper have none): off unless
// mpc-hc.ini says debug_logs=1. The host tests turn the default on with MPCPS5_LOGS_DEFAULT=1 (they read boot.log).
bool IniSaysLogsOff()
{
	const char* def = getenv("MPCPS5_LOGS_DEFAULT");
	bool off = !(def && atoi(def) != 0);
	FILE* f = fopen((OrbisRoot() + "/mpc-hc.ini").c_str(), "r");
	if (!f)
		return off;
	char line[512];
	while (fgets(line, sizeof(line), f))
		if (strncmp(line, "debug_logs=", 11) == 0)
			off = atoi(line + 11) == 0;
	fclose(f);
	return off;
}

const char* const kSubdirs[] = {"media", "logs", "fonts", "screenshots"};
} // namespace

const std::string& OrbisRoot()
{
	static const std::string root = [] {
		const char* env = getenv("MPCPS5_PS5_ROOT");
		return std::string(env && *env ? env : ORBIS_ROOT_DEFAULT);
	}();
	return root;
}

std::string OrbisDir(const char* sub)
{
	return OrbisRoot() + "/" + sub;
}

bool OrbisIsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool OrbisIsFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool OrbisMkdirs(const std::string& path)
{
	if (path.empty())
		return false;
	std::string cur;
	size_t pos = 0;
	while (pos != std::string::npos)
	{
		pos = path.find('/', pos + 1);
		cur = path.substr(0, pos);
		if (cur.empty() || OrbisIsDir(cur))
			continue;
		if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST)
			return false;
	}
	return OrbisIsDir(path);
}

bool OrbisPathsInit()
{
	if (!OrbisMkdirs(OrbisRoot()))
		return false;
	for (const char* sub : kSubdirs)
		OrbisMkdirs(OrbisDir(sub));
	return true;
}

std::vector<std::string> OrbisRomRoots()
{
	std::vector<std::string> roots;
	const std::string internal = OrbisDir("media");
	if (OrbisIsDir(internal))
		roots.push_back(internal);
	char buf[64];
	for (int i = 0; i < 8; i++)
	{
		snprintf(buf, sizeof(buf), "/mnt/usb%d", i);
		if (OrbisIsDir(buf))
			roots.push_back(buf);
	}
	for (int i = 0; i < 2; i++)
	{
		snprintf(buf, sizeof(buf), "/mnt/ext%d", i);
		if (OrbisIsDir(buf))
			roots.push_back(buf);
	}
	return roots;
}

// Lines logged before the log file is open (the native app logs its jailbreak before /data is visible).
static std::vector<std::string>& EarlyLines()
{
	static std::vector<std::string>* lines = new std::vector<std::string>();
	return *lines;
}

void OrbisLogOpen(const char* name)
{
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_log)
		return;
	s_name = name;
	if (IniSaysLogsOff())
	{
		// off: nothing written, and the files of earlier runs are not rotated away
		s_enabled = false;
		EarlyLines().clear();
		return;
	}
	s_enabled = true;
	const std::string dir = OrbisDir("logs");
	if (!OrbisIsDir(dir))
		return;
	const std::string cur = dir + "/" + name + ".log";
	const std::string prev = dir + "/" + name + ".prev.log";
	rename(cur.c_str(), prev.c_str()); // keep the run before, like PS5SX2's logs
	s_log = fopen(cur.c_str(), "w");
	s_rotated = s_log != nullptr;
	if (s_log)
	{
		for (const std::string& line : EarlyLines())
		{
			fputs(line.c_str(), s_log);
			printf("[mpc-ps5 %s", line.c_str() + 1); // held back until the setting could be read
		}
		fflush(stdout);
		fflush(s_log);
		EarlyLines().clear();
	}
}

int OrbisLogFd()
{
	// the open boot.log's fd, for the crash handler's signal-safe write(); -1 before it is open
	return s_log ? fileno(s_log) : -1;
}

// A password in an address (scheme://user:password@host/...) as "***", in place: an smb:// share's password, or an
// http:// one, never reaches a log or the console's output, whoever logs the address.
void OrbisHidePasswords(char* line)
{
	for (char* p = strstr(line, "://"); p; p = strstr(p, "://"))
	{
		char* auth = p + 3;
		char* end = auth;
		while (*end && *end != '/' && *end != ' ' && *end != '\t' && *end != '"' && *end != '\'' && *end != ')' &&
			   *end != ',')
			end++;
		char* at = nullptr;
		for (char* q = auth; q < end; q++)
			if (*q == '@')
				at = q; // the last @: the host follows it
		char* colon = nullptr;
		if (at)
			for (char* q = auth; q < at; q++)
				if (*q == ':')
				{
					colon = q;
					break;
				}
		if (colon && at > colon + 1)
		{
			// user:secret@ -> user:***@ (the line only gets shorter or keeps its length)
			const size_t secret = size_t(at - colon - 1);
			if (secret >= 3)
			{
				memcpy(colon + 1, "***", 3);
				memmove(colon + 4, at, strlen(at) + 1);
			}
			else
				memset(colon + 1, '*', secret);
		}
		p = auth;
	}
}

void OrbisLog(const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	OrbisHidePasswords(line);

	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const double t = ts.tv_sec + ts.tv_nsec / 1e9;

	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (!s_enabled)
		return;
	if (s_log || !s_name.empty())
	{
		printf("[mpc-ps5 %10.3f] %s\n", t, line);
		fflush(stdout);
	}
	if (s_log)
	{
		fprintf(s_log, "[%10.3f] %s\n", t, line);
		fflush(s_log); // a crash must not lose the lines before it
	}
	else if (EarlyLines().size() < 4000)
	{
		char stamped[1100];
		snprintf(stamped, sizeof(stamped), "[%10.3f] %s\n", t, line);
		EarlyLines().push_back(stamped);
	}
}

void OrbisLogSetEnabled(bool on)
{
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (on == s_enabled)
		return;
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const double t = ts.tv_sec + ts.tv_nsec / 1e9;
	if (!on)
	{
		printf("[mpc-ps5 %10.3f] [log] debug logs turned off\n", t);
		fflush(stdout);
		if (s_log)
		{
			fprintf(s_log, "[%10.3f] [log] debug logs turned off\n", t);
			fclose(s_log);
			s_log = nullptr;
		}
		EarlyLines().clear();
		s_enabled = false;
		return;
	}
	s_enabled = true;
	// this run's file again, added to (its lines before the switch stay in it); the first one of the run (logs off at the
	// start, turned on in Options) starts a new file, the last run's kept as <name>.prev.log
	const std::string dir = OrbisDir("logs");
	if (!s_name.empty() && OrbisIsDir(dir))
	{
		const std::string cur = dir + "/" + s_name + ".log";
		if (!s_rotated)
			rename(cur.c_str(), (dir + "/" + s_name + ".prev.log").c_str());
		s_log = fopen(cur.c_str(), s_rotated ? "a" : "w");
		s_rotated = s_rotated || s_log != nullptr;
	}
	printf("[mpc-ps5 %10.3f] [log] debug logs turned on\n", t);
	fflush(stdout);
	if (s_log)
	{
		fprintf(s_log, "[%10.3f] [log] debug logs turned on\n", t);
		fflush(s_log);
	}
}

void OrbisLogRefresh()
{
	bool open;
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		open = !s_name.empty();
	}
	if (open)
		OrbisLogSetEnabled(!IniSaysLogsOff());
}

bool OrbisLogEnabled()
{
	std::lock_guard<std::mutex> lock(s_log_mutex);
	return s_enabled;
}

void OrbisLogClose()
{
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_log)
	{
		fclose(s_log);
		s_log = nullptr;
	}
}
