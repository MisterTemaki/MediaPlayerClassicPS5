// Monocypher 4.0.2: Argon2id (64 MiB, three passes) and XChaCha20-Poly1305.
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
constexpr uint32_t Blocks = 65536, Passes = 3, Lanes = 1;
constexpr size_t Header = 80, MaxPayload = 1024 * 1024, MaxEntries = 256;
constexpr unsigned char Magic[] = {'M', 'P', 'C', 'V', 'L', 'T', '0', '1'};
using Bytes = std::vector<unsigned char>;
bool Fail(const char* operation)
{
#ifndef __PROSPERO__
	const char* fail = getenv("MPCPS5_HOST_VAULT_FAIL");
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
bool Derive(const std::string& password, const unsigned char* salt, Key& key, std::string& error)
{
	if (password.empty() || password.size() > 1024)
	{
		error = "Invalid master password length.";
		return false;
	}
	try
	{
		std::vector<uint64_t> memory(size_t(Blocks) * 1024 / sizeof(uint64_t));
		const crypto_argon2_config config{CRYPTO_ARGON2_ID, Blocks, Passes, Lanes};
		const crypto_argon2_inputs input{reinterpret_cast<const unsigned char*>(password.data()), salt,
			static_cast<uint32_t>(password.size()), 16};
		crypto_argon2(key.bytes.data(), key.bytes.size(), memory.data(), config, input, crypto_argon2_no_extras);
		return true; // Monocypher wipes the work area.
	}
	catch (const std::bad_alloc&)
	{
		error = "Not enough memory to unlock the credential vault.";
		return false;
	}
}
bool Read(const std::string& path, Bytes& bytes, std::string& error)
{
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	struct stat info = {};
	if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < static_cast<off_t>(Header) ||
		info.st_size > static_cast<off_t>(Header + MaxPayload))
	{
		if (fd >= 0) close(fd);
		error = "The credential vault cannot be read or has an invalid format.";
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
	if (done != bytes.size() || memcmp(bytes.data(), Magic, 8) != 0 || Get32(bytes, 8) != Blocks ||
		Get32(bytes, 12) != Passes || Get32(bytes, 16) != Lanes || Get32(bytes, 60) != bytes.size() - Header)
	{
		error = "The credential vault cannot be read or has an invalid format.";
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
	if (crypto_aead_unlock(plain.data(), bytes.data() + 64, key.data(), bytes.data() + 36, bytes.data(), 64,
		bytes.data() + Header, plain.size()) != 0)
	{
		error = "The master password is incorrect or the credential vault is damaged.";
		return false;
	}
	if (plain.size() < 4 || Get32(plain, 0) > MaxEntries) { error = "The credential vault has an invalid format."; return false; }
	SensitiveEntries parsed;
	size_t at = 4;
	for (uint32_t i = 0; i < Get32(plain, 0); ++i)
	{
		if (plain.size() - at < 8) { error = "The credential vault has an invalid format."; return false; }
		const size_t a = Get32(plain, at), b = Get32(plain, at + 4); at += 8;
		if (!a || a > 4096 || b > 4096 || a + b > plain.size() - at)
		{ error = "The credential vault has an invalid format."; return false; }
		const std::string account(reinterpret_cast<char*>(plain.data() + at), a); at += a;
		std::string secret(reinterpret_cast<char*>(plain.data() + at), b); at += b;
		if (account.find('\0') != std::string::npos || secret.find('\0') != std::string::npos || parsed.count(account))
		{ Wipe(secret); error = "The credential vault has an invalid format."; return false; }
		parsed.emplace(account, std::move(secret));
		Wipe(secret);
	}
	if (at != plain.size()) { error = "The credential vault has an invalid format."; return false; }
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
Vault::Vault(std::string directory) : directory_(std::move(directory)) {}
Vault::~Vault() { Lock(); }
std::string Vault::Path() const { return directory_ + "/credentials.vault"; }
bool Vault::Exists() const { struct stat info = {}; return lstat(Path().c_str(), &info) == 0; }
bool Vault::Unlocked() const { std::lock_guard<std::mutex> guard(mutex_); return unlocked_; }
bool Vault::Pending() const { std::lock_guard<std::mutex> guard(mutex_); return !legacy_.empty() || !staged_.empty(); }
void Vault::ImportLegacy(const Entries& entries)
{
	std::lock_guard<std::mutex> guard(mutex_);
	for (const auto& entry : entries) legacy_.emplace(entry);
}
void Vault::Stage(const Entries& entries)
{
	std::lock_guard<std::mutex> guard(mutex_);
	for (const auto& entry : entries) { Wipe(staged_[entry.first]); staged_[entry.first] = entry.second; }
}
Entries Vault::Merged() const
{
	Entries result = entries_;
	for (const auto& entry : legacy_) result.emplace(entry);
	for (const auto& entry : staged_) { Wipe(result[entry.first]); result[entry.first] = entry.second; }
	return result;
}
bool Vault::SaveLocked(const Entries& entries, const std::array<unsigned char, 32>& key,
	const std::array<unsigned char, 16>& salt, std::string& error)
{
	SensitiveBytes plain;
	if (!Encode(entries, plain, error)) return false;
	Bytes bytes(Header + plain.size());
	std::copy(std::begin(Magic), std::end(Magic), bytes.begin());
	Put32(bytes, 8, Blocks); Put32(bytes, 12, Passes); Put32(bytes, 16, Lanes);
	std::copy(salt.begin(), salt.end(), bytes.begin() + 20);
	if (!Entropy(bytes.data() + 36, 24)) { error = "Secure random data is unavailable. The vault was not changed."; return false; }
	Put32(bytes, 60, plain.size());
	crypto_aead_lock(bytes.data() + Header, bytes.data() + 64, key.data(), bytes.data() + 36, bytes.data(), 64,
		plain.data(), plain.size());
	const std::string temp = Path() + ".tmp";
	unlink(temp.c_str()); // only an encrypted temporary file can exist here
	const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) { error = "The credential vault could not be saved."; return false; }
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
	ok = ok && Read(temp, verify, error) && Decode(verify, key, decoded, error) && decoded == entries;
	if (!ok || Fail("rename") || rename(temp.c_str(), Path().c_str()) != 0)
	{
		unlink(temp.c_str()); error = "The credential vault could not be saved. The previous vault was kept."; return false;
	}
	const int dir = open(directory_.c_str(), O_RDONLY);
	if (dir >= 0) { if (fsync(dir) != 0) OrbisLog("[credentials] directory sync failed"); close(dir); }
	return true;
}
bool Vault::Unlock(const std::string& password, std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	Bytes bytes;
	if (!Read(Path(), bytes, error)) return false;
	Key key;
	if (!Derive(password, bytes.data() + 20, key, error)) return false;
	SensitiveEntries decoded;
	if (!Decode(bytes, key.bytes, decoded, error)) return false;
	Clear(entries_); entries_.swap(decoded);
	crypto_wipe(key_.data(), key_.size()); key_ = key.bytes;
	std::copy(bytes.begin() + 20, bytes.begin() + 36, salt_.begin());
	unlocked_ = true;
	return true;
}
bool Vault::CreateOrChange(const std::string& password, std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	const size_t characters = std::count_if(password.begin(), password.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
	if (characters < 8 || password.size() > 1024) { error = "Use a master password with at least 8 characters."; return false; }
	if (!unlocked_ && Exists()) { error = "Unlock the existing credential vault first."; return false; }
	std::array<unsigned char, 16> salt{};
	Key key;
	if (!Entropy(salt.data(), salt.size())) { error = "Secure random data is unavailable. The vault was not changed."; return false; }
	if (!Derive(password, salt.data(), key, error)) return false;
	SensitiveEntries merged(Merged());
	if (!SaveLocked(merged, key.bytes, salt, error)) return false;
	crypto_wipe(key_.data(), key_.size()); key_ = key.bytes; salt_ = salt;
	Clear(entries_); entries_.swap(merged); Clear(legacy_); Clear(staged_);
	unlocked_ = true;
	return true;
}
bool Vault::Commit(std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	if (legacy_.empty() && staged_.empty()) return true;
	if (!unlocked_) { error = "Unlock the credential vault to save passwords."; return false; }
	SensitiveEntries merged(Merged());
	if (!SaveLocked(merged, key_, salt_, error)) return false;
	Clear(entries_); entries_.swap(merged); Clear(legacy_); Clear(staged_);
	return true;
}
std::string Vault::RememberPath() const { return directory_ + "/.credentials-device/unlock.key"; }
bool Vault::Remembered() const
{
	struct stat info = {};
	return lstat(RememberPath().c_str(), &info) == 0;
}
bool Vault::SetRemembered(bool remember, std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	const std::string path = RememberPath(), dir = directory_ + "/.credentials-device";
	if (!remember)
	{
		for (const auto& file : {path, path + ".tmp"})
			if (unlink(file.c_str()) != 0 && errno != ENOENT)
			{ error = "The remembered unlock key could not be removed."; return false; }
		const int fd = open(dir.c_str(), O_RDONLY | O_NOFOLLOW);
		if (fd >= 0) { fsync(fd); close(fd); }
		return true;
	}
	if (!unlocked_) { error = "Unlock the credential vault first."; return false; }
	if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
	{ error = "Automatic unlock could not be saved."; return false; }
	struct stat info = {};
	if (lstat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || (info.st_mode & 077) != 0)
	{ error = "The automatic unlock directory is not private."; return false; }
	SensitiveBytes record(56);
	memcpy(record.data(), "MPCAUTO1", 8);
	std::copy(salt_.begin(), salt_.end(), record.begin() + 8);
	std::copy(key_.begin(), key_.end(), record.begin() + 24);
	const std::string temp = path + ".tmp";
	unlink(temp.c_str());
	const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) { error = "Automatic unlock could not be saved."; return false; }
	size_t done = 0;
	while (done < record.size() && !Fail("remember-write"))
	{
		const ssize_t n = write(fd, record.data() + done, record.size() - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += static_cast<size_t>(n);
	}
	bool ok = done == record.size() && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	if (!ok || Fail("remember-rename") || rename(temp.c_str(), path.c_str()) != 0)
	{ unlink(temp.c_str()); error = "Automatic unlock could not be saved."; return false; }
	const int folder = open(dir.c_str(), O_RDONLY | O_NOFOLLOW);
	if (folder >= 0) { fsync(folder); close(folder); }
	return true;
}
bool Vault::UnlockRemembered(std::string& error)
{
	std::lock_guard<std::mutex> guard(mutex_);
	error.clear();
	if (!Remembered()) return false;
	const std::string dir = directory_ + "/.credentials-device";
	struct stat folder = {}, file = {};
	if (lstat(dir.c_str(), &folder) != 0 || !S_ISDIR(folder.st_mode) || (folder.st_mode & 077) != 0)
	{ error = "The automatic unlock directory is not private."; return false; }
	const int fd = open(RememberPath().c_str(), O_RDONLY | O_NOFOLLOW);
	if (fd < 0 || fstat(fd, &file) != 0 || !S_ISREG(file.st_mode) || file.st_size != 56 || (file.st_mode & 077) != 0)
	{
		if (fd >= 0) close(fd);
		error = "The remembered unlock key cannot be used. Enter your master password."; return false;
	}
	SensitiveBytes record(56);
	size_t done = 0;
	while (done < record.size())
	{
		const ssize_t n = read(fd, record.data() + done, record.size() - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += static_cast<size_t>(n);
	}
	close(fd);
	Bytes bytes;
	if (done != record.size() || memcmp(record.data(), "MPCAUTO1", 8) != 0 || !Read(Path(), bytes, error) ||
		memcmp(record.data() + 8, bytes.data() + 20, 16) != 0)
	{ error = "The remembered unlock key cannot be used. Enter your master password."; return false; }
	Key key;
	std::copy(record.begin() + 24, record.end(), key.bytes.begin());
	SensitiveEntries decoded;
	if (!Decode(bytes, key.bytes, decoded, error)) return false;
	Clear(entries_); entries_.swap(decoded);
	crypto_wipe(key_.data(), key_.size()); key_ = key.bytes;
	std::copy(bytes.begin() + 20, bytes.begin() + 36, salt_.begin());
	unlocked_ = true;
	return true;
}
std::string Vault::Resolve(const std::string& url) const
{
	std::lock_guard<std::mutex> guard(mutex_);
	if (!unlocked_) return WithoutPassword(url);
	std::string key, secret, bare;
	if (Split(url, key, secret, bare)) { Wipe(secret); return url; }
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos || !HasUser(url)) return url;
	const size_t end = std::min(url.find('/', scheme + 3), url.size());
	key = url.substr(0, end);
	const auto entry = entries_.find(key);
	if (entry == entries_.end()) return url;
	const size_t at = url.rfind('@', end);
	return url.substr(0, at) + ":" + entry->second + url.substr(at);
}
void Vault::Lock()
{
	std::lock_guard<std::mutex> guard(mutex_);
	Clear(entries_); Clear(legacy_); Clear(staged_);
	crypto_wipe(key_.data(), key_.size()); crypto_wipe(salt_.data(), salt_.size()); unlocked_ = false;
}
Vault& Get() { static Vault vault(OrbisRoot()); return vault; }
} // namespace credentials
} // namespace mpc
