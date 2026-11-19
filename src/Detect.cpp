#include "Detect.h"
#include "VanillaManifest.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <utility>

#include <algorithm>
#include <chrono>
#include <cstdint>
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

// Milliseconds on a monotonic clock (scan time budget).
static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// "<lib>/steamapps/common/Dark Souls Prepare to Die Edition/DATA" in the
// platform's separator style.
static std::string ptdeDataIn(const std::string& lib) {
    return (fs::path(lib) / "steamapps" / "common" / "Dark Souls Prepare to Die Edition" / "DATA").string();
}

#ifdef _WIN32
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
#else
static std::string steamRoot() { return ""; }   // no registry on native Linux
#endif

// ---------------------------------------------------------------------------
// Linux (Wine / Proton) support.
//
// Under Wine the registry only knows about a Windows Steam inside the prefix
// (Proton even writes a fake one), so a native Linux Steam library is
// invisible to the lookup above. Wine maps the Unix filesystem onto a drive
// letter (normally Z:), so we find the Linux Steam roots from $HOME and convert
// their Unix paths to DOS paths the rest of the code can use as-is.
// (Wine does not pass $HOME through; the home comes from WINEHOMEDIR.)
// ---------------------------------------------------------------------------

#ifdef _WIN32
bool isWine() {
    static const bool wine = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return wine;
}

// Wine's version string ("9.0", "11.22", ...), or "" when not under Wine.
static std::string wineVersion() {
    typedef const char* (CDECL *WineGetVersion)(void);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    WineGetVersion fn = ntdll ? (WineGetVersion)GetProcAddress(ntdll, "wine_get_version") : nullptr;
    const char* v = fn ? fn() : nullptr;
    return v ? std::string(v) : std::string();
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// An environment variable of this process, or "" if unset.
// Note Wine does NOT pass the Unix $HOME through; it sets WINEHOMEDIR instead.
static std::string envVar(const char* name) {
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, 4096);
    if (n == 0 || n >= 4096) return "";
    return narrow(std::wstring(buf, n));
}
#else
// Native Linux build: no Wine, no drive letters, Unix paths are used as-is.
bool isWine() { return false; }
static std::string wineVersion() { return ""; }
static std::string envVar(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
#endif

// True when the Unix filesystem is what we're looking at: under Wine/Proton,
// or in the native Linux build.
static bool onLinux() {
#ifdef _WIN32
    return isWine();
#else
    return true;
#endif
}

static std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// Convert a Unix path ("/home/me/...") to the DOS path Wine exposes it as
// ("Z:\home\me\..."). Uses Wine's own kernel32 export so a remapped drive
// letter still works (and, with no drive mapped to "/", Wine returns a
// "\\?\unix\home\me" path that still opens). Falls back to the default Z:.
std::string unixToDosPath(const std::string& unixPath) {
#ifndef _WIN32
    return unixPath;   // native Linux: nothing to convert
#else
    if (unixPath.empty() || unixPath[0] != '/') return unixPath;
    typedef WCHAR* (CDECL *WineGetDosFileName)(LPCSTR);
    static const WineGetDosFileName fn = [] {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return k32 ? (WineGetDosFileName)GetProcAddress(k32, "wine_get_dos_file_name") : nullptr;
    }();
    if (fn) {
        if (WCHAR* w = fn(unixPath.c_str())) {
            std::string out = narrow(w);
            HeapFree(GetProcessHeap(), 0, w);
            if (!out.empty()) return out;
        }
    }
    std::string out = "Z:" + unixPath;
    for (char& c : out) if (c == '/') c = '\\';
    return out;
#endif
}

// WINEHOMEDIR is an NT path: "\??\Z:\home\me", or "\??\unix\home\me" when no
// drive letter maps "/". Turn it into a DOS path we can open.
static std::string wineHomeDirToDos(const std::string& nt) {
    std::string p = nt;
    if (p.rfind("\\??\\", 0) == 0 || p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.size() >= 2 && p[1] == ':') return p;                    // Z:\home\me
    if (lower(p).rfind("unix\\", 0) == 0) {                         // unix\home\me
        std::string u = p.substr(4);
        for (char& c : u) if (c == '\\') c = '/';
        return unixToDosPath(u);                                    // -> Z:\.. or \\?\unix\..
    }
    return "";
}

// Two paths naming the same existing file or folder (symlinks, case, slashes).
// Compares volume serial + file index like fs::equivalent, plus size and
// timestamps: under Wine every Unix path reports volume serial 0 and the file
// index is the inode, so two different folders on different disks can share
// an index. A symlink resolves to the very same inode, so its times match too.
static bool sameFile(const fs::path& a, const fs::path& b) {
#ifndef _WIN32
    struct stat x, y;   // native: device + inode identify a file exactly
    if (stat(a.c_str(), &x) != 0 || stat(b.c_str(), &y) != 0) return false;
    return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
#else
    auto info = [](const fs::path& p, BY_HANDLE_FILE_INFORMATION& i) {
        HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        BOOL ok = GetFileInformationByHandle(h, &i);
        CloseHandle(h);
        return ok != FALSE;
    };
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    if (!info(a, x) || !info(b, y)) return false;
    auto eqTime = [](const FILETIME& m, const FILETIME& n) {
        return m.dwLowDateTime == n.dwLowDateTime && m.dwHighDateTime == n.dwHighDateTime;
    };
    return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
           x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow &&
           x.nFileSizeHigh == y.nFileSizeHigh && x.nFileSizeLow == y.nFileSizeLow &&
           eqTime(x.ftCreationTime, y.ftCreationTime) &&
           eqTime(x.ftLastWriteTime, y.ftLastWriteTime);
#endif
}

static bool sameDir(const std::string& a, const std::string& b) {
    if (lower(a) == lower(b)) return true;
    return sameFile(fs::path(a), fs::path(b));
}

// The Unix home directory(ies), as DOS paths, most trustworthy first:
// WINEHOMEDIR (set by Wine), $HOME (only if something passed it through),
// /home/$USER (or $USERNAME), then every folder under /home. Duplicates and
// missing folders are dropped.
static std::vector<std::string> linuxHomes() {
    static const std::vector<std::string> cached = [] {
        std::vector<std::string> homes;
        if (!onLinux()) return homes;
        auto add = [&](const std::string& dos) {
            if (dos.empty()) return;
            std::error_code ec;
            if (!fs::is_directory(fs::path(dos), ec)) return;
            for (const std::string& h : homes) if (sameDir(h, dos)) return;
            homes.push_back(dos);
        };
        add(wineHomeDirToDos(envVar("WINEHOMEDIR")));
        std::string h = envVar("HOME");
        if (!h.empty() && h[0] == '/') add(unixToDosPath(h));
        for (const char* var : {"USER", "USERNAME", "WINEUSERNAME"}) {
            std::string u = envVar(var);
            if (!u.empty() && u.find_first_of("/\\") == std::string::npos)
                add(unixToDosPath("/home/" + u));
        }
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(unixToDosPath("/home")),
                                       fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_directory(ec2)) add(it->path().string());
        }
        return homes;
    }();
    return cached;
}

// Every place a native Linux Steam might live, as DOS paths (whether or not it
// exists). Covers the regular package, its ~/.steam symlinks, Flatpak and Snap,
// plus the root Proton advertises.
static std::vector<std::string> linuxSteamRootCandidates() {
    std::vector<std::string> c;
    if (!onLinux()) return c;
    auto add = [&](std::string p) {
        while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        for (const std::string& q : c) if (lower(q) == lower(p)) return;
        c.push_back(p);
    };
    std::string p = envVar("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    if (!p.empty() && p[0] == '/') add(unixToDosPath(p));
    for (const std::string& home : linuxHomes())
        for (const char* rel : {".local/share/Steam", ".steam/steam", ".steam/root",
                                ".steam/debian-installation",
                                ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                                ".var/app/com.valvesoftware.Steam/.steam/steam",
                                "snap/steam/common/.local/share/Steam"})
            add((fs::path(home) / rel).make_preferred().string());
    return c;
}

// The candidates above that really hold a Steam install, with the ~/.steam
// symlinks to the same folder collapsed.
static std::vector<std::string> linuxSteamRoots() {
    std::vector<std::string> roots;
    for (const std::string& r : linuxSteamRootCandidates()) {
        std::error_code ec;
        if (!fs::is_directory(fs::path(r) / "steamapps", ec)) continue;
        bool dup = false;
        for (const std::string& prev : roots) if (sameDir(prev, r)) { dup = true; break; }
        if (!dup) roots.push_back(r);
    }
    return roots;
}

// Parse the "path" entries out of libraryfolders.vdf (Valve KeyValues text).
// A Linux Steam writes Unix paths there; those are converted to DOS paths.
static std::vector<std::string> libraryPaths(const std::string& steam) {
    std::vector<std::string> libs;
    if (steam.empty()) return libs;
    libs.push_back(steam);

    std::ifstream f(fs::path(steam) / "steamapps" / "libraryfolders.vdf", std::ios::binary);
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
        if (!path.empty() && path[0] == '/') path = unixToDosPath(path);
        bool dup = false;
        for (const std::string& l : libs) if (sameDir(l, path)) { dup = true; break; }
        if (!dup) libs.push_back(path);
        pos = b + 1;
    }
    return libs;
}

// Every Steam library we can find: the Windows Steam from the registry, plus
// (under Wine) the native Linux Steam's libraries.
static std::vector<std::string> allLibraryPaths() {
    std::vector<std::string> libs = libraryPaths(steamRoot());
    for (const std::string& root : linuxSteamRoots())
        for (std::string& l : libraryPaths(root)) {
            bool dup = false;
            for (const std::string& prev : libs) if (sameDir(prev, l)) { dup = true; break; }
            if (!dup) libs.push_back(std::move(l));
        }
    return libs;
}

// The drive letter Wine maps "/" to (e.g. 'Z'), or 0 when not under Wine.
// Scanning it from the root would crawl /proc, /usr etc., so the drive loops
// skip it and the Linux-specific roots below are scanned instead.
static char wineRootDrive() {
    if (!isWine()) return 0;
    std::string r = unixToDosPath("/");
    return (r.size() >= 2 && r[1] == ':') ? char(std::toupper((unsigned char)r[0])) : 0;
}

// Mount points scanned on their own (deeper than the generic root folders).
static const char* const kLinuxMounts[] = {"/mnt", "/media", "/run/media", "/data", "/games"};

// Top-level folders of the Linux root ("/") worth scanning: everything except
// system folders, hidden ones (/.snapshots holds whole filesystem copies), and
// the ones scanned separately (/home, the mount points, /run).
static std::vector<std::string> linuxRootFolders() {
    std::vector<std::string> out;
    if (!onLinux()) return out;
    static const char* const skip[] = {
        "proc", "sys", "dev", "usr", "bin", "sbin", "etc", "boot", "var", "tmp",
        "snap", "nix", "lost+found", "run", "home", "mnt", "media", "data", "games",
        "efi", "root"};
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(unixToDosPath("/")),
                                   fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_directory(ec2)) continue;
        std::string n;
        try { n = lower(it->path().filename().string()); } catch (const std::exception&) { continue; }
        if (n.empty() || n[0] == '.' || n.rfind("lib", 0) == 0) continue;
        bool skipped = false;
        for (const char* s : skip) if (n == s) { skipped = true; break; }
        if (!skipped) out.push_back(it->path().string());
    }
    return out;
}

// Extra scan roots under Wine, in order: the Unix home(s), the usual mount
// points for other disks, then the remaining top-level folders of "/".
static std::vector<std::pair<fs::path, int>> linuxScanRoots() {
    std::vector<std::pair<fs::path, int>> roots;
    if (!onLinux()) return roots;
    for (const std::string& h : linuxHomes()) roots.push_back({h, 5});
    for (const char* m : kLinuxMounts) roots.push_back({unixToDosPath(m), 5});
    return roots;
}

std::vector<std::string> protonGameLocalAppData() {
    std::vector<std::string> out;
    if (!onLinux()) return out;
    for (const std::string& lib : allLibraryPaths()) {
        fs::path lad = fs::path(lib) / "steamapps" / "compatdata" / "211420" / "pfx" /
                       "drive_c" / "users" / "steamuser" / "AppData" / "Local";
        std::error_code ec;
        if (!fs::is_directory(lad, ec)) continue;
        std::string p = lad.string();
        bool dup = false;
        for (const std::string& q : out) if (sameDir(q, p)) { dup = true; break; }
        if (!dup) out.push_back(p);
    }
    return out;
}

std::vector<std::string> detectDiagnostics() {
    std::vector<std::string> out;
    auto yn = [](const std::string& p, const char* sub = nullptr) {
        std::error_code ec;
        fs::path q(p);
        if (sub) q /= sub;
        return fs::exists(q, ec) ? "ok" : "missing";
    };
    auto show = [](const std::string& v) { return v.empty() ? std::string("(unset)") : v; };

    if (!onLinux()) {
        out.push_back("Platform: Windows (not Wine)");
    } else {
        out.push_back(isWine() ? "Platform: Wine " + wineVersion() : std::string("Platform: Linux (native)"));
        out.push_back("  HOME=" + show(envVar("HOME")) +
                      "  WINEHOMEDIR=" + show(envVar("WINEHOMEDIR")));
        out.push_back("  USER=" + show(envVar("USER")) +
                      "  USERNAME=" + show(envVar("USERNAME")));
        if (isWine()) {
            char d = wineRootDrive();
            out.push_back("  Unix / is " + unixToDosPath("/") +
                          (d ? std::string(" (drive ") + d + ":)" : std::string(" (no drive letter)")));
        }
        std::vector<std::string> homes = linuxHomes();
        if (homes.empty()) out.push_back("  Home: none found");
        for (const std::string& h : homes) out.push_back("  Home: " + h);
        std::string top;
        for (const std::string& d : linuxRootFolders())
            top += (top.empty() ? "" : ", ") + fs::path(d).filename().string();
        out.push_back("  Also scanning under /: " + (top.empty() ? std::string("(nothing)") : top));
    }

#ifdef _WIN32
    std::string reg = steamRoot();
    out.push_back("Steam (registry): " + (reg.empty() ? std::string("(none)")
                                                      : reg + " [" + yn(reg, "steamapps") + "]"));
#endif
    if (onLinux()) {
        std::vector<std::string> found = linuxSteamRoots();
        for (const std::string& r : linuxSteamRootCandidates()) {
            std::error_code ec;
            if (!fs::is_directory(fs::path(r) / "steamapps", ec)) continue;  // keep it short
            bool primary = false;
            for (const std::string& f : found) if (lower(f) == lower(r)) primary = true;
            out.push_back("Linux Steam: " + r + (primary ? " [ok]" : " [ok, same as above]"));
        }
        if (found.empty()) out.push_back("Linux Steam: none found (checked " +
                                         std::to_string(linuxSteamRootCandidates().size()) +
                                         " locations)");
    }
    for (const std::string& lad : protonGameLocalAppData())
        out.push_back("Proton prefix (PTDE): " + lad);
    for (const std::string& lib : allLibraryPaths()) {
        std::error_code ec;
        bool ptde = fs::exists(fs::path(ptdeDataIn(lib)) / "DARKSOULS.exe", ec);
        out.push_back("Library: " + lib + " [" + yn(lib) + "]" + (ptde ? " <- PTDE here" : ""));
    }
    return out;
}

std::string detectSteamDataDir() {
    for (const std::string& lib : allLibraryPaths()) {
        std::string data = ptdeDataIn(lib);
        std::error_code ec;
        if (fs::exists(fs::path(data) / "DARKSOULS.exe", ec))
            return data;
    }
    return "";
}

#ifdef _WIN32
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
#endif

// Folder holding the running installer (for the AppImage: the folder the
// .AppImage file sits in, not its temporary mount).
static fs::path selfDir() {
#ifdef _WIN32
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return fs::path(self).parent_path();
#else
    if (const char* ai = std::getenv("APPIMAGE")) return fs::path(ai).parent_path();
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : exe.parent_path();
#endif
}

// Roots for the bounded recursive scan, in priority order, with max depths:
// the installer's own folder, the user's Desktop/Downloads/Documents, (under
// Wine) the Unix home(s) and mount points, every drive root except the one
// mapping "/", and finally the non-system top-level folders of "/".
static std::vector<std::pair<fs::path, int>> scanRoots() {
    std::vector<std::pair<fs::path, int>> roots;
    roots.push_back({selfDir(), 6});
#ifdef _WIN32
    roots.push_back({knownFolder(FOLDERID_Desktop), 4});
    roots.push_back({knownFolder(FOLDERID_Downloads), 4});
    roots.push_back({knownFolder(FOLDERID_Documents), 4});
#endif
    for (const auto& r : linuxScanRoots()) roots.push_back(r);
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    const char unixRoot = wineRootDrive();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        if (char('A' + i) == unixRoot) continue;
        roots.push_back({std::string(1, char('A' + i)) + ":\\", 4});  // drive roots, shallow
    }
#endif
    for (const std::string& d : linuxRootFolders()) roots.push_back({d, 4});
    return roots;
}

// Directories that never hold the game and only waste the scan budget (or
// surface deleted copies). Compared case-insensitively on the folder name.
static bool isJunkDir(const std::string& nameRaw) {
    std::string n = lower(nameRaw);
    // DATA-Backup-Packed / DATA-Backup-Unpacked are our own backups: they hold a
    // copy of DARKSOULS.exe, so they must not be picked up as separate installs.
    if (n.rfind("data-backup", 0) == 0) return true;
    return n == "$recycle.bin" || n == "system volume information" ||
           n == "windows" || n == "$windows.~ws" || n == "$winreagent" ||
           n == "$sysreset" || n == "msocache" || n == "windows.old" ||
           n == "unpackds-backup" || n == "mctde-backup";  // archive backups, not installs
}

// Case-insensitive "is this file DARKSOULS.exe".
static bool isGameExe(const fs::path& p) {
    return lower(p.filename().string()) == "darksouls.exe";
}

// Depth- and time-bounded recursive search for DARKSOULS.exe under root.
static std::string searchRoot(const fs::path& root, int maxDepth, uint64_t deadline) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return "";
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return "";
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (nowMs() > deadline) break;
        try {
            if (it.depth() >= maxDepth) it.disable_recursion_pending();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (isJunkDir(it->path().filename().string())) it.disable_recursion_pending();
            } else if (it->is_regular_file(ec2) && isGameExe(it->path())) {
                return it->path().parent_path().string();
            }
        } catch (const std::exception&) { /* skip un-representable entry */ }
    }
    return "";
}

std::string findDataDir() {
    // 1. Steam libraries (fast and almost always right).
    std::string d = detectSteamDataDir();
    if (!d.empty()) return d;

#ifdef _WIN32
    const std::string sub = "\\steamapps\\common\\Dark Souls Prepare to Die Edition\\DATA";

    // 2. Likely fixed Steam-library locations on every drive.
    DWORD drives = GetLogicalDrives();
    const char unixRoot = wineRootDrive();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        if (char('A' + i) == unixRoot) continue;
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
#endif

    // 3. Bounded recursive scan, sharing one ~20s budget across roots.
    uint64_t deadline = nowMs() + 20000;
    for (const auto& r : scanRoots()) {
        if (nowMs() > deadline) break;
        std::string hit = searchRoot(r.first, r.second, deadline);
        if (!hit.empty()) return hit;
    }
    return "";
}

// Like searchRoot, but reports every DARKSOULS.exe folder it finds (not just
// the first) via onHit.
static void searchRootAll(const fs::path& root, int maxDepth, uint64_t deadline,
                          const std::function<void(const std::string&)>& onHit) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (nowMs() > deadline) break;
        try {
            if (it.depth() >= maxDepth) it.disable_recursion_pending();
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                if (isJunkDir(it->path().filename().string())) it.disable_recursion_pending();
            } else if (it->is_regular_file(ec2) && isGameExe(it->path())) {
                onHit(it->path().parent_path().string());
            }
        } catch (const std::exception&) { /* skip un-representable entry */ }
    }
}

void findAllDataDirs(const std::function<void(const GameInstall&)>& onFound) {
    std::set<std::string> seen;
    std::vector<std::string> seenDirs;
    auto report = [&](const std::string& dir) {
        std::string key = dir;
        for (char& c : key) c = char(std::tolower((unsigned char)c));
        if (!seen.insert(key).second) return;  // already reported
        // Same install reached by another path (e.g. Linux ~/.steam/steam is a
        // symlink to ~/.local/share/Steam): compare the exe's file identity.
        for (const std::string& prev : seenDirs) {
            if (sameFile(fs::path(dir) / "DARKSOULS.exe", fs::path(prev) / "DARKSOULS.exe"))
                return;
        }
        seenDirs.push_back(dir);
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

    // 1. Steam libraries (registry + libraryfolders.vdf, plus a native Linux
    //    Steam under Wine): fast, most likely.
    //    Then any other folder in steamapps\common holding DATA\DARKSOULS.exe
    //    (renamed copies such as "Dark Souls Prepare to Die Edition - 1.2.3"),
    //    which sit too deep for the bounded scan below.
    const std::vector<std::string> libs = allLibraryPaths();
    for (const std::string& lib : libs) {
        std::error_code ec;
        std::string data = ptdeDataIn(lib);
        if (fs::exists(fs::path(data) / "DARKSOULS.exe", ec)) reportInstall(data);
    }
    for (const std::string& lib : libs) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(lib) / "steamapps" / "common",
                                       fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (!it->is_directory(ec2)) continue;
            fs::path data = it->path() / "DATA";
            if (fs::exists(data / "DARKSOULS.exe", ec2)) reportInstall(data.string());
        }
    }

#ifdef _WIN32
    // 2. Fixed Steam-library spots on every drive.
    const std::string sub = "\\steamapps\\common\\Dark Souls Prepare to Die Edition\\DATA";
    DWORD drives = GetLogicalDrives();
    const char unixRoot = wineRootDrive();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        if (char('A' + i) == unixRoot) continue;
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
#endif

    // 3. Bounded recursive scan (installer folder, user folders, Linux home and
    //    mounts, drive roots, then the rest of the Linux root).
    uint64_t deadline = nowMs() + 20000;
    for (const auto& r : scanRoots()) {
        if (nowMs() > deadline) break;
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
        if (n.size() >= 8 && n.compare(n.size() - 8, 8, L".partial") == 0) continue;  // unfinished
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
