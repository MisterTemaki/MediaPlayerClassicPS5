// Encrypted network passwords, loaded automatically using a private local key.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mpc
{
namespace credentials
{
using Entries = std::map<std::string, std::string>;
void Wipe(std::string& value);
bool Split(const std::string& url, std::string& key, std::string& password, std::string& bare);
std::string WithoutPassword(const std::string& url);
bool HasUser(const std::string& url);

class Store
{
public:
	explicit Store(std::string directory);
	~Store();
	Store(const Store&) = delete;
	Store& operator=(const Store&) = delete;
	bool Load(std::string& error);
	bool Pending() const;
	void ImportLegacy(const Entries& entries);
	void Stage(const Entries& entries);
	bool Commit(std::string& error);
	// Called after settings are committed, so a failed settings write cannot discard old passwords.
	bool Retain(const std::vector<std::string>& urls, std::string& error);
	// Unchecked passwords stay in memory for this session and replace any saved password.
	bool SaveUrl(const std::string& url, bool remember, std::string& bare, std::string& error);
	std::string Resolve(const std::string& url) const;
	void ClearSession();
	std::string Path() const;
	std::string KeyPath() const;

private:
	bool LoadLocked(std::string& error);
	bool KeyLocked(std::string& error);
	bool SaveLocked(const Entries& entries, std::string& error);
	bool CommitLocked(std::string& error);
	Entries Merged() const;
	std::string directory_;
	mutable std::mutex mutex_;
	Entries entries_, legacy_, staged_, session_;
	std::array<unsigned char, 32> key_{};
	bool loaded_ = false, have_key_ = false;
};
Store& Get();
} // namespace credentials
} // namespace mpc
