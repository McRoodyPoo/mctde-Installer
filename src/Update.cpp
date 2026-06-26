#include "Update.h"
#include "Download.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace mctde {

// Bump this on every installer release, in lockstep with latest.txt.
const char* const kInstallerVersion = "0.1.10";

// Version comes from the central latest.txt (same manifest the in-game updater
// reads). The installer exe itself is attached to each mctde-Installer release.
static const char* kManifestUrl =
    "https://raw.githubusercontent.com/McRoodyPoo/mctde-Link/main/latest.txt";
static const char* kInstallerExeUrl =
    "https://github.com/McRoodyPoo/mctde-Installer/releases/latest/download/mctde-Installer.exe";

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \r\n\t");
    size_t b = s.find_last_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring selfPathW() {
    wchar_t p[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Pull "mctde-installer=" (case-insensitive, '_' treated as '-') out of the
// key=value manifest. Matches the lenient format the in-game checker parses.
static std::string parseInstallerVersion(const std::string& text) {
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) eq = line.find(':');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        for (char& c : key) { c = (char)tolower((unsigned char)c); if (c == '_') c = '-'; }
        if (key == "mctde-installer" || key == "mctdeinstaller")
            return trim(line.substr(eq + 1));
    }
    return "";
}

bool isNewer(const std::string& latest, const std::string& current) {
    auto parts = [](const std::string& v) -> std::vector<int> {
        std::vector<int> out;
        std::stringstream ss(trim(v));
        std::string p;
        while (std::getline(ss, p, '.')) {
            p = trim(p);
            if (p.empty()) return {};
            for (char c : p) if (c < '0' || c > '9') return {};
            out.push_back(std::atoi(p.c_str()));
        }
        return out;
    };
    std::vector<int> L = parts(latest), C = parts(current);
    if (L.empty()) return false;   // unparseable remote -> never auto-update
    size_t n = L.size() > C.size() ? L.size() : C.size();
    for (size_t i = 0; i < n; ++i) {
        int l = i < L.size() ? L[i] : 0;
        int c = i < C.size() ? C[i] : 0;
        if (l != c) return l > c;
    }
    return false;
}

std::string fetchLatestInstallerVersion() {
    wchar_t tmpDir[MAX_PATH] = {0};
    if (!GetTempPathW(MAX_PATH, tmpDir)) return "";
    std::wstring tmp = std::wstring(tmpDir) + L"mctde_latest.txt";
    std::string err;
    if (!downloadUrl(kManifestUrl, narrow(tmp), err)) return "";
    std::ifstream f(tmp);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    DeleteFileW(tmp.c_str());
    return parseInstallerVersion(text);
}

void cleanupOldSelf() {
    std::wstring oldP = selfPathW() + L".old";
    for (int i = 0; i < 10; ++i) {
        if (GetFileAttributesW(oldP.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        if (DeleteFileW(oldP.c_str())) return;
        Sleep(200);   // the previous instance may still be exiting
    }
}

bool selfUpdate(std::string& err, const ProgressFn& progress) {
    const std::wstring self = selfPathW();
    const std::wstring newP = self + L".new";
    const std::wstring oldP = self + L".old";

    // 1. Download the new exe alongside ourselves.
    if (!downloadUrl(kInstallerExeUrl, narrow(newP), err, progress)) {
        DeleteFileW(newP.c_str());
        return false;
    }

    // 2. Sanity-check it's a real PE (guards against an HTML error page slipping
    //    through a redirect, and against a truncated download).
    {
        std::ifstream f(newP, std::ios::binary | std::ios::ate);
        std::streamoff sz = f ? (std::streamoff)f.tellg() : 0;
        char mz[2] = {0};
        if (f) { f.seekg(0); f.read(mz, 2); }
        f.close();
        if (sz < 50000 || mz[0] != 'M' || mz[1] != 'Z') {
            err = "downloaded installer was not a valid .exe";
            DeleteFileW(newP.c_str());
            return false;
        }
    }

    // 3. Swap. Windows lets us rename the running exe (just not delete it), so
    //    move ourselves aside, then move the new build into our path.
    DeleteFileW(oldP.c_str());
    if (!MoveFileW(self.c_str(), oldP.c_str())) {
        err = "could not move the running installer aside";
        DeleteFileW(newP.c_str());
        return false;
    }
    if (!MoveFileW(newP.c_str(), self.c_str())) {
        err = "could not put the new installer in place";
        MoveFileW(oldP.c_str(), self.c_str());   // roll back
        return false;
    }

    // 4. Relaunch the new exe. CreateProcess inherits our (already elevated)
    //    token, so the requireAdministrator manifest won't trigger a second UAC
    //    prompt. The new instance deletes the .old on startup (cleanupOldSelf).
    STARTUPINFOW si = {0}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    std::wstring cmd = L"\"" + self + L"\"";
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(0);
    if (!CreateProcessW(self.c_str(), cmdbuf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        // The new build is already in place; the user can just reopen it.
        err = "updated, but could not relaunch automatically";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace mctde
