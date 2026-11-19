#include "LinuxLauncher.h"
#include "Download.h"
#include "Extract.h"
#include "LaunchLinux.h"
#include "Release.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace mctde {

// The Linux launcher is released alongside the Windows one: the "v<version>"
// release of mctde-Launcher carries mctde-launcher-linux.zip, and the version is
// latest.txt's shared "mctde-launcher=" line.
static const char* kReleaseUrlPrefix =
    "https://github.com/McRoodyPoo/mctde-Launcher/releases/download/v";

static bool isElf(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char m[4] = {0};
    f.read(m, 4);
    return f.gcount() == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

bool installLinuxLauncher(const std::string& dataDir, std::string& message,
                          const std::function<void(const std::string&, int)>& step) {
    auto say = [&](const std::string& s, int pct) { if (step) step(s, pct); };
    fs::path data(dataDir);
    std::error_code ec;
    fs::path work = data / "_mctde_launcher_linux";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};

    // 1. Which version, then that release's Linux zip (SHA-256 checked against
    //    its sidecar). MCTDE_LINUX_LAUNCHER_ZIP=<local zip> skips the network
    //    (testing only).
    std::string err, ver;
    fs::path pkg = work / "launcher.zip";
    const char* localZip = std::getenv("MCTDE_LINUX_LAUNCHER_ZIP");
    if (localZip && *localZip) {
        ver = "(local)";
        fs::copy_file(localZip, pkg, fs::copy_options::overwrite_existing, ec);
        if (ec) { message = "could not read " + std::string(localZip); return false; }
    } else {
        ver = manifestValue(fetchManifest(), "mctde-launcher");
        if (ver.empty()) { message = "could not read the launcher version from latest.txt"; return false; }
        say("Downloading the Linux launcher " + ver + "...", -1);
        std::string url = std::string(kReleaseUrlPrefix) + ver + "/mctde-launcher-linux.zip";
        if (!downloadVerifiedZip(url, pkg.string(), err)) { message = "download failed: " + err; return false; }
    }

    // 2. Pull out the two programs (and the readme) and swap them in atomically.
    const char* names[] = {"mctde-launcher", "mctde-launcher-cli", "README-LINUX.md"};
    for (const char* name : names) {
        bool bin = std::string(name) != "README-LINUX.md";
        fs::path dst = data / name;
        fs::path tmp = dst; tmp += ".mctde-new";
        if (!extractZipEntry(pkg.string(), name, tmp.string(), err)) {
            if (!bin) continue;   // the readme is optional
            message = "the Linux launcher zip is missing " + std::string(name);
            return false;
        }
        if (bin && !isElf(tmp)) {
            fs::remove(tmp, ec);
            message = std::string(name) + " in the Linux launcher zip isn't a Linux program";
            return false;
        }
        if (bin) ::chmod(tmp.c_str(), 0755);
        std::error_code ce;
        fs::rename(tmp, dst, ce);   // atomic swap, safe even if it is running
        if (ce) { message = "could not install " + std::string(name) + ": " + ce.message(); return false; }
    }
    message = "Linux launcher " + ver + " installed (mctde-launcher, mctde-launcher-cli)";
    return true;
}

std::string linuxLauncherIn(const std::string& dataDir) {
    fs::path p = fs::path(dataDir) / "mctde-launcher";
    return ::access(p.c_str(), X_OK) == 0 ? p.string() : "";
}

bool qt6Available() {
    // dlopen succeeds only if Qt 6 Widgets and everything it needs resolve.
    if (void* h = ::dlopen("libQt6Widgets.so.6", RTLD_LAZY | RTLD_LOCAL)) { ::dlclose(h); return true; }
    return false;
}

} // namespace mctde
