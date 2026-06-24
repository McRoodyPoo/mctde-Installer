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
