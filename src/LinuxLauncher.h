#pragma once
//
// LinuxLauncher: the native Linux mctde launcher (mctde-launcher, Qt 6, plus
// mctde-launcher-cli). Linux build only.
//
// It is released together with the Windows launcher: mctde-Launcher's
// "v<version>" release carries mctde-launcher-linux.zip (+ .sha256), holding
// mctde-launcher, mctde-launcher-cli, README-LINUX.md and LICENSE. The version
// is latest.txt's shared "mctde-launcher=" line. The two programs go into DATA
// beside DARKSOULS.exe.
//
#include <functional>
#include <string>

namespace mctde {

// Download the latest Linux launcher (checksum-verified) into dataDir.
// false + message if it isn't published / reachable / valid; callers treat
// that as non-fatal (the Windows mctde_launcher.exe still works under Proton).
// MCTDE_LINUX_LAUNCHER_ZIP=<local zip> installs from that file instead
// (testing).
bool installLinuxLauncher(const std::string& dataDir, std::string& message,
                          const std::function<void(const std::string&, int)>& step = {});

// dataDir/mctde-launcher if it is there and executable, else "".
std::string linuxLauncherIn(const std::string& dataDir);

// True if Qt 6 Widgets can be loaded, i.e. the launcher's window can open
// (otherwise use mctde-launcher-cli).
bool qt6Available();

} // namespace mctde
