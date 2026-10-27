// Updating the app from its GitHub releases (ProsperoUpdate.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoUpdate.h"

#include "OrbisPaths.h"
#include "monocypher-ed25519.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

namespace update
{
namespace
{
// ---- SHA-256 (FIPS 180-4) --------------------------------------------------------------------------------------
struct Sha256
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t buf[64];
	size_t used = 0;
	uint64_t bits = 0;

	static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void Block(const uint8_t* p)
	{
		static const uint32_t k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
			0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
			0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138,
			0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70,
			0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
			0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
			0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
	}

	void Add(const uint8_t* p, size_t n)
	{
		bits += uint64_t(n) * 8;
		while (n > 0)
		{
			if (used == 0 && n >= 64)
			{
				Block(p);
				p += 64;
				n -= 64;
				continue;
			}
			const size_t take = std::min(n, 64 - used);
			memcpy(buf + used, p, take);
			used += take;
			p += take;
			n -= take;
			if (used == 64)
			{
				Block(buf);
				used = 0;
			}
		}
	}

	std::string Hex()
	{
		const uint64_t total = bits;
		const uint8_t one = 0x80, zero = 0;
		Add(&one, 1);
		while (used != 56)
			Add(&zero, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++)
			len[i] = uint8_t(total >> (56 - 8 * i));
		Add(len, 8);
		char out[65];
		for (int i = 0; i < 8; i++)
			snprintf(out + i * 8, 9, "%08x", h[i]);
		return std::string(out, 64);
	}
};

// ---- a little JSON reading: the string, number fields of objects ---------------------------------------------------
// The end of the value (object, array or string) that opens at `at`, minding strings; npos when it doesn't close.
size_t ValueEnd(const std::string& s, size_t at)
{
	if (at >= s.size())
		return std::string::npos;
	if (s[at] == '"')
	{
		for (size_t i = at + 1; i < s.size(); i++)
		{
			if (s[i] == '\\')
				i++;
			else if (s[i] == '"')
				return i + 1;
		}
		return std::string::npos;
	}
	int depth = 0;
	bool in_str = false;
	for (size_t i = at; i < s.size(); i++)
	{
		const char c = s[i];
		if (in_str)
		{
			if (c == '\\')
				i++;
			else if (c == '"')
				in_str = false;
			continue;
		}
		if (c == '"')
			in_str = true;
		else if (c == '{' || c == '[')
			depth++;
		else if (c == '}' || c == ']')
		{
			if (--depth == 0)
				return i + 1;
		}
	}
	return std::string::npos;
}

std::string Unescape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] != '\\' || i + 1 >= s.size())
		{
			out += s[i];
			continue;
		}
		const char c = s[++i];
		switch (c)
		{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'u':
				if (i + 4 < s.size())
				{
					const unsigned cp = unsigned(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
					i += 4;
					if (cp < 0x80)
						out += char(cp);
					else if (cp < 0x800)
						out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
					else
						out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
				}
				break;
			default: out += c; break;
		}
	}
	return out;
}

// Top-level members of one object: key -> raw value text.
std::map<std::string, std::string> Members(const std::string& obj)
{
	std::map<std::string, std::string> m;
	size_t i = obj.find('{');
	if (i == std::string::npos)
		return m;
	i++;
	while (i < obj.size())
	{
		while (i < obj.size() && (isspace(static_cast<unsigned char>(obj[i])) || obj[i] == ','))
			i++;
		if (i >= obj.size() || obj[i] != '"')
			break;
		const size_t ke = ValueEnd(obj, i);
		if (ke == std::string::npos)
			break;
		const std::string key = Unescape(obj.substr(i + 1, ke - i - 2));
		i = obj.find(':', ke);
		if (i == std::string::npos)
			break;
		i++;
		while (i < obj.size() && isspace(static_cast<unsigned char>(obj[i])))
			i++;
		size_t ve;
		if (i < obj.size() && (obj[i] == '"' || obj[i] == '{' || obj[i] == '['))
			ve = ValueEnd(obj, i);
		else
		{
			ve = i;
			while (ve < obj.size() && obj[ve] != ',' && obj[ve] != '}')
				ve++;
		}
		if (ve == std::string::npos)
			break;
		m[key] = obj.substr(i, ve - i);
		i = ve;
	}
	return m;
}

std::string Str(const std::map<std::string, std::string>& m, const char* key)
{
	auto it = m.find(key);
	if (it == m.end() || it->second.size() < 2 || it->second[0] != '"')
		return "";
	return Unescape(it->second.substr(1, it->second.size() - 2));
}

// ---- files -----------------------------------------------------------------------------------------------------
bool SafeRel(const std::string& rel)
{
	if (rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos)
		return false;
	size_t a = 0;
	while (a <= rel.size())
	{
		size_t b = rel.find('/', a);
		if (b == std::string::npos)
			b = rel.size();
		const std::string part = rel.substr(a, b - a);
		if (part.empty() || part == "." || part == "..")
			return false;
		a = b + 1;
	}
	return true;
}

bool MakeDirs(const std::string& path)
{
	for (size_t i = 1; i <= path.size(); i++)
		if (i == path.size() || path[i] == '/')
		{
			const std::string p = path.substr(0, i);
			if (mkdir(p.c_str(), 0777) != 0 && errno != EEXIST)
				return false;
		}
	return true;
}

bool WriteFile(const std::string& path, const uint8_t* data, size_t size)
{
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos && !MakeDirs(path.substr(0, slash)))
		return false;
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
		return false;
	size_t off = 0;
	while (off < size)
	{
		const ssize_t n = write(fd, data + off, size - off);
		if (n <= 0)
		{
			close(fd);
			unlink(path.c_str());
			return false;
		}
		off += size_t(n);
	}
	const bool ok = fsync(fd) == 0;
	return close(fd) == 0 && ok;
}

std::string ReadText(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return "";
	std::string s;
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (1u << 20))
		s.append(buf, n);
	fclose(f);
	return s;
}

// manifest.sha256: "<hex> *<path>" (or "<hex>  <path>") lines -> path -> hex
std::map<std::string, std::string> ParseManifest(const std::string& text)
{
	std::map<std::string, std::string> m;
	size_t a = 0;
	while (a < text.size())
	{
		size_t b = text.find('\n', a);
		if (b == std::string::npos)
			b = text.size();
		std::string line = text.substr(a, b - a);
		a = b + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (line.size() < 66)
			continue;
		const std::string hex = line.substr(0, 64);
		size_t p = 64;
		while (p < line.size() && (line[p] == ' ' || line[p] == '*'))
			p++;
		std::string path = line.substr(p);
		if (path.compare(0, 2, "./") == 0)
			path = path.substr(2);
		std::string lower = hex;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		if (lower.find_first_not_of("0123456789abcdef") == std::string::npos && SafeRel(path))
			m[path] = lower;
	}
	return m;
}

uint32_t Le16(const uint8_t* p)
{
	return uint32_t(p[0]) | uint32_t(p[1]) << 8;
}
uint32_t Le32(const uint8_t* p)
{
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct ZipEntry
{
	std::string name;
	const uint8_t* data = nullptr;
	size_t size = 0;
};

// The stored (uncompressed) zip's files, from its central directory. "" or why not.
std::string ReadZip(const std::vector<uint8_t>& z, std::vector<ZipEntry>& out)
{
	if (z.size() < 22)
		return "the download isn't a zip";
	size_t eocd = std::string::npos;
	for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i <= 22 + 65535;)
		if (Le32(&z[i]) == 0x06054b50)
		{
			eocd = i;
			break;
		}
	if (eocd == std::string::npos)
		return "the download isn't a zip";
	const uint32_t count = Le16(&z[eocd + 10]);
	size_t cd = Le32(&z[eocd + 16]);
	for (uint32_t k = 0; k < count; k++)
	{
		if (cd + 46 > z.size() || Le32(&z[cd]) != 0x02014b50)
			return "the zip's directory is damaged";
		const uint32_t method = Le16(&z[cd + 10]);
		const uint32_t csize = Le32(&z[cd + 20]), usize = Le32(&z[cd + 24]);
		const uint32_t nlen = Le16(&z[cd + 28]), xlen = Le16(&z[cd + 30]), clen = Le16(&z[cd + 32]);
		const uint32_t local = Le32(&z[cd + 42]);
		if (cd + 46 + nlen > z.size())
			return "the zip's directory is damaged";
		ZipEntry e;
		e.name.assign(reinterpret_cast<const char*>(&z[cd + 46]), nlen);
		cd += 46 + nlen + xlen + clen;
		if (!e.name.empty() && e.name.back() == '/')
			continue; // a folder
		if (method != 0 || csize != usize)
			return "the zip is compressed (" + e.name + "): the release zip must be stored (zip -0)";
		if (size_t(local) + 30 > z.size() || Le32(&z[local]) != 0x04034b50)
			return "the zip is damaged at " + e.name;
		const size_t data = size_t(local) + 30 + Le16(&z[local + 26]) + Le16(&z[local + 28]);
		if (data + usize > z.size())
			return "the zip is cut short at " + e.name;
		e.data = &z[data];
		e.size = usize;
		out.push_back(e);
	}
	return "";
}

// ---- the worker ------------------------------------------------------------------------------------------------
std::mutex g_mu;
Config g_config;
Fetch g_fetch;
Status g_status;
Release g_found;
bool g_busy = false;
bool g_notify = false;
std::function<void(const std::string&)> g_on_newer;
std::atomic<size_t> g_bytes{0}, g_files{0}, g_files_total{0};

std::string ApiUrl()
{
	if (!g_config.api_url.empty())
		return g_config.api_url;
	return "https://api.github.com/repos/" + g_config.repo + "/releases/latest";
}

void* CheckMain(void*)
{
	std::vector<uint8_t> data;
	const std::string url = ApiUrl();
	OrbisLog("[update] checking %s", url.c_str());
	const int status = g_fetch(url, data, 2u << 20, nullptr);
	Release r;
	std::string why;
	if (status == 404)
		why = "This version has no releases on GitHub yet.";
	else if (status != 200)
		why = status < 0 ? "Couldn't reach GitHub (is the console online?)." : "GitHub answered " + std::to_string(status) + ".";
	else if (!ParseRelease(std::string(data.begin(), data.end()), g_config.title_id, r, &why))
		why = "The newest release has no " + g_config.title_id + " zip (" + why + ").";
	{
		std::lock_guard<std::mutex> l(g_mu);
		g_busy = false;
		if (!why.empty())
		{
			g_status.phase = Phase::Failed;
			g_status.error = why;
			OrbisLog("[update] check: %s", why.c_str());
			return nullptr;
		}
		g_found = r;
		g_status.latest = r.version;
		g_status.size = r.size;
		g_status.error.clear();
		g_status.phase = CompareVersions(r.version, g_config.version) > 0 ? Phase::Available : Phase::UpToDate;
		if (g_status.phase == Phase::Available && g_notify && g_on_newer)
			g_on_newer(r.version);
		OrbisLog("[update] newest release %s (%s, %llu bytes, sha-256 %s); this is %s: %s", r.tag.c_str(),
			r.zip_name.c_str(), (unsigned long long)r.size, r.sha256.empty() ? "not given" : r.sha256.c_str(),
			g_config.version.c_str(), g_status.phase == Phase::Available ? "newer" : "up to date");
	}
	return nullptr;
}

void* InstallMain(void*)
{
	Release r;
	{
		std::lock_guard<std::mutex> l(g_mu);
		r = g_found;
		g_status.phase = Phase::Downloading;
		g_status.done = 0;
		g_status.total = r.size;
	}
	g_bytes = 0;
	std::vector<uint8_t> zip;
	OrbisLog("[update] downloading %s", r.zip_url.c_str());
	const int status = g_fetch(r.zip_url, zip, size_t(std::max<uint64_t>(r.size, 1) + (1u << 20)), &g_bytes);
	std::string why;
	if (status != 200)
		why = status < 0 ? "The download failed (network)." : "The download failed (HTTP " + std::to_string(status) + ").";
	else if (r.size && zip.size() != r.size)
		why = "The download is " + std::to_string(zip.size()) + " bytes, not " + std::to_string(r.size) + ".";
	if (why.empty())
	{
		{
			std::lock_guard<std::mutex> l(g_mu);
			g_status.phase = Phase::Installing;
			g_status.done = 0;
			g_status.total = 0;
		}
		g_files = 0;
		g_files_total = 0;
		std::vector<uint8_t> sig;
		if (!g_config.release_key.empty() && !r.sig_url.empty())
		{
			const int st = g_fetch(r.sig_url, sig, 1024, nullptr);
			if (st != 200)
			{
				OrbisLog("[update] the signature couldn't be downloaded (%d)", st);
				sig.clear();
			}
		}
		why = InstallZip(zip, r.sha256, g_config, &g_files, &g_files_total, &sig);
	}
	std::lock_guard<std::mutex> l(g_mu);
	g_busy = false;
	if (!why.empty())
	{
		g_status.phase = Phase::Failed;
		g_status.error = why;
		OrbisLog("[update] install: %s", why.c_str());
	}
	else
	{
		g_status.phase = Phase::Done;
		g_status.error.clear();
		OrbisLog("[update] %s %s installed in %s", g_config.app_name.c_str(), r.version.c_str(), g_config.app_dir.c_str());
	}
	return nullptr;
}

bool Spawn(void* (*fn)(void*))
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 1 << 20);
	pthread_t t;
	const bool ok = pthread_create(&t, &attr, fn, nullptr) == 0;
	pthread_attr_destroy(&attr);
	return ok;
}
} // namespace

// ---- the parts ---------------------------------------------------------------------------------------------------
std::string Sha256Hex(const uint8_t* data, size_t size)
{
	Sha256 s;
	s.Add(data, size);
	return s.Hex();
}

int CompareVersions(const std::string& a, const std::string& b)
{
	auto parts = [](std::string s) {
		std::vector<long> v;
		if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
			s.erase(0, 1);
		size_t i = 0;
		while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])))
		{
			char* end = nullptr;
			v.push_back(strtol(s.c_str() + i, &end, 10));
			i = size_t(end - s.c_str());
			if (i < s.size() && s[i] == '.')
				i++;
			else
				break;
		}
		return v;
	};
	const std::vector<long> x = parts(a), y = parts(b);
	for (size_t i = 0; i < std::max(x.size(), y.size()); i++)
	{
		const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
		if (p != q)
			return p < q ? -1 : 1;
	}
	return 0;
}

bool ParseRelease(const std::string& json, const std::string& title_id, Release& out, std::string* why)
{
	const auto top = Members(json);
	out.tag = Str(top, "tag_name");
	if (out.tag.empty())
	{
		if (why)
			*why = "no tag";
		return false;
	}
	out.version = out.tag[0] == 'v' || out.tag[0] == 'V' ? out.tag.substr(1) : out.tag;
	auto it = top.find("assets");
	if (it == top.end())
	{
		if (why)
			*why = "no assets";
		return false;
	}
	const std::string& arr = it->second;
	const std::string suffix = "-" + title_id + ".zip";
	std::map<std::string, std::string> urls; // every asset's name -> its address (the zip's .sig among them)
	bool found = false;
	size_t i = 1;
	while (i < arr.size())
	{
		const size_t o = arr.find('{', i);
		if (o == std::string::npos)
			break;
		const size_t e = ValueEnd(arr, o);
		if (e == std::string::npos)
			break;
		const auto a = Members(arr.substr(o, e - o));
		const std::string name = Str(a, "name");
		urls[name] = Str(a, "browser_download_url");
		if (!found && name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
		{
			found = true;
			out.zip_name = name;
			out.zip_url = Str(a, "browser_download_url");
			auto sz = a.find("size");
			out.size = sz == a.end() ? 0 : strtoull(sz->second.c_str(), nullptr, 10);
			std::string d = Str(a, "digest");
			if (d.compare(0, 7, "sha256:") == 0)
				out.sha256 = d.substr(7);
			std::transform(out.sha256.begin(), out.sha256.end(), out.sha256.begin(), ::tolower);
		}
		i = e;
	}
	if (found)
	{
		if (out.zip_url.empty())
		{
			if (why)
				*why = "no download address";
			return false;
		}
		auto sig = urls.find(out.zip_name + ".sig");
		if (sig != urls.end())
			out.sig_url = sig->second;
		return true;
	}
	if (why)
		*why = "no asset named *" + suffix;
	return false;
}

// The install's journal (see InstallZip, step 3) and what puts an interrupted install back.
//
// update.journal is written whole (a temporary file, synced, renamed) before any app file moves:
//   mpcps5-update-journal 1 <transaction>
//   1<TAB><sha-256>TAB>eboot.bin   a file that replaced one (kept as <name>.old.<transaction>), the old one's SHA-256
//   0<TAB>-<TAB>sce_sys/extra.bin  a file the install adds
//   end
// A journal that doesn't read like that (empty, cut short, damaged) isn't guessed at: it stays, nothing is
// installed while it is there, and the app says to send the payload, which rewrites every file.
static const char* const kJournal = "update.journal";
static const char* const kJournalHead = "mpcps5-update-journal 1 ";

static bool Exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}

// host tests: MPCPS5_HOST_UPDATE_FAIL_<WHAT>=<file> makes that step fail for that file (errno EIO)
static bool FailForTest(const char* what, const std::string& rel)
{
#ifndef __PROSPERO__
	const char* f = getenv((std::string("MPCPS5_HOST_UPDATE_FAIL_") + what).c_str());
	if (f && rel == f)
	{
		errno = EIO;
		return true;
	}
#else
	(void)what;
	(void)rel;
#endif
	return false;
}

static bool SyncDir(const std::string& dir)
{
	if (FailForTest("SYNC", dir.substr(dir.rfind('/') + 1)))
		return false;
	const int fd = open(dir.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	const bool ok = fsync(fd) == 0;
	close(fd);
	return ok;
}

struct JournalEntry
{
	std::string rel;
	bool had = false;
	std::string old_sha; // the replaced file's SHA-256: tells "already put back" from "lost" when its copy is gone
};

static std::string FileSha(const std::string& path)
{
	const std::string t = ReadText(path);
	return Sha256Hex(reinterpret_cast<const uint8_t*>(t.data()), t.size());
}

struct Journal
{
	std::string tx;
	std::vector<JournalEntry> entries;
	std::string Old(const std::string& app_dir, const std::string& rel) const { return app_dir + "/" + rel + ".old." + tx; }
};

static std::string JournalText(const Journal& j)
{
	std::string t = kJournalHead + j.tx + "\n";
	for (const JournalEntry& e : j.entries)
		t += std::string(e.had ? "1\t" + e.old_sha : "0\t-") + "\t" + e.rel + "\n";
	return t + "end\n";
}

// The journal read back; false when it isn't a whole, well-formed one.
static bool ParseJournal(const std::string& text, Journal& j)
{
	j = Journal{};
	const size_t nl = text.find('\n');
	if (nl == std::string::npos || text.compare(0, strlen(kJournalHead), kJournalHead) != 0)
		return false;
	j.tx = text.substr(strlen(kJournalHead), nl - strlen(kJournalHead));
	if (j.tx.empty() || j.tx.size() > 32 || j.tx.find_first_not_of("0123456789-") != std::string::npos)
		return false;
	size_t pos = nl + 1;
	bool ended = false;
	while (pos < text.size())
	{
		size_t e = text.find('\n', pos);
		if (e == std::string::npos)
			return false; // cut short
		const std::string line = text.substr(pos, e - pos);
		pos = e + 1;
		if (ended)
			return false; // something after "end"
		if (line == "end")
		{
			ended = true;
			continue;
		}
		const size_t tab2 = line.find('\t', 2);
		if (line.size() < 5 || line[1] != '\t' || (line[0] != '0' && line[0] != '1') || tab2 == std::string::npos)
			return false;
		JournalEntry je{line.substr(tab2 + 1), line[0] == '1', line.substr(2, tab2 - 2)};
		const bool sha_ok = je.had ? je.old_sha.size() == 64 &&
										 je.old_sha.find_first_not_of("0123456789abcdef") == std::string::npos
								   : je.old_sha == "-";
		if (!SafeRel(je.rel) || !sha_ok)
			return false;
		j.entries.push_back(je);
	}
	return ended && !j.entries.empty();
}

// Written whole: a temporary file (synced), renamed over the journal, the folder synced.
static bool WriteJournal(const std::string& app_dir, const Journal& j)
{
	const std::string path = app_dir + "/" + kJournal, tmp = path + ".tmp";
	const std::string t = JournalText(j);
	if (FailForTest("JOURNAL", kJournal) || !WriteFile(tmp, reinterpret_cast<const uint8_t*>(t.data()), t.size()) ||
		rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return SyncDir(app_dir);
}

// Every folder a file of the journal is in (the app folder, sce_sys...), synced so the renames are on the disk.
static bool SyncDirsOf(const std::string& app_dir, const Journal& j)
{
	std::set<std::string> dirs = {app_dir};
	for (const JournalEntry& e : j.entries)
	{
		const size_t slash = e.rel.rfind('/');
		if (slash != std::string::npos)
			dirs.insert(app_dir + "/" + e.rel.substr(0, slash));
	}
	bool ok = true;
	for (const std::string& d : dirs)
		if (!SyncDir(d))
		{
			OrbisLog("[update] couldn't sync %s to the disk (errno %d)", d.c_str(), errno);
			ok = false;
		}
	return ok;
}

// Puts the old version back for the journal's files: each old copy renamed back over the new file, a file the
// install added removed, a <name>.new not yet moved deleted. Returns the entries that could NOT be put back and can
// be tried again (their old copy is still there); `lost` gets the files whose old copy is gone.
static std::vector<JournalEntry> PutBack(const std::string& app_dir, const Journal& j, std::vector<std::string>* lost)
{
	std::vector<JournalEntry> left;
	for (auto it = j.entries.rbegin(); it != j.entries.rend(); ++it)
	{
		const std::string f = app_dir + "/" + it->rel;
		const std::string old = j.Old(app_dir, it->rel);
		const bool moved = !Exists(f + ".new"); // still beside it: never put in place, the old file untouched
		if (it->had)
		{
			if (Exists(old))
			{
				if (FailForTest("RESTORE", it->rel) || rename(old.c_str(), f.c_str()) != 0)
				{
					OrbisLog("[update] couldn't put back the old %s (errno %d)", it->rel.c_str(), errno);
					left.push_back(*it);
					continue;
				}
			}
			else if (moved && FileSha(f) != it->old_sha && lost)
				lost->push_back(it->rel); // not the old file, and its old copy is gone (else: already put back)
		}
		else if (moved && Exists(f) && unlink(f.c_str()) != 0)
		{
			OrbisLog("[update] couldn't remove %s (errno %d)", it->rel.c_str(), errno);
			left.push_back(*it);
			continue;
		}
		unlink((f + ".new").c_str());
	}
	std::reverse(left.begin(), left.end());
	return left;
}

// After PutBack: the journal goes only when everything is back and on the disk; else it keeps what is left.
static bool SettleJournal(const std::string& app_dir, const Journal& j, const std::vector<JournalEntry>& left)
{
	const bool synced = SyncDirsOf(app_dir, j);
	if (left.empty() && synced)
	{
		const std::string journal = app_dir + "/" + kJournal;
		if (unlink(journal.c_str()) == 0 && SyncDir(app_dir))
			return true;
		OrbisLog("[update] couldn't remove %s", journal.c_str());
		return false;
	}
	if (!left.empty())
	{
		Journal rest = j;
		rest.entries = left;
		if (!WriteJournal(app_dir, rest))
			OrbisLog("[update] couldn't rewrite the journal: it keeps every file");
	}
	return false;
}

static std::string NewTransaction()
{
	timespec ts = {};
	clock_gettime(CLOCK_REALTIME, &ts);
	return std::to_string((long long)ts.tv_sec) + "-" + std::to_string((long long)getpid());
}

bool SignatureValid(const std::vector<uint8_t>& zip, const std::vector<uint8_t>& signature, const std::string& key_hex)
{
	if (signature.size() != 64 || key_hex.size() != 64)
		return false;
	uint8_t key[32];
	for (int i = 0; i < 32; i++)
	{
		const std::string byte = key_hex.substr(size_t(i) * 2, 2);
		if (byte.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
			return false;
		key[i] = uint8_t(strtoul(byte.c_str(), nullptr, 16));
	}
	return crypto_ed25519_check(signature.data(), key, zip.data(), zip.size()) == 0;
}

std::string InstallZip(const std::vector<uint8_t>& zip, const std::string& expect_sha256, const Config& c,
	std::atomic<size_t>* files_done, std::atomic<size_t>* files_total, const std::vector<uint8_t>* signature)
{
	if (!c.cant_install.empty())
		return c.cant_install;
	// an earlier install that isn't put back yet: its old copies are the only way back, so nothing is installed
	// over it (RecoverInterrupted runs first; if the journal is still there, it couldn't finish)
	if (Exists(c.app_dir + "/" + kJournal))
	{
		RecoverInterrupted(c.app_dir);
		if (Exists(c.app_dir + "/" + kJournal))
			return "An earlier update isn't finished being put back: nothing was installed. Send the payload again to "
				   "repair the app, then try again.";
	}
	// GitHub gives every release file's SHA-256 ("digest"); a download it can't be checked against isn't installed
	if (expect_sha256.size() != 64)
		return "GitHub gave no SHA-256 for the download, so it can't be checked: nothing was changed.";
	if (Sha256Hex(zip.data(), zip.size()) != expect_sha256)
		return "The download's SHA-256 doesn't match GitHub's: nothing was changed.";
	// signed by the app's own key (built in): a release GitHub serves but this project's key didn't sign isn't
	// installed, even with a matching SHA-256
	if (!c.release_key.empty())
	{
		if (!signature || signature->empty())
			return "The release has no signature (.sig) from this app's key: nothing was changed.";
		if (!SignatureValid(zip, *signature, c.release_key))
			return "The release's signature doesn't match this app's key: nothing was changed.";
		OrbisLog("[update] the release's Ed25519 signature checks out");
	}
	else
		OrbisLog("[update] this build has no release key: the signature isn't checked (SHA-256 only)");
	std::vector<ZipEntry> entries;
	std::string why = ReadZip(zip, entries);
	if (!why.empty())
		return why + ": nothing was changed.";
	const std::string prefix = c.title_id + "/";
	std::map<std::string, const ZipEntry*> files;
	for (const ZipEntry& e : entries)
	{
		if (e.name.compare(0, prefix.size(), prefix) != 0 || !SafeRel(e.name.substr(prefix.size())))
			return "The zip holds " + e.name + ", outside " + prefix + ": nothing was changed.";
		files[e.name.substr(prefix.size())] = &e;
	}
	auto man_it = files.find("manifest.sha256");
	if (man_it == files.end())
		return "The zip has no manifest.sha256: nothing was changed.";
	const std::string man_text(reinterpret_cast<const char*>(man_it->second->data), man_it->second->size);
	const auto manifest = ParseManifest(man_text);
	if (manifest.empty())
		return "The zip's manifest is empty: nothing was changed.";
	for (const char* must : {"eboot.bin", "sce_sys/param.json"})
		if (!manifest.count(must))
			return std::string("The release has no ") + must + ": nothing was changed.";
	for (const auto& kv : manifest)
	{
		auto f = files.find(kv.first);
		if (f == files.end())
			return "The zip lacks " + kv.first + ": nothing was changed.";
		if (Sha256Hex(f->second->data, f->second->size) != kv.second)
			return kv.first + " doesn't match the manifest: nothing was changed.";
	}
	// the release is ours: its param.json names this title
	{
		const ZipEntry* p = files["sce_sys/param.json"];
		const std::string param(reinterpret_cast<const char*>(p->data), p->size);
		if (param.find("\"" + c.title_id + "\"") == std::string::npos)
			return "The release's param.json isn't for " + c.title_id + ": nothing was changed.";
	}
	if (files_total)
		*files_total = manifest.size();
	// 1. every file beside the old one as <name>.new
	std::vector<std::string> written;
	auto undo = [&] {
		for (const std::string& rel : written)
			unlink((c.app_dir + "/" + rel + ".new").c_str());
	};
	std::vector<std::string> order;
	for (const auto& kv : manifest)
		if (kv.first != "eboot.bin")
			order.push_back(kv.first);
	order.push_back("eboot.bin");
	for (const std::string& rel : order)
	{
		const ZipEntry* e = files[rel];
		if (!WriteFile(c.app_dir + "/" + rel + ".new", e->data, e->size))
		{
			undo();
			return "Couldn't write " + c.app_dir + "/" + rel + " (disk full?): nothing was changed.";
		}
		written.push_back(rel);
		if (files_done)
			(*files_done)++;
	}
	if (!WriteFile(c.app_dir + "/manifest.sha256.new", man_it->second->data, man_it->second->size))
	{
		undo();
		return "Couldn't write the manifest: nothing was changed.";
	}
	written.push_back("manifest.sha256");
	// 2. the old manifest, for the files that are gone
	const auto old_manifest = ParseManifest(ReadText(c.app_dir + "/manifest.sha256"));
	// 3. into place, eboot.bin then the manifest last, each old file kept as <name>.old.<transaction> until every new
	// one is in and on the disk. update.journal lists the files while this goes on: a rename that fails puts the old
	// ones back, and an install cut off half way (the console turned off) is put back when the app next starts.
	Journal jr;
	jr.tx = NewTransaction();
	for (const std::string& rel : written)
	{
		const std::string f = c.app_dir + "/" + rel;
		const bool had = Exists(f);
		jr.entries.push_back({rel, had, had ? FileSha(f) : "-"});
	}
	if (!WriteJournal(c.app_dir, jr))
	{
		unlink((c.app_dir + "/" + kJournal).c_str());
		undo();
		return "Couldn't write the update's journal (disk full?): nothing was changed.";
	}
	std::vector<std::pair<std::string, bool>> moved; // the files in place, and whether they replaced one
	auto give_back = [&](const std::string& head) {
		std::vector<std::string> lost;
		const std::vector<JournalEntry> left = PutBack(c.app_dir, jr, &lost);
		const bool all = SettleJournal(c.app_dir, jr, left) && lost.empty();
		if (all)
			return head + "the old version was put back.";
		if (left.empty() && lost.empty())
			return head + "the old version was put back, but the disk didn't confirm it; it is checked again when the "
						  "app starts.";
		OrbisLog("[update] %zu file(s) not put back, %zu without their old copy", left.size(), lost.size());
		return head + "the old version couldn't be put back completely (" + std::to_string(left.size() + lost.size()) +
			   " file(s)); it is tried again when the app starts. If the app doesn't open, send the payload again "
			   "to repair it.";
	};
	for (const std::string& rel : written)
	{
		const std::string f = c.app_dir + "/" + rel;
		const bool had = Exists(f);
		if ((had && rename(f.c_str(), jr.Old(c.app_dir, rel).c_str()) != 0) || FailForTest("RENAME", rel) ||
			rename((f + ".new").c_str(), f.c_str()) != 0)
			return give_back("Couldn't replace " + rel + " (errno " + std::to_string(errno) + "): ");
		moved.push_back({rel, had});
		if (files_done)
			(*files_done)++;
	}
	// all in: on the disk first (else everything goes back), then the journal goes, then the old copies
	if (!SyncDirsOf(c.app_dir, jr))
		return give_back("The new files couldn't be written to the disk for sure: ");
	if (unlink((c.app_dir + "/" + kJournal).c_str()) != 0 || !SyncDir(c.app_dir))
		OrbisLog("[update] the journal couldn't be removed: the next start will check the files again");
	for (const auto& m : moved)
		if (m.second)
			unlink(jr.Old(c.app_dir, m.first).c_str());
	for (const auto& kv : old_manifest)
		if (!manifest.count(kv.first) && kv.first != "manifest.sha256")
		{
			unlink((c.app_dir + "/" + kv.first).c_str());
			OrbisLog("[update] removed %s (not in the new release)", kv.first.c_str());
		}
	// 4. the home screen's copy of the art (it only reads it from there)
	struct stat st;
	if (!c.appmeta_dir.empty() && stat(c.appmeta_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
		for (const auto& kv : manifest)
			if (kv.first.compare(0, 8, "sce_sys/") == 0 && kv.first.find('/', 8) == std::string::npos)
			{
				const ZipEntry* e = files[kv.first];
				if (!WriteFile(c.appmeta_dir + "/" + kv.first.substr(8), e->data, e->size))
					OrbisLog("[update] couldn't refresh %s/%s", c.appmeta_dir.c_str(), kv.first.substr(8).c_str());
			}
	return "";
}

// ---- the worker's face -------------------------------------------------------------------------------------------
namespace
{
std::string g_startup_problem;
}

int RecoverInterrupted(const std::string& app_dir)
{
	const std::string journal = app_dir + "/" + kJournal;
	if (!Exists(journal))
		return 0;
	Journal j;
	if (!ParseJournal(ReadText(journal), j))
	{
		// not a whole journal: which files moved can't be told, so nothing is touched and the journal stays
		OrbisLog("[update] %s is there but can't be read as a whole journal: left alone; the payload repairs the app",
			journal.c_str());
		g_startup_problem = "An update was cut off and its record is damaged, so the app can't put itself back. "
							"Send the payload again to repair it.";
		return 0;
	}
	std::vector<std::string> lost;
	const std::vector<JournalEntry> left = PutBack(app_dir, j, &lost);
	const bool all = SettleJournal(app_dir, j, left);
	const int back = int(j.entries.size() - left.size() - lost.size());
	for (const std::string& rel : lost)
		OrbisLog("[update] %s: its old copy is gone, it can't be put back", rel.c_str());
	if (all && lost.empty())
		OrbisLog("[update] an install was cut off half way: the old version put back (%d file(s))", back);
	else
	{
		OrbisLog("[update] an install was cut off half way: %d file(s) put back, %zu not (journal %s), %zu without their "
				 "old copy",
			back, left.size(), all ? "removed" : "kept for the next start", lost.size());
		g_startup_problem = "An update was cut off and the app couldn't be fully put back as it was. If something "
							"doesn't work, send the payload again to repair it.";
	}
	return back;
}

std::string StartupProblem()
{
	return g_startup_problem;
}

void Init(const Config& config, Fetch fetch)
{
	std::lock_guard<std::mutex> l(g_mu);
	g_config = config;
	g_fetch = std::move(fetch);
	if (!config.app_dir.empty() && config.cant_install.empty())
		RecoverInterrupted(config.app_dir);
}

void SetCurrentVersion(const std::string& version)
{
	std::lock_guard<std::mutex> l(g_mu);
	if (!version.empty() && version.size() < 32)
		g_config.version = version;
}

const Config& CurrentConfig()
{
	return g_config;
}

void SetOnNewer(std::function<void(const std::string&)> on_newer)
{
	std::lock_guard<std::mutex> l(g_mu);
	g_on_newer = std::move(on_newer);
}

bool StartCheck(bool notify)
{
	std::lock_guard<std::mutex> l(g_mu);
	if (g_busy || !g_fetch)
		return false;
	// a download or an install done stays shown until the app is opened again
	if (g_status.phase == Phase::Done)
		return false;
	g_busy = true;
	g_notify = notify;
	g_status = Status{};
	g_status.phase = Phase::Checking;
	if (!Spawn(CheckMain))
	{
		g_busy = false;
		g_status.phase = Phase::Failed;
		g_status.error = "no thread";
		return false;
	}
	return true;
}

bool StartInstall()
{
	std::lock_guard<std::mutex> l(g_mu);
	if (g_busy || !g_fetch || g_status.phase != Phase::Available)
		return false;
	if (!g_config.cant_install.empty())
	{
		OrbisLog("[update] not installed: %s", g_config.cant_install.c_str());
		return false;
	}
	g_busy = true;
	if (!Spawn(InstallMain))
	{
		g_busy = false;
		return false;
	}
	return true;
}

Status GetStatus()
{
	std::lock_guard<std::mutex> l(g_mu);
	Status s = g_status;
	if (s.phase == Phase::Downloading)
		s.done = g_bytes;
	else if (s.phase == Phase::Installing)
	{
		s.done = g_files;
		s.total = g_files_total;
	}
	return s;
}

void Acknowledge()
{
	std::lock_guard<std::mutex> l(g_mu);
	if (!g_busy && (g_status.phase == Phase::Failed || g_status.phase == Phase::UpToDate))
		g_status.phase = Phase::Idle;
}

const char* PhaseName(Phase p)
{
	switch (p)
	{
		case Phase::Idle: return "idle";
		case Phase::Checking: return "checking";
		case Phase::UpToDate: return "uptodate";
		case Phase::Available: return "available";
		case Phase::Downloading: return "downloading";
		case Phase::Installing: return "installing";
		case Phase::Done: return "done";
		case Phase::Failed: return "failed";
	}
	return "idle";
}

std::string Encode(const Status& s)
{
	std::string err = s.error;
	std::replace(err.begin(), err.end(), '|', '/');
	char buf[128];
	snprintf(buf, sizeof(buf), "%s|%s|%llu|%llu|%llu|", PhaseName(s.phase), s.latest.c_str(), (unsigned long long)s.size,
		(unsigned long long)s.done, (unsigned long long)s.total);
	return buf + err;
}

bool Decode(const std::string& text, Status& s)
{
	std::vector<std::string> f;
	size_t a = 0;
	for (int i = 0; i < 5; i++)
	{
		const size_t b = text.find('|', a);
		if (b == std::string::npos)
			return false;
		f.push_back(text.substr(a, b - a));
		a = b + 1;
	}
	f.push_back(text.substr(a));
	s = Status{};
	for (Phase p : {Phase::Idle, Phase::Checking, Phase::UpToDate, Phase::Available, Phase::Downloading, Phase::Installing,
			 Phase::Done, Phase::Failed})
		if (f[0] == PhaseName(p))
			s.phase = p;
	s.latest = f[1];
	s.size = strtoull(f[2].c_str(), nullptr, 10);
	s.done = strtoull(f[3].c_str(), nullptr, 10);
	s.total = strtoull(f[4].c_str(), nullptr, 10);
	s.error = f[5];
	return true;
}
} // namespace update
