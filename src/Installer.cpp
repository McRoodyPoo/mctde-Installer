#include "Installer.h"
#include "Detect.h"
#include "DisplayConfig.h"
#include "Download.h"
#include "ExePatch.h"
#include "Extract.h"
#include "Unpacker.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;

namespace mctde {

// Download endpoints. The mod payload is host-agnostic (one constant); Link and
// launcher track the latest GitHub release.
static const char* kModUrl =
    "https://pub-a585848bebaf4be2a03b1c1051f1ba79.r2.dev/mctde-0.88.zip";
static const char* kLinkUrl =
    "https://github.com/McRoodyPoo/mctde-Link/releases/latest/download/mctde-Link.zip";
static const char* kLauncherUrl =
    "https://github.com/McRoodyPoo/mctde-Launcher/releases/latest/download/mctde_launcher.exe";

// Pick a backup folder name that doesn't collide with an existing one: returns
// `base` if it's free, otherwise `base (2)`, `base (3)`, ... This way a backup a
// previous run left behind is preserved instead of being skipped or overwritten
// (overwriting into a populated/locked leftover folder is what used to stall the
// install). The chosen folder is always fresh, so the copy starts from empty.
static fs::path uniqueBackupPath(const fs::path& base) {
    std::error_code ec;
    if (!fs::exists(base, ec)) return base;
    for (int n = 2; n < 1000; ++n) {
        fs::path cand = base.parent_path() / (base.filename().string() + " (" + std::to_string(n) + ")");
        if (!fs::exists(cand, ec)) return cand;
    }
    return base;   // pathological fallback (1000 existing backups); copy will report any error
}

// Copy one file over any existing one. CopyFileW is the native call: under Wine
// it is ~8x faster than libstdc++'s fs::copy_file (which moves data in small
// chunks: ~6 MB/s vs ~46 MB/s measured on an NTFS disk).
static bool copyOneFile(const fs::path& src, const fs::path& dst, std::string& err) {
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    err = "Windows error " + std::to_string(GetLastError());
    return false;
}

// Recursively copy the contents of `src` into `dst` (created as needed), one
// file at a time so progress can be reported: counts the files first, then
// calls `step("  <relpath>", pct)` at most every 50 ms. On the first failure
// sets `failMsg` and stops. Returns the number of files copied.
static size_t copyTree(const fs::path& src, const fs::path& dst,
                       const std::function<void(const std::string&, int)>& step,
                       std::string& failMsg) {
    std::error_code ec;
    size_t total = 0;
    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code fc;
        if (it->is_regular_file(fc)) ++total;
    }

    fs::create_directories(dst, ec);
    size_t done = 0;
    auto lastTick = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::path& from = it->path();
        fs::path rel = fs::relative(from, src, ec);
        fs::path to = dst / rel;
        std::error_code ce;
        if (it->is_directory(ce)) {
            fs::create_directories(to, ce);
        } else if (it->is_regular_file(ce)) {
            fs::create_directories(to.parent_path(), ce);
            std::string err;
            if (!copyOneFile(from, to, err)) {
                failMsg = "copy failed for " + rel.string() + ": " + err;
                break;
            }
            ++done;
            auto now = std::chrono::steady_clock::now();
            if (now - lastTick >= std::chrono::milliseconds(50)) {
                lastTick = now;
                step("  " + rel.string(), total ? (int)(done * 100 / total) : -1);
            }
        }
    }
    return done;
}

// Snapshot the whole DATA folder to `dst` (a sibling backup folder). `dst` is
// expected to be a fresh, non-existing path (see uniqueBackupPath). The copy
// goes into "<dst>.partial" and is renamed to `dst` only once it is complete,
// so an interrupted backup (installer closed or killed) never shows up as a
// restorable backup. Returns false (and sets `message`) on a copy error.
static bool backupDataFolder(const fs::path& data, const fs::path& dst, std::string& message,
                             const std::function<void(const std::string&, int)>& step) {
    std::error_code ec;
    fs::path partial = dst;
    partial += ".partial";
    fs::remove_all(partial, ec);   // leftover from an interrupted run
    std::string failMsg;
    copyTree(data, partial, step, failMsg);
    if (failMsg.empty()) {
        fs::rename(partial, dst, ec);
        if (ec) failMsg = "could not rename " + partial.filename().string() + ": " + ec.message();
    }
    if (!failMsg.empty()) {
        fs::remove_all(partial, ec);
        message = "backup to " + dst.filename().string() + " failed: " + failMsg;
        return false;
    }
    return true;
}

InstallResult installFlow(const std::string& dataDir,
                          const std::string& namelistPath,
                          std::string& message,
                          const InstallProgress& progress) {
    fs::path data(dataDir);
    fs::path exe = data / "DARKSOULS.exe";
    if (!fs::exists(exe)) {
        message = "no DARKSOULS.exe in " + dataDir + " (not a PTDE DATA folder)";
        return InstallResult::Failed;
    }

    fs::path sentinel = data / ".mctde-unpacked";
    if (fs::exists(sentinel)) {
        message = "already unpacked (sentinel present); nothing to do";
        return InstallResult::AlreadyDone;
    }

    try {
        // 1. Unpack dvdbnd -> loose files (in place) + nested archives.
        //    (Backups, if any, are made by the caller before we get here.)
        //    Report each file to the log, but throttle to a readable rate so a
        //    fast disk doesn't flood the UI with thousands of messages.
        auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        UnpackProgress onFile = [&](const std::string& rel, int pct) {
            if (!progress) return;
            auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::milliseconds(10)) return;
            last = now;
            progress("  " + rel, pct);
        };
        UnpackStats st = unpackAll(dataDir, dataDir, namelistPath, onFile);
        if (progress)
            progress("Unpacked " + std::to_string(st.files) + " files (" +
                     std::to_string(st.decompressed) + " decompressed, " +
                     std::to_string(st.errors) + " errors).", 100);

        // 2. Patch the exe (to a temp file, then swap over the original).
        std::string pm;
        fs::path tmpExe = data / "DARKSOULS.exe.mctde-tmp";
        PatchResult pr = patchExe(exe.string(), tmpExe.string(), pm);
        if (pr == PatchResult::Patched) {
            fs::remove(exe);
            fs::rename(tmpExe, exe);
        } else if (pr == PatchResult::AlreadyPatched) {
            if (fs::exists(tmpExe)) fs::remove(tmpExe);
        } else {
            if (fs::exists(tmpExe)) fs::remove(tmpExe);
            message = "exe patch failed: " + pm;
            return InstallResult::Failed;
        }

        // 3. Delete the now-redundant archives.
        for (int i = 0; i < 4; ++i) {
            std::error_code ec;
            fs::remove(data / ("dvdbnd" + std::to_string(i) + ".bhd5"), ec);
            fs::remove(data / ("dvdbnd" + std::to_string(i) + ".bdt"), ec);
        }

        // 4. Sentinel so re-runs are no-ops.
        std::ofstream(sentinel) << "unpacked by mctde-installer\n";

        message = "unpacked " + std::to_string(st.files) + " files (" +
                  std::to_string(st.decompressed) + " decompressed, " +
                  std::to_string(st.errors) + " errors); exe patched; archives removed";
        return InstallResult::Done;
    } catch (const std::exception& e) {
        message = std::string("install failed: ") + e.what();
        return InstallResult::Failed;
    }
}

// Build a byte-level progress adapter for a download stage.
static ProgressFn dlProgress(const InstallProgress& progress, const std::string& stage) {
    return [progress, stage](uint64_t got, uint64_t total) -> bool {
        if (progress) progress(stage, total ? int(got * 100 / total) : -1);
        return true;
    };
}

InstallResult fullInstall(const std::string& dataDir, const std::string& namelistPath,
                          std::string& message, const InstallProgress& progress,
                          bool backupPacked, bool backupUnpacked) {
    auto step = [&](const std::string& s, int pct) { if (progress) progress(s, pct); };

    fs::path data(dataDir);
    if (!fs::exists(data / "DARKSOULS.exe")) {
        message = "no DARKSOULS.exe in " + dataDir;
        return InstallResult::Failed;
    }
    fs::path parent = data.parent_path();

    try {
        bool packed = (detectGameState(dataDir) == GameState::Packed);

        // 0a. Back up the still-packed copy (before unpacking), if requested.
        //     Only meaningful while the install is actually packed.
        if (packed && backupPacked) {
            fs::path dst = uniqueBackupPath(parent / "DATA-Backup-Packed");
            step("Backing up the packed copy to " + dst.filename().string() + "...", -1);
            if (!backupDataFolder(data, dst, message, step))
                return InstallResult::Failed;
        }

        // 1. Unpack + patch only if the game is still packed.
        if (packed) {
            step("Unpacking the game (this takes a minute)...", -1);
            std::string um;
            if (installFlow(dataDir, namelistPath, um, progress) == InstallResult::Failed) {
                message = "unpack failed: " + um;
                return InstallResult::Failed;
            }
        }

        // 1b. Back up the unpacked (still-vanilla) copy before the mod goes in.
        if (backupUnpacked) {
            fs::path dst = uniqueBackupPath(parent / "DATA-Backup-Unpacked");
            step("Backing up the unpacked copy to " + dst.filename().string() + "...", -1);
            if (!backupDataFolder(data, dst, message, step))
                return InstallResult::Failed;
        }

        std::string err;

        // The download-source line doubles as the progress stage, so the URL is
        // logged once and the repeated percentage updates don't spam the log.

        // 2. Mod payload -> extracted into DATA. The zip is DATA-relative
        //    (loose chr/, map/, ... plus DSFix's DINPUT8.dll/DSfix.ini/dsfix/),
        //    so it must land next to DARKSOULS.exe, not the game root.
        step("Downloading the mod...", -1);
        std::string modFrom = std::string("    from ") + kModUrl;
        step(modFrom, 0);
        fs::path modZip = data / "_mctde_mod.zip";
        if (!downloadUrl(kModUrl, modZip.string(), err, dlProgress(progress, modFrom))) {
            message = "mod download failed: " + err;
            return InstallResult::Failed;
        }
        step("Installing the mod...", -1);
        extractZip(modZip.string(), dataDir, err);
        std::error_code ec; fs::remove(modZip, ec);

        // 3. mctde-Link -> DATA.
        step("Downloading mctde-Link...", -1);
        std::string linkFrom = std::string("    from ") + kLinkUrl;
        step(linkFrom, 0);
        fs::path linkZip = data / "_mctde_link.zip";
        if (!downloadUrl(kLinkUrl, linkZip.string(), err, dlProgress(progress, linkFrom))) {
            message = "mctde-Link download failed: " + err;
            return InstallResult::Failed;
        }
        step("Installing mctde-Link...", -1);
        extractZip(linkZip.string(), dataDir, err);
        fs::remove(linkZip, ec);

        // 4. Launcher -> DATA (a single exe, not an archive).
        step("Downloading the launcher...", -1);
        std::string launcherFrom = std::string("    from ") + kLauncherUrl;
        step(launcherFrom, 0);
        if (!downloadUrl(kLauncherUrl, (data / "mctde_launcher.exe").string(), err,
                         dlProgress(progress, launcherFrom))) {
            message = "launcher download failed: " + err;
            return InstallResult::Failed;
        }

        // 5. steam_appid.txt (exactly "480", Spacewar) so the game launches under
        //    the mod's Steam setup. Written last, into the DATA folder.
        {
            std::ofstream sa((data / "steam_appid.txt"), std::ios::binary | std::ios::trunc);
            sa.write("480", 3);
        }

        // 6. Apply display tweaks in the user's DarkSouls.ini (windowed mode +
        //    no in-game AA) so DSFix works cleanly. Best-effort: a locked or
        //    unwritable ini shouldn't fail an otherwise-complete install, so log it.
        {
            std::string dm;
            if (ensureDisplayConfig(dm))
                step("Display: " + dm + ".", -1);
            else
                step("Warning: could not apply display settings (" + dm + ").", -1);
        }

        step("Done!", 100);
        message = "Install complete.";
        return InstallResult::Done;
    } catch (const std::exception& e) {
        message = std::string("install failed: ") + e.what();
        return InstallResult::Failed;
    }
}

InstallResult restoreFromBackup(const std::string& backupDir,
                                const std::string& dataDir,
                                std::string& message,
                                const InstallProgress& progress) {
    fs::path backup(backupDir), data(dataDir);
    std::error_code ec;

    if (!fs::exists(backup / "DARKSOULS.exe", ec)) {
        message = "that backup has no DARKSOULS.exe: " + backupDir;
        return InstallResult::Failed;
    }
    if (fs::weakly_canonical(backup, ec) == fs::weakly_canonical(data, ec)) {
        message = "backup and DATA are the same folder";
        return InstallResult::Failed;
    }

    auto step = [&](const std::string& s, int pct) { if (progress) progress(s, pct); };

    try {
        fs::path parent = data.parent_path();

        // 1. Move the live DATA aside with a fast same-volume rename. It is only
        //    deleted once the copy below fully succeeds, so a mid-copy failure
        //    can be rolled back to the original.
        fs::path aside;
        bool haveData = fs::exists(data, ec);
        if (haveData) {
            aside = uniqueBackupPath(parent / "DATA (replaced by restore)");
            fs::rename(data, aside, ec);
            if (ec) {
                message = "could not move the current DATA aside (game still running?): " + ec.message();
                return InstallResult::Failed;
            }
        }

        // 2. Copy the backup into place as the new DATA.
        step("Reading " + backup.filename().string() + "...", -1);
        std::string failMsg;
        size_t done = copyTree(backup, data, step, failMsg);

        if (!failMsg.empty()) {
            // Roll back: discard the partial copy and put the original DATA back.
            fs::remove_all(data, ec);
            if (haveData) fs::rename(aside, data, ec);
            message = failMsg;
            return InstallResult::Failed;
        }

        // 3. Success: drop the now-stale pre-restore copy.
        if (haveData) fs::remove_all(aside, ec);

        step("Restore complete.", 100);
        message = "Restored " + std::to_string(done) + " files from " + backup.filename().string() + ".";
        return InstallResult::Done;
    } catch (const std::exception& e) {
        message = std::string("restore failed: ") + e.what();
        return InstallResult::Failed;
    }
}

} // namespace mctde
