#pragma once
//
// SteamShortcut: add an installed copy of the game to the user's Steam library
// as a non-Steam shortcut (Linux build), the way Quiver Launcher does it:
//   - the shortcut runs this installer (the .AppImage file itself, never its
//     temporary mount) with "play <DATA>", which starts the game through Proton
//     and stays open until it exits;
//   - shortcuts.vdf is only written while Steam is closed (Steam rewrites it
//     from memory). If Steam is running, a small background worker waits for
//     Steam to exit and writes it then, so "restart Steam once" is enough;
//   - an existing entry for the same copy is updated instead of duplicated, and
//     every other entry is kept byte-for-byte.
//
#include <string>

namespace mctde {

// The name the shortcut gets in Steam for this copy.
std::string steamShortcutName(const std::string& dataDir);

// Write (or update) the shortcut now. Steam must not be running.
// MCTDE_SHORTCUTS_VDF=<file> redirects the write (testing).
bool addSteamShortcutNow(const std::string& dataDir, std::string& message);

// The player-facing action: add now if Steam is closed, otherwise start the
// background worker that adds it once Steam exits. `message` says which.
bool addToSteam(const std::string& dataDir, std::string& message);

// The worker body ("add-to-steam <DATA> --wait-for-steam-exit"): wait until
// Steam is no longer running, then write the shortcut.
bool addSteamShortcutAfterSteamExits(const std::string& dataDir, std::string& message);

} // namespace mctde
