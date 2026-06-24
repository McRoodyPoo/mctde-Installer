#include "ExePatch.h"
#include "Sha256.h"

#include <fstream>
#include <vector>

namespace mctde {

// PTDE Steam DARKSOULS.exe — clean and UDSFM-patched SHA-256 (lowercase).
static const char* kCleanSha =
    "67bcab513c8f0ed6164279d85f302e06b1d8a53abff5df7f3d10e1d4dfd81459";
static const char* kPatchedSha =
    "903a946273bfe123fe5c85740c3613374e2cf538564bb661db371c6cb5a421ff";

static std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n > 0 ? size_t(n) : 0);
    if (n > 0) f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

static bool writeAll(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    if (!data.empty())
        o.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    return bool(o);
}

PatchResult patchExe(const std::string& inExe, const std::string& outExe,
                     std::string& message) {
    std::vector<uint8_t> data = readAll(inExe);
    if (data.empty()) { message = "cannot read " + inExe; return PatchResult::Failed; }

    std::string sha = sha256Hex(data);
    if (sha == kPatchedSha) {
        message = "exe is already patched (no changes needed)";
        return PatchResult::AlreadyPatched;
    }
    if (sha != kCleanSha) {
        message = "unrecognized DARKSOULS.exe (sha256 " + sha +
                  "); refusing to patch an unknown build";
        return PatchResult::UnknownExe;
    }

    // Apply the byte-diff, verifying every original byte first.
    for (unsigned i = 0; i < exePatchTableLen; ++i) {
        const ExePatchByte& p = exePatchTable[i];
        if (p.offset >= data.size() || data[p.offset] != p.oldVal) {
            message = "patch site mismatch at offset " + std::to_string(p.offset);
            return PatchResult::Failed;
        }
        data[p.offset] = p.newVal;
    }

    std::string outSha = sha256Hex(data);
    if (outSha != kPatchedSha) {
        message = "post-patch sha256 mismatch (" + outSha + ")";
        return PatchResult::Failed;
    }
    if (!writeAll(outExe, data)) {
        message = "cannot write " + outExe;
        return PatchResult::Failed;
    }
    message = "patched OK -> " + outExe;
    return PatchResult::Patched;
}

} // namespace mctde
