#pragma once
//
// LaunchLinux: start an installed copy of the game from the native Linux build,
// through the user's Steam Proton (Linux only; the Windows build just runs the
// exe). The copy's steam_appid.txt is 480, so Steam shows it as Spacewar.
//
#include <string>

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

// Launch dataDir's mctde_launcher.exe (or DARKSOULS.exe) through Proton,
// detached from the installer. Starts Steam first if it isn't running (the
// game needs it), waiting up to ~30 s for it. `log` receives progress lines.
// Proton's output goes to ~/.cache/mctde-installer/proton.log.
bool launchGameLinux(const std::string& dataDir, std::string& message,
                     void (*log)(const std::string&) = nullptr);

} // namespace mctde
