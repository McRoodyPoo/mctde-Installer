#pragma once
//
// DisplayConfig: set DARK SOULS' user display config so DSFix works cleanly.
// DSFix only takes effect in windowed mode, and its own AA replaces the game's,
// so we force, in %LOCALAPPDATA%\NBGI\DarkSouls\DarkSouls.ini:
//   [DisplaySetting]        WindowMode   = 1   (windowed; required by DSFix)
//   [DisplaySettingFilter]  Antialiasing = 0   (let DSFix handle AA)
//
#include <string>

namespace mctde {

// Apply the display tweaks above. Edits the file in place (preserving every
// other setting and the file's CRLF style) if it exists, or creates a minimal
// one if the game has never been run. Returns true on success (including
// "already set"); sets `message` to a summary of what changed, or the error.
bool ensureDisplayConfig(std::string& message);

} // namespace mctde
