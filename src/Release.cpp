#include "Release.h"
#include "Extract.h"
#include "Sha256.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace mctde {

static const char* kManifestUrl =
    "https://raw.githubusercontent.com/McRoodyPoo/mctde-Link/main/latest.txt";

const char* platformName() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \r\n\t");
    size_t b = s.find_last_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string normKey(std::string k) {
    k = trim(k);
    for (char& c : k) { c = (char)std::tolower((unsigned char)c); if (c == '_') c = '-'; }
    return k;
}

static bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string fetchManifest() {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) return "";
    tmp /= "mctde_latest.txt";
    std::string err, text;
    if (!downloadUrl(kManifestUrl, tmp.u8string(), err)) return "";
    readFile(tmp, text);
    fs::remove(tmp, ec);
    return text;
}

std::string manifestValue(const std::string& text, const std::string& key) {
    const std::string want = normKey(key);
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) eq = line.find(':');
        if (eq == std::string::npos) continue;
        std::string k = normKey(line.substr(0, eq));
        if (k == want || (want.rfind("mctde-", 0) == 0 && k == "mctde" + want.substr(6)))
            return trim(line.substr(eq + 1));
    }
    return "";
}

bool downloadVerifiedZip(const std::string& zipUrl, const std::string& zipPath,
                         std::string& err, const ProgressFn& progress) {
    std::error_code ec;
    auto cleanup = [&] { fs::remove(fs::u8path(zipPath), ec); };

    if (!downloadUrl(zipUrl, zipPath, err, progress)) { cleanup(); return false; }

    // Integrity: when a .sha256 sidecar is published, the zip must match it.
    // (Without one we still have HTTPS plus the zip's own per-file CRCs.)
    std::string sidecarErr, sidecar;
    const std::string sidecarPath = zipPath + ".sha256";
    if (downloadUrl(zipUrl + ".sha256", sidecarPath, sidecarErr) &&
        readFile(fs::u8path(sidecarPath), sidecar)) {
        std::string want = trim(sidecar);
        size_t sp = want.find_first_of(" \t\r\n");
        if (sp != std::string::npos) want = want.substr(0, sp);
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        std::string bytes;
        readFile(fs::u8path(zipPath), bytes);
        std::string got = sha256Hex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (want.size() == 64 && got != want) {
            err = "download failed its SHA-256 check";
            fs::remove(fs::u8path(sidecarPath), ec);
            cleanup();
            return false;
        }
    }
    fs::remove(fs::u8path(sidecarPath), ec);
    return true;
}

bool fetchFromReleaseZip(const std::string& zipUrl, const std::string& member,
                         const std::string& outPath, std::string& err,
                         const ProgressFn& progress) {
    const std::string zipPath = outPath + ".zip.part";
    if (!downloadVerifiedZip(zipUrl, zipPath, err, progress)) return false;
    bool ok = extractZipEntry(zipPath, member, outPath, err);
    std::error_code ec;
    fs::remove(fs::u8path(zipPath), ec);
    return ok;
}

} // namespace mctde
