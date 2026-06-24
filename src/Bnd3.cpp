#include "Bnd3.h"
#include "BinaryReader.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mctde {

// Read a NUL-terminated ASCII string at an absolute offset in the blob.
static std::string cstrAt(const std::vector<uint8_t>& d, size_t off) {
    std::string s;
    while (off < d.size() && d[off] != 0) s.push_back(char(d[off++]));
    return s;
}

// Normalize an internal BND name to an output-relative path:
//   backslashes -> slashes, strip the dev-root prefix, drop leading '/'.
static std::string normalizeName(std::string n) {
    for (char& c : n) if (c == '\\') c = '/';

    // Strip a leading dev root like "N:/FRPG/data/INTERROOT_win32" (or _x64
    // etc). Heuristic: if it starts with a drive letter, cut to the first
    // recognized data subdir.
    if (n.size() >= 2 && n[1] == ':') {
        std::string lower = n;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        size_t cut = std::string::npos;
        for (const char* dir : {"/map/", "/chr/", "/obj/", "/parts/", "/sfx/",
                                "/menu/", "/other/", "/facegen/"}) {
            size_t p = lower.find(dir);
            if (p != std::string::npos && (cut == std::string::npos || p < cut)) cut = p;
        }
        if (cut != std::string::npos) n = n.substr(cut);
    }
    while (!n.empty() && n.front() == '/') n.erase(0, 1);
    return n;
}

Bnd3 parseBnd3(const std::vector<uint8_t>& header) {
    BinaryReader r(header);

    std::string magic = r.ascii(4);
    if (magic != "BND3" && magic != "BHF3")
        throw std::runtime_error("BND3: bad magic '" + magic + "'");

    Bnd3 out;
    out.split = (magic == "BHF3");

    r.ascii(8);                     // version signature, e.g. "07D7R6\0\0"
    uint8_t rawFormat    = r.u8();  // 0x0C
    r.u8();                         // 0x0D bigEndian
    uint8_t bitBigEndian = r.u8();  // 0x0E
    r.u8();                         // 0x0F
    uint32_t count = r.u32();       // 0x10
    r.u32();                        // header_size
    r.u32();                        // 0
    r.u32();                        // 0

    // DS1 stores the format byte bit-reversed unless bitBigEndian is set.
    // e.g. on-disk 0x74 -> 0x2E (IDs | Names1 | Names2 | Compression).
    uint8_t format = rawFormat;
    if (!bitBigEndian) {
        uint8_t rev = 0;
        for (int b = 0; b < 8; ++b)
            if (rawFormat & (1u << b)) rev |= uint8_t(1u << (7 - b));
        format = rev;
    }

    // Each record's fields are gated by the format bitfield (SoulsFormats):
    //   0x02 IDs   0x04|0x08 Names   0x10 LongOffsets(64-bit)   0x20 Compression
    const bool hasIDs   = (format & 0x02) != 0;
    const bool hasNames = (format & (0x04 | 0x08)) != 0;
    const bool longOff  = (format & 0x10) != 0;
    const bool hasComp  = (format & 0x20) != 0;
    if (!hasNames)
        throw std::runtime_error("BND3: format 0x" + std::to_string(format) +
                                 " has no names (cannot place files)");

    out.entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        r.u32();                                       // file flags byte + 3 pad
        uint32_t size       = r.u32();                 // compressed size
        uint64_t offset     = longOff ? r.u64() : r.u32();
        uint32_t id         = hasIDs ? r.u32() : 0;
        uint32_t nameOffset = hasNames ? r.u32() : 0;
        if (hasComp) r.u32();                          // uncompressed size (unused)

        Bnd3Entry e;
        e.size = size;
        e.offset = offset;
        e.id = id;
        e.rawName = cstrAt(header, nameOffset);
        e.name = normalizeName(e.rawName);
        out.entries.push_back(std::move(e));
    }
    return out;
}

} // namespace mctde
