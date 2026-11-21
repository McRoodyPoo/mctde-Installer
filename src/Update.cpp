#include "Update.h"
#include "Download.h"
#include "Release.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace mctde {

// One version for both platforms. Bump on every installer release, in lockstep
// with latest.txt's "mctde-installer=" line.
const char* const kInstallerVersion = "0.5.1";

// Each version is one GitHub release tagged "v<version>" carrying a zip per
// platform (see Release.h). The self-update takes the zip for the OS it was
// built for and pulls the program out of it.
static const char* kReleaseUrlPrefix =
    "https://github.com/McRoodyPoo/mctde-Installer/releases/download/v";
#ifdef _WIN32
static const char* kProgramName = "mctde-Installer.exe";
#else
static const char* kProgramName = "mctde-Installer-x86_64.AppImage";
#endif
static std::string g_latest;   // set by fetchLatestInstallerVersion()

// Release zip for this OS, e.g. .../v0.4.0/mctde-Installer-linux.zip
static std::string releaseZipUrl(const std::string& version) {
    return std::string(kReleaseUrlPrefix) + version + "/mctde-Installer-" + platformName() + ".zip";
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \r\n\t");
    size_t b = s.find_last_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

#ifdef _WIN32
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
#endif

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

// Older builds read "mctde-installer-linux=" on Linux; that line stays in
// latest.txt (kept equal to "mctde-installer=") so they still update.
std::string fetchLatestInstallerVersion() {
    std::string text = fetchManifest();
    if (text.empty()) return "";
    g_latest = manifestValue(text, "mctde-installer");
    return g_latest;
}

#ifdef _WIN32
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

    // 1. Download this OS's release zip and pull the new exe out of it,
    //    alongside ourselves.
    if (g_latest.empty()) { err = "no installer version advertised"; return false; }
    if (!fetchFromReleaseZip(releaseZipUrl(g_latest), kProgramName, narrow(newP), err, progress)) {
        DeleteFileW(newP.c_str());
        return false;
    }

    // 2. Sanity-check it's a real PE (guards against an HTML error page slipping
    //    through a redirect, and against a truncated download).
    {
        std::ifstream f(std::filesystem::path(newP), std::ios::binary | std::ios::ate);
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

#else
// Linux: the new AppImage is renamed straight over the running one (allowed on
// Unix), so nothing is left behind.
void cleanupOldSelf() {}

bool selfUpdate(std::string& err, const ProgressFn& progress) {
    // Only the AppImage can replace itself; a hand-built binary just carries on.
    const char* appimage = std::getenv("APPIMAGE");
    if (!appimage || !*appimage) {
        err = "not running from the AppImage; download the new release manually";
        return false;
    }
    const std::string self = appimage;
    const std::string newP = self + ".new";

    // 1. Download this OS's release zip and pull the new AppImage out of it,
    //    alongside ourselves.
    if (g_latest.empty()) { err = "no installer version advertised"; return false; }
    if (!fetchFromReleaseZip(releaseZipUrl(g_latest), kProgramName, newP, err, progress)) {
        ::unlink(newP.c_str());
        return false;
    }

    // 2. Sanity-check it's an ELF executable of a plausible size.
    {
        std::ifstream f(newP, std::ios::binary | std::ios::ate);
        std::streamoff sz = f ? (std::streamoff)f.tellg() : 0;
        char magic[4] = {0};
        if (f) { f.seekg(0); f.read(magic, 4); }
        f.close();
        if (sz < 500000 || magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
            err = "downloaded installer was not a valid AppImage";
            ::unlink(newP.c_str());
            return false;
        }
    }

    // 3. Make it executable and rename it over ourselves (atomic; the running
    //    process keeps its already-mounted copy).
    ::chmod(newP.c_str(), 0755);
    if (::rename(newP.c_str(), self.c_str()) != 0) {
        err = "could not put the new installer in place";
        ::unlink(newP.c_str());
        return false;
    }

    // 4. Relaunch it detached; the caller exits so the new instance takes over.
    pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::execl(self.c_str(), self.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    if (pid < 0) {
        err = "updated, but could not relaunch automatically";
        return false;
    }
    return true;
}
#endif

} // namespace mctde
