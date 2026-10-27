// Media Player Classic PS5: MPC-HC's dialog boxes, laid out from mpc-hc.rc's templates (dialog units, as
// Windows lays them out): Open (IDD_OPEN_DLG), Go To (IDD_GOTO_DLG), About (IDD_ABOUTBOX), Properties
// (IDD_FILEPROPDETAILS / IDD_FILEMEDIAINFO), Options (the property sheet with its page tree), Add to
// Favorites / Organize Favorites -- and the Windows "Open" common dialog that MPC-HC's Browse... buttons
// open, here browsing the console's storage, USB drives and network shares.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mpc_settings.h"

namespace mpc
{
class Player;

enum class BrowseFilter
{
	Media,
	Subtitles,
	Folder, // pick a folder (Open Directory)
};

// The "Open" common dialog; done(path or smb:// URL), or done("") when cancelled.
void ShowFileBrowser(const std::string& title, BrowseFilter filter, std::function<void(const std::string&)> done);
// File > Open File...: done(url, dub_url, add_to_playlist_only)
void ShowOpenDialog(std::function<void(const std::string&, const std::string&, bool)> done);
// Navigate > Go To...: done(seconds)
void ShowGoToDialog(double pos, double fps, std::function<void(double)> done);
void ShowPropertiesDialog(Player& player, const std::string& url);
void ShowAboutDialog();
// View > Options...: applied() after OK / Apply
// page: the page it opens at (OptionsPage::Last: the one it was left on in this session, as MPC-HC)
void ShowOptionsDialog(std::function<void()> applied, OptionsPage page = OptionsPage::Last);
// Favorites > Add to Favorites...: done(name, remember_position)
void ShowAddFavoriteDialog(const std::string& name, std::function<void(const std::string&, bool)> done);
void ShowOrganizeFavoritesDialog();
// a network location (smb://), as Windows' "Add a network location" wizard: done(name, url)
void ShowAddNetworkDialog(std::function<void(const std::string&, const std::string&)> done);

// the media and subtitle extensions MPC-HC registers (CMediaFormats)
bool IsMediaFile(const std::string& name);
bool IsSubtitleFile(const std::string& name);
bool IsPlaylistFile(const std::string& name);
// a folder's media files, sorted as Explorer sorts (natural order), local or smb://
std::vector<std::string> ListFolderMedia(const std::string& folder);
std::string FileName(const std::string& path);
std::string ParentFolder(const std::string& path);
// "1.4 GB"
std::string SizeText(uint64_t bytes);
} // namespace mpc
