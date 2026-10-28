// Media Player Classic PS5 1.5.1: the app's build of ProsperoAppPlace.cpp -- no statfs(2) / getfsstat(2) (they are
// only in libkernel_sys, which a title can't load: 1.5 crashed at launch, #3); the helper payload reads the mounts.
//
// SPDX-License-Identifier: MIT

#define MPCPS5_APP_PROCESS 1
#include "ProsperoAppPlace.cpp"
