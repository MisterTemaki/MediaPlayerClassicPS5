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
	const std::string mode = argv[2], master = "MPC PS5 synthetic test master";
	if (mode != "setup" && mode != "migrate" && mode != "disable") return 2;
	if (mode == "setup" && argc != 4) return 2;
	auto& settings = mpc::AppSettings(); settings.Load();
	auto& vault = mpc::credentials::Get();
	std::string error;
	if (mode == "setup")
	{
		std::string account, password, bare;
		if (!mpc::credentials::Split(argv[3], account, password, bare)) return 2;
		vault.Stage({{account, password}}); mpc::credentials::Wipe(password);
		settings.recent = {bare};
		settings.servers = {{"SMB test", bare.substr(0, bare.find_last_of('/'))}};
	}
	bool ok = vault.Exists() ? vault.Unlock(master, error) : vault.CreateOrChange(master, error);
	ok = ok && vault.Commit(error) && vault.SetRemembered(mode != "disable", error) && settings.Save();
	if (!ok) std::cerr << "Synthetic credential fixture could not be prepared: " << error << '\n';
	return ok ? 0 : 1;
}
