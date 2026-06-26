#pragma once
//
// Embedded vanilla baseline used to classify an install as clean vs. modified.
// Generated from a reference unpack of canonical PTDE.
//
//   kVanillaManifest  - newline-separated "<relpath>\t<size>" for every file in a
//                       clean *unpacked* game's param/chr/event/script dirs. Used
//                       to spot a modified loose install (a vanilla-named file in
//                       those dirs with a different size => modified).
//   kVanillaArchives  - the packed dvdbnd file sizes, to spot a modified *packed*
//                       install (those four dirs live inside the archives there).
//
#include <cstddef>
#include <cstdint>

namespace mctde {

extern const char* const kVanillaManifest;

struct VanillaArchive { const char* name; uint64_t size; };
extern const VanillaArchive kVanillaArchives[];
extern const size_t kVanillaArchiveCount;

} // namespace mctde
