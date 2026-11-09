// SPDX-License-Identifier: MIT
#include "OrbisPaths.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	setenv("MPCPS5_PS5_ROOT", argv[1], 1);
	setenv("MPCPS5_LOGS_DEFAULT", "1", 1);
	if (!OrbisPathsInit()) return 2;
	OrbisLogOpen();
	OrbisLog("short: %s", "https://user:ordinary-secret@host/media");
	const std::string secret(1400, 'x');
	OrbisLog("truncated: https://user:%s@host/media", secret.c_str());
	OrbisLog("port: https://host:8443/media");
	OrbisLog("ipv6: https://[::1]:8443/media");
	OrbisLogClose();
	std::ifstream file(std::filesystem::path(argv[1]) / "logs/boot.log");
	const std::string log{std::istreambuf_iterator<char>(file), {}};
	int failed = 0;
	auto check = [&](bool ok, const char* name) {
		std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
		failed += !ok;
	};
	check(log.find("ordinary-secret") == std::string::npos && log.find("user:***@host") != std::string::npos,
		"complete URL password is hidden");
	check(log.find(std::string(20, 'x')) == std::string::npos && log.find("truncated:") != std::string::npos,
		"truncated URL password is hidden before reaching logs");
	check(log.find("https://host:8443/media") != std::string::npos && log.find("https://[::1]:8443/media") != std::string::npos,
		"host ports and IPv6 URLs remain readable");
	return failed ? 1 : 0;
}
