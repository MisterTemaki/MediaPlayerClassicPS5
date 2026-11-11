// Network passwords encrypted with Monocypher XChaCha20-Poly1305 and a private random local key.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include "OrbisPaths.h"
#include "monocypher.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// The same operating-system entropy source already used by the bundled Mbed TLS transport.
extern "C" int mbedtls_hardware_poll(void*, unsigned char*, size_t, size_t*);

namespace mpc
{
namespace credentials
{
namespace
{
constexpr size_t Header = 64, MaxPayload = 1024 * 1024, MaxEntries = 256;
constexpr unsigned char Magic[] = {'M', 'P', 'C', 'E', 'N', 'C', '0', '1'};
using Bytes = std::vector<unsigned char>;
bool Fail(const char* operation)
{
#ifndef __PROSPERO__
	const char* fail = getenv("MPCPS5_HOST_CREDENTIALS_FAIL");
	return fail && strcmp(fail, operation) == 0;
#else
	(void)operation;
	return false;
#endif
}
void Clear(Entries& entries)
{
	for (auto& entry : entries)
		Wipe(entry.second);
	entries.clear();
}
struct SensitiveBytes : Bytes
{
	using Bytes::Bytes;
	~SensitiveBytes() { if (!empty()) crypto_wipe(data(), size()); }
};
struct SensitiveEntries : Entries
{
	SensitiveEntries() = default;
	explicit SensitiveEntries(Entries entries) { swap(entries); Clear(entries); }
	~SensitiveEntries() { Clear(*this); }
};
struct Key
{
	std::array<unsigned char, 32> bytes{};
	~Key() { crypto_wipe(bytes.data(), bytes.size()); }
};
void Put32(Bytes& data, size_t at, uint32_t value)
{
	for (int i = 0; i < 4; ++i) data[at + i] = static_cast<unsigned char>(value >> (i * 8));
}
uint32_t Get32(const Bytes& data, size_t at)
{
	uint32_t value = 0;
	for (int i = 0; i < 4; ++i) value |= uint32_t(data[at + i]) << (i * 8);
	return value;
}
bool Entropy(unsigned char* out, size_t size)
{
	size_t done = 0;
	return !Fail("entropy") && mbedtls_hardware_poll(nullptr, out, size, &done) == 0 && done == size;
}
bool Read(const std::string& path, Bytes& bytes, std::string& error)
{
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	struct stat info = {};
	if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < static_cast<off_t>(Header) ||
		info.st_size > static_cast<off_t>(Header + MaxPayload))
	{
		if (fd >= 0) close(fd);
		error = "Saved network credentials cannot be read or have an invalid format.";
		return false;
	}
	bytes.resize(static_cast<size_t>(info.st_size));
	size_t done = 0;
	while (done < bytes.size())
	{
		const ssize_t n = read(fd, bytes.data() + done, bytes.size() - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += static_cast<size_t>(n);
	}
	close(fd);
	if (done != bytes.size() || memcmp(bytes.data(), Magic, 8) != 0 || Get32(bytes, 8) != 1 || Get32(bytes, 12) != bytes.size() - Header)
	{
		error = "Saved network credentials cannot be read or have an invalid format.";
		return false;
	}
	return true;
}
bool Encode(const Entries& entries, SensitiveBytes& plain, std::string& error)
{
	if (entries.size() > MaxEntries) { error = "Too many saved credentials."; return false; }
	size_t size = 4;
	for (const auto& entry : entries)
	{
		if (entry.first.empty() || entry.first.size() > 4096 || entry.second.size() > 4096 ||
			entry.first.find('\0') != std::string::npos || entry.second.find('\0') != std::string::npos)
		{ error = "A saved credential is too large or invalid."; return false; }
		size += 8 + entry.first.size() + entry.second.size();
	}
	if (size > MaxPayload) { error = "Too many saved credentials."; return false; }
	plain.resize(size);
	Put32(plain, 0, entries.size());
	size_t at = 4;
	for (const auto& entry : entries)
	{
		Put32(plain, at, entry.first.size()); Put32(plain, at + 4, entry.second.size()); at += 8;
		memcpy(plain.data() + at, entry.first.data(), entry.first.size()); at += entry.first.size();
		memcpy(plain.data() + at, entry.second.data(), entry.second.size()); at += entry.second.size();
	}
	return true;
}
bool Decode(const Bytes& bytes, const std::array<unsigned char, 32>& key, Entries& entries, std::string& error)
{
	SensitiveBytes plain(bytes.size() - Header);
	if (crypto_aead_unlock(plain.data(), bytes.data() + 48, key.data(), bytes.data() + 16, bytes.data(), 48,
		bytes.data() + Header, plain.size()) != 0)
	{
		error = "Saved network credentials could not be decrypted. The original files were kept.";
		return false;
	}
	if (plain.size() < 4 || Get32(plain, 0) > MaxEntries) { error = "Saved network credentials have an invalid format."; return false; }
	SensitiveEntries parsed;
	size_t at = 4;
	for (uint32_t i = 0; i < Get32(plain, 0); ++i)
	{
		if (plain.size() - at < 8) { error = "Saved network credentials have an invalid format."; return false; }
		const size_t a = Get32(plain, at), b = Get32(plain, at + 4); at += 8;
		if (!a || a > 4096 || b > 4096 || a + b > plain.size() - at)
		{ error = "Saved network credentials have an invalid format."; return false; }
		const std::string account(reinterpret_cast<char*>(plain.data() + at), a); at += a;
		std::string secret(reinterpret_cast<char*>(plain.data() + at), b); at += b;
		if (account.find('\0') != std::string::npos || secret.find('\0') != std::string::npos || parsed.count(account))
		{ Wipe(secret); error = "Saved network credentials have an invalid format."; return false; }
		parsed.emplace(account, std::move(secret));
		Wipe(secret);
	}
	if (at != plain.size()) { error = "Saved network credentials have an invalid format."; return false; }
	entries.swap(parsed);
	return true;
}
} // namespace

void Wipe(std::string& value)
{
	value.resize(value.capacity());
	if (!value.empty()) crypto_wipe(&value[0], value.size());
	value.clear();
}
bool Split(const std::string& url, std::string& key, std::string& password, std::string& bare)
{
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos) return false;
	const size_t start = scheme + 3, end = std::min(url.find('/', start), url.size());
	const size_t at = url.rfind('@', end), colon = url.find(':', start);
	if (at == std::string::npos || at < start || at >= end || colon == std::string::npos || colon >= at) return false;
	password = url.substr(colon + 1, at - colon - 1);
	bare = url.substr(0, colon) + url.substr(at);
	key = url.substr(0, colon) + url.substr(at, end - at);
	return true;
}
std::string WithoutPassword(const std::string& url)
{
	std::string key, password, bare;
	const bool found = Split(url, key, password, bare);
	Wipe(password);
	return found ? bare : url;
}
bool HasUser(const std::string& url)
{
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos) return false;
	const size_t start = scheme + 3, end = std::min(url.find('/', start), url.size());
	const size_t at = url.rfind('@', end);
	return at != std::string::npos && at > start && at < end;
}
Store::Store(std::string directory) : directory_(std::move(directory)) {}
Store::~Store() { ClearSession(); }
std::string Store::Path() const { return directory_ + "/credentials.enc"; }
std::string Store::KeyPath() const { return directory_ + "/.credentials-device/key.bin"; }
bool Store::Pending() const { std::lock_guard<std::mutex> guard(mutex_); return !legacy_.empty() || !staged_.empty(); }
void Store::ImportLegacy(const Entries& entries)
{
	std::lock_guard<std::mutex> guard(mutex_);
	for (const auto& entry : entries) if (!session_.count(entry.first)) legacy_.emplace(entry);
}
void Store::Stage(const Entries& entries)
{
	std::lock_guard<std::mutex> guard(mutex_);
	for (const auto& entry : entries)
		if (!session_.count(entry.first)) { Wipe(staged_[entry.first]); staged_[entry.first] = entry.second; }
}
Entries Store::Merged() const
{
	Entries result = entries_;
	for (const auto& entry : legacy_) result.emplace(entry);
	for (const auto& entry : staged_) { Wipe(result[entry.first]); result[entry.first] = entry.second; }
	return result;
}
namespace
{
bool ReadKey(const std::string& path, std::array<unsigned char, 32>& key, std::string& error)
{
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	struct stat info = {};
	if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != 40 || (info.st_mode & 077) != 0)
	{
		if (fd >= 0) close(fd);
		error = "The local credential key is missing, damaged or not private."; return false;
	}
	SensitiveBytes bytes(40);
	size_t done = 0;
	while (done < bytes.size())
	{
		const ssize_t n = read(fd, bytes.data() + done, bytes.size() - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += static_cast<size_t>(n);
	}
	close(fd);
	if (done != bytes.size() || memcmp(bytes.data(), "MPCKEY01", 8) != 0)
	{ error = "The local credential key is missing, damaged or not private."; return false; }
	std::copy(bytes.begin() + 8, bytes.end(), key.begin());
	return true;
}
bool PrivateDirectory(const std::string& dir, std::string& error)
{
	struct stat info = {};
	if (lstat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || (info.st_mode & 077) != 0)
	{ error = "The local credential key directory is not private."; return false; }
	return true;
}
} // namespace
bool Store::KeyLocked(std::string& error)
{
	if (have_key_) return true;
	const std::string dir = directory_ + "/.credentials-device", path = KeyPath();
	if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
	{ error = "The local credential key could not be saved."; return false; }
	if (!PrivateDirectory(dir, error)) return false;
	struct stat info = {};
	if (lstat(path.c_str(), &info) == 0)
	{
		have_key_ = ReadKey(path, key_, error);
		return have_key_;
	}
	if (errno != ENOENT) { error = "The local credential key could not be read."; return false; }
	Key candidate;
	if (!Entropy(candidate.bytes.data(), candidate.bytes.size()))
	{ error = "Secure random data is unavailable. Saved credentials were not changed."; return false; }
	SensitiveBytes record(40);
	memcpy(record.data(), "MPCKEY01", 8);
	std::copy(candidate.bytes.begin(), candidate.bytes.end(), record.begin() + 8);
	const std::string temp = path + ".tmp";
	unlink(temp.c_str());
	const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) { error = "The local credential key could not be saved."; return false; }
	size_t done = 0;
	while (done < record.size() && !Fail("key-write"))
	{
		const ssize_t n = write(fd, record.data() + done, record.size() - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += static_cast<size_t>(n);
	}
	bool ok = done == record.size() && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	Key verify;
	ok = ok && ReadKey(temp, verify.bytes, error) && verify.bytes == candidate.bytes;
	if (!ok || Fail("key-rename") || rename(temp.c_str(), path.c_str()) != 0)
	{ unlink(temp.c_str()); error = "The local credential key could not be saved."; return false; }
	const int folder = open(dir.c_str(), O_RDONLY | O_NOFOLLOW);
	if (folder >= 0) { fsync(folder); close(folder); }
	key_ = candidate.bytes; have_key_ = true;
	return true;
}
bool Store::LoadLocked(std::string& error)
{
	if (loaded_) return true;
	struct stat info = {};
	if (lstat(Path().c_str(), &info) != 0)
	{
		if (errno != ENOENT) { error = "Saved network credentials could not be read."; return false; }
		loaded_ = true;
		return true;
	}
	// Never generate a replacement key for existing encrypted passwords.
	if (!PrivateDirectory(directory_ + "/.credentials-device", error) || !ReadKey(KeyPath(), key_, error)) return false;
	have_key_ = true;
	Bytes bytes;
	SensitiveEntries decoded;
	if (!Read(Path(), bytes, error) || !Decode(bytes, key_, decoded, error)) return false;
	Clear(entries_); entries_.swap(decoded); loaded_ = true;
	return true;
}
bool Store::Load(std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear(); return LoadLocked(error);
}
bool Store::SaveLocked(const Entries& entries, std::string& error)
{
	SensitiveBytes plain;
	if (!Encode(entries, plain, error) || !KeyLocked(error)) return false;
	Bytes bytes(Header + plain.size());
	std::copy(std::begin(Magic), std::end(Magic), bytes.begin());
	Put32(bytes, 8, 1); Put32(bytes, 12, plain.size());
	if (!Entropy(bytes.data() + 16, 24))
	{ error = "Secure random data is unavailable. Saved credentials were not changed."; return false; }
	crypto_aead_lock(bytes.data() + Header, bytes.data() + 48, key_.data(), bytes.data() + 16, bytes.data(), 48,
		plain.data(), plain.size());
	const std::string temp = Path() + ".tmp";
	unlink(temp.c_str());
	const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) { error = "Network passwords could not be saved."; return false; }
	size_t done = 0;
	while (done < bytes.size() && !Fail("write"))
	{
		const ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += static_cast<size_t>(n);
	}
	bool ok = done == bytes.size() && !Fail("fsync") && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	Bytes verify;
	SensitiveEntries decoded;
	ok = ok && Read(temp, verify, error) && Decode(verify, key_, decoded, error) && decoded == entries;
	if (!ok || Fail("rename") || rename(temp.c_str(), Path().c_str()) != 0)
	{ unlink(temp.c_str()); error = "Network passwords could not be saved. The previous file was kept."; return false; }
	const int dir = open(directory_.c_str(), O_RDONLY);
	if (dir >= 0) { if (fsync(dir) != 0) OrbisLog("[credentials] directory sync failed"); close(dir); }
	return true;
}
bool Store::CommitLocked(std::string& error)
{
	if (!LoadLocked(error)) return false;
	if (legacy_.empty() && staged_.empty()) return true;
	SensitiveEntries merged(Merged());
	if (!SaveLocked(merged, error)) return false;
	Clear(entries_); entries_.swap(merged); Clear(legacy_); Clear(staged_);
	return true;
}
bool Store::Commit(std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear(); return CommitLocked(error);
}
bool Store::Retain(const std::vector<std::string>& urls, std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	if (!LoadLocked(error)) return false;
	std::set<std::string> accounts;
	for (const auto& url : urls)
	{
		const std::string clean = WithoutPassword(url);
		const size_t scheme = clean.find("://");
		if (scheme != std::string::npos && HasUser(clean))
			accounts.insert(clean.substr(0, std::min(clean.find('/', scheme + 3), clean.size())));
	}
	SensitiveEntries kept, removed;
	for (const auto& entry : entries_)
		(accounts.count(entry.first) ? kept : removed).emplace(entry);
	if (removed.empty()) return true;
	if (!SaveLocked(kept, error)) return false;
	for (const auto& entry : removed) session_.emplace(entry); // current playback may still need it
	Clear(entries_); entries_.swap(kept);
	return true;
}
bool Store::SaveUrl(const std::string& url, bool remember, std::string& bare, std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	std::string account, secret;
	if (!Split(url, account, secret, bare)) { bare = url; return true; }
	if (!LoadLocked(error)) { Wipe(secret); return false; }
	if (remember)
	{
		Wipe(staged_[account]); staged_[account] = secret;
		if (!CommitLocked(error)) { Wipe(secret); return false; }
		const auto old = session_.find(account);
		if (old != session_.end()) { Wipe(old->second); session_.erase(old); }
	}
	else
	{
		SensitiveEntries merged(Merged());
		const auto old = merged.find(account);
		if (old != merged.end()) { Wipe(old->second); merged.erase(old); }
		if (merged != entries_ && !SaveLocked(merged, error)) { Wipe(secret); return false; }
		Clear(entries_); entries_.swap(merged); Clear(legacy_); Clear(staged_);
		Wipe(session_[account]); session_[account] = secret;
	}
	Wipe(secret);
	return true;
}
std::string Store::Resolve(const std::string& url) const
{
	std::lock_guard<std::mutex> guard(mutex_);
	std::string account, secret, bare;
	if (Split(url, account, secret, bare)) { Wipe(secret); return url; }
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos || !HasUser(url)) return url;
	const size_t end = std::min(url.find('/', scheme + 3), url.size());
	account = url.substr(0, end);
	const auto local = session_.find(account), saved = entries_.find(account);
	const std::string* password = local != session_.end() ? &local->second : saved != entries_.end() ? &saved->second : nullptr;
	if (!password) return url;
	const size_t at = url.rfind('@', end);
	return url.substr(0, at) + ":" + *password + url.substr(at);
}
void Store::ClearSession()
{
	std::lock_guard<std::mutex> guard(mutex_);
	Clear(entries_); Clear(legacy_); Clear(staged_); Clear(session_);
	crypto_wipe(key_.data(), key_.size()); loaded_ = have_key_ = false;
}
Store& Get() { static Store store(OrbisRoot()); return store; }
} // namespace credentials
} // namespace mpc
