// Media Player Classic PS5 1.4: disc drives -- the PS5's own (/dev/cd0) and USB drives (/dev/cd1...) -- and what is on
// their discs: data discs (CD-ROM through a USB drive, DVD±R, BD-R with files), unencrypted DVD-Video and Blu-ray.
//
// Disc reading after 5S2CD by The7mirrors, a fork of PS5SX2, GPL-3.0 (ps5/coreorbis/orbis-shims/ProsperoDisc.cpp):
// the drives are FreeBSD cd(4) devices through CAM, read with plain 2048-byte pread()s of whole sectors (payload-tested
// on firmware 13.00 for both drives), sized with DIOCGMEDIASIZE; a disc is "there" when its sector 16 reads. The
// console's own drive can't read CDs (a USB drive can); audio CD tracks can't be read through cd(4) at all. Reading
// needs the HEN running (the app let out of its sandbox, ProsperoJailbreak.h).
//
// No decryption of any kind: no libdvdcss, libaacs, libbdplus, keys, nothing that gets around CSS / AACS / BD+. An
// encrypted disc (a commercial film) is recognised -- DVD: the drive's copyright information (CSS/CPPM) or packs marked
// scrambled; Blu-ray: an AACS or BD+ structure -- and refused with a message. libdvdread is built without libdvdcss
// and loads nothing at run time, libbluray loads nothing either (tools/patches/*-mpcps5.patch).
//
// Here:
//   - a watcher thread finds the drives, notices a disc put in or taken out, reads its file system (UDF through
//     libudfread, ISO 9660 + Joliet) into memory, and says what the disc is; the UI only reads that snapshot (the
//     drive is never read on the UI thread);
//   - a sector reader per disc with a read-ahead cache (an optical drive seeks slowly: 128 KiB at a time, the next two
//     chunks read in the background while a film plays);
//   - disc://cd0/path/file.mkv -- a file on a data disc, to FFmpeg through an AVIOContext (seekable, nothing copied);
//   - dvd://cd0[?title=N] -- the DVD-Video's main (longest) title, or title N: FFmpeg's dvdvideo demuxer (libdvdnav,
//     libdvdread), libdvdread reading the drive through the sector reader ("mpcdisc:cd0", DVDSetFilesystemFactory);
//   - bluray://cd0[?title=N] -- the Blu-ray's main title (libbluray's choice) or title N of its list: libbluray reads
//     the disc through the sector reader (bd_open_stream), the title's stream goes to FFmpeg's MPEG-TS demuxer.
//   dvd:///folder and bluray:///folder do the same for a VIDEO_TS / BDMV folder on a drive (File > Open DVD/BD...).
//   Disc menus are not shown (1.4): the main title plays, Navigate > Titles picks another.
//
// Host builds: MPCPS5_HOST_DISC=<image>[,<image2>...] puts a disc image in drive cd0 (the console's), cd1 (USB)...;
// a missing file is an empty drive; the test script's "eject cdN" / "insert cdN" take it out and put it back.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AVIOContext;
struct AVFormatContext;
struct AVDictionary;
struct AVInputFormat;

namespace mpc
{
namespace disc
{
enum class Kind
{
	NoDisc,
	Reading, // a disc was put in: its file system is being read
	Data,
	DvdVideo,
	BluRay,
	Encrypted, // a DVD-Video / Blu-ray with CSS, AACS or BD+: refused
	Unreadable, // nothing readable: an audio CD, a blank or damaged disc, a CD in the PS5's own drive
};

struct DriveState
{
	std::string id; // "cd0"
	bool usb = false; // a USB drive (cd1 and up), not the console's own (cd0)
	Kind kind = Kind::NoDisc;
	std::string label; // the volume's name
	std::string fs; // "UDF 1.02", "ISO 9660", "ISO 9660 + Joliet"
	std::string protection; // "CSS", "AACS", "BD+"
	uint64_t generation = 0; // a new number for every disc put in (0: none)
	uint64_t sectors = 0;
};

// the watcher (a thread), started once; Stop() at exit
void Start();
void Stop();
// the drives and their discs, as last seen (never waits for a drive)
std::vector<DriveState> Drives();
bool GetDrive(const std::string& id, DriveState* out);
// "Disc drive (LABEL)", "USB disc drive (No disc)"... (translated)
std::string DriveName(const DriveState& d);
// what the Open dialog says instead of the files: "" (the files), "No disc", the encrypted-disc message...
std::string DriveMessage(const DriveState& d);
// the message for an encrypted disc: in English (the player's error, translated where it is shown) and translated
const char* EncryptedText();
std::string EncryptedMessage();

// disc://, dvd://, bluray://
bool IsDiscUrl(const std::string& url);
bool IsTitleUrl(const std::string& url); // dvd:// or bluray://
// the drive of a disc URL ("cd0"), "" for a folder or another URL
std::string DriveOf(const std::string& url);
// the disc a URL plays from: its generation now (0: gone / not a drive URL)
uint64_t Generation(const std::string& url);
// a name for the title bar and the OSD: "TEST_DVD - Title 2" ("" when not a title URL)
std::string DisplayName(const std::string& url);
// dvd://cd0?title=3 -> dvd://cd0 (the disc, any title) and 3 (0: the main title)
std::string TitleBase(const std::string& url, int* title = nullptr);

// files on a data disc: disc://cd0/path
struct Entry
{
	std::string name;
	bool dir = false;
	uint64_t size = 0;
	int64_t mtime = 0;
};
bool List(const std::string& url, std::vector<Entry>& out, std::string* error);
bool Exists(const std::string& url);
bool IsDir(const std::string& url);
bool ReadFile(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes);
AVIOContext* OpenAvio(const std::string& url, void** owner, std::string* error);
void CloseAvio(AVIOContext* io, void* owner);

// a DVD-Video or Blu-ray title, as Navigate > Titles lists them
struct Title
{
	int number = 0; // 1-based
	double seconds = 0;
	int chapters = 0;
};
struct TitleChapter
{
	double time = 0;
	std::string name;
};
// What the player opens for a dvd:// or bluray:// URL (on its opening thread: this reads the disc).
struct TitleOpen
{
	const AVInputFormat* format = nullptr;
	std::string ffmpeg_url; // dvdvideo: the "file" libdvdread opens
	AVDictionary* options = nullptr; // dvdvideo: title=N
	AVIOContext* io = nullptr; // bluray: the title's MPEG-TS
	void* owner = nullptr;
	std::vector<Title> titles;
	int title = 0; // the one opened
	std::vector<TitleChapter> chapters; // Blu-ray (the DVD demuxer has its own)
	std::vector<std::pair<int, std::string>> languages; // Blu-ray: PID -> ISO 639-2 language
	double duration = 0; // Blu-ray
	bool encrypted = false; // refused: error is EncryptedText()
	std::string error; // in English (T() where it is shown)
};
bool OpenTitle(const std::string& url, TitleOpen& out);
void CloseTitle(TitleOpen& t); // after avformat_close_input

// whether a folder holds a DVD-Video (VIDEO_TS) or a Blu-ray (BDMV): its dvd:// / bluray:// URL, else ""
std::string TitleUrlForFolder(const std::string& folder);
// the URL that plays a drive's disc: dvd://cdN, bluray://cdN, "" for a data disc
std::string PlayUrl(const DriveState& d);

#ifndef __PROSPERO__
// host tests: take the disc out of drive id (or put it back)
void HostEject(const std::string& id, bool out);
#endif
} // namespace disc
} // namespace mpc
