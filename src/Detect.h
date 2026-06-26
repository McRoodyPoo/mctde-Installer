#pragma once
//
// Detect: locate the PTDE install via Steam, and classify a DATA folder as
// packed (archives present) or unpacked (loose files).
//
#include <functional>
#include <string>
#include <vector>

namespace mctde {

enum class GameState { Packed, Unpacked, Unknown };

// What an install actually is, beyond packed/unpacked:
//   Ready   - a clean naked packed/unpacked game (safe to install onto). Backup
//             copies that are clean fall here too.
//   Mctde   - mctde is installed (launcher / Link present)
//   Unknown - modified: a file in param/chr/event/script differs from vanilla
//             (extra files are fine; changed vanilla files are not). Not safe to
//             install onto.
enum class InstallKind { Ready, Mctde, Unknown };

// Find the Dark Souls: Prepare to Die Edition DATA folder by reading Steam's
// install path + library folders. Returns the DATA dir (containing
// DARKSOULS.exe) or "" if not found.
std::string detectSteamDataDir();

// Classify a DATA folder. Packed = dvdbnd archives still present;
// Unpacked = loose files (and no archives); Unknown = neither.
GameState detectGameState(const std::string& dataDir);

// Aggressive hunt for the DATA folder: Steam registry first, then likely fixed
// locations on every drive, then a depth- and time-bounded recursive scan of
// the installer's own folder, the user's Desktop/Documents/Downloads, and drive
// roots. Returns the DATA dir (containing DARKSOULS.exe) or "".
std::string findDataDir();

struct GameInstall {
    std::string dataDir;   // folder containing DARKSOULS.exe
    bool steam;            // path lives under a Steam library
    GameState state;       // packed / unpacked / unknown
    InstallKind kind;      // vanilla / mctde / backup / unknown
};

// Classify an install folder, given its already-determined packed/unpacked state.
// Backup is decided by the DATA-Backup-* folder name; mctde by its marker files;
// Unknown by comparing param/chr/event/script (loose) or the dvdbnd sizes
// (packed) against the embedded vanilla baseline.
InstallKind classifyInstall(const std::string& dataDir, GameState state);

// Find ALL PTDE installs, invoking `onFound` for each as it is discovered
// (Steam libraries first, then the bounded scan). Deduplicates by path, so the
// callback fires once per install. Designed to drive a live-updating UI list.
void findAllDataDirs(const std::function<void(const GameInstall&)>& onFound);

// A restorable backup folder sitting next to a DATA folder.
struct BackupInfo {
    std::string path;   // full path to the backup folder
    std::string name;   // folder name, e.g. "DATA-Backup-Unpacked (2)"
    GameState   state;  // packed (original archives) / unpacked (loose files)
};

// Enumerate the installer's own backup folders that sit beside `dataDir`:
// siblings named DATA-Backup-Packed / DATA-Backup-Unpacked (and their numbered
// "(2)", "(3)", ... variants) that actually contain a DARKSOULS.exe, so they can
// be restored over the live DATA folder. Packed backups are listed first.
std::vector<BackupInfo> findBackups(const std::string& dataDir);

} // namespace mctde
