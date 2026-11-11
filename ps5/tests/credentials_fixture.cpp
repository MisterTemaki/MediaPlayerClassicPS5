// Host-only setup for synthetic SMB test credentials. Never linked into the PS5 application.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include "mpc_settings.h"
#include "OrbisPaths.h"
#include <cstdlib>
#include <iostream>
int main(int argc, char** argv)
{
	if (argc < 3 || !getenv("MPCPS5_TEST_FIXTURE")) return 2;
	setenv("MPCPS5_PS5_ROOT", argv[1], 1);
	if (!OrbisPathsInit()) return 2;
	const std::string mode = argv[2];
	if (mode != "setup" && mode != "migrate" && mode != "session") return 2;
	if (mode != "migrate" && argc != 4) return 2;
	auto& settings = mpc::AppSettings(); settings.Load();
	auto& store = mpc::credentials::Get();
	std::string error, bare;
	bool ok = store.Load(error);
	if (ok && mode != "migrate")
	{
		ok = store.SaveUrl(argv[3], mode == "setup", bare, error);
		settings.recent = {bare};
		settings.servers = {{"SMB test", bare.substr(0, bare.find_last_of('/'))}};
	}
	ok = ok && settings.Save();
	if (!ok) std::cerr << "Synthetic credential fixture failed: " << error << '\n';
	return ok ? 0 : 1;
}
