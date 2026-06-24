#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mctde {

// Lowercase hex SHA-256 of a buffer.
std::string sha256Hex(const uint8_t* data, size_t len);
inline std::string sha256Hex(const std::vector<uint8_t>& v) {
    return sha256Hex(v.data(), v.size());
}

} // namespace mctde
