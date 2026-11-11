// Capture passwords before URLs enter the interface or saved settings.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpc_credentials.h"
#include "mpc_dialogs.h"
#include "mpc_i18n.h"
#include "mpc_ui.h"

namespace mpc
{
bool HasInlinePassword(const std::string& url)
{
	std::string account, secret, bare;
	const bool found = credentials::Split(url, account, secret, bare);
	credentials::Wipe(secret);
	return found;
}
void UseNetworkCredentials(std::string url, std::function<void(const std::string&)> ready, bool remember)
{
	std::string bare, error;
	const bool ok = credentials::Get().SaveUrl(url, remember, bare, error);
	credentials::Wipe(url);
	if (!ok) { MessageBox(T(error), T("Network credentials"), MB_OK, 3); return; }
	if (ready) ready(bare);
}
} // namespace mpc
