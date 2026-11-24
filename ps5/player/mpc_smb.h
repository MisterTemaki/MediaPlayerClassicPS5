// Media Player Classic PS5: files on Windows shares (SMB 2/3), through libsmb2 (LGPL-2.1, Ronnie Sahlberg) --
// the library VLC's smb2 module uses.
//
// URLs: smb://[user[:password]@]server[:port]/share/path/file.mkv
// A share is read with its own connection per open file (libsmb2 contexts aren't shared between threads),
// handed to FFmpeg as an AVIOContext with a 1 MiB buffer.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
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

// (1.7.1, audits B1, C1, D1, D2) the calling thread's requests to a share give up within 0.1 s once *flag is true (the
// player's quit, the file browser's Cancel), as disc::CancelScope does for discs. A file opened under it keeps the flag
// for its later reads (the demuxer's), and its close then skips the goodbyes to the server.
class CancelScope
{
public:
	explicit CancelScope(const std::atomic<bool>* flag);
	~CancelScope();
	CancelScope(const CancelScope&) = delete;
	CancelScope& operator=(const CancelScope&) = delete;

private:
	const std::atomic<bool>* prev_;
};

// (1.7.1 review R5) the calling thread's requests (and those of a file opened under it) end at this deadline: a
// subtitle file read whole on the UI thread
class DeadlineScope
{
public:
	explicit DeadlineScope(double seconds);
	~DeadlineScope();
	DeadlineScope(const DeadlineScope&) = delete;
	DeadlineScope& operator=(const DeadlineScope&) = delete;

private:
	double prev_;
};

AVIOContext* OpenAvio(const std::string& url, void** owner, std::string* error);
void CloseAvio(AVIOContext* io, void* owner);
// the whole file, up to max_bytes; (1.7.1) within max_seconds (0: no limit but the requests' own 15 s each)
bool ReadFile(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, double max_seconds = 0);
bool Exists(const std::string& url);
// (1.7.1) a URL's last name as the share knows it (%XX decoded), and a name made safe for a URL ('%' as %25: audit D8)
std::string FileNameOf(const std::string& url);
std::string EscapeName(const std::string& name);
// the folders and files of smb://server/share/path ("" path: the share's root)
bool List(const std::string& url, std::vector<Entry>& out, std::string* error);
// the shares a server offers (smb://server)
bool Shares(const std::string& url, std::vector<std::string>& out, std::string* error);
} // namespace smb
} // namespace mpc
