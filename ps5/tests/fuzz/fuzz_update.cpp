// libFuzzer harness for the updater's parsers (ProsperoUpdate.cpp): GitHub's release JSON (ParseRelease), the
// release zip with its manifest (InstallZip, given the data's own SHA-256 so the zip reader and the manifest are
// reached), and update.journal (RecoverInterrupted on a scratch app folder). Host only, not part of the app:
//   for f in monocypher monocypher-ed25519; do clang -c -g -O1 -fsanitize=fuzzer-no-link,address,undefined \
//     third_party/monocypher/$f.c -o /tmp/$f.o; done
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -DORBIS_ROOT_DEFAULT='"/tmp/fuzzroot"' \
//     -Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Ithird_party/monocypher tests/fuzz/fuzz_update.cpp \
//     coreorbis/orbis-shims/ProsperoUpdate.cpp coreorbis/orbis-shims/orbis_paths.cpp /tmp/monocypher*.o -o fuzz_update
//   ./fuzz_update -max_total_time=300
//
// SPDX-License-Identifier: MIT
#include "ProsperoUpdate.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	static const std::string dir = [] {
		char tmpl[] = "/tmp/fuzz_update.XXXXXX";
		const char* d = mkdtemp(tmpl);
		return std::string(d ? d : "/tmp");
	}();
	if (size == 0)
		return 0;
	if ((data[0] & 3) == 2)
	{
		const std::string app = dir + "/japp";
		mkdir(app.c_str(), 0755);
		if (FILE* f = fopen((app + "/update.journal").c_str(), "wb"))
		{
			fwrite(data + 1, 1, size - 1, f);
			fclose(f);
		}
		update::RecoverInterrupted(app);
		unlink((app + "/update.journal").c_str());
		return 0;
	}
	if (data[0] & 1)
	{
		update::Release r;
		std::string why;
		update::ParseRelease(std::string(reinterpret_cast<const char*>(data + 1), size - 1), "PPSA99015", r, &why);
	}
	else
	{
		std::vector<uint8_t> zip(data + 1, data + size);
		update::Config c;
		c.title_id = "PPSA99015";
		c.app_dir = dir + "/app";
		update::InstallZip(zip, update::Sha256Hex(zip.data(), zip.size()), c);
	}
	return 0;
}
