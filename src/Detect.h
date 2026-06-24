#pragma once
//
// Detect: locate the PTDE install via Steam, and classify a DATA folder as
// packed (archives present) or unpacked (loose files).
//
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

} // namespace mctde
