#pragma once
//
// Installer: the end-to-end, idempotent install flow that turns a packed PTDE
// install into an unpacked, loose-file-loading one — operating in place on the
// given DATA directory, with a backup of the originals first.
//
#include <functional>
#include <string>

namespace mctde {

enum class InstallResult { Done, AlreadyDone, Failed };

// Progress callback for the full flow: a stage label and 0-100 percent
// (pct < 0 means indeterminate / spinner).
using InstallProgress = std::function<void(const std::string& stage, int pct)>;

// What kind of pre-mod backup(s) to make, and where.
struct BackupOptions {
    bool packed = false;     // keep a copy of the vanilla packed archives
    bool unpacked = false;   // build a vanilla unpacked copy (ready for other mods)
    std::string destRoot;    // parent folder the backup(s) are written under
};

// Make the requested vanilla backups before any modding. Writes
// "<game> - vanilla (packed)/DATA" and/or "<game> - vanilla (unpacked)/DATA"
// under destRoot. `message` gets a summary.
bool runBackups(const std::string& dataDir, const BackupOptions& opts,
                std::string& message, const InstallProgress& progress = {});

// The whole player-facing flow on a DATA folder: unpack+patch if the game is
// still packed, then download + install the mod, mctde-Link, and the launcher.
// `namelistPath` points at dvdbnd_namelist.txt. Idempotent on the unpack step.
InstallResult fullInstall(const std::string& dataDir, const std::string& namelistPath,
                          std::string& message, const InstallProgress& progress = {});

// Run the full flow on `dataDir` (the game's DATA folder):
//   1. back up DARKSOULS.exe + dvdbnd0-3 to mctde-backup/
//   2. unpack dvdbnd -> loose files (+ nested tpf/hkx/chr textures)
//   3. patch DARKSOULS.exe to load loose files
//   4. delete the dvdbnd archives
//   5. drop a sentinel so re-runs are no-ops
// `namelistPath` points at dvdbnd_namelist.txt. `message` gets a summary.
InstallResult installFlow(const std::string& dataDir,
                          const std::string& namelistPath,
                          std::string& message);

} // namespace mctde
