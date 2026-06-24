#pragma once
//
// BND3: FromSoftware's general archive (DS1). Two flavors:
//   - inline:  "BND3" magic, file data lives in the same blob.
//   - split:   "BHF3" header + "BDF3" data file (e.g. *.tpfbhd / *.tpfbdt).
// Entries carry their own names, so no namelist is needed.
//
#include <cstdint>
#include <string>
#include <vector>

namespace mctde {

struct Bnd3Entry {
    std::string name;   // normalized: prefix-stripped, forward slashes, leading '/' removed
    std::string rawName;// original internal name (for diagnostics)
    uint32_t size;      // bytes of the (possibly DCX) packed file
    uint64_t offset;    // offset of data: into the BDF3 (split) or this blob (inline)
    uint32_t id;
};

struct Bnd3 {
    bool split = false;            // true for BHF3 (data in a separate BDF3)
    std::vector<Bnd3Entry> entries;
};

// Parse a BND3/BHF3 header blob. Throws std::runtime_error on malformed input.
Bnd3 parseBnd3(const std::vector<uint8_t>& header);

} // namespace mctde
