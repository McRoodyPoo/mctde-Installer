#include "Detect.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include <utility>

#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace mctde {

// Read a string value from the registry (returns "" if absent).
static std::string regString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD len = sizeof(buf);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, nullptr, buf, &len) != ERROR_SUCCESS)
        return "";
    std::wstring w(buf);
    return std::string(w.begin(), w.end());  // Steam paths are ASCII
}

static std::string steamRoot() {
    std::string p = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (p.empty())
        p = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    for (char& c : p) if (c == '/') c = '\\';
    return p;
}

// Parse the "path" entries out of libraryfolders.vdf (Valve KeyValues text).
static std::vector<std::string> libraryPaths(const std::string& steam) {
    std::vector<std::string> libs;
    if (!steam.empty()) libs.push_back(steam);

    std::string vdf = steam + "\\steamapps\\libraryfolders.vdf";
    std::ifstream f(vdf, std::ios::binary);
    if (!f) return libs;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const std::string key = "\"path\"";
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        pos += key.size();
        size_t a = text.find('"', pos);
        if (a == std::string::npos) break;
        size_t b = text.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string raw = text.substr(a + 1, b - a - 1);
        // VDF escapes backslashes as "\\".
        std::string path;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') { path += '\\'; ++i; }
            else path += raw[i];
        }
        libs.push_back(path);
        pos = b + 1;
    }
    return libs;
}

std::string detectSteamDataDir() {
    const std::string sub = "\\steamapps\\common\\Dark Souls Prepare to Die Edition\\DATA";
    for (const std::string& lib : libraryPaths(steamRoot())) {
        std::string data = lib + sub;
        std::error_code ec;
        if (fs::exists(fs::path(data) / "DARKSOULS.exe", ec))
            return data;
    }
    return "";
}

static std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        std::wstring w(p);
        out.assign(w.begin(), w.end());
    }
    if (p) CoTaskMemFree(p);
    return out;
}

// Directories that never hold the game and only waste the scan budget (or
// surface deleted copies). Compared case-insensitively on the folder name.
static bool isJunkDir(const std::wstring& nameRaw) {
    std::wstring n = nameRaw;
    for (wchar_t& c : n) c = towlower(c);
    return n == L"$recycle.bin" || n == L"system volume information" ||
           n == L"windows" || n == L"$windows.~ws" || n == L"$winreagent" ||
           n == L"$sysreset" || n == L"msocache" || n == L"windows.old" ||
           n == L"unpackds-backup" || n == L"mctde-backup";  // archive backups, not installs
}

// Depth- and time-bounded recursive search for DARKSOULS.exe under root.
static std::string searchRoot(const fs::path& root, int maxDepth, ULONGLONG deadline) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return "";
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return "";
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (GetTickCount64() > deadline) break;
        try {
            if (it.depth() >= maxDepth) it.disable_recursion_pending();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (isJunkDir(it->path().filename().wstring())) it.disable_recursion_pending();
            } else if (it->is_regular_file(ec2) &&
                       _wcsicmp(it->path().filename().c_str(), L"DARKSOULS.exe") == 0) {
                return it->path().parent_path().string();
            }
        } catch (const std::exception&) { /* skip un-representable entry */ }
    }
    return "";
}

std::string findDataDir() {
    // 1. Steam registry — fast and almost always right.
    std::string d = detectSteamDataDir();
    if (!d.empty()) return d;

    const std::string sub = "\\steamapps\\common\\Dark Souls Prepare to Die Edition\\DATA";

    // 2. Likely fixed Steam-library locations on every drive.
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        std::string r(1, char('A' + i));
        r += ":";
        for (const char* p : {"\\Steam", "\\SteamLibrary", "\\Games\\Steam",
                              "\\Program Files (x86)\\Steam", "\\Program Files\\Steam",
                              "\\SteamLibrary\\steamapps\\common"}) {
            std::error_code ec;
            std::string cand = r + p + sub;
            if (fs::exists(fs::path(cand) / "DARKSOULS.exe", ec)) return cand;
        }
    }

    // 3. Bounded recursive scan, sharing one ~20s budget across roots.
    ULONGLONG deadline = GetTickCount64() + 20000;
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    std::vector<std::pair<fs::path, int>> roots;
    roots.push_back({fs::path(self).parent_path(), 6});          // installer's own folder
    roots.push_back({knownFolder(FOLDERID_Desktop), 4});
    roots.push_back({knownFolder(FOLDERID_Downloads), 4});
    roots.push_back({knownFolder(FOLDERID_Documents), 4});
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        roots.push_back({std::string(1, char('A' + i)) + ":\\", 4}); // drive roots, shallow
    }

    for (const auto& r : roots) {
        if (GetTickCount64() > deadline) break;
        std::string hit = searchRoot(r.first, r.second, deadline);
        if (!hit.empty()) return hit;
    }
    return "";
}

// Like searchRoot, but reports every DARKSOULS.exe folder it finds (not just
// the first) via onHit.
static void searchRootAll(const fs::path& root, int maxDepth, ULONGLONG deadline,
                          const std::function<void(const std::string&)>& onHit) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (GetTickCount64() > deadline) break;
        try {
            if (it.depth() >= maxDepth) it.disable_recursion_pending();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (isJunkDir(it->path().filename().wstring())) it.disable_recursion_pending();
            } else if (it->is_regular_file(ec2) &&
                       _wcsicmp(it->path().filename().c_str(), L"DARKSOULS.exe") == 0) {
                onHit(it->path().parent_path().string());
            }
        } catch (const std::exception&) { /* skip un-representable entry */ }
    }
}

void findAllDataDirs(const std::function<void(const GameInstall&)>& onFound) {
    std::set<std::string> seen;
    auto report = [&](const std::string& dir) {
        std::string key = dir;
        for (char& c : key) c = char(std::tolower((unsigned char)c));
        if (!seen.insert(key).second) return;  // already reported
        bool steam = key.find("steamapps") != std::string::npos;
        onFound(GameInstall{dir, steam, detectGameState(dir)});
    };

    const std::string sub = "\\steamapps\\common\\Dark Souls Prepare to Die Edition\\DATA";

    // 1. Steam libraries (registry + libraryfolders.vdf) — fast, most likely.
    for (const std::string& lib : libraryPaths(steamRoot())) {
        std::error_code ec;
        std::string data = lib + sub;
        if (fs::exists(fs::path(data) / "DARKSOULS.exe", ec)) report(data);
    }

    // 2. Fixed Steam-library spots on every drive.
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        std::string r(1, char('A' + i));
        r += ":";
        for (const char* p : {"\\Steam", "\\SteamLibrary", "\\Games\\Steam",
                              "\\Program Files (x86)\\Steam", "\\Program Files\\Steam",
                              "\\SteamLibrary\\steamapps\\common"}) {
            std::error_code ec;
            std::string cand = r + p + sub;
            if (fs::exists(fs::path(cand) / "DARKSOULS.exe", ec)) report(cand);
        }
    }

    // 3. Bounded recursive scan (installer folder, user folders, drive roots).
    ULONGLONG deadline = GetTickCount64() + 20000;
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::vector<std::pair<fs::path, int>> roots;
    roots.push_back({fs::path(self).parent_path(), 6});
    roots.push_back({knownFolder(FOLDERID_Desktop), 4});
    roots.push_back({knownFolder(FOLDERID_Downloads), 4});
    roots.push_back({knownFolder(FOLDERID_Documents), 4});
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        roots.push_back({std::string(1, char('A' + i)) + ":\\", 4});
    }
    for (const auto& r : roots) {
        if (GetTickCount64() > deadline) break;
        searchRootAll(r.first, r.second, deadline, report);
    }
}

GameState detectGameState(const std::string& dataDir) {
    fs::path d(dataDir);
    std::error_code ec;

    bool packed = false;
    for (int i = 0; i < 4; ++i)
        if (fs::exists(d / ("dvdbnd" + std::to_string(i) + ".bhd5"), ec)) { packed = true; break; }
    if (packed) return GameState::Packed;

    bool unpacked = fs::exists(d / ".mctde-unpacked", ec) ||
                    fs::is_directory(d / "chr", ec) ||
                    fs::is_directory(d / "map", ec);
    return unpacked ? GameState::Unpacked : GameState::Unknown;
}

} // namespace mctde
