#pragma once
//
// DS1/PTDE dvdbnd path hashing + the hash->path namelist.
//
// Each archive record is keyed by a 32-bit hash of its virtual path. A
// namelist maps those hashes back to real paths so the unpacker can write
// files to their correct on-disk locations.
//
#include <cstdint>
#include <string>
#include <unordered_map>

namespace mctde {

// Hash a virtual path the way the PTDE engine does: normalize (lowercase,
// backslashes -> forward slashes), then fold with h = h*37 + byte.
uint32_t hashPath(const std::string& path);

// The namelist embedded into the binary (see NamelistData.cpp).
extern const unsigned char kNamelistData[];
extern const unsigned int kNamelistLen;

class NameMap {
public:
    // Load a namelist file: one virtual path per line. Lines that do not start
    // with '/' (comments, blanks) are ignored. Returns the number of paths
    // loaded. Throws std::runtime_error if the file cannot be opened.
    size_t load(const std::string& file);

    // Load the namelist embedded in the binary (no external file needed).
    size_t loadEmbedded();

    // Parse a namelist from an in-memory buffer.
    size_t loadString(const char* data, size_t len);

    size_t size() const { return byHash_.size(); }
    bool contains(uint32_t hash) const { return byHash_.count(hash) != 0; }

    // Returns the path for a hash, or nullptr if the hash is unknown.
    const std::string* find(uint32_t hash) const;

private:
    std::unordered_map<uint32_t, std::string> byHash_;
};

} // namespace mctde
