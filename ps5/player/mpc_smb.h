// Media Player Classic PS5: files on Windows shares (SMB 2/3), through libsmb2 (LGPL-2.1, Ronnie Sahlberg) --
// the library VLC's smb2 module uses.
//
// URLs: smb://[user[:password]@]server[:port]/share/path/file.mkv
// A share is read with its own connection per open file (libsmb2 contexts aren't shared between threads),
// handed to FFmpeg as an AVIOContext with a 1 MiB buffer.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AVIOContext;

namespace mpc
{
namespace smb
{
struct Entry
{
	std::string name;
	bool dir = false;
	uint64_t size = 0;
	int64_t mtime = 0;
};

AVIOContext* OpenAvio(const std::string& url, void** owner, std::string* error);
void CloseAvio(AVIOContext* io, void* owner);
bool ReadFile(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes);
bool Exists(const std::string& url);
// the folders and files of smb://server/share/path ("" path: the share's root)
bool List(const std::string& url, std::vector<Entry>& out, std::string* error);
// the shares a server offers (smb://server)
bool Shares(const std::string& url, std::vector<std::string>& out, std::string* error);
} // namespace smb
} // namespace mpc
