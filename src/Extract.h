#pragma once
//
// Extract: unzip a .zip payload into a destination folder (miniz-backed).
//
#include <string>

namespace mctde {

struct ExtractStats {
    size_t files = 0;
    size_t errors = 0;
};

// Extract `zipPath` into `outDir` (creating directories, overwriting files).
// Rejects unsafe entry paths (zip-slip). On a fatal error sets `err`.
ExtractStats extractZip(const std::string& zipPath, const std::string& outDir,
                        std::string& err);

// Extract the first entry whose file name (ignoring any folders, case-insensitive)
// is `baseName` to exactly `outPath`. Used to pull one program out of a release
// zip (mctde-Installer-windows.zip -> mctde-Installer.exe, ...). false + err if
// the zip can't be read or holds no such file.
bool extractZipEntry(const std::string& zipPath, const std::string& baseName,
                     const std::string& outPath, std::string& err);

} // namespace mctde
