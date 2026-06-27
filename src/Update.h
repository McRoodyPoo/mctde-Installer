#pragma once
//
// Update: keep the installer exe itself current. The installer is meant to be
// passed around freely, so on launch it checks latest.txt for the advertised
// "mctde-installer" version and, if a newer one exists, downloads the new exe,
// swaps it in for the running one, and relaunches it.
//
#include <string>
#include "Download.h"   // ProgressFn

namespace mctde {

// Version baked into this build. Bump on every installer release and keep the
// "mctde-installer=" line in latest.txt in sync.
extern const char* const kInstallerVersion;

// Fetch latest.txt and return the advertised installer version ("" on any
// failure, including no network; callers should treat that as "up to date").
std::string fetchLatestInstallerVersion();

// True iff dotted-int 'latest' is strictly higher than 'current'. Unparseable
// 'latest' returns false, so a malformed manifest never forces an update.
bool isNewer(const std::string& latest, const std::string& current);

// Download the newest installer, swap it in for the running exe, and relaunch
// it (inheriting our elevated token, so no second UAC prompt). On success the
// caller should exit so the new instance takes over. false + err on any failure,
// in which case the running installer is left untouched and should carry on.
bool selfUpdate(std::string& err, const ProgressFn& progress = {});

// Best-effort delete of the "<self>.old" a previous self-update left behind.
// Safe to call on every launch.
void cleanupOldSelf();

} // namespace mctde
