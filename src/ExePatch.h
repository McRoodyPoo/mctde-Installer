#pragma once
//
// ExePatch: turns the clean DARKSOULS.exe (PTDE Steam build) into the
// UDSFM-patched version that reads loose files. The patch is the exact
// byte-diff of clean vs patched, applied only after a SHA-256 match so an
// unknown/wrong build is never touched. The input file is never modified.
//
#include <cstdint>
#include <string>

namespace mctde {

struct ExePatchByte {
    uint32_t offset;
    uint8_t  oldVal;  // expected byte in the clean exe (verified before patching)
    uint8_t  newVal;  // patched byte
};
extern const ExePatchByte exePatchTable[];
extern const unsigned int exePatchTableLen;

enum class PatchResult { Patched, AlreadyPatched, UnknownExe, Failed };

// Patch `inExe` -> `outExe` (never modifies inExe). `message` gets a
// human-readable explanation. Verifies the output reproduces the known patched
// SHA-256 before writing.
PatchResult patchExe(const std::string& inExe, const std::string& outExe,
                     std::string& message);

} // namespace mctde
