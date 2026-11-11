// Password-protected credential storage. Automatic unlock is an explicit opt-in that stores a local key.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <map>
#include <mutex>
#include <string>

namespace mpc
{
namespace credentials
{
using Entries = std::map<std::string, std::string>;
void Wipe(std::string& value);
// The password stays percent-encoded. The key includes scheme, user, host and port, but no path.
bool Split(const std::string& url, std::string& key, std::string& password, std::string& bare);
std::string WithoutPassword(const std::string& url);
bool HasUser(const std::string& url);

class Vault
{
public:
	explicit Vault(std::string directory);
	~Vault();
	Vault(const Vault&) = delete;
	Vault& operator=(const Vault&) = delete;
	bool Exists() const;
	bool Unlocked() const;
	bool Pending() const;
	// Legacy entries are merged only after successful authentication. Explicit new passwords take priority.
	void ImportLegacy(const Entries& entries);
	void Stage(const Entries& entries);
	bool Unlock(const std::string& password, std::string& error);
	bool CreateOrChange(const std::string& password, std::string& error);
	bool Commit(std::string& error);
	// Optional automatic unlock stores a local key under an owner-only directory. Off by default.
	bool Remembered() const;
	bool SetRemembered(bool remember, std::string& error);
	bool UnlockRemembered(std::string& error);
	std::string Resolve(const std::string& url) const;
	void Lock();
	std::string Path() const;

private:
	bool SaveLocked(const Entries& entries, const std::array<unsigned char, 32>& key,
		const std::array<unsigned char, 16>& salt, std::string& error);
	Entries Merged() const;
	std::string RememberPath() const;
	std::string directory_;
	mutable std::mutex mutex_;
	Entries entries_, legacy_, staged_;
	std::array<unsigned char, 32> key_{};
	std::array<unsigned char, 16> salt_{};
	bool unlocked_ = false;
};
Vault& Get();
} // namespace credentials
} // namespace mpc
