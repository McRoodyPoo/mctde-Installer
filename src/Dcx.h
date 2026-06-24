#pragma once
//
// DCX: FromSoftware's compression container (DS1 uses the "DFLT" = zlib
// variant). Many files inside a dvdbnd are wrapped in DCX.
//
#include <cstdint>
#include <vector>

namespace mctde {

// True if the blob begins with a DCX container ("DCX\0").
bool isDcx(const std::vector<uint8_t>& data);

// If `data` is a DCX container, return the decompressed payload.
// Otherwise return `data` unchanged. Throws std::runtime_error on a
// malformed/unsupported container.
std::vector<uint8_t> dcxDecompress(const std::vector<uint8_t>& data);

} // namespace mctde
