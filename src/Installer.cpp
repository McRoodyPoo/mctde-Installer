#include "Installer.h"
#include "Detect.h"
#include "Download.h"
#include "ExePatch.h"
#include "Extract.h"
#include "Unpacker.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace mctde {

// Download endpoints. The mod payload is host-agnostic (one constant); Link and
// launcher track the latest GitHub release.
static const char* kModUrl =
    "https://pub-7f583dbe74f94799be8d82e955438332.r2.dev/0.88.zip";
static const char* kLinkUrl =
    "https://github.com/McRoodyPoo/mctde-Link/releases/latest/download/mctde-Link.zip";
static const char* kLauncherUrl =
    "https://github.com/McRoodyPoo/mctde-Launcher/releases/latest/download/mctde_launcher.exe";

static void backupIfNeeded(const fs::path& src, const fs::path& backupDir) {
    if (!fs::exists(src)) return;
    fs::path dst = backupDir / src.filename();
    if (!fs::exists(dst)) fs::copy_file(src, dst);
}

InstallResult installFlow(const std::string& dataDir,
                          const std::string& namelistPath,
                          std::string& message) {
    fs::path data(dataDir);
    fs::path exe = data / "DARKSOULS.exe";
    if (!fs::exists(exe)) {
        message = "no DARKSOULS.exe in " + dataDir + " — not a PTDE DATA folder";
        return InstallResult::Failed;
    }

    fs::path sentinel = data / ".mctde-unpacked";
    if (fs::exists(sentinel)) {
        message = "already unpacked (sentinel present); nothing to do";
        return InstallResult::AlreadyDone;
    }

    try {
        // 1. Back up originals before any destructive step.
        fs::path backup = data / "mctde-backup";
        fs::create_directories(backup);
        backupIfNeeded(exe, backup);
        for (int i = 0; i < 4; ++i) {
            backupIfNeeded(data / ("dvdbnd" + std::to_string(i) + ".bhd5"), backup);
            backupIfNeeded(data / ("dvdbnd" + std::to_string(i) + ".bdt"), backup);
        }

        // 2. Unpack dvdbnd -> loose files (in place) + nested archives.
        UnpackStats st = unpackAll(dataDir, dataDir, namelistPath);

        // 3. Patch the exe (to a temp file, then swap over the original).
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

        // 4. Delete the now-redundant archives.
        for (int i = 0; i < 4; ++i) {
            std::error_code ec;
            fs::remove(data / ("dvdbnd" + std::to_string(i) + ".bhd5"), ec);
            fs::remove(data / ("dvdbnd" + std::to_string(i) + ".bdt"), ec);
        }

        // 5. Sentinel so re-runs are no-ops.
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

static void copyIfExists(const fs::path& src, const fs::path& dstDir) {
    std::error_code ec;
    if (fs::exists(src, ec))
        fs::copy_file(src, dstDir / src.filename(), fs::copy_options::overwrite_existing, ec);
}

// Copy the vanilla essentials (exe + dvdbnd archives + the stock dlls) into dst.
static void copyVanilla(const fs::path& data, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    copyIfExists(data / "DARKSOULS.exe", dst);
    for (int i = 0; i < 4; ++i) {
        copyIfExists(data / ("dvdbnd" + std::to_string(i) + ".bhd5"), dst);
        copyIfExists(data / ("dvdbnd" + std::to_string(i) + ".bdt"), dst);
    }
    for (const char* d : {"fmodex.dll", "fmod_event.dll", "steam_api.dll"})
        copyIfExists(data / d, dst);
}

bool runBackups(const std::string& dataDir, const BackupOptions& opts,
                std::string& message, const InstallProgress& progress) {
    auto step = [&](const std::string& s) { if (progress) progress(s, -1); };
    fs::path data(dataDir);
    std::string gname = data.parent_path().filename().string();
    if (gname.empty()) gname = "Dark Souls PTDE";
    bool packedSrc = false;
    for (int i = 0; i < 4; ++i)
        if (fs::exists(data / ("dvdbnd" + std::to_string(i) + ".bdt"))) { packedSrc = true; break; }

    try {
        if (opts.packed) {
            step("Backing up a packed copy...");
            fs::path dst = fs::path(opts.destRoot) / (gname + " - vanilla (packed)") / "DATA";
            if (packedSrc) {
                copyVanilla(data, dst);
            } else {  // already unpacked: copy the whole folder as-is
                std::error_code ec;
                fs::create_directories(dst, ec);
                fs::copy(data, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            }
        }
        if (opts.unpacked) {
            fs::path dst = fs::path(opts.destRoot) / (gname + " - vanilla (unpacked)") / "DATA";
            if (packedSrc) {
                step("Building a vanilla unpacked copy (this takes a minute)...");
                copyVanilla(data, dst);                  // archives + exe into the backup
                unpackAll(dst.string(), dst.string(), "");
                std::string pm;
                fs::path tmp = dst / "DARKSOULS.exe.tmp";
                if (patchExe((dst / "DARKSOULS.exe").string(), tmp.string(), pm) == PatchResult::Patched) {
                    std::error_code ec;
                    fs::remove(dst / "DARKSOULS.exe", ec);
                    fs::rename(tmp, dst / "DARKSOULS.exe", ec);
                }
                std::error_code ec;
                for (int i = 0; i < 4; ++i) {
                    fs::remove(dst / ("dvdbnd" + std::to_string(i) + ".bhd5"), ec);
                    fs::remove(dst / ("dvdbnd" + std::to_string(i) + ".bdt"), ec);
                }
            } else {
                step("Backing up the unpacked copy...");
                std::error_code ec;
                fs::create_directories(dst, ec);
                fs::copy(data, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            }
        }
        message = "Backup complete.";
        return true;
    } catch (const std::exception& e) {
        message = std::string("backup failed: ") + e.what();
        return false;
    }
}

InstallResult fullInstall(const std::string& dataDir, const std::string& namelistPath,
                          std::string& message, const InstallProgress& progress) {
    auto step = [&](const std::string& s, int pct) { if (progress) progress(s, pct); };

    fs::path data(dataDir);
    if (!fs::exists(data / "DARKSOULS.exe")) {
        message = "no DARKSOULS.exe in " + dataDir;
        return InstallResult::Failed;
    }

    try {
        // 1. Unpack + patch only if the game is still packed.
        if (detectGameState(dataDir) == GameState::Packed) {
            step("Unpacking the game (this takes a minute)...", -1);
            std::string um;
            if (installFlow(dataDir, namelistPath, um) == InstallResult::Failed) {
                message = "unpack failed: " + um;
                return InstallResult::Failed;
            }
        }

        std::string err;

        // 2. Mod payload -> extracted into DATA. The zip is DATA-relative
        //    (loose chr/, map/, ... plus DSFix's DINPUT8.dll/DSfix.ini/dsfix/),
        //    so it must land next to DARKSOULS.exe, not the game root.
        step("Downloading the mod...", 0);
        fs::path modZip = data / "_mctde_mod.zip";
        if (!downloadUrl(kModUrl, modZip.string(), err, dlProgress(progress, "Downloading the mod..."))) {
            message = "mod download failed: " + err;
            return InstallResult::Failed;
        }
        step("Installing the mod...", -1);
        extractZip(modZip.string(), dataDir, err);
        std::error_code ec; fs::remove(modZip, ec);

        // 3. mctde-Link -> DATA.
        step("Downloading mctde-Link...", 0);
        fs::path linkZip = data / "_mctde_link.zip";
        if (!downloadUrl(kLinkUrl, linkZip.string(), err, dlProgress(progress, "Downloading mctde-Link..."))) {
            message = "mctde-Link download failed: " + err;
            return InstallResult::Failed;
        }
        step("Installing mctde-Link...", -1);
        extractZip(linkZip.string(), dataDir, err);
        fs::remove(linkZip, ec);

        // 4. Launcher -> DATA (a single exe, not an archive).
        step("Downloading the launcher...", 0);
        if (!downloadUrl(kLauncherUrl, (data / "mctde_launcher.exe").string(), err,
                         dlProgress(progress, "Downloading the launcher..."))) {
            message = "launcher download failed: " + err;
            return InstallResult::Failed;
        }

        // 5. steam_appid.txt (exactly "480", Spacewar) so the game launches under
        //    the mod's Steam setup. Written last, into the DATA folder.
        {
            std::ofstream sa((data / "steam_appid.txt"), std::ios::binary | std::ios::trunc);
            sa.write("480", 3);
        }

        step("Done!", 100);
        message = "Install complete.";
        return InstallResult::Done;
    } catch (const std::exception& e) {
        message = std::string("install failed: ") + e.what();
        return InstallResult::Failed;
    }
}

} // namespace mctde
