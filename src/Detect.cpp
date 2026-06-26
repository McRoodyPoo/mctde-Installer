#include "Detect.h"
#include "VanillaManifest.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include <utility>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>
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
    // DATA-Backup-Packed / DATA-Backup-Unpacked are our own backups: they hold a
    // copy of DARKSOULS.exe, so they must not be picked up as separate installs.
    if (n.rfind(L"data-backup", 0) == 0) return true;
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
        GameState state = detectGameState(dir);
        onFound(GameInstall{dir, steam, state, classifyInstall(dir, state)});
    };
    // Report an install plus any of the installer's own backups sitting beside it,
    // so DATA-Backup-* folders show up in the list (classified as Backup) instead
    // of being invisible.
    auto reportInstall = [&](const std::string& dir) {
        report(dir);
        for (const auto& b : findBackups(dir)) report(b.path);
    };

    const std::string sub = "\\steamapps\\common\\Dark Souls Prepare to Die Edition\\DATA";

    // 1. Steam libraries (registry + libraryfolders.vdf) — fast, most likely.
    for (const std::string& lib : libraryPaths(steamRoot())) {
        std::error_code ec;
        std::string data = lib + sub;
        if (fs::exists(fs::path(data) / "DARKSOULS.exe", ec)) reportInstall(data);
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
            if (fs::exists(fs::path(cand) / "DARKSOULS.exe", ec)) reportInstall(cand);
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
        searchRootAll(r.first, r.second, deadline, reportInstall);
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

std::vector<BackupInfo> findBackups(const std::string& dataDir) {
    std::vector<BackupInfo> out;
    std::error_code ec;
    fs::path data(dataDir);
    fs::path parent = data.parent_path();
    if (parent.empty()) return out;

    for (fs::directory_iterator it(parent, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_directory(ec)) continue;

        // Folder name must start with "data-backup" (our backups, plus the
        // numbered "(2)" variants), and it must actually hold a game.
        std::wstring n = it->path().filename().wstring();
        for (wchar_t& c : n) c = towlower(c);
        if (n.rfind(L"data-backup", 0) != 0) continue;
        if (!fs::exists(it->path() / "DARKSOULS.exe", ec)) continue;

        std::string p = it->path().string();
        out.push_back(BackupInfo{p, it->path().filename().string(), detectGameState(p)});
    }

    // Packed backups (the pristine originals) first, then by name so the numbered
    // variants stay in order.
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
        if ((a.state == GameState::Packed) != (b.state == GameState::Packed))
            return a.state == GameState::Packed;
        return a.name < b.name;
    });
    return out;
}

// Parse the embedded vanilla manifest once into relpath -> size.
static const std::unordered_map<std::string, uint64_t>& vanillaSizes() {
    static const std::unordered_map<std::string, uint64_t> m = [] {
        std::unordered_map<std::string, uint64_t> r;
        std::string line;
        for (const char* c = kVanillaManifest;; ++c) {
            if (*c == '\n' || *c == '\0') {
                size_t tab = line.find('\t');
                if (tab != std::string::npos && tab > 0)
                    r.emplace(line.substr(0, tab),
                              std::strtoull(line.c_str() + tab + 1, nullptr, 10));
                line.clear();
                if (*c == '\0') break;
            } else if (*c != '\r') {
                line.push_back(*c);
            }
        }
        return r;
    }();
    return m;
}

static bool hasMctde(const fs::path& data) {
    std::error_code ec;
    return fs::exists(data / "mctde-link.ini", ec) ||
           fs::exists(data / "mctde_launcher.exe", ec);
}

// A loose install is modified if any vanilla param/chr/event/script file is
// present with a different size. Extra files are ignored; a missing vanilla file
// is not treated as a modification (lenient). Short-circuits on the first hit.
static bool hasModifiedCoreFiles(const fs::path& data) {
    std::error_code ec;
    for (const auto& kv : vanillaSizes()) {
        uint64_t sz = (uint64_t)fs::file_size(data / kv.first, ec);
        if (!ec && sz != kv.second) return true;
    }
    return false;
}

// A packed install is modified if any vanilla dvdbnd file is missing or a
// different size (those four dirs live inside the archives when packed).
static bool hasModifiedArchives(const fs::path& data) {
    std::error_code ec;
    for (size_t i = 0; i < kVanillaArchiveCount; ++i) {
        uint64_t sz = (uint64_t)fs::file_size(data / kVanillaArchives[i].name, ec);
        if (ec || sz != kVanillaArchives[i].size) return true;
    }
    return false;
}

InstallKind classifyInstall(const std::string& dataDir, GameState state) {
    fs::path data(dataDir);
    if (hasMctde(data)) return InstallKind::Mctde;
    if (state == GameState::Packed)
        return hasModifiedArchives(data) ? InstallKind::Unknown : InstallKind::Ready;
    if (state == GameState::Unpacked)
        return hasModifiedCoreFiles(data) ? InstallKind::Unknown : InstallKind::Ready;
    return InstallKind::Unknown;
}

} // namespace mctde
