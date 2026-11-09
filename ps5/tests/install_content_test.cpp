// SPDX-License-Identifier: MIT
// Exercise the installer's real private comparator, including a file growing after stat().
#include "ProsperoInstall.cpp"

#include <filesystem>
#include <fstream>

static std::string growing_path;
static size_t snapshot_size;
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* path, struct stat* st)
{
	const int rc = __real_stat(path, st);
	if (rc == 0 && path == growing_path)
		st->st_size = off_t(snapshot_size); // metadata observed immediately before the file grew
	return rc;
}

int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	const std::string path = std::string(argv[1]) + "/contents";
	std::filesystem::create_directories(argv[1]);
	const unsigned char expected[] = {'a', 'b', 'c'};
	int failures = 0;
	auto test = [&](const std::string& bytes, size_t size, bool snapshot, bool want, const char* label) {
		{ std::ofstream file(path, std::ios::binary); file << bytes; if (!file) std::abort(); }
		growing_path = snapshot ? path : "";
		snapshot_size = size;
		const bool ok = SameContent(path, expected, size) == want;
		std::printf("%s %s\n", ok ? "ok  " : "FAIL", label);
		failures += !ok;
	};
	test("abc", 3, false, true, "identical file is unchanged");
	test("abx", 3, false, false, "changed bytes require replacement");
	test("a", 3, false, false, "short file requires replacement");
	test("abc", 0, true, false, "file growing from empty is not treated as unchanged");
	test("abc-extra-bytes", 3, true, false, "file growing after stat does not read beyond embedded data");
	return failures ? 1 : 0;
}
