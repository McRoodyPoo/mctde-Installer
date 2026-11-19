#include "LaunchLinux.h"
#include "Detect.h"
#include "LinuxLauncher.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

extern char** environ;
namespace fs = std::filesystem;

namespace mctde {

static bool isProtonDir(const fs::path& d) {
    std::error_code ec;
    return fs::is_regular_file(d / "proton", ec);
}

// "Proton 9.0" -> {9, 0}; non-numbered names sort lowest.
static std::vector<int> versionOf(const std::string& name) {
    std::vector<int> v;
    int cur = -1;
    for (char c : name) {
        if (std::isdigit((unsigned char)c)) cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else if (cur >= 0) { v.push_back(cur); cur = -1; if (c != '.') break; }
    }
    if (cur >= 0) v.push_back(cur);
    return v;
}

bool findProtonSetup(ProtonSetup& out, std::string& err) {
    out = ProtonSetup{};
    std::vector<std::string> roots = linuxSteamRootPaths();
    if (roots.empty()) { err = "no Steam installation found"; return false; }
    out.steamRoot = roots.front();
    std::vector<std::string> libs = steamLibraryPaths();

    // Prefix: PTDE's own (that's also where the installer writes DarkSouls.ini).
    for (const std::string& lib : libs) {
        fs::path p = fs::path(lib) / "steamapps" / "compatdata" / "211420";
        std::error_code ec;
        if (fs::is_directory(p / "pfx", ec)) { out.prefix = p.string(); out.prefixExists = true; break; }
    }
    if (out.prefix.empty())   // Proton creates it on first run
        out.prefix = (fs::path(out.steamRoot) / "steamapps" / "compatdata" / "211420").string();

    // 1. The Proton that created the prefix (config_info line 2 is a path inside it).
    if (out.prefixExists) {
        std::ifstream ci(fs::path(out.prefix) / "config_info");
        std::string line;
        std::getline(ci, line);
        if (std::getline(ci, line)) {
            size_t f = line.find("/files/");
            if (f == std::string::npos) f = line.find("/dist/");
            if (f != std::string::npos && isProtonDir(line.substr(0, f))) {
                out.proton = line.substr(0, f);
                return true;
            }
        }
    }

    // 2. Installed Proton builds in the Steam libraries.
    std::string experimental, best;
    std::vector<int> bestV;
    for (const std::string& lib : libs) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(lib) / "steamapps" / "common", ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.rfind("Proton", 0) != 0 || !isProtonDir(it->path())) continue;
            if (name == "Proton - Experimental") { experimental = it->path().string(); continue; }
            std::vector<int> v = versionOf(name);
            if (best.empty() || v > bestV) { best = it->path().string(); bestV = v; }
        }
    }
    if (!experimental.empty()) { out.proton = experimental; return true; }
    if (!best.empty()) { out.proton = best; return true; }

    // 3. Custom tools (GE-Proton etc.).
    for (const std::string& root : roots) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(root) / "compatibilitytools.d", ec), end;
             !ec && it != end; it.increment(ec))
            if (isProtonDir(it->path())) { out.proton = it->path().string(); return true; }
    }
    err = "no Proton found. Install \"Proton Experimental\" from your Steam library (Tools)";
    return false;
}

bool steamRunning() {
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        std::string pid = it->path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), ::isdigit)) continue;
        std::ifstream comm(it->path() / "comm");
        std::string name;
        std::getline(comm, name);
        if (name == "steam" || name == "steamwebhelper") return true;
    }
    return false;
}

bool runProcess(const std::vector<std::string>& argv, const std::string& cwd,
                const std::vector<std::string>& extraEnv, const std::string& logFile,
                bool detach, int* exitCode) {
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        // Don't leak the AppImage's library path into Proton / Steam.
        if (kv.rfind("LD_LIBRARY_PATH=", 0) == 0 && std::getenv("APPDIR")) continue;
        bool overridden = false;
        for (const std::string& x : extraEnv)
            if (kv.compare(0, x.find('=') + 1, x, 0, x.find('=') + 1) == 0) overridden = true;
        if (!overridden) env.push_back(kv);
    }
    for (const std::string& x : extraEnv) env.push_back(x);

    std::vector<char*> av, ev;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    for (const auto& e : env) ev.push_back(const_cast<char*>(e.c_str()));
    ev.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (detach) setsid();
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        int fd = logFile.empty() ? open("/dev/null", O_WRONLY)
                                 : open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) { dup2(in, 0); close(in); }
        execvpe(av[0], av.data(), ev.data());
        _exit(127);
    }
    if (detach) return true;   // not waited on: the child outlives the installer
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (exitCode) *exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return true;
}

bool launchGameLinux(const std::string& dataDir, std::string& message,
                     void (*log)(const std::string&), bool wait) {
    auto say = [&](const std::string& s) { if (log) log(s); };

    fs::path data(dataDir);
    std::error_code ec;

    // The native Linux launcher, when installed, does it all itself (Steam,
    // PTDE's Proton + runtime, DSCM, settings, mctde-Link's launcher guard):
    // open its window, or run its CLI's play where Qt 6 isn't available.
    if (std::string native = linuxLauncherIn(dataDir); !native.empty()) {
        std::vector<std::string> argv;
        if (qt6Available()) {
            argv = {native};
            say("Opening the mctde launcher...");
        } else {
            fs::path cli = data / "mctde-launcher-cli";
            if (::access(cli.c_str(), X_OK) == 0) {
                argv = {cli.string(), "play"};
                if (wait) argv.push_back("--wait");
                say("Qt 6 not found; starting the game with mctde-launcher-cli...");
            }
        }
        if (!argv.empty()) {
            int rc = 0;
            if (!runProcess(argv, data.string(), {}, "", /*detach=*/!wait, &rc)) {
                message = "could not start " + argv[0];
                return false;
            }
            message = wait ? "mctde launcher exited (code " + std::to_string(rc) + ")"
                           : "Started the mctde launcher (native Linux)";
            return true;
        }
    }

    fs::path exe = data / "mctde_launcher.exe";
    if (!fs::exists(exe, ec)) exe = data / "DARKSOULS.exe";
    if (!fs::exists(exe, ec)) { message = "no mctde_launcher.exe or DARKSOULS.exe in " + dataDir; return false; }

    ProtonSetup ps;
    std::string err;
    if (!findProtonSetup(ps, err)) { message = err; return false; }
    say("Proton: " + ps.proton);
    say("Prefix: " + ps.prefix + (ps.prefixExists ? "" : " (will be created)"));

    // The game needs the Steam client (it runs as Spacewar, app 480).
    if (!steamRunning()) {
        say("Starting Steam...");
        runProcess({"steam", "-silent"}, "", {}, "", /*detach=*/true);
        for (int i = 0; i < 30 && !steamRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!steamRunning()) { message = "Steam did not start; start it and click Play again"; return false; }
        std::this_thread::sleep_for(std::chrono::seconds(8));   // let it log in
    }

    fs::create_directories(ps.prefix, ec);
    std::string logDir;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) logDir = xdg;
    else if (const char* home = std::getenv("HOME")) logDir = std::string(home) + "/.cache";
    std::string logFile;
    if (!logDir.empty()) {
        fs::create_directories(fs::path(logDir) / "mctde-installer", ec);
        logFile = (fs::path(logDir) / "mctde-installer" / "proton.log").string();
    }

    std::vector<std::string> env = {
        "STEAM_COMPAT_DATA_PATH=" + ps.prefix,
        "STEAM_COMPAT_CLIENT_INSTALL_PATH=" + ps.steamRoot,
        "STEAM_COMPAT_INSTALL_PATH=" + data.parent_path().string(),
        "SteamAppId=480",
        "SteamGameId=480",
    };
    say("Launching " + exe.filename().string() + "...");
    // waitforexitandrun also waits for the prefix's wineserver, i.e. for the
    // launcher *and* the game it starts.
    int rc = 0;
    if (!runProcess({(fs::path(ps.proton) / "proton").string(), wait ? "waitforexitandrun" : "run", exe.string()},
                    data.string(), env, logFile, /*detach=*/!wait, &rc)) {
        message = "could not start Proton";
        return false;
    }
    if (wait) {
        message = "Game exited (Proton exit code " + std::to_string(rc) + ")";
        return true;
    }
    message = "Started " + exe.filename().string() + " with " + fs::path(ps.proton).filename().string() +
              (logFile.empty() ? "" : " (log: " + logFile + ")");
    return true;
}

} // namespace mctde
