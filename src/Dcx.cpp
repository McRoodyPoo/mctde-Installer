#include "Dcx.h"
#include "miniz.h"
#include <stdexcept>
#include <string>

namespace mctde {

// DCX numeric fields are big-endian.
static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// Find a 4-byte section marker within the first `limit` bytes. -1 if absent.
// `marker` is a 4-char literal whose terminating NUL is the 4th byte
// (e.g. "DCS" -> {'D','C','S','\0'}).
static long findMarker(const std::vector<uint8_t>& d, const char* marker, size_t limit) {
    const uint8_t m[4] = {
        uint8_t(marker[0]), uint8_t(marker[1]), uint8_t(marker[2]), uint8_t(marker[3])
    };
    size_t end = d.size() < limit ? d.size() : limit;
    for (size_t i = 0; i + 4 <= end; ++i) {
        if (d[i] == m[0] && d[i + 1] == m[1] && d[i + 2] == m[2] && d[i + 3] == m[3])
            return long(i);
    }
    return -1;
}

bool isDcx(const std::vector<uint8_t>& data) {
    return data.size() >= 4 &&
           data[0] == 'D' && data[1] == 'C' && data[2] == 'X' && data[3] == '\0';
}

// DCX layout (DS1 / DFLT):
//   "DCX\0" ... "DCS\0" + u32 uncompressedSize + u32 compressedSize
//   ... "DCP\0" "DFLT" ... "DCA\0" + u32 dcaHeaderSize, then the zlib payload.
// We locate the DCS and DCA markers rather than hard-coding offsets so minor
// header variants still parse.
std::vector<uint8_t> dcxDecompress(const std::vector<uint8_t>& data) {
    if (!isDcx(data)) return data;

    long dcs = findMarker(data, "DCS", 0x100);
    if (dcs < 0) throw std::runtime_error("DCX: missing DCS section");
    if (size_t(dcs) + 12 > data.size()) throw std::runtime_error("DCX: truncated DCS");
    uint32_t uncompressedSize = be32(&data[dcs + 4]);
    uint32_t compressedSize   = be32(&data[dcs + 8]);

    long dca = findMarker(data, "DCA", 0x100);
    if (dca < 0) throw std::runtime_error("DCX: missing DCA section");
    if (size_t(dca) + 8 > data.size()) throw std::runtime_error("DCX: truncated DCA");
    uint32_t dcaHeaderSize = be32(&data[dca + 4]);

    size_t payloadOffset = size_t(dca) + dcaHeaderSize;
    if (payloadOffset + compressedSize > data.size())
        throw std::runtime_error("DCX: payload out of range");

    // Payload is a zlib stream (RFC 1950).
    std::vector<uint8_t> out(uncompressedSize);
    mz_ulong destLen = uncompressedSize;
    int rc = mz_uncompress(out.data(), &destLen, &data[payloadOffset], compressedSize);
    if (rc != MZ_OK)
        throw std::runtime_error("DCX: zlib inflate failed, code " + std::to_string(rc));
    out.resize(destLen);
    return out;
}

} // namespace mctde
