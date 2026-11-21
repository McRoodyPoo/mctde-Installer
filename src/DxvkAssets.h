#pragma once
#include <cstddef>
#include <cstdint>

namespace mctde {

// Accessors for the DXVK payload embedded as RCDATA (dxvk_assets.rc). Windows-only:
// DXVK is bundled only in the Windows build (on Linux, Proton provides its own DXVK),
// so these return false on non-Windows. On Windows they point at bytes inside the
// module's resource section, valid for the process lifetime (no copy/free needed).
bool dxvkDllBytes(const uint8_t** data, size_t* len);
bool dxvkConfBytes(const uint8_t** data, size_t* len);

} // namespace mctde
