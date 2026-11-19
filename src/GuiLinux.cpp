// GuiLinux.cpp: native Linux front-end for the mctde installer (Dear ImGui +
// SDL3). Mirrors the Win32 GUI in Gui.cpp: Artorias banner, a live list of every
// PTDE install found (Steam libraries first, then a bounded scan), a console-style
// log, a progress bar, and Browse / Refresh / Restore / Cancel / Install.
//
// All the real work (scan, unpack, patch, downloads, backups, restore) is the
// shared code in Detect / Installer / Update; this file is only the UI. Workers
// run on std::threads and talk to the UI thread through a small event queue.
//
#include <SDL3/SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include "Detect.h"
#include "Installer.h"
#include "Update.h"

#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char **environ;
namespace fs = std::filesystem;
using namespace mctde;

// ---- embedded assets (src/linux_assets.S) ----
extern "C" {
extern const unsigned char asset_banner_bmp[], asset_banner_bmp_end[];
extern const unsigned char asset_icon_png[], asset_icon_png_end[];
extern const unsigned char asset_font_ui[], asset_font_ui_end[];
extern const unsigned char asset_font_mono[], asset_font_mono_end[];
}

// ---- palette (same as the Win32 GUI) ----
static ImU32 rgb(int r, int g, int b) { return IM_COL32(r, g, b, 255); }
static ImVec4 rgbv(int r, int g, int b) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.f); }
static const ImU32 CLR_BAR    = IM_COL32(150, 42, 42, 255);
static const ImU32 CLR_BARBG  = IM_COL32(46, 46, 54, 255);
static const ImU32 CLR_BARBUSY= IM_COL32(74, 52, 52, 255);
static const ImU32 CLR_FRAME  = IM_COL32(92, 92, 104, 255);
static const ImVec4 CLR_TEXT  = rgbv(228, 228, 230);
static const ImVec4 CLR_DIM   = rgbv(150, 150, 156);
static const ImVec4 CLR_STEAM = rgbv(120, 200, 120);
static const ImVec4 CLR_READY = rgbv(110, 160, 220);
static const ImVec4 CLR_MCTDE = rgbv(230, 150, 70);
static const ImVec4 CLR_UNKNOWN = rgbv(210, 80, 80);

static const float WIN_W = 560, WIN_H = 560, BANNER_H = 115;
static const float LOG_TOP = BANNER_H + 188;

// ---- events from worker threads to the UI thread ----
struct Event {
    enum Type { LogLine, Found, ScanDone, Progress, Done, RestoreDone,
                UpdateAvail, UpdateNone, UpdateDone, Browsed } type;
    int pct = -1;
    bool ok = false;
    std::string text;
    GameInstall gi{};
};
static std::mutex g_qm;
static std::vector<Event> g_queue;
static Uint32 g_wakeEvent = 0;

static void post(Event e) {
    { std::lock_guard<std::mutex> lk(g_qm); g_queue.push_back(std::move(e)); }
    SDL_Event ev{};                 // wake the UI loop out of SDL_WaitEventTimeout
    ev.type = g_wakeEvent;
    SDL_PushEvent(&ev);
}
static void postText(Event::Type t, const std::string& s, int pct = -1, bool ok = false) {
    Event e; e.type = t; e.text = s; e.pct = pct; e.ok = ok; post(std::move(e));
}

// ---- UI state (UI thread only) ----
static SDL_Window* g_window = nullptr;
static std::vector<GameInstall> g_installs;
static int g_sel = -1;
static std::deque<std::string> g_log;
static bool g_logScroll = false;
static std::string g_lastLog;
static int  g_pct = -1;
static bool g_scanning = false, g_installing = false, g_restoring = false,
            g_updating = false, g_done = false, g_checkingUpdate = true;
static bool g_quit = false;
static std::string g_selDir, g_restoreBackup, g_latest;

enum class Modal { None, Backup, Restore, NoBackups, Unknown, Update, Message };
static Modal g_modal = Modal::None;
static bool g_modalOpenPending = false;
static bool g_modalPacked = false;
static int  g_modalRadio = 0;
static std::vector<BackupInfo> g_modalBackups;
static std::string g_msgTitle, g_msgText;

static void openModal(Modal m) { g_modal = m; g_modalOpenPending = true; g_modalRadio = 0; }
static void message(const std::string& title, const std::string& text) {
    g_msgTitle = title; g_msgText = text; openModal(Modal::Message);
}

static void logAppend(const std::string& line) {
    g_log.push_back(line);
    while (g_log.size() > 2000) g_log.pop_front();   // keep it bounded (unpack streams thousands)
    g_logScroll = true;
}

// ---- install classification (same rules as the Win32 GUI) ----
static bool isSteamInstallPath(const std::string& dir) {
    std::string p = dir;
    for (char& c : p) { if (c == '\\') c = '/'; c = (char)std::tolower((unsigned char)c); }
    static const std::string suffix = "/steamapps/common/dark souls prepare to die edition/data";
    return p.size() >= suffix.size() &&
           p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0;
}
static void installTag(const GameInstall& gi, const char*& label, ImVec4& color) {
    bool canon = isSteamInstallPath(gi.dataDir);
    switch (gi.kind) {
        case InstallKind::Mctde:   label = "mctde"; color = canon ? CLR_STEAM : CLR_MCTDE; break;
        case InstallKind::Unknown: label = "Unknown"; color = CLR_UNKNOWN; break;
        case InstallKind::Ready:
        default: label = canon ? "Steam" : "Ready"; color = canon ? CLR_STEAM : CLR_READY; break;
    }
}
static int installRank(const GameInstall& gi) {
    if (isSteamInstallPath(gi.dataDir)) return 0;
    switch (gi.kind) {
        case InstallKind::Ready:   return 1;
        case InstallKind::Mctde:   return 2;
        case InstallKind::Unknown: return 3;
    }
    return 3;
}

static void addInstall(const GameInstall& gi) {
    for (const auto& e : g_installs) if (e.dataDir == gi.dataDir) return;
    g_installs.push_back(gi);
    if (g_installs.size() == 1) g_sel = 0;
}
static void sortInstalls() {
    std::string selPath = (g_sel >= 0 && g_sel < (int)g_installs.size()) ? g_installs[g_sel].dataDir : "";
    std::stable_sort(g_installs.begin(), g_installs.end(), [](const GameInstall& a, const GameInstall& b) {
        int ra = installRank(a), rb = installRank(b);
        return ra != rb ? ra < rb : a.dataDir < b.dataDir;
    });
    g_sel = g_installs.empty() ? -1 : 0;
    for (size_t i = 0; i < g_installs.size(); ++i)
        if (g_installs[i].dataDir == selPath) g_sel = (int)i;
}

// ---- workers ----
static void startScan() {
    if (g_scanning || g_installing) return;
    g_installs.clear();
    g_sel = -1;
    g_scanning = true;
    logAppend("Scanning for Dark Souls installs...");
    std::thread([] {
        for (const std::string& line : detectDiagnostics()) postText(Event::LogLine, line);
        findAllDataDirs([](const GameInstall& gi) {
            Event e; e.type = Event::Found; e.gi = gi; post(std::move(e));
        });
        postText(Event::ScanDone, "");
    }).detach();
}

static void startInstall(bool backupPacked, bool backupUnpacked) {
    if (g_sel < 0 || g_sel >= (int)g_installs.size()) return;
    g_selDir = g_installs[g_sel].dataDir;
    g_installing = true;
    g_done = false;
    g_pct = -1;
    std::string dir = g_selDir;
    std::thread([dir, backupPacked, backupUnpacked] {
        std::string msg;
        InstallResult r = fullInstall(dir, "", msg,   // "" -> embedded namelist
            [](const std::string& stage, int pct) { postText(Event::Progress, stage, pct); },
            backupPacked, backupUnpacked);
        postText(Event::Done, msg, -1, r != InstallResult::Failed);
    }).detach();
}

static void startRestore() {
    g_restoring = g_installing = true;
    g_done = false;
    g_pct = -1;
    std::string backup = g_restoreBackup, dir = g_selDir;
    logAppend("Restoring from " + backup + "...");
    std::thread([backup, dir] {
        std::string msg;
        InstallResult r = restoreFromBackup(backup, dir, msg,
            [](const std::string& stage, int pct) { postText(Event::Progress, stage, pct); });
        postText(Event::RestoreDone, msg, -1, r != InstallResult::Failed);
    }).detach();
}

static void startSelfUpdate() {
    g_updating = true;
    g_pct = -1;
    logAppend("Downloading the latest installer...");
    std::thread([] {
        std::string err;
        bool ok = selfUpdate(err, [](uint64_t got, uint64_t total) -> bool {
            postText(Event::Progress, "Downloading installer update...", total ? (int)(got * 100 / total) : -1);
            return true;
        });
        postText(Event::UpdateDone, ok ? "Update ready. Restarting the installer..."
                                       : "Update failed (" + err + "). Continuing with this version.",
                 -1, ok);
    }).detach();
}

// The DATA folder inside `dir` if the user picked the game root instead.
static bool hasGameExe(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().string();
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == "darksouls.exe") return true;
    }
    return false;
}

static void SDLCALL onFolderPicked(void*, const char* const* files, int) {
    if (!files) { postText(Event::LogLine, std::string("Folder picker failed: ") + SDL_GetError()); return; }
    if (!files[0]) return;   // cancelled
    postText(Event::Browsed, files[0]);
}

static void browse() {
    const char* home = SDL_getenv("HOME");
    SDL_ShowOpenFolderDialog(onFolderPicked, nullptr, g_window, home, false);
}

static void handleBrowsed(const std::string& picked) {
    fs::path d(picked);
    if (!hasGameExe(d)) {
        if (hasGameExe(d / "DATA")) d /= "DATA";
        else { message("No DARKSOULS.exe", "That folder doesn't contain DARKSOULS.exe.\n\nPick the DATA folder of your Dark Souls install."); return; }
    }
    std::string s = d.string();
    std::string low = s;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    GameState st = detectGameState(s);
    addInstall(GameInstall{s, low.find("steamapps") != std::string::npos, st, classifyInstall(s, st)});
    for (size_t i = 0; i < g_installs.size(); ++i) if (g_installs[i].dataDir == s) g_sel = (int)i;
    sortInstalls();
    logAppend("Added: " + s);
}

// Launch the Steam copy through Steam (Proton), which is how the game runs on
// Linux; mctde-Link then hands over to the mctde launcher.
static void launchViaSteam() {
    const char* argv[] = {"xdg-open", "steam://rungameid/211420", nullptr};
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, (char* const*)argv, environ) == 0) {
        std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    }
}

// ---- event handling (UI thread) ----
static void drainEvents() {
    std::vector<Event> evs;
    { std::lock_guard<std::mutex> lk(g_qm); evs.swap(g_queue); }
    for (Event& e : evs) {
        switch (e.type) {
        case Event::LogLine: logAppend(e.text); break;
        case Event::Found: {
            addInstall(e.gi);
            const GameInstall& gi = e.gi;
            const char* st = gi.state == GameState::Packed ? "packed" : gi.state == GameState::Unpacked ? "unpacked" : "unknown";
            const char* kind = gi.kind == InstallKind::Mctde ? "mctde " : gi.kind == InstallKind::Unknown ? "Unknown " : "Ready ";
            logAppend(std::string("Found [") + (gi.steam ? "Steam" : "non-Steam") + ", " + kind + st + "]: " + gi.dataDir);
            break;
        }
        case Event::ScanDone:
            g_scanning = false;
            sortInstalls();
            logAppend(g_installs.empty()
                ? "No Dark Souls install found. Click Browse to locate DARKSOULS.exe."
                : "Found " + std::to_string(g_installs.size()) + " install(s). Pick one and click Install.");
            break;
        case Event::Progress:
            g_pct = e.pct;
            if (e.text != g_lastLog) { logAppend(e.text); g_lastLog = e.text; }
            break;
        case Event::Done:
            g_installing = false;
            logAppend(e.text); g_lastLog = e.text;
            g_pct = e.ok ? 100 : -1;
            g_done = e.ok;
            if (e.ok) {
                if (isSteamInstallPath(g_selDir)) {
                    logAppend("Click Play to start Dark Souls through Steam (Proton).");
                } else {
                    logAppend("To play this copy: in Steam, add mctde_launcher.exe from this DATA folder as a");
                    logAppend("non-Steam game and turn on Proton under Properties > Compatibility.");
                }
            }
            break;
        case Event::RestoreDone:
            g_restoring = g_installing = false;
            logAppend(e.text); g_lastLog = e.text;
            g_pct = e.ok ? 100 : -1;
            // The restored copy's state changed: refresh its row.
            for (GameInstall& gi : g_installs)
                if (gi.dataDir == g_selDir) { gi.state = detectGameState(gi.dataDir); gi.kind = classifyInstall(gi.dataDir, gi.state); }
            break;
        case Event::UpdateAvail:
            g_checkingUpdate = false;
            g_latest = e.text;
            logAppend(std::string("Installer update available: v") + kInstallerVersion + " -> v" + g_latest);
            openModal(Modal::Update);
            break;
        case Event::UpdateNone:
            g_checkingUpdate = false;
            startScan();
            break;
        case Event::UpdateDone:
            g_updating = false;
            logAppend(e.text); g_lastLog = e.text;
            if (e.ok) g_quit = true;           // the new AppImage is taking over
            else { g_pct = -1; startScan(); }
            break;
        case Event::Browsed: handleBrowsed(e.text); break;
        }
    }
}

// ---- drawing ----
// Shorten a path from the left ("...tail") to fit `width` pixels.
static std::string fitPath(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    size_t cut = 0;
    while (cut < s.size()) {
        std::string t = "..." + s.substr(cut);
        if (ImGui::CalcTextSize(t.c_str()).x <= width) return t;
        ++cut;
    }
    return "...";
}

static bool button(const char* label, float x, float y, float w, float h, bool enabled, float s) {
    ImGui::SetCursorPos(ImVec2(x * s, y * s));
    if (!enabled) ImGui::BeginDisabled();
    bool pressed = ImGui::Button(label, ImVec2(w * s, h * s));
    if (!enabled) ImGui::EndDisabled();
    return pressed && enabled;
}

static void drawModals(float s) {
    if (g_modalOpenPending) {
        const char* id = g_modal == Modal::Backup ? "Back up before installing?"
                       : g_modal == Modal::Restore ? "Restore a backup?"
                       : g_modal == Modal::NoBackups ? "No backups"
                       : g_modal == Modal::Unknown ? "This copy looks modified"
                       : g_modal == Modal::Update ? "A newer installer is available"
                       : "mctde Installer";
        ImGui::OpenPopup(id);
        g_modalOpenPending = false;
    }
    ImGuiWindowFlags mf = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;
    ImVec2 center(WIN_W * s * 0.5f, WIN_H * s * 0.5f);
    auto beginModal = [&](const char* id) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(420 * s, 0), ImVec2(500 * s, 520 * s));
        return ImGui::BeginPopupModal(id, nullptr, mf);
    };
    const float wrap = 460 * s;

    if (beginModal("Back up before installing?")) {
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted(g_modalPacked
            ? "Installing will unpack and modify this copy of Dark Souls. Keep a vanilla backup first? It's saved right next to the DATA folder."
            : "Installing will modify this copy of Dark Souls. Keep a vanilla backup of it first? It's saved as DATA-Backup-Unpacked next to the DATA folder.");
        ImGui::PopTextWrapPos();
        if (g_modalPacked) {
            ImGui::Spacing();
            ImGui::RadioButton("Packed copy  (DATA-Backup-Packed - the original archives)", &g_modalRadio, 0);
            ImGui::RadioButton("Unpacked copy  (DATA-Backup-Unpacked - vanilla, ready for other mods)", &g_modalRadio, 1);
            ImGui::RadioButton("Both", &g_modalRadio, 2);
        }
        ImGui::Spacing();
        if (ImGui::Button("Back up, then install")) {
            bool p = g_modalPacked && (g_modalRadio == 0 || g_modalRadio == 2);
            bool u = !g_modalPacked || g_modalRadio == 1 || g_modalRadio == 2;
            ImGui::CloseCurrentPopup(); g_modal = Modal::None;
            startInstall(p, u);
        }
        ImGui::SameLine();
        if (ImGui::Button("Install without backing up")) {
            ImGui::CloseCurrentPopup(); g_modal = Modal::None;
            startInstall(false, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); g_modal = Modal::None; }
        ImGui::EndPopup();
    }

    if (beginModal("Restore a backup?")) {
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted("This replaces the current DATA folder with the selected backup. "
                               "Your current files (including the mod) are discarded. The backup itself is kept.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        for (size_t i = 0; i < g_modalBackups.size(); ++i) {
            const BackupInfo& b = g_modalBackups[i];
            const char* st = b.state == GameState::Packed ? "packed - original archives, full vanilla restore"
                           : b.state == GameState::Unpacked ? "unpacked - loose files" : "unknown";
            std::string label = b.name + "   (" + st + ")";
            ImGui::RadioButton(label.c_str(), &g_modalRadio, (int)i);
        }
        ImGui::Spacing();
        if (ImGui::Button("Restore")) {
            if (g_modalRadio >= 0 && g_modalRadio < (int)g_modalBackups.size()) {
                g_restoreBackup = g_modalBackups[g_modalRadio].path;
                ImGui::CloseCurrentPopup(); g_modal = Modal::None;
                startRestore();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); g_modal = Modal::None; }
        ImGui::EndPopup();
    }

    if (beginModal("No backups")) {
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted("No backups were found next to this DATA folder.\n\n"
                               "Backups are created during install (DATA-Backup-Packed / DATA-Backup-Unpacked).");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK")) { ImGui::CloseCurrentPopup(); g_modal = Modal::None; }
        ImGui::EndPopup();
    }

    if (beginModal("This copy looks modified")) {
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted("Cracked, pre-patch, or heavily modified versions are not compatible with mctde.\n\n"
                               "Please use a clean (vanilla) copy of PTDE.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK")) { ImGui::CloseCurrentPopup(); g_modal = Modal::None; }
        ImGui::EndPopup();
    }

    if (beginModal("A newer installer is available")) {
        ImGui::PushTextWrapPos(wrap);
        std::string t = std::string("You're running installer v") + kInstallerVersion + ", but v" + g_latest +
            " is available.\n\nUpdating makes sure you install the latest mctde-Link. "
            "The installer will download the new version and restart itself.";
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Yes")) { ImGui::CloseCurrentPopup(); g_modal = Modal::None; startSelfUpdate(); }
        ImGui::SameLine();
        if (ImGui::Button("No")) {
            ImGui::CloseCurrentPopup(); g_modal = Modal::None;
            logAppend("Skipped the update; using this installer.");
            startScan();
        }
        ImGui::EndPopup();
    }

    if (beginModal("mctde Installer")) {
        ImGui::TextUnformatted(g_msgTitle.c_str());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted(g_msgText.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK")) { ImGui::CloseCurrentPopup(); g_modal = Modal::None; }
        ImGui::EndPopup();
    }
}

static void drawUI(SDL_Texture* banner, ImFont* mono, float s) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(WIN_W * s, WIN_H * s));
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (banner) dl->AddImage((ImTextureID)(intptr_t)banner, ImVec2(0, 0), ImVec2(WIN_W * s, BANNER_H * s));

    ImGui::SetCursorPos(ImVec2(24 * s, (BANNER_H + 8) * s));
    ImGui::TextColored(CLR_DIM, "Select the install to patch:");

    const bool busy = g_installing || g_updating;
    const bool modalUp = g_modal != Modal::None;

    // Install list.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * s, 4 * s));
    ImGui::SetCursorPos(ImVec2(24 * s, (BANNER_H + 28) * s));
    ImGui::BeginChild("list", ImVec2((WIN_W - 48) * s, 150 * s), ImGuiChildFlags_Borders);
    if (busy || g_done) ImGui::BeginDisabled();
    for (size_t i = 0; i < g_installs.size(); ++i) {
        const GameInstall& gi = g_installs[i];
        ImGui::PushID((int)i);
        float rowW = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", g_sel == (int)i, 0, ImVec2(rowW, 22 * s))) g_sel = (int)i;
        // right-side tags
        const char* stateText = gi.state == GameState::Packed ? "packed" : gi.state == GameState::Unpacked ? "unpacked" : "";
        const char* tag = nullptr; ImVec4 tagColor = CLR_DIM;
        installTag(gi, tag, tagColor);
        float right = p.x + rowW - 8 * s;
        float ty = p.y + (22 * s - ImGui::GetTextLineHeight()) * 0.5f;
        if (stateText[0]) {
            float w = ImGui::CalcTextSize(stateText).x;
            dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(right - w, ty), ImGui::ColorConvertFloat4ToU32(CLR_DIM), stateText);
            right -= w + 12 * s;
        }
        if (tag) {
            float w = ImGui::CalcTextSize(tag).x;
            ImGui::GetWindowDrawList()->AddText(ImVec2(right - w, ty), ImGui::ColorConvertFloat4ToU32(tagColor), tag);
            right -= w + 12 * s;
        }
        std::string path = fitPath(gi.dataDir, right - (p.x + 8 * s) - 6 * s);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 8 * s, ty), ImGui::ColorConvertFloat4ToU32(CLR_TEXT), path.c_str());
        ImGui::PopID();
    }
    if (busy || g_done) ImGui::EndDisabled();
    ImGui::EndChild();

    // Log.
    ImGui::SetCursorPos(ImVec2(24 * s, LOG_TOP * s));
    ImGui::PushFont(mono);
    ImGui::BeginChild("log", ImVec2((WIN_W - 48) * s, ((WIN_H - 80) - LOG_TOP) * s), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGuiListClipper clip;
    clip.Begin((int)g_log.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(g_log[i].c_str());
    if (g_logScroll && atBottom) ImGui::SetScrollHereY(1.0f);
    g_logScroll = false;
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleVar();

    // Progress bar.
    {
        ImVec2 o = ImGui::GetWindowPos();
        ImVec2 a(o.x + 24 * s, o.y + (WIN_H - 72) * s), b(o.x + (WIN_W - 24) * s, o.y + (WIN_H - 56) * s);
        dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, CLR_BARBG);
        if (g_pct >= 0) dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * (g_pct / 100.f), b.y), CLR_BAR);
        else if (busy) dl->AddRectFilled(a, b, CLR_BARBUSY);
        dl->AddRect(a, b, CLR_FRAME);
    }

    // Buttons.
    const bool idle = !busy && !g_scanning && !g_checkingUpdate && !modalUp;
    const bool hasSel = g_sel >= 0 && g_sel < (int)g_installs.size();
    const float by = WIN_H - 50;
    if (button("Browse...", 24, by, 100, 34, !busy && !g_checkingUpdate && !modalUp && !g_done, s)) browse();
    if (button("Refresh", 130, by, 100, 34, idle && !g_done, s)) startScan();
    if (button(g_restoring ? "Restoring..." : "Restore", 236, by, 94, 34, idle && hasSel, s)) {
        g_selDir = g_installs[g_sel].dataDir;
        g_modalBackups = findBackups(g_selDir);
        openModal(g_modalBackups.empty() ? Modal::NoBackups : Modal::Restore);
    }
    if (button(g_done ? "Close" : "Cancel", WIN_W - 224, by, 90, 34, !busy, s)) g_quit = true;
    const bool canPlay = g_done && isSteamInstallPath(g_selDir);
    const char* instLabel = g_installing && !g_restoring ? "Installing..." : (g_done ? (canPlay ? "Play" : "Installed") : "Install");
    bool instEnabled = g_done ? canPlay : (!busy && !modalUp && !g_checkingUpdate && hasSel);
    if (button(instLabel, WIN_W - 124, by, 100, 34, instEnabled, s)) {
        if (g_done) { launchViaSteam(); g_quit = true; }
        else if (g_installs[g_sel].kind == InstallKind::Unknown) openModal(Modal::Unknown);
        else { g_modalPacked = g_installs[g_sel].state == GameState::Packed; openModal(Modal::Backup); }
    }

    drawModals(s);
    ImGui::End();
}

static void applyStyle(float s) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0; st.FrameRounding = 0; st.ChildRounding = 0; st.PopupRounding = 2;
    st.WindowBorderSize = 0; st.ChildBorderSize = 1; st.FrameBorderSize = 1;
    st.WindowPadding = ImVec2(0, 0);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]       = rgbv(24, 24, 28);
    c[ImGuiCol_ChildBg]        = rgbv(34, 34, 40);
    c[ImGuiCol_PopupBg]        = rgbv(34, 34, 40);
    c[ImGuiCol_Border]         = rgbv(92, 92, 104);
    c[ImGuiCol_Text]           = CLR_TEXT;
    c[ImGuiCol_TextDisabled]   = rgbv(120, 120, 124);
    c[ImGuiCol_Button]         = rgbv(56, 56, 64);
    c[ImGuiCol_ButtonHovered]  = rgbv(70, 70, 80);
    c[ImGuiCol_ButtonActive]   = rgbv(82, 82, 94);
    c[ImGuiCol_Header]         = rgbv(62, 62, 78);
    c[ImGuiCol_HeaderHovered]  = rgbv(50, 50, 62);
    c[ImGuiCol_HeaderActive]   = rgbv(62, 62, 78);
    c[ImGuiCol_FrameBg]        = rgbv(46, 46, 54);
    c[ImGuiCol_CheckMark]      = rgbv(150, 42, 42);
    c[ImGuiCol_TitleBgActive]  = rgbv(46, 46, 54);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    st.ScaleAllSizes(s);
    st.WindowPadding = ImVec2(0, 0);   // main window: absolute layout
}

int main(int argc, char** argv) {
    // With arguments, act as the command-line tool (bundled next to us in the
    // AppImage): `mctde-Installer-x86_64.AppImage detectall` etc.
    if (argc > 1) {
        std::error_code ec;
        fs::path cli = fs::read_symlink("/proc/self/exe", ec).parent_path() / "mctde-installer";
        if (!ec) {
            argv[0] = (char*)"mctde-installer";
            execv(cli.c_str(), argv);
        }
        SDL_Log("could not start %s", cli.c_str());
        return 1;
    }
    SDL_SetAppMetadata("mctde Installer", kInstallerVersion, "mctde-installer");
    SDL_SetHint(SDL_HINT_APP_ID, "mctde-installer");   // Wayland app id -> .desktop icon
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    g_wakeEvent = SDL_RegisterEvents(1);

    float s = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (s < 1.0f) s = 1.0f;
    g_window = SDL_CreateWindow("mctde Installer", (int)(WIN_W * s), (int)(WIN_H * s), 0);
    if (!g_window) { SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError()); return 1; }
    SDL_Renderer* ren = SDL_CreateRenderer(g_window, nullptr);
    if (!ren) { SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);

    if (SDL_Surface* icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(asset_icon_png, asset_icon_png_end - asset_icon_png), true)) {
        SDL_SetWindowIcon(g_window, icon);
        SDL_DestroySurface(icon);
    }
    SDL_Texture* banner = nullptr;
    if (SDL_Surface* bmp = SDL_LoadBMP_IO(SDL_IOFromConstMem(asset_banner_bmp, asset_banner_bmp_end - asset_banner_bmp), true)) {
        banner = SDL_CreateTextureFromSurface(ren, bmp);
        SDL_DestroySurface(bmp);
        if (banner) SDL_SetTextureScaleMode(banner, SDL_SCALEMODE_LINEAR);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // no imgui.ini next to the user's files
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;   // embedded data, not heap memory
    io.Fonts->AddFontFromMemoryTTF((void*)asset_font_ui, (int)(asset_font_ui_end - asset_font_ui), 15.0f * s, &fc);
    ImFont* mono = io.Fonts->AddFontFromMemoryTTF((void*)asset_font_mono, (int)(asset_font_mono_end - asset_font_mono), 13.0f * s, &fc);
    applyStyle(s);
    ImGui_ImplSDL3_InitForSDLRenderer(g_window, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    cleanupOldSelf();
    logAppend(std::string("mctde Installer v") + kInstallerVersion + " (native Linux)");
    logAppend("Checking for installer updates...");
    std::thread([] {
        std::string latest = fetchLatestInstallerVersion();
        if (!latest.empty() && isNewer(latest, kInstallerVersion)) postText(Event::UpdateAvail, latest);
        else postText(Event::UpdateNone, "");
    }).detach();

    // Debug aid: MCTDE_SCREENSHOT=<file.bmp> saves the window after
    // MCTDE_SCREENSHOT_AFTER seconds (default 8); add MCTDE_SCREENSHOT_QUIT=1 to exit.
    const char* shotPath = SDL_getenv("MCTDE_SCREENSHOT");
    const char* shotAfter = SDL_getenv("MCTDE_SCREENSHOT_AFTER");
    const Uint64 shotAt = SDL_GetTicks() + (Uint64)((shotAfter ? SDL_atof(shotAfter) : 8.0) * 1000);
    bool shotDone = false;

    while (!g_quit) {
        SDL_Event ev;
        // Sleep until input or a worker event; tick at ~10 fps otherwise so
        // hover states and the indeterminate bar stay live without burning CPU.
        if (SDL_WaitEventTimeout(&ev, 100)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&ev);
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    if (!g_installing && !g_updating) g_quit = true;   // never abandon an install midway
                }
            } while (SDL_PollEvent(&ev));
        }
        drainEvents();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawUI(banner, mono, s);
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 24, 24, 28, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (shotPath && !shotDone && SDL_GetTicks() >= shotAt) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr)) {
                SDL_SaveBMP(shot, shotPath);
                SDL_DestroySurface(shot);
            }
            shotDone = true;
            if (SDL_getenv("MCTDE_SCREENSHOT_QUIT") && !g_installing && !g_updating) g_quit = true;
        }
        SDL_RenderPresent(ren);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (banner) SDL_DestroyTexture(banner);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
