#pragma once
//
// Release: the shared plumbing for "fetch the build that matches this OS".
//
// Every mctde release ships one zip per platform, attached to a single GitHub
// release per version, e.g. for mctde-Installer v0.4.0:
//   mctde-Installer-windows.zip  (mctde-Installer.exe + README/LICENSE/CREDITS)
//   mctde-Installer-linux.zip    (mctde-Installer-x86_64.AppImage + docs)
// each with a "<zip>.sha256" sidecar. Versions are shared across platforms and
// announced in the central manifest, mctde-Link's main/latest.txt.
//
#include <string>
#include "Download.h"   // ProgressFn

namespace mctde {

// "windows" or "linux": the platform suffix of the release zips this build uses.
const char* platformName();

// The central manifest (mctde-Link main/latest.txt). "" on any failure.
std::string fetchManifest();

// Value for `key` in a key=value manifest ("" if absent). Keys are matched
// case-insensitively with '_' treated as '-'; '#' starts a comment.
std::string manifestValue(const std::string& text, const std::string& key);

// Download the zip at `zipUrl` to `zipPath` and check it against its
// "<zipUrl>.sha256" sidecar when one is published (a mismatch is fatal, and
// removes the download).
bool downloadVerifiedZip(const std::string& zipUrl, const std::string& zipPath,
                         std::string& err, const ProgressFn& progress = {});

// downloadVerifiedZip, then extract the entry named `member` (found anywhere in
// the zip) to `outPath`. The zip is deleted after.
bool fetchFromReleaseZip(const std::string& zipUrl, const std::string& member,
                         const std::string& outPath, std::string& err,
                         const ProgressFn& progress = {});

} // namespace mctde
