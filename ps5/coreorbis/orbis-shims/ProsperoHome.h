// Media Player Classic PS5 1.7: where the PS5's home screen lists the app (Options > Miscellaneous). The console reads
// sce_sys/param.json's applicationCategoryType when it registers the app: 65536 lists it under Media (1.6.3's), 0 under
// Games. The installer (and its copy for the home screen in /user/appmeta) and the app at each start write the setting
// into it; the home screen shows the change once the app is deleted from it and ShadowMount+ registers it again.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

// HomeScreenCategory= in mpc-hc.ini (read here: the installer has no mpc::Settings): 0 Media (the default), 1 Games
int HomeCategory();
// param.json's text with its applicationCategoryType for `category` (0 Media: 65536, 1 Games: 0); unchanged without one
std::string WithHomeCategory(const std::string& param, int category);
// <dir>/sce_sys/param.json, and <appmeta>/param.json when it is there, made to say `category`: 1 one of them written,
// 0 both already said it, -1 not written (*error: why, an English sentence for the user)
int ApplyHomeCategory(const std::string& dir, const std::string& appmeta, int category, std::string* error);
