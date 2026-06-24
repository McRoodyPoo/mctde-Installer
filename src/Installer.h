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
//
// Optional backups are written as siblings of the DATA folder, and are only
// made if the corresponding flag is set:
//   backupPacked   -> "DATA-Backup-Packed"   (a copy of the still-packed DATA,
//                      taken before unpacking; only if the install is packed)
//   backupUnpacked -> "DATA-Backup-Unpacked" (a copy of the unpacked DATA,
//                      taken after unpacking and before the mod is applied)
// A pre-existing backup folder is preserved: the backup goes to a fresh numbered
// sibling ("DATA-Backup-Packed (2)", ...) instead of being skipped or overwritten.
//
// The install operates in place: the folder keeps its original name (DATA).
InstallResult fullInstall(const std::string& dataDir, const std::string& namelistPath,
                          std::string& message, const InstallProgress& progress = {},
                          bool backupPacked = false, bool backupUnpacked = false);

// Unpack-and-patch a DATA folder in place (no backups, no downloads):
//   1. unpack dvdbnd -> loose files (+ nested tpf/hkx/chr textures)
//   2. patch DARKSOULS.exe to load loose files
//   3. delete the dvdbnd archives
//   4. drop a sentinel so re-runs are no-ops
// Backups are the caller's responsibility (see fullInstall). `namelistPath`
// points at dvdbnd_namelist.txt. `message` gets a summary. `progress`, if set,
// receives a line per file as it's unpacked (throttled internally).
InstallResult installFlow(const std::string& dataDir,
                          const std::string& namelistPath,
                          std::string& message,
                          const InstallProgress& progress = {});

} // namespace mctde
