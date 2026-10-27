// Updating the app from its GitHub releases (ProsperoUpdate.cpp), as Porpoise does (github.com/elripalda/Porpoise,
// src/porpoise_update.cpp), and as this port's installer payload already installs: files replaced one by one,
// eboot.bin last.
//
//   1. Check: GitHub's answer for the newest release (api.github.com/repos/<repo>/releases/latest) names the app's
//      zip (the asset ending in "-<TITLE_ID>.zip", the same zip the homebrew catalog lists), its size and SHA-256.
//      Newer than this version (by the tag, "v1.8" > "1.7"): it can be installed.
//   2. Install: the zip is downloaded (into memory), its size and SHA-256 checked (a release GitHub gives no SHA-256
//      for isn't installed); it holds the app folder (<TITLE_ID>/ ...) stored without compression and a manifest
//      (<TITLE_ID>/manifest.sha256: "<sha-256> *<path>" lines); every file is checked against the manifest. Only
//      then is anything written: each file beside the old one as <name>.new; then, with update.journal listing
//      them, each old file renamed to <name>.old and the new one into its place, eboot.bin and the manifest last;
//      a rename that fails puts every old file back, and an install cut off half way (the console turned off) is
//      put back when the app next starts; what can't be put back stays in the journal for the next try, and the
//      error (or StartupProblem) says so. Then the .old files go, files the old manifest had and the new one hasn't
//      are removed, and the home screen's copy of the art (/user/appmeta/<ID>) is brought up to date.
//   3. The running app is the old build: it has to be opened again.
//
// The data folder (/data/<app>/: games, saves, settings) isn't touched. Everything runs on a thread of its own;
// Status() says how far it is. The transport (Fetch) is the caller's: the emulators' helper uses its own HTTPS
// (Mbed TLS), Media Player Classic PS5 FFmpeg's (mpc_update.cpp).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace update
{
struct Config
{
	std::string repo;        // "MisterTemaki/genplusgxPS5"
	std::string title_id;    // "PPSA99921"
	std::string version;     // this build's, "1.7"
	std::string app_name;    // "Genesis Plus GX PS5"
	std::string app_dir;     // /data/homebrew/<title_id>
	std::string appmeta_dir; // /user/appmeta/<title_id>
	std::string api_url;     // "" -> https://api.github.com/repos/<repo>/releases/latest
	std::string release_key; // the release signing key, 64 hex digits (Ed25519 public key); "": no signature checked
	// set when app_dir can't take an update (an image, a read-only folder: ProsperoAppPlace.h): why, and then nothing
	// is downloaded or written (StartInstall refuses, InstallZip returns it)
	std::string cant_install;
};

// GET url into out (at most max bytes); progress (when not null) follows the bytes read. The HTTP status, or -1.
using Fetch = std::function<int(const std::string& url, std::vector<uint8_t>& out, size_t max,
	std::atomic<size_t>* progress)>;

enum class Phase
{
	Idle,
	Checking,
	UpToDate,
	Available,   // latest/size/notes say what
	Downloading, // done/total in bytes
	Installing,  // done/total in files
	Done,        // installed: the app must be opened again
	Failed,      // error says why; nothing was changed
};

struct Status
{
	Phase phase = Phase::Idle;
	std::string latest; // "1.8"
	uint64_t size = 0;  // the zip's
	uint64_t done = 0, total = 0;
	std::string error;
};

// Also puts back an install that was cut off half way (RecoverInterrupted on config.app_dir).
void Init(const Config& config, Fetch fetch);
// An install interrupted while its files were going into place (update.journal still there): the old files back
// (<name>.old), the new ones removed. The journal is removed only when every file is back; what couldn't be put
// back stays in it for the next start. Returns the files put back; 0 when there was nothing to do.
int RecoverInterrupted(const std::string& app_dir);
// Set when RecoverInterrupted couldn't put everything back (a sentence for the user); "" otherwise.
std::string StartupProblem();
// The version installed (the app says which when it asks: a helper of an older build may be the one running).
void SetCurrentVersion(const std::string& version);
const Config& CurrentConfig();
// Start a check / an install of the release the last check found; false when one is running (or nothing to install).
// notify: a check of the app's own (at start, once a day): on_newer is told when it finds a newer version.
bool StartCheck(bool notify = false);
void SetOnNewer(std::function<void(const std::string& version)> on_newer);
bool StartInstall();
Status GetStatus();
// Back to Idle (after the screen showed a result), unless something runs.
void Acknowledge();

// "phase|latest|size|done|total|error" and back, for the app <-> helper requests.
std::string Encode(const Status& s);
bool Decode(const std::string& text, Status& s);
const char* PhaseName(Phase p);

// ---- the parts, for the tests -------------------------------------------------------------------------------------
struct Release
{
	std::string tag, version; // "v1.8", "1.8"
	std::string zip_url, zip_name;
	uint64_t size = 0;
	std::string sha256; // hex, from the asset's digest ("" when GitHub gave none: then it isn't installed)
	std::string sig_url; // the zip's Ed25519 signature, the asset "<zip name>.sig" (64 bytes); "" when there is none
};
// One release object of GitHub's answer; the asset ending in "-<title_id>.zip".
bool ParseRelease(const std::string& json, const std::string& title_id, Release& out, std::string* why = nullptr);
// "1.10" > "1.9"; a leading "v" is ignored. <0, 0, >0.
int CompareVersions(const std::string& a, const std::string& b);
std::string Sha256Hex(const uint8_t* data, size_t size);
// Checks and installs the zip into config.app_dir (see above). "" when done, else why not (nothing changed then).
// expect_sha256 is required: without GitHub's SHA-256 of the zip nothing is installed. files_done follows the files.
// signature: the zip's Ed25519 signature (64 bytes), required when config.release_key is set.
std::string InstallZip(const std::vector<uint8_t>& zip, const std::string& expect_sha256, const Config& config,
	std::atomic<size_t>* files_done = nullptr, std::atomic<size_t>* files_total = nullptr,
	const std::vector<uint8_t>* signature = nullptr);
// The zip signed by config.release_key's private key (Ed25519, RFC 8032)? False for a malformed key or signature.
bool SignatureValid(const std::vector<uint8_t>& zip, const std::vector<uint8_t>& signature, const std::string& key_hex);
} // namespace update
