#pragma once
//
// LinuxLauncher: the native Linux mctde launcher (mctde-launcher, Qt 6, plus
// mctde-launcher-cli) from mctde-Launcher's `linux` branch. Linux build only.
//
// Its releases are pinned by a manifest on that branch, not by
// releases/latest (which is the Windows launcher's):
//   https://raw.githubusercontent.com/McRoodyPoo/mctde-Launcher/linux/latest.txt
//     mctde-launcher-linux=<version>
//     mctde-launcher-linux-url=https://github.com/McRoodyPoo/mctde-Launcher/releases/download/linux-v<ver>/mctde-launcher-linux-<ver>.tar.gz
//     mctde-launcher-linux-sha256=<64 hex>
// The tarball holds mctde-launcher-linux-<ver>/{mctde-launcher, mctde-launcher-cli,
// README-LINUX.md, LICENSE}; the two binaries go into DATA beside DARKSOULS.exe.
//
#include <functional>
#include <string>

namespace mctde {

// Download the latest Linux launcher (checksum-verified) into dataDir.
// false + message if it isn't published / reachable / valid; callers treat
// that as non-fatal (the Windows mctde_launcher.exe still works under Proton).
// MCTDE_LINUX_LAUNCHER_MANIFEST=<local file> overrides the manifest, and then a
// local path is accepted as the url (testing).
bool installLinuxLauncher(const std::string& dataDir, std::string& message,
                          const std::function<void(const std::string&, int)>& step = {});

// dataDir/mctde-launcher if it is there and executable, else "".
std::string linuxLauncherIn(const std::string& dataDir);

// True if Qt 6 Widgets can be loaded, i.e. the launcher's window can open
// (otherwise use mctde-launcher-cli).
bool qt6Available();

} // namespace mctde
