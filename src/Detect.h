#pragma once
//
// Detect: locate the PTDE install via Steam, and classify a DATA folder as
// packed (archives present) or unpacked (loose files).
//
#include <functional>
#include <string>

namespace mctde {

enum class GameState { Packed, Unpacked, Unknown };

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
};

// Find ALL PTDE installs, invoking `onFound` for each as it is discovered
// (Steam libraries first, then the bounded scan). Deduplicates by path, so the
// callback fires once per install. Designed to drive a live-updating UI list.
void findAllDataDirs(const std::function<void(const GameInstall&)>& onFound);

} // namespace mctde
