#include "DxvkAssets.h"

// DXVK is a Windows-only payload (Linux uses Proton's own DXVK), so the whole
// implementation is gated to Windows. On other platforms this is an empty TU and the
// accessors below provide the "not available" answer.
#ifdef _WIN32

#include "gui_resource.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mctde {

static bool loadRcData(int id, const uint8_t** data, size_t* len) {
    *data = nullptr;
    *len = 0;
    HMODULE mod = GetModuleHandleW(nullptr);           // the running exe (GUI or CLI)
    // RT_RCDATA (value 10) built wide, so this compiles whether or not UNICODE is
    // defined for this translation unit (the CLI build doesn't define it).
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!res) return false;
    HGLOBAL h = LoadResource(mod, res);
    if (!h) return false;
    const void* p = LockResource(h);                   // no real lock; valid for process life
    DWORD n = SizeofResource(mod, res);
    if (!p || n == 0) return false;
    *data = static_cast<const uint8_t*>(p);
    *len = static_cast<size_t>(n);
    return true;
}

bool dxvkDllBytes(const uint8_t** data, size_t* len)  { return loadRcData(IDR_DXVK_DLL, data, len); }
bool dxvkConfBytes(const uint8_t** data, size_t* len) { return loadRcData(IDR_DXVK_CONF, data, len); }

} // namespace mctde

#else   // non-Windows: DXVK isn't bundled.

namespace mctde {
bool dxvkDllBytes(const uint8_t** data, size_t* len)  { *data = nullptr; *len = 0; return false; }
bool dxvkConfBytes(const uint8_t** data, size_t* len) { *data = nullptr; *len = 0; return false; }
}

#endif
