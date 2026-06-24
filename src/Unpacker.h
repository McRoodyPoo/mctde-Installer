#pragma once
//
// Unpacker: turns the dvdbnd archive pairs into loose files on disk, the way
// the patched engine expects to read them (DCX entries decompressed, written
// at their real virtual paths).
//
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mctde {

struct UnpackStats {
    size_t files = 0;         // files written
    size_t decompressed = 0;  // DCX entries inflated
    size_t unknown = 0;       // records with no namelist entry
    size_t errors = 0;        // read/inflate failures
};

// Invoked once per file as it is written, with the file's loose relative path
// and the running file count. Optional — pass {} to skip reporting.
using UnpackProgress = std::function<void(const std::string& relPath, size_t fileIndex)>;

// Unpack dvdbnd0-3 found in `dataDir` into `outDir`, naming files via the
// namelist at `namelistFile`. Unknown-hash records go to _unknown/<hash>.bin.
UnpackStats unpackAll(const std::string& dataDir,
                      const std::string& outDir,
                      const std::string& namelistFile,
                      const UnpackProgress& progress = {});

// Second-level pass: unpack inner split archives found under outDir, then
// delete them. *.tpfbhd/.tpfbdt -> map/tx/, *.hkxbhd/.hkxbdt -> alongside the
// archive. Accumulates into `st`.
void unpackNested(const std::string& outDir, UnpackStats& st,
                  const UnpackProgress& progress = {});

// Extract a single virtual path (e.g. "/chr/c0000.chrbnd.dcx") from whichever
// dvdbnd in `dataDir` contains it. Returns the (DCX-decompressed) bytes.
// Throws std::runtime_error if the path is not found.
std::vector<uint8_t> extractOne(const std::string& dataDir,
                                const std::string& virtualPath);

} // namespace mctde
