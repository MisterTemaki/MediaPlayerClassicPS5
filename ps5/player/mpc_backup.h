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
// - Automatic: with a drive chosen (Settings::backup_drive), every time mpc-hc.ini is saved and its text differs
//   from the backup's, the backup is written again (beside it, then renamed over it: a backup interrupted halfway
//   leaves the previous one whole). Nothing is written when no drive is chosen, when the drive isn't connected,
//   or when nothing changed. A fresh install (no drive chosen) never touches a backup.
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

// the backup file's time, 0 when there's none
time_t BackupTime(const std::string& drive);

// mpc-hc.ini (as just saved) to the drive's backup. False with why in error. *written: false when the backup
// already had the same text (nothing written).
bool WriteTo(const std::string& drive, std::string& error, bool* written = nullptr);

// called by Settings::Save once mpc-hc.ini is written: the automatic backup (s.backup_drive), when there is one
void AfterSave(const Settings& s);

// the last automatic or manual attempt in this session: its drive, its time, why it failed ("" when it didn't)
struct Status
{
	std::string drive;
	time_t when = 0;
	std::string error;
};
Status LastStatus();

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
