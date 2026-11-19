#include "SteamShortcut.h"
#include "Detect.h"
#include "LaunchLinux.h"
#include "miniz.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// The embedded window icon (src/linux_assets.S), reused as the shortcut icon.
// Only linked into the GUI; the CLI build provides weak empty fallbacks.
extern "C" {
extern const unsigned char asset_icon_png[] __attribute__((weak));
extern const unsigned char asset_icon_png_end[] __attribute__((weak));
}

namespace mctde {

// ---- binary KeyValues (shortcuts.vdf) ----
// Node types: 0x00 object, 0x01 string, 0x02 int32, 0x07 uint64; 0x08 ends an
// object. The file is one root object list holding "shortcuts" { "0" {...} ... }.
struct VdfNode {
    uint8_t type = 0;
    std::string key;
    std::string str;
    uint32_t i32 = 0;
    uint64_t u64 = 0;
    std::vector<VdfNode> kids;
};

static bool parseList(const std::string& d, size_t& p, std::vector<VdfNode>& out) {
    while (p < d.size()) {
        uint8_t t = (uint8_t)d[p++];
        if (t == 0x08) return true;
        size_t z = d.find('\0', p);
        if (z == std::string::npos) return false;
        VdfNode n;
        n.type = t;
        n.key = d.substr(p, z - p);
        p = z + 1;
        if (t == 0x00) {
            if (!parseList(d, p, n.kids)) return false;
        } else if (t == 0x01) {
            z = d.find('\0', p);
            if (z == std::string::npos) return false;
            n.str = d.substr(p, z - p);
            p = z + 1;
        } else if (t == 0x02) {
            if (p + 4 > d.size()) return false;
            std::memcpy(&n.i32, d.data() + p, 4); p += 4;
        } else if (t == 0x07) {
            if (p + 8 > d.size()) return false;
            std::memcpy(&n.u64, d.data() + p, 8); p += 8;
        } else {
            return false;   // unknown type: refuse rather than corrupt the file
        }
        out.push_back(std::move(n));
    }
    return false;
}

static void writeList(std::string& o, const std::vector<VdfNode>& list) {
    for (const VdfNode& n : list) {
        o += (char)n.type;
        o += n.key; o += '\0';
        if (n.type == 0x00) writeList(o, n.kids);
        else if (n.type == 0x01) { o += n.str; o += '\0'; }
        else if (n.type == 0x02) o.append((const char*)&n.i32, 4);
        else if (n.type == 0x07) o.append((const char*)&n.u64, 8);
    }
    o += (char)0x08;
}

static VdfNode str(const std::string& k, const std::string& v) { VdfNode n; n.type = 1; n.key = k; n.str = v; return n; }
static VdfNode num(const std::string& k, uint32_t v) { VdfNode n; n.type = 2; n.key = k; n.i32 = v; return n; }

static VdfNode* child(VdfNode& obj, const std::string& key) {
    for (VdfNode& k : obj.kids) {
        if (k.key.size() != key.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < key.size(); ++i)
            if (std::tolower((unsigned char)k.key[i]) != std::tolower((unsigned char)key[i])) { eq = false; break; }
        if (eq) return &k;
    }
    return nullptr;
}

// ---- locations ----
static std::string homeDir() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// The Steam account's config folder: the most recent login per loginusers.vdf,
// else the userdata/<id>/config with the newest localconfig.vdf.
static fs::path steamUserConfigDir(std::string& err) {
    std::vector<std::string> roots = linuxSteamRootPaths();
    if (roots.empty()) { err = "no Steam installation found"; return {}; }
    for (const std::string& root : roots) {
        std::ifstream f(fs::path(root) / "config" / "loginusers.vdf");
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // Find "7656...." { ... "MostRecent" "1" ... }
        size_t p = 0;
        std::string lastId;
        while ((p = text.find('"', p)) != std::string::npos) {
            size_t e = text.find('"', p + 1);
            if (e == std::string::npos) break;
            std::string tok = text.substr(p + 1, e - p - 1);
            p = e + 1;
            if (tok.size() == 17 && tok.rfind("7656", 0) == 0) lastId = tok;
            if ((tok == "MostRecent" || tok == "mostrecent") && !lastId.empty()) {
                size_t q = text.find('"', p), r = q == std::string::npos ? q : text.find('"', q + 1);
                if (r != std::string::npos && text.substr(q + 1, r - q - 1) == "1") {
                    uint64_t id64 = std::strtoull(lastId.c_str(), nullptr, 10);
                    fs::path cfg = fs::path(root) / "userdata" / std::to_string(id64 - 76561197960265728ULL) / "config";
                    std::error_code ec;
                    if (fs::is_directory(cfg, ec)) return cfg;
                }
            }
        }
    }
    fs::path best;
    fs::file_time_type bestTime{};
    for (const std::string& root : roots) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(root) / "userdata", ec), end; !ec && it != end; it.increment(ec)) {
            fs::path cfg = it->path() / "config";
            std::error_code ec2;
            if (!fs::is_directory(cfg, ec2) || it->path().filename() == "0" || it->path().filename() == "anonymous") continue;
            auto t = fs::last_write_time(cfg / "localconfig.vdf", ec2);
            if (ec2) t = fs::last_write_time(cfg, ec2);
            if (best.empty() || t > bestTime) { best = cfg; bestTime = t; }
        }
    }
    if (best.empty()) err = "could not find your Steam account's config folder. Open Steam once on this computer first";
    return best;
}

// A path that keeps working after we exit: the .AppImage file itself (never its
// /tmp/.mount_* directory), else the CLI binary next to this executable.
static std::string stableLauncherPath() {
    if (const char* ai = std::getenv("APPIMAGE"); ai && *ai) return ai;
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return "";
    fs::path cli = self.parent_path() / "mctde-installer";
    if (fs::exists(cli, ec)) return cli.string();
    return self.string();
}

// Our icon at ~/.local/share/mctde-installer/mctde.png. The GUI (which has
// the image embedded) writes it; the CLI worker reuses the file if present.
static std::string shortcutIcon() {
    fs::path dir = fs::path(homeDir()) / ".local" / "share" / "mctde-installer";
    fs::path icon = dir / "mctde.png";
    std::error_code ec;
    if (asset_icon_png && asset_icon_png_end) {
        fs::create_directories(dir, ec);
        size_t len = (size_t)(asset_icon_png_end - asset_icon_png);
        if (!fs::exists(icon, ec) || fs::file_size(icon, ec) != len) {
            std::ofstream o(icon, std::ios::binary | std::ios::trunc);
            o.write((const char*)asset_icon_png, (std::streamsize)len);
        }
    }
    return fs::exists(icon, ec) ? icon.string() : std::string();
}

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string steamShortcutName(const std::string& dataDir) {
    // The game's Steam store name plus "(mctde)". The canonical Steam copy gets
    // just that; any other copy also gets its folder name so several installs
    // stay apart in the library.
    static const std::string base = "DARK SOULS\u2122: Prepare To Die Edition (mctde)";
    fs::path data(dataDir);
    std::string folder = data.parent_path().filename().string();
    if (folder == "Dark Souls Prepare to Die Edition") return base;
    return base + " - " + folder;
}

bool addSteamShortcutNow(const std::string& dataDir, std::string& message) {
    if (steamRunning() && !std::getenv("MCTDE_SHORTCUTS_VDF")) {
        message = "Steam must be closed before modifying shortcuts.vdf";
        return false;
    }
    std::string launcher = stableLauncherPath();
    if (launcher.empty() || launcher.rfind("/tmp/.mount_", 0) == 0) {
        message = "could not determine a stable path to the installer";
        return false;
    }

    fs::path vdf;
    if (const char* over = std::getenv("MCTDE_SHORTCUTS_VDF")) vdf = over;
    else {
        std::string err;
        fs::path cfg = steamUserConfigDir(err);
        if (cfg.empty()) { message = err; return false; }
        vdf = cfg / "shortcuts.vdf";
    }

    // Load the existing file (or start an empty "shortcuts" object).
    std::vector<VdfNode> root;
    std::error_code ec;
    if (fs::exists(vdf, ec)) {
        std::ifstream f(vdf, std::ios::binary);
        std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t p = 0;
        if (!d.empty() && (!parseList(d, p, root))) {
            message = "could not read " + vdf.string() + " (left unchanged)";
            return false;
        }
    }
    VdfNode* shortcuts = nullptr;
    for (VdfNode& n : root) if (n.type == 0 && n.key == "shortcuts") shortcuts = &n;
    if (!shortcuts) { VdfNode s; s.type = 0; s.key = "shortcuts"; root.push_back(s); shortcuts = &root.back(); }

    const std::string name = steamShortcutName(dataDir);
    const std::string exe = quote(launcher);
    const std::string startDir = quote(fs::path(launcher).parent_path().string());
    const std::string launchOptions = "play " + quote(dataDir);
    // Steam's id for a non-Steam shortcut: crc32(exe + name) with the top bit set.
    const std::string idSrc = exe + name;
    uint32_t appid = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)idSrc.data(), idSrc.size()) | 0x80000000u;

    // Same copy already added (our launch options naming this DATA folder)?
    VdfNode* entry = nullptr;
    for (VdfNode& e : shortcuts->kids) {
        VdfNode* lo = child(e, "LaunchOptions");
        if (lo && lo->type == 1 && lo->str == launchOptions) { entry = &e; break; }
    }
    bool updated = entry != nullptr;
    if (!entry) {
        int next = 0;
        for (const VdfNode& e : shortcuts->kids) next = std::max(next, std::atoi(e.key.c_str()) + 1);
        VdfNode e; e.type = 0; e.key = std::to_string(next);
        shortcuts->kids.push_back(e);
        entry = &shortcuts->kids.back();
    }

    auto set = [&](const VdfNode& v) {
        if (VdfNode* k = child(*entry, v.key)) { std::string key = k->key; *k = v; k->key = key; }
        else entry->kids.push_back(v);
    };
    set(num("appid", appid));
    set(str("AppName", name));
    set(str("Exe", exe));
    set(str("StartDir", startDir));
    set(str("icon", shortcutIcon()));
    set(str("ShortcutPath", ""));
    set(str("LaunchOptions", launchOptions));
    if (!updated) {
        set(num("IsHidden", 0));
        set(num("AllowDesktopConfig", 1));
        set(num("AllowOverlay", 1));
        set(num("OpenVR", 0));
        set(num("Devkit", 0));
        set(str("DevkitGameID", ""));
        set(num("DevkitOverrideAppID", 0));
        set(num("LastPlayTime", 0));
        set(str("FlatpakAppID", ""));
        set(str("sortas", ""));
        VdfNode tags; tags.type = 0; tags.key = "tags";
        set(tags);
    }

    // Write atomically, keeping a one-time backup of the original.
    std::string out;
    writeList(out, root);
    fs::create_directories(vdf.parent_path(), ec);
    fs::path backup = vdf; backup += ".mctde-backup";
    if (fs::exists(vdf, ec) && !fs::exists(backup, ec)) fs::copy_file(vdf, backup, ec);
    fs::path tmp = vdf; tmp += ".mctde-tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(out.data(), (std::streamsize)out.size());
        if (!o) { message = "could not write " + tmp.string(); return false; }
    }
    fs::rename(tmp, vdf, ec);
    if (ec) { message = "could not replace " + vdf.string() + ": " + ec.message(); return false; }

    message = (updated ? "Updated \"" : "Added \"") + name + "\" in Steam. Restart Steam (or return to Game Mode) to see it in your library.";
    return true;
}

bool addSteamShortcutAfterSteamExits(const std::string& dataDir, std::string& message) {
    while (steamRunning()) std::this_thread::sleep_for(std::chrono::seconds(2));
    std::this_thread::sleep_for(std::chrono::seconds(3));   // let Steam finish writing its files
    return addSteamShortcutNow(dataDir, message);
}

bool addToSteam(const std::string& dataDir, std::string& message) {
    shortcutIcon();   // put the icon file in place now, for the worker
    if (!steamRunning() || std::getenv("MCTDE_SHORTCUTS_VDF")) return addSteamShortcutNow(dataDir, message);

    std::string launcher = stableLauncherPath();
    if (launcher.empty() || launcher.rfind("/tmp/.mount_", 0) == 0) {
        message = "could not determine a stable path to the installer";
        return false;
    }
    std::error_code ec;
    fs::path logDir = fs::path(homeDir()) / ".cache" / "mctde-installer";
    fs::create_directories(logDir, ec);
    std::string logFile = fs::is_directory(logDir, ec) ? (logDir / "steam-shortcut.log").string() : "";
    if (!runProcess({launcher, "add-to-steam", dataDir, "--wait-for-steam-exit"}, "", {}, logFile, /*detach=*/true)) {
        message = "could not start the Steam shortcut worker";
        return false;
    }
    message = "Steam is running, so \"" + steamShortcutName(dataDir) +
              "\" is queued: restart Steam once and the shortcut is added after Steam fully closes.";
    return true;
}

} // namespace mctde
