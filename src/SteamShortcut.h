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

// ---- PTDE's own Play button (localconfig.vdf LaunchOptions) ----
// On Linux, Steam's Play button for the real PTDE entry runs DARKSOULS.exe through
// Proton, and mctde-Link's launcher guard then reopens the *Windows* launcher there.
// For the Steam library copy, point PTDE's launch options at the native launcher:
//   [ -x "<DATA>/mctde-launcher" ] && exec "<DATA>/mctde-launcher"; %command%
// Steam runs launch options through /bin/sh, so Play opens the native launcher,
// and falls back to the normal game if the launcher is gone (e.g. after a restore).
// Launch options the player set themselves are left alone. localconfig.vdf is only
// written while Steam is closed (Steam rewrites it from memory on exit).

// True if dataDir is the copy Steam itself launches (steamapps/common/Dark Souls
// Prepare to Die Edition/DATA in a Steam library).
bool isSteamLibraryCopy(const std::string& dataDir);

// The launch options string for this copy.
std::string steamPlayLaunchOptions(const std::string& dataDir);

// Write PTDE's LaunchOptions now. Steam must not be running.
// MCTDE_LOCALCONFIG_VDF=<file> redirects the write (testing).
bool setSteamPlayNow(const std::string& dataDir, std::string& message);

// Now if Steam is closed, else a background worker that writes it once Steam exits.
bool setSteamPlay(const std::string& dataDir, std::string& message);

// The worker body ("steam-play <DATA> --wait-for-steam-exit").
bool setSteamPlayAfterSteamExits(const std::string& dataDir, std::string& message);

} // namespace mctde
