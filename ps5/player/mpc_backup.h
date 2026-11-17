// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// 1.6.3 (Sulfurico-One64, #10): the settings backed up to a USB drive, and imported back -- after a console's
// factory reset, or on another PS5. Options > Miscellaneous > Backup.
//
// - The backup is a copy of mpc-hc.ini (everything Options and the menus keep: Keys, audio, subtitles, the
//   window, favorites, recent files, file positions, network locations) in <drive>/mpcps5/backup/mpc-hc.ini.
//   mpc-hc.ini has had no passwords in it since 1.6.2 (they are in the encrypted store, whose key never leaves
//   the console): network passwords are not in a backup, and are typed again after an import. The fonts folder
//   (fonts the user added for subtitles) isn't copied either.
// - Whose backup: mpc-hc.ini has a random BackupId= (made once, kept by Reset and by an import). A backup is this
//   installation's when its BackupId is the same. The automatic backup only writes over this installation's backup
//   (or where there is none): a drive holding another one -- the backup of a console before its factory reset,
//   another PS5's, another stick now at the same /mnt/usbN, one that can't be read -- is never written over without
//   the user saying yes (Back up now asks, No first). An import writes the imported backup again with this
//   installation's id: from then on it is this one's (a console still writing the old id finds another's there).
// - Automatic: with a drive chosen (Settings::backup_drive), every time mpc-hc.ini is saved its text is handed to
//   a thread that writes it to the drive when it differs from the backup's (beside it, then renamed over it: a
//   backup interrupted halfway leaves the previous one whole). The screen never waits for the drive; the app's
//   exit waits a few seconds for the last one. Nothing is written when no drive is chosen, when the drive isn't
//   connected, or when nothing changed. A fresh install (no drive chosen) never touches a backup.
// - Import: the newest backup on the connected drives, read as mpc-hc.ini is read (any password in it is
//   dropped, not used), after the user says yes. It replaces every setting and list.
#include <ctime>
#include <string>
#include <vector>

namespace mpc
{
struct Settings;

namespace backup
{
// the drives a backup can go to: /mnt/usb0..7, /mnt/ext0..1 (host tests: MPCPS5_HOST_DRIVES, colon-separated)
std::vector<std::string> ConnectedDrives();
bool ValidDrive(const std::string& drive); // one of those names (connected or not)
std::string FolderOn(const std::string& drive); // <drive>/mpcps5/backup
std::string FileOn(const std::string& drive);   // <drive>/mpcps5/backup/mpc-hc.ini

// a new installation's BackupId (32 hex digits)
std::string NewId();
bool ValidId(const std::string& id);

// the backup on a drive: there or not, its time, and whether it is the installation's with this id
struct Info
{
	bool exists = false;
	time_t when = 0;
	bool ours = false;
};
Info InfoOn(const std::string& drive, const std::string& id);

enum class Result
{
	Written,   // the backup now has the settings
	Unchanged, // it already had them: nothing written
	Foreign,   // another installation's backup is there: not written over (force: written over)
	Failed,    // error says why
};
// mpc-hc.ini (as saved) to the drive's backup, now, on the calling thread (Back up now, after an import); error:
// why it failed, translated (call it from the UI thread)
Result WriteTo(const std::string& drive, const std::string& id, std::string& error, bool force = false);

// called by Settings::Save once mpc-hc.ini is written: the automatic backup (s.backup_drive), when there is one,
// written by the backup thread
void AfterSave(const Settings& s);
// waits (up to ms) until the backup thread has written what it was given (the app's exit)
void Flush(int ms);

// why a write failed: kept as a code and its details, made into words on the thread that shows them (T() reads
// the language setting, which only the UI thread changes)
enum class Error
{
	None,
	NoDrive,
	NotConnected,  // path: the drive
	CantRead,      // the settings file
	CantCreate,    // path, reason
	CantWrite,     // path, reason
	CantReplace,   // path, reason
	CantReplaceKept, // path, reason, kept: the new backup left as mpc-hc.ini.tmp
};
// the last attempt in this session: its drive, its result, why it failed
struct Status
{
	std::string drive;
	time_t when = 0;
	Result result = Result::Unchanged;
	Error error = Error::None;
	std::string path, reason, kept;
};
Status LastStatus();
// a status' error in words: translated (the UI thread) or in English (the log); "" when there is none
std::string ErrorText(const Status& s, bool translated = true);
// changes whenever an attempt ends (the page reads the drive again only then)
unsigned Generation();

// the backups on the connected drives, the newest first
struct Found
{
	std::string drive, path;
	time_t when = 0;
};
std::vector<Found> FindBackups();

// a backup file read into out (a fresh Settings first): false with why when it isn't a settings file
bool ReadBackup(const std::string& path, Settings& out, std::string& error);
} // namespace backup
} // namespace mpc
