#include "Bhd5.h"
#include "BinaryReader.h"
#include <stdexcept>

namespace mctde {

// PTDE BHD5 header (all little-endian):
//   0x00  char[4]  "BHD5"
//   0x04  u8       big-endian flag (0x00 for PTDE)
//   0x05  u8       unk
//   0x06  u16      padding
//   0x08  u32      version (== 1)
//   0x0C  u32      file size (informational)
//   0x10  u32      bucket count
//   0x14  u32      buckets offset
//
// Buckets live at `buckets offset`, each: { u32 count, u32 offset }.
// Records live at each bucket's offset, each 16 bytes:
//   u32 nameHash, u32 size, u64 offset
// (The high dword of `offset` is always 0 on PTDE, but we read the full 64.)
Bhd5 parseBhd5(const std::vector<uint8_t>& data) {
    BinaryReader r(data);

    if (r.ascii(4) != "BHD5")
        throw std::runtime_error("BHD5: bad magic");

    r.u8();                          // endian flag
    r.u8();                          // unk05
    r.u16();                         // padding
    r.u32();                         // version (1 on PTDE)
    r.u32();                         // file size
    uint32_t bucketCount  = r.u32();
    uint32_t bucketsOffset = r.u32();

    struct Bucket { uint32_t count; uint32_t offset; };
    std::vector<Bucket> buckets;
    buckets.reserve(bucketCount);

    r.seek(bucketsOffset);
    for (uint32_t i = 0; i < bucketCount; ++i) {
        uint32_t count  = r.u32();
        uint32_t offset = r.u32();
        buckets.push_back({count, offset});
    }

    Bhd5 out;
    for (const Bucket& b : buckets) {
        r.seek(b.offset);
        for (uint32_t i = 0; i < b.count; ++i) {
            Bhd5Record rec;
            rec.nameHash = r.u32();
            rec.size     = r.u32();
            rec.offset   = r.u64();
            out.records.push_back(rec);
        }
    }
    return out;
}

} // namespace mctde
