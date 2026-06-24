#pragma once
//
// BHD5: the header half of a Dark Souls (PTDE) dvdbnd archive pair.
// It is a flat table of file records; the matching .bdt holds the data.
//
#include <cstdint>
#include <vector>

namespace mctde {

struct Bhd5Record {
    uint32_t nameHash;  // DS1 path hash (see NameHash)
    uint32_t size;      // bytes to read from the .bdt at `offset`
    uint64_t offset;    // absolute offset into the .bdt
};

struct Bhd5 {
    std::vector<Bhd5Record> records;
};

// Parse a PTDE BHD5 blob. Throws std::runtime_error on malformed input.
Bhd5 parseBhd5(const std::vector<uint8_t>& data);

} // namespace mctde
