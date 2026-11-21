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


// ---- PTDE launch options (text KeyValues, localconfig.vdf) ----
static const char* const kPtdeAppId = "211420";

bool isSteamLibraryCopy(const std::string& dataDir) {
    fs::path data = fs::path(dataDir).lexically_normal();
    if (data.filename().empty()) data = data.parent_path();
    fs::path game = data.parent_path(), common = game.parent_path();
    return data.filename() == "DATA" && game.filename() == "Dark Souls Prepare to Die Edition" &&
           common.filename() == "common" && common.parent_path().filename() == "steamapps";
}

std::string steamPlayLaunchOptions(const std::string& dataDir) {
    std::string l = (fs::path(dataDir) / "mctde-launcher").string();
    return "[ -x " + quote(l) + " ] && exec " + quote(l) + "; %command%";
}

// One token of a text VDF: a quoted or bare string, or a brace. [begin, end) is
// its span in the file (quotes included), so values can be replaced in place.
struct KvTok { char kind; std::string s; size_t begin, end; };   // kind: 's', '{', '}'

static bool kvTokenize(const std::string& t, std::vector<KvTok>& out) {
    size_t i = 0, n = t.size();
    while (i < n) {
        char ch = t[i];
        if (std::isspace((unsigned char)ch)) { ++i; continue; }
        if (ch == '/' && i + 1 < n && t[i + 1] == '/') { while (i < n && t[i] != '\n') ++i; continue; }
        if (ch == '{' || ch == '}') { out.push_back({ch, "", i, i + 1}); ++i; continue; }
        if (ch == '"') {
            size_t b = i++;
            std::string s;
            while (i < n && t[i] != '"') {
                if (t[i] == '\\' && i + 1 < n) {
                    char e = t[++i];
                    s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else s += t[i];
                ++i;
            }
            if (i >= n) return false;
            out.push_back({'s', s, b, ++i});
            continue;
        }
        size_t b = i;
        while (i < n && !std::isspace((unsigned char)t[i]) && t[i] != '"' && t[i] != '{' && t[i] != '}') ++i;
        out.push_back({'s', t.substr(b, i - b), b, i});
    }
    return true;
}

// Index of the '}' closing the object whose '{' is at tok[open].
static size_t kvClose(const std::vector<KvTok>& tok, size_t open) {
    int depth = 0;
    for (size_t i = open; i < tok.size(); ++i) {
        if (tok[i].kind == '{') ++depth;
        else if (tok[i].kind == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

// In the object spanning tok(open, close), find direct child `key` (case-insensitive).
// Returns the index of its value token (a string, or the '{' of an object), or npos.
static size_t kvChild(const std::vector<KvTok>& tok, size_t open, size_t close, const std::string& key) {
    auto ieq = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
            [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
    };
    size_t i = open + 1;
    while (i < close) {
        if (tok[i].kind != 's' || i + 1 >= close) return std::string::npos;
        size_t v = i + 1;
        if (ieq(tok[i].s, key)) return v;
        if (tok[v].kind == '{') { size_t c = kvClose(tok, v); if (c == std::string::npos) return c; i = c + 1; }
        else i = v + 1;
    }
    return std::string::npos;
}

static std::string kvEscape(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
    return o;
}

// The line indentation (tabs) in front of position p.
static std::string kvIndentAt(const std::string& t, size_t p) {
    size_t ls = t.rfind('\n', p == 0 ? 0 : p - 1);
    ls = ls == std::string::npos ? 0 : ls + 1;
    size_t e = ls;
    while (e < t.size() && (t[e] == '\t' || t[e] == ' ')) ++e;
    return t.substr(ls, e - ls);
}

bool setSteamPlayNow(const std::string& dataDir, std::string& message) {
    const char* over = std::getenv("MCTDE_LOCALCONFIG_VDF");
    if (steamRunning() && !over) { message = "Steam must be closed before changing PTDE's launch options"; return false; }
    fs::path vdf;
    if (over) vdf = over;
    else {
        std::string err;
        fs::path cfg = steamUserConfigDir(err);
        if (cfg.empty()) { message = err; return false; }
        vdf = cfg / "localconfig.vdf";
    }
    std::string t;
    {
        std::ifstream f(vdf, std::ios::binary);
        if (!f) { message = "could not read " + vdf.string(); return false; }
        t.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    std::vector<KvTok> tok;
    if (!kvTokenize(t, tok) || tok.size() < 2 || tok[1].kind != '{') {
        message = "could not read " + vdf.string() + " (left unchanged)";
        return false;
    }
    // UserLocalConfigStore > Software > Valve > Steam > apps
    size_t open = 1, close = kvClose(tok, 1);
    for (const char* k : {"Software", "Valve", "Steam", "apps"}) {
        size_t v = close == std::string::npos ? close : kvChild(tok, open, close, k);
        if (v == std::string::npos || tok[v].kind != '{') {
            message = "PTDE's Steam settings weren't found in " + vdf.string() + ". Start the game from Steam once, then reinstall";
            return false;
        }
        open = v; close = kvClose(tok, v);
    }
    const std::string want = steamPlayLaunchOptions(dataDir);
    const std::string val = "\"" + kvEscape(want) + "\"";
    std::string out;
    size_t app = kvChild(tok, open, close, kPtdeAppId);
    if (app != std::string::npos && tok[app].kind == '{') {
        size_t appClose = kvClose(tok, app);
        size_t lo = kvChild(tok, app, appClose, "LaunchOptions");
        if (lo != std::string::npos && tok[lo].kind == 's') {
            const std::string& cur = tok[lo].s;
            if (cur == want) { message = "Steam's Play button for PTDE already opens the mctde launcher"; return true; }
            if (!cur.empty() && cur.find("mctde-launcher") == std::string::npos) {
                message = "PTDE already has your own Steam launch options (" + cur + "), so they were left as they are";
                return false;
            }
            out = t.substr(0, tok[lo].begin) + val + t.substr(tok[lo].end);
        } else {
            std::string ind = kvIndentAt(t, tok[appClose].begin);
            out = t.substr(0, tok[appClose].begin) + "\t\"LaunchOptions\"\t\t" + val + "\n" + ind + t.substr(tok[appClose].begin);
        }
    } else {
        std::string ind = kvIndentAt(t, tok[close].begin);
        out = t.substr(0, tok[close].begin) + "\t\"" + kPtdeAppId + "\"\n" + ind + "\t{\n" + ind +
              "\t\t\"LaunchOptions\"\t\t" + val + "\n" + ind + "\t}\n" + ind + t.substr(tok[close].begin);
    }

    std::error_code ec;
    fs::path backup = vdf; backup += ".mctde-backup";
    if (!fs::exists(backup, ec)) fs::copy_file(vdf, backup, ec);
    fs::path tmp = vdf; tmp += ".mctde-tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(out.data(), (std::streamsize)out.size());
        if (!o) { message = "could not write " + tmp.string(); return false; }
    }
    fs::permissions(tmp, fs::status(vdf, ec).permissions(), ec);
    fs::rename(tmp, vdf, ec);
    if (ec) { message = "could not replace " + vdf.string() + ": " + ec.message(); return false; }
    message = "Steam's Play button for PTDE now opens the mctde launcher";
    return true;
}

bool setSteamPlayAfterSteamExits(const std::string& dataDir, std::string& message) {
    while (steamRunning()) std::this_thread::sleep_for(std::chrono::seconds(2));
    std::this_thread::sleep_for(std::chrono::seconds(3));   // let Steam finish writing its files
    return setSteamPlayNow(dataDir, message);
}

bool setSteamPlay(const std::string& dataDir, std::string& message) {
    if (!steamRunning() || std::getenv("MCTDE_LOCALCONFIG_VDF")) return setSteamPlayNow(dataDir, message);
    std::string launcher = stableLauncherPath();
    if (launcher.empty() || launcher.rfind("/tmp/.mount_", 0) == 0) {
        message = "could not determine a stable path to the installer";
        return false;
    }
    std::error_code ec;
    fs::path logDir = fs::path(homeDir()) / ".cache" / "mctde-installer";
    fs::create_directories(logDir, ec);
    std::string logFile = fs::is_directory(logDir, ec) ? (logDir / "steam-play.log").string() : "";
    if (!runProcess({launcher, "steam-play", dataDir, "--wait-for-steam-exit"}, "", {}, logFile, /*detach=*/true)) {
        message = "could not start the Steam launch-options worker";
        return false;
    }
    message = "Steam is running, so Steam's Play button for PTDE will open the mctde launcher after you restart Steam";
    return true;
}

} // namespace mctde
