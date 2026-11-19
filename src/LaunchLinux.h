#pragma once
//
// LaunchLinux: start an installed copy of the game from the native Linux build,
// through the user's Steam Proton (Linux only; the Windows build just runs the
// exe). The copy's steam_appid.txt is 480, so Steam shows it as Spacewar.
//
#include <string>
#include <vector>

namespace mctde {

struct ProtonSetup {
    std::string proton;      // folder holding the "proton" script
    std::string prefix;      // STEAM_COMPAT_DATA_PATH (PTDE's compatdata/211420)
    std::string steamRoot;   // STEAM_COMPAT_CLIENT_INSTALL_PATH
    bool prefixExists = false;
};

// Pick the Proton build and prefix to run the game with: the Proton that
// created PTDE's prefix if it is still installed, else Proton - Experimental,
// else the newest "Proton X.Y", else a custom tool in compatibilitytools.d.
// false + err when no Steam or no Proton is installed.
bool findProtonSetup(ProtonSetup& out, std::string& err);

// True if a Steam client process is running.
bool steamRunning();

// Launch dataDir's mctde_launcher.exe (or DARKSOULS.exe) through Proton.
// Starts Steam first if it isn't running (the game needs it), waiting up to
// ~30 s for it. `log` receives progress lines. Proton's output goes to
// ~/.cache/mctde-installer/proton.log.
//   wait = false: detached; returns as soon as Proton has been started (the
//                 installer's Play button, which then closes).
//   wait = true:  stays in the foreground until the game has exited, so a
//                 Steam shortcut running "play" shows as playing meanwhile.
bool launchGameLinux(const std::string& dataDir, std::string& message,
                     void (*log)(const std::string&) = nullptr, bool wait = false);

// Run argv with extra "KEY=value" environment entries in cwd, stdout/stderr to
// logFile ("" = /dev/null). detach = true: new session, not waited on
// (returns true once started, exitCode untouched). detach = false: waits and
// stores the exit status in *exitCode.
bool runProcess(const std::vector<std::string>& argv, const std::string& cwd,
                const std::vector<std::string>& extraEnv, const std::string& logFile,
                bool detach, int* exitCode = nullptr);

} // namespace mctde
