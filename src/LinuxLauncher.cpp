#include "LinuxLauncher.h"
#include "Download.h"
#include "LaunchLinux.h"
#include "Sha256.h"

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

static const char* kManifestUrl =
    "https://raw.githubusercontent.com/McRoodyPoo/mctde-Launcher/linux/latest.txt";
// Launcher archives may only come from mctde-Launcher's own release assets.
static const char* kAssetPrefix =
    "https://github.com/McRoodyPoo/mctde-Launcher/releases/download/";

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string kv(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        size_t eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return "";
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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

    // 1. The manifest on mctde-Launcher's linux branch.
    const char* over = std::getenv("MCTDE_LINUX_LAUNCHER_MANIFEST");
    std::string err, manifest;
    if (over && *over) {
        manifest = readFile(over);
    } else {
        fs::path m = work / "latest.txt";
        if (!downloadUrl(kManifestUrl, m.string(), err)) {
            message = "no Linux launcher published yet (" + err + ")";
            return false;
        }
        manifest = readFile(m);
    }
    std::string ver = kv(manifest, "mctde-launcher-linux");
    std::string url = kv(manifest, "mctde-launcher-linux-url");
    std::string sha = kv(manifest, "mctde-launcher-linux-sha256");
    for (char& c : sha) c = (char)std::tolower((unsigned char)c);
    bool shaOk = sha.size() == 64;
    for (char c : sha) if (!std::isxdigit((unsigned char)c)) shaOk = false;
    bool urlOk = url.rfind(kAssetPrefix, 0) == 0 || (over && *over && !url.empty() && url[0] == '/');
    if (ver.empty() || !urlOk || !shaOk) {
        message = "the Linux launcher manifest is incomplete or points outside mctde-Launcher's releases";
        return false;
    }

    // 2. The release archive, verified against the manifest's SHA-256.
    say("Downloading the Linux launcher " + ver + "...", -1);
    fs::path pkg = work / "launcher.tar.gz";
    if (url[0] == '/') fs::copy_file(url, pkg, fs::copy_options::overwrite_existing, ec);
    else if (!downloadUrl(url, pkg.string(), err)) { message = "download failed: " + err; return false; }
    std::string bytes = readFile(pkg);
    std::string got = sha256Hex((const uint8_t*)bytes.data(), bytes.size());
    if (got != sha) { message = "checksum mismatch (expected " + sha + ", got " + got + ")"; return false; }

    // 3. Unpack (system tar; the archive is a plain .tar.gz) and copy the binaries in.
    int rc = -1;
    if (!runProcess({"tar", "-xzf", pkg.string(), "-C", work.string()}, "", {}, "", false, &rc) || rc != 0) {
        message = "could not unpack the Linux launcher (tar exit " + std::to_string(rc) + ")";
        return false;
    }
    int copied = 0;
    for (fs::recursive_directory_iterator it(work, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (!it->is_regular_file(ec)) continue;
        bool bin = name == "mctde-launcher" || name == "mctde-launcher-cli";
        if (!bin && name != "README-LINUX.md") continue;
        fs::path dst = data / name;
        fs::path tmp = dst; tmp += ".mctde-new";
        std::error_code ce;
        fs::copy_file(it->path(), tmp, fs::copy_options::overwrite_existing, ce);
        if (!ce && bin) ::chmod(tmp.c_str(), 0755);
        if (!ce) fs::rename(tmp, dst, ce);   // atomic swap, safe even if it is running
        if (ce) { message = "could not install " + name + ": " + ce.message(); return false; }
        if (bin) ++copied;
    }
    if (copied < 2) { message = "the Linux launcher archive didn't contain both binaries"; return false; }
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
