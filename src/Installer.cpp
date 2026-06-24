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

InstallResult fullInstall(const std::string& dataDir, const std::string& namelistPath,
                          std::string& message, const InstallProgress& progress) {
    auto step = [&](const std::string& s, int pct) { if (progress) progress(s, pct); };

    fs::path data(dataDir);
    fs::path gameRoot = data.parent_path();
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

        // 2. Mod payload -> extracted over the game root (its DATA/ merges in).
        step("Downloading the mod...", 0);
        fs::path modZip = data / "_mctde_mod.zip";
        if (!downloadUrl(kModUrl, modZip.string(), err, dlProgress(progress, "Downloading the mod..."))) {
            message = "mod download failed: " + err;
            return InstallResult::Failed;
        }
        step("Installing the mod...", -1);
        extractZip(modZip.string(), gameRoot.string(), err);
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
