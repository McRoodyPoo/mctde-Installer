#include "Installer.h"
#include "Detect.h"
#include "DisplayConfig.h"
#include "Download.h"
#include "DxvkAssets.h"
#include "ExePatch.h"
#include "Extract.h"
#include "Release.h"
#include "Unpacker.h"
#ifndef _WIN32
#include "LinuxLauncher.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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
static const char* kLauncherZipUrl =
    "https://github.com/McRoodyPoo/mctde-Launcher/releases/latest/download/mctde-launcher-windows.zip";
static const char* kLauncherExeUrl =   // pre-0.6.0 releases: the bare exe
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
#ifdef _WIN32
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    err = "Windows error " + std::to_string(GetLastError());
    return false;
#else
    // libstdc++ uses copy_file_range / sendfile here: kernel-side, fast.
    std::error_code ec;
    if (fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) return true;
    err = ec.message();
    return false;
#endif
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

        // 4. Windows launcher -> DATA, taken out of mctde-launcher-windows.zip.
        //    Installed on Linux too: it runs through Proton, and it's what
        //    mctde-Link's launcher guard reopens. Falls back to the bare
        //    mctde_launcher.exe asset older releases carried.
        step("Downloading the launcher...", -1);
        std::string launcherFrom = std::string("    from ") + kLauncherZipUrl;
        step(launcherFrom, 0);
        const std::string launcherExe = (data / "mctde_launcher.exe").string();
        if (!fetchFromReleaseZip(kLauncherZipUrl, "mctde_launcher.exe", launcherExe, err,
                                 dlProgress(progress, launcherFrom))) {
            launcherFrom = std::string("    from ") + kLauncherExeUrl;
            step(launcherFrom, 0);
            if (!downloadUrl(kLauncherExeUrl, launcherExe, err, dlProgress(progress, launcherFrom))) {
                message = "launcher download failed: " + err;
                return InstallResult::Failed;
            }
        }
#ifndef _WIN32
        // 4b. Linux: the native launcher (mctde-launcher-linux.zip), next to the
        //     Windows one. Optional: if it can't be fetched, the Windows
        //     launcher above still works through Proton.
        {
            std::string lm;
            if (installLinuxLauncher(dataDir, lm, step)) step(lm + ".", -1);
            else step("Linux launcher skipped: " + lm + ". The Windows launcher will be used through Proton.", -1);
        }
#endif

        // 4c. DXVK (Vulkan) -> DATA. Windows only: drops d3d9_dxvk.dll + a default
        //     dxvk.conf and points mctde-Link at it (Vulkan on by default). A no-op on
        //     Linux/Proton (Proton has its own DXVK). Best-effort: a DXVK write problem
        //     shouldn't fail an otherwise-complete install.
        {
            std::string dm;
            if (deployDxvk(dataDir, dm, progress) == InstallResult::Failed)
                step("Warning: could not deploy DXVK (" + dm + ").", -1);
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
#ifdef _WIN32
            std::string dm;
            if (ensureDisplayConfig(dm))
                step("Display: " + dm + ".", -1);
            else
                step("Warning: could not apply display settings (" + dm + ").", -1);
#endif
            // On Linux (native or Wine) the game runs in Steam's Proton prefix for
            // PTDE, which reads its own DarkSouls.ini: apply the same tweaks there.
            const std::vector<std::string> prefixes = protonGameLocalAppData();
#ifndef _WIN32
            if (prefixes.empty())
                step("Note: no Proton prefix for Dark Souls found yet. Launch the game once "
                     "through Steam, then run the installer again to set windowed mode / no AA "
                     "for DSFix (or set them in the game's options).", -1);
#endif
            for (const std::string& lad : prefixes) {
                std::string pm;
                if (ensureDisplayConfigIn(lad, pm))
                    step("Display (Proton prefix " + lad + "): " + pm + ".", -1);
                else
                    step("Warning: could not apply display settings in " + lad + " (" + pm + ").", -1);
            }
        }

        step("Done!", 100);
        message = "Install complete.";
        return InstallResult::Done;
    } catch (const std::exception& e) {
        message = std::string("install failed: ") + e.what();
        return InstallResult::Failed;
    }
}

#ifdef _WIN32
// True when the Windows installer is itself running under Wine / Proton (Linux). ntdll
// exports wine_get_version only there. On Linux the game already renders on Vulkan via
// Proton's own DXVK, so bundling our Windows DXVK is redundant (and double-DXVK can glitch).
static bool runningUnderWine() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
}
#endif

InstallResult deployDxvk(const std::string& dataDir, std::string& message,
                         const InstallProgress& progress) {
#ifndef _WIN32
    // Native Linux build: never ship DXVK; Proton provides its own.
    (void)dataDir; (void)progress;
    message = "DXVK not deployed on Linux (Proton provides its own DXVK)";
    return InstallResult::Done;
#else
    auto step = [&](const std::string& s, int pct) { if (progress) progress(s, pct); };
    fs::path data(dataDir);
    std::error_code ec;
    if (!fs::exists(data, ec)) {
        message = "DATA folder not found: " + dataDir;
        return InstallResult::Failed;
    }

    // Windows installer running under Wine/Proton: same as native Linux -- skip DXVK.
    if (runningUnderWine()) {
        message = "skipped DXVK (running under Wine/Proton; Proton provides its own DXVK)";
        step("DXVK: skipped on Linux/Proton (Proton has its own DXVK).", -1);
        return InstallResult::Done;
    }

    // 1. DXVK's 32-bit d3d9, dropped as d3d9_dxvk.dll (never as d3d9.dll -- that's
    //    mctde-Link's slot). Always refreshed to the build we ship.
    const uint8_t* dll = nullptr; size_t dllLen = 0;
    if (!dxvkDllBytes(&dll, &dllLen) || dllLen == 0) {
        message = "embedded DXVK payload missing (built without dxvk_assets.res?)";
        return InstallResult::Failed;
    }
    fs::path dllPath = data / "d3d9_dxvk.dll";
    {
        std::ofstream o(dllPath, std::ios::binary | std::ios::trunc);
        if (!o) { message = "cannot write " + dllPath.string(); return InstallResult::Failed; }
        o.write(reinterpret_cast<const char*>(dll), (std::streamsize)dllLen);
        if (!o)  { message = "write failed for " + dllPath.string(); return InstallResult::Failed; }
    }
    step("DXVK: wrote d3d9_dxvk.dll (" + std::to_string(dllLen / 1024) + " KB).", -1);

    // 2. dxvk.conf -- only if the player doesn't already have one, so a tuned conf is
    //    never clobbered (same best-effort spirit as the DarkSouls.ini tweaks).
    fs::path confPath = data / "dxvk.conf";
    if (!fs::exists(confPath, ec)) {
        const uint8_t* cf = nullptr; size_t cfLen = 0;
        if (dxvkConfBytes(&cf, &cfLen) && cfLen) {
            std::ofstream o(confPath, std::ios::binary | std::ios::trunc);
            if (o) o.write(reinterpret_cast<const char*>(cf), (std::streamsize)cfLen);
        }
        step("DXVK: wrote default dxvk.conf.", -1);
    } else {
        step("DXVK: kept the existing dxvk.conf.", -1);
    }

    // 3. Point mctde-Link at DXVK so a fresh install runs on Vulkan by default. An
    //    absolute path into this DATA folder is correct on any machine. The launcher's
    //    Vulkan tab flips this key off/on later. Best-effort.
    fs::path ini = data / "mctde-link.ini";
    std::wstring wIni = ini.wstring();
    std::wstring wDll = dllPath.wstring();
    if (WritePrivateProfileStringW(L"Compatibility", L"RealD3D9Path", wDll.c_str(), wIni.c_str()))
        step("DXVK: enabled Vulkan (RealD3D9Path set in mctde-link.ini).", -1);
    else
        step("DXVK: note - could not set RealD3D9Path (enable it from the launcher).", -1);

    message = "DXVK deployed (Vulkan on by default).";
    return InstallResult::Done;
#endif
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
