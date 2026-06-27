#pragma once
//
// Download: stream an http(s) URL to a file via WinHTTP, with progress and a
// Google Drive large-file helper (handles the "can't scan for viruses" confirm
// page).
//
#include <cstdint>
#include <functional>
#include <string>

namespace mctde {

// progress(received, total): return false to cancel. total == 0 if unknown.
using ProgressFn = std::function<bool(uint64_t received, uint64_t total)>;

// Stream any http(s) URL to outPath (follows redirects). false + err on failure.
bool downloadUrl(const std::string& url, const std::string& outPath,
                 std::string& err, const ProgressFn& progress = {});

// Download a Google Drive file by its file id, working around the large-file
// confirm interstitial.
bool downloadGoogleDrive(const std::string& fileId, const std::string& outPath,
                         std::string& err, const ProgressFn& progress = {});

} // namespace mctde
