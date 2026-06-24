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

} // namespace mctde
