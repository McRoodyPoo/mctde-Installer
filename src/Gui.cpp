// Gui.cpp — native Win32 front-end for the mctde installer.
//
// Dark window with the Artorias banner. On launch it scans for every Dark Souls
// PTDE install (Steam libraries first, then a bounded drive scan) and lists them
// live as they're found, tagging Steam copies. The user picks one; Install runs
// the full flow on that copy via fullInstall() on a worker thread.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <cctype>
#include <string>
#include <vector>

#include <cstdint>

#include "gui_resource.h"
#include "Detect.h"
#include "Installer.h"
#include "Update.h"

using namespace mctde;

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

// ---- control / message ids ----
#define IDC_LIST      1000
#define IDC_INSTALL   1001
#define IDC_CANCEL    1002
#define IDC_BROWSE    1003
#define IDC_LOG       1004
#define IDC_REFRESH   1005
#define WM_PROGRESS   (WM_APP + 1)   // wParam = pct (-1 indeterminate), lParam = new wchar_t[] status
#define WM_DONE       (WM_APP + 2)   // wParam = success?1:0, lParam = new wchar_t[] message
#define WM_FOUND      (WM_APP + 3)   // lParam = new GameInstall*
#define WM_SCANDONE   (WM_APP + 4)
#define WM_UPDATE_AVAIL (WM_APP + 5) // lParam = new wchar_t[] latest version
#define WM_UPDATE_NONE  (WM_APP + 6) // no update (or check failed) -> just scan
#define WM_UPDATE_DONE  (WM_APP + 7) // wParam = ok?1:0, lParam = new wchar_t[] message

// ---- palette ----
static const COLORREF CLR_BG     = RGB(24, 24, 28);
static const COLORREF CLR_TEXT   = RGB(228, 228, 230);
static const COLORREF CLR_DIM    = RGB(150, 150, 156);
static const COLORREF CLR_BTN    = RGB(56, 56, 64);
static const COLORREF CLR_BTNHI  = RGB(82, 82, 94);
static const COLORREF CLR_BTNDIS = RGB(40, 40, 46);
static const COLORREF CLR_BAR    = RGB(150, 42, 42);
static const COLORREF CLR_BARBG  = RGB(46, 46, 54);
static const COLORREF CLR_FRAME  = RGB(92, 92, 104);
static const COLORREF CLR_LIST   = RGB(34, 34, 40);
static const COLORREF CLR_LISTSEL= RGB(62, 62, 78);
static const COLORREF CLR_STEAM  = RGB(120, 200, 120);   // green-ish "Steam" tag

static const int WIN_W = 560, WIN_H = 560, BANNER_H = 115;
static const int LOG_TOP = BANNER_H + 188;   // scrolling log spans LOG_TOP .. WIN_H-80

// ---- state ----
static HINSTANCE g_inst = nullptr;
static HBITMAP   g_banner = nullptr;
static HFONT     g_font = nullptr, g_fontLog = nullptr;
static HWND      g_hwnd = nullptr, g_list = nullptr, g_log = nullptr;
static HWND      g_btnInstall = nullptr, g_btnCancel = nullptr, g_btnBrowse = nullptr, g_btnRefresh = nullptr;
static HBRUSH    g_listBrush = nullptr;   // dark fill for the listbox / log
static int       g_pct = -1;
static std::wstring g_lastLog;            // last line appended (dedup repeated progress msgs)
static volatile bool g_installing = false;
static volatile bool g_scanning = false;
static volatile bool g_updating = false;   // self-update download in progress
static bool      g_done = false;
static std::vector<GameInstall> g_installs;
static std::string g_selDir;   // the install the worker is operating on
static bool g_backupPacked = false;
static bool g_backupUnpacked = false;

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wchar_t* dupw(const std::wstring& s) {
    wchar_t* p = new wchar_t[s.size() + 1];
    wmemcpy(p, s.c_str(), s.size() + 1);
    return p;
}
static void postStatus(int pct, const std::wstring& s) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_PROGRESS, (WPARAM)pct, (LPARAM)dupw(s));
}
// Append one line to the scrolling log and keep the newest line in view.
// Must be called on the UI thread (it talks to the edit control directly).
static void logAppend(const std::wstring& line) {
    if (!g_log) return;
    // Keep the control bounded: a full unpack streams thousands of lines, so
    // drop the oldest once it grows large (keeps scrolling snappy).
    if (SendMessageW(g_log, EM_GETLINECOUNT, 0, 0) > 800) {
        LRESULT cut = SendMessageW(g_log, EM_LINEINDEX, 400, 0);  // first char of line 400
        if (cut > 0) {
            SendMessageW(g_log, EM_SETSEL, 0, (LPARAM)cut);
            SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
        }
    }
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring s = line + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
    SendMessageW(g_log, WM_VSCROLL, SB_BOTTOM, 0);
}

// ---- worker threads ----
static DWORD WINAPI ScanThread(LPVOID) {
    findAllDataDirs([](const GameInstall& gi) {
        if (g_hwnd) PostMessageW(g_hwnd, WM_FOUND, 0, (LPARAM)new GameInstall(gi));
    });
    if (g_hwnd) PostMessageW(g_hwnd, WM_SCANDONE, 0, 0);
    return 0;
}

// Check latest.txt for a newer installer; the UI thread decides what to do next.
static DWORD WINAPI UpdateCheckThread(LPVOID) {
    std::string latest = fetchLatestInstallerVersion();
    if (!latest.empty() && isNewer(latest, kInstallerVersion))
        PostMessageW(g_hwnd, WM_UPDATE_AVAIL, 0, (LPARAM)dupw(widen(latest)));
    else
        PostMessageW(g_hwnd, WM_UPDATE_NONE, 0, 0);
    return 0;
}

// Download the new installer, swap it in, and relaunch it. On success the UI
// thread closes this (now-stale) instance.
static DWORD WINAPI SelfUpdateThread(LPVOID) {
    std::string err;
    bool ok = selfUpdate(err, [](uint64_t got, uint64_t total) -> bool {
        postStatus(total ? (int)(got * 100 / total) : -1, L"Downloading installer update...");
        return true;
    });
    PostMessageW(g_hwnd, WM_UPDATE_DONE, (WPARAM)(ok ? 1 : 0),
                 (LPARAM)dupw(ok ? L"Update ready. Restarting the installer..."
                                 : widen("Update failed (" + err + "). Continuing with this version.")));
    return 0;
}

static DWORD WINAPI InstallThread(LPVOID) {
    std::string msg;
    // Backups (DATA-Backup-Packed before unpack, DATA-Backup-Unpacked after) are
    // handled inside fullInstall at the right moments; a backup failure aborts.
    std::string finalDir;
    InstallResult r = fullInstall(g_selDir, "", msg,   // "" -> embedded namelist
        [](const std::string& stage, int pct) { postStatus(pct, widen(stage)); },
        g_backupPacked, g_backupUnpacked, &finalDir);
    // The install renames the folder to DATA-mctde; follow it so the Play button
    // and the shortcuts point at where the game actually ended up.
    if (r != InstallResult::Failed && !finalDir.empty()) g_selDir = finalDir;
    PostMessageW(g_hwnd, WM_DONE, (WPARAM)(r == InstallResult::Failed ? 0 : 1), (LPARAM)dupw(widen(msg)));
    g_installing = false;
    return 0;
}

static std::string browseForData(HWND owner) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select a Dark Souls DATA folder (the one with DARKSOULS.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return "";
    wchar_t path[MAX_PATH] = {0};
    BOOL got = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!got) return "";
    std::string s = narrow(path);
    if (!s.empty() && GetFileAttributesA((s + "\\DARKSOULS.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        return s;
    MessageBoxW(owner, L"That folder doesn't contain DARKSOULS.exe.", L"mctde Installer",
                MB_OK | MB_ICONWARNING);
    return "";
}

enum class BackupChoice { Cancel, NoBackup, BackUp };

// Warn and ask whether to back up first. Packed installs get packed/unpacked/both.
static BackupChoice showBackupChoice(HWND owner, bool packed, bool& outPacked, bool& outUnpacked) {
    outPacked = outUnpacked = false;
    TASKDIALOGCONFIG tc = {0};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_COMMAND_LINKS;
    tc.pszWindowTitle = L"mctde Installer";
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = L"Back up before installing?";
    tc.pszContent = packed
        ? L"Installing will unpack and modify this copy of Dark Souls. Keep a vanilla "
          L"backup first? It's saved right next to the DATA folder."
        : L"Installing will modify this copy of Dark Souls. Keep a vanilla backup of "
          L"it first? It's saved as DATA-Backup-Unpacked next to the DATA folder.";

    TASKDIALOG_BUTTON btns[2] = {
        {101, L"Back up, then install\nThe backup is saved next to your DATA folder"},
        {102, L"Install without backing up"},
    };
    tc.pButtons = btns;
    tc.cButtons = 2;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.nDefaultButton = 101;

    TASKDIALOG_BUTTON radios[3] = {
        {201, L"Packed copy  (DATA-Backup-Packed - the original archives)"},
        {202, L"Unpacked copy  (DATA-Backup-Unpacked - vanilla, ready for other mods)"},
        {203, L"Both"},
    };
    if (packed) {
        tc.pRadioButtons = radios;
        tc.cRadioButtons = 3;
        tc.nDefaultRadioButton = 201;
    }

    int pressed = 0, radio = 201;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, &radio, nullptr))) return BackupChoice::Cancel;
    if (pressed == 102) return BackupChoice::NoBackup;
    if (pressed != 101) return BackupChoice::Cancel;
    if (packed) {
        outPacked   = (radio == 201 || radio == 203);
        outUnpacked = (radio == 202 || radio == 203);
    } else {
        outUnpacked = true;   // unpacked install -> back up the current copy
    }
    return BackupChoice::BackUp;
}

static void addInstall(const GameInstall& gi) {
    for (const auto& e : g_installs) if (e.dataDir == gi.dataDir) return;  // dedup
    g_installs.push_back(gi);
    SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)widen(gi.dataDir).c_str());
    if (g_installs.size() == 1) {            // auto-select the first
        SendMessageW(g_list, LB_SETCURSEL, 0, 0);
        EnableWindow(g_btnInstall, TRUE);
    }
}

// Clear the current results and (re)launch the scan worker. Used on startup and
// by the Refresh button; ignored while a scan or install is already running.
static void startScan() {
    if (g_scanning || g_installing) return;
    g_installs.clear();
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    EnableWindow(g_btnInstall, FALSE);
    EnableWindow(g_btnRefresh, FALSE);
    EnableWindow(g_btnBrowse, TRUE);   // browsing is available during/after a scan
    g_scanning = true;
    logAppend(L"Scanning for Dark Souls installs...");
    if (HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr)) CloseHandle(t);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void startInstall() {
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_installs.size()) return;
    g_selDir = g_installs[sel].dataDir;
    g_installing = true;
    g_done = false;
    g_pct = -1;
    EnableWindow(g_btnInstall, FALSE);
    EnableWindow(g_btnCancel, FALSE);
    EnableWindow(g_btnBrowse, FALSE);
    EnableWindow(g_btnRefresh, FALSE);
    EnableWindow(g_list, FALSE);
    SetWindowTextW(g_btnInstall, L"Installing...");
    if (HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// The exe a shortcut / Play button should launch: the mctde launcher if present,
// otherwise the bare game exe.
static std::wstring resolveLaunchExe() {
    std::wstring exe = widen(g_selDir) + L"\\mctde_launcher.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        exe = widen(g_selDir) + L"\\DARKSOULS.exe";
    return exe;
}

static void launchAndExit() {
    std::wstring exe = resolveLaunchExe();
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, widen(g_selDir).c_str(), SW_SHOWNORMAL);
    DestroyWindow(g_hwnd);
}

// Resolve a CSIDL folder (e.g. the user's Desktop or Start Menu\Programs).
static std::wstring knownDir(int csidl) {
    wchar_t path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path)))
        return path;
    return L"";
}

// Write a .lnk pointing at targetExe (icon pulled from the exe, working dir set so
// relative game files resolve). Returns false on any COM failure.
static bool createShortcut(const std::wstring& targetExe, const std::wstring& workingDir,
                           const std::wstring& lnkPath, const std::wstring& desc) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)))
        return false;
    link->SetPath(targetExe.c_str());
    link->SetWorkingDirectory(workingDir.c_str());
    link->SetDescription(desc.c_str());
    link->SetIconLocation(targetExe.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

// After a successful install, ask whether to drop a Desktop and/or Start Menu
// shortcut for the freshly patched copy, then create whichever were chosen.
static void offerShortcuts(HWND owner) {
    TASKDIALOGCONFIG tc = {0};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"mctde Installer";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = L"Create shortcuts?";
    tc.pszContent = L"Add a shortcut to launch Dark Souls with mctde-Link.";

    TASKDIALOG_BUTTON radios[4] = {
        {301, L"Desktop and Start Menu"},
        {302, L"Desktop only"},
        {303, L"Start Menu only"},
        {304, L"Don't create shortcuts"},
    };
    tc.pRadioButtons = radios;
    tc.cRadioButtons = 4;
    tc.nDefaultRadioButton = 301;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;

    int pressed = 0, radio = 301;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, &radio, nullptr))) return;
    if (radio == 304) return;

    const bool wantDesktop   = (radio == 301 || radio == 302);
    const bool wantStartMenu = (radio == 301 || radio == 303);
    const std::wstring exe     = resolveLaunchExe();
    const std::wstring workdir = widen(g_selDir);
    const std::wstring name    = L"Dark Souls (mctde-Link).lnk";
    const std::wstring desc    = L"Launch Dark Souls with mctde-Link";

    // We run elevated, so write to the all-users (common) locations: the shortcut
    // shows up for every account, not just the admin who ran the installer.
    int made = 0;
    if (wantDesktop) {
        std::wstring d = knownDir(CSIDL_COMMON_DESKTOPDIRECTORY);
        if (!d.empty() && createShortcut(exe, workdir, d + L"\\" + name, desc)) made++;
    }
    if (wantStartMenu) {
        std::wstring p = knownDir(CSIDL_COMMON_PROGRAMS);
        if (!p.empty() && createShortcut(exe, workdir, p + L"\\" + name, desc)) made++;
    }
    logAppend(made ? L"Created shortcut(s)." : L"Could not create shortcuts.");
}

// Ask whether to pull a newer installer before continuing. Yes is the default.
static bool askInstallerUpdate(HWND owner, const std::wstring& latest) {
    std::wstring content =
        L"You're running installer v" + widen(kInstallerVersion) + L", but v" + latest +
        L" is available.\n\nUpdating makes sure you install the latest mctde-Link. "
        L"The installer will download the new version and restart itself.";
    TASKDIALOGCONFIG tc = {0};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"mctde Installer";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = L"A newer installer is available";
    tc.pszContent = content.c_str();
    tc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    tc.nDefaultButton = IDYES;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) return false;
    return pressed == IDYES;
}

// ---- drawing ----
static void paintButton(DRAWITEMSTRUCT* di) {
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    COLORREF face = disabled ? CLR_BTNDIS : (pressed ? CLR_BTNHI : CLR_BTN);
    HBRUSH fb = CreateSolidBrush(face);
    FillRect(di->hDC, &di->rcItem, fb);
    DeleteObject(fb);
    HBRUSH frame = CreateSolidBrush(CLR_FRAME);
    FrameRect(di->hDC, &di->rcItem, frame);
    DeleteObject(frame);
    wchar_t txt[48] = {0};
    GetWindowTextW(di->hwndItem, txt, 48);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? RGB(120, 120, 124) : CLR_TEXT);
    SelectObject(di->hDC, g_font);
    DrawTextW(di->hDC, txt, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paintListItem(DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1) return;
    const GameInstall& gi = g_installs[di->itemID];
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(sel ? CLR_LISTSEL : CLR_LIST);
    FillRect(di->hDC, &di->rcItem, bg);
    DeleteObject(bg);

    SetBkMode(di->hDC, TRANSPARENT);
    SelectObject(di->hDC, g_font);

    // Right-side tags: "Steam" (if so) and the packed/unpacked state.
    const wchar_t* stateText = gi.state == GameState::Packed ? L"packed"
                             : gi.state == GameState::Unpacked ? L"unpacked" : L"";
    SIZE sz{0, 0};
    GetTextExtentPoint32W(di->hDC, stateText, (int)wcslen(stateText), &sz);
    int right = di->rcItem.right - 10;
    RECT tr = di->rcItem;
    if (stateText[0]) {
        tr.left = right - sz.cx; tr.right = right;
        SetTextColor(di->hDC, CLR_DIM);
        DrawTextW(di->hDC, stateText, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        right -= sz.cx + 12;
    }
    if (gi.steam) {
        const wchar_t* tag = L"Steam";
        SIZE s2{0, 0};
        GetTextExtentPoint32W(di->hDC, tag, 5, &s2);
        tr = di->rcItem; tr.left = right - s2.cx; tr.right = right;
        SetTextColor(di->hDC, CLR_STEAM);
        DrawTextW(di->hDC, tag, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        right -= s2.cx + 12;
    }

    // Path on the left, ellipsized to fit the remaining width.
    RECT pr = di->rcItem;
    pr.left += 10; pr.right = right - 6;
    SetTextColor(di->hDC, CLR_TEXT);
    std::wstring path = widen(gi.dataDir);
    DrawTextW(di->hDC, path.c_str(), -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
}

static void paintWindow(HWND hWnd, HDC dc) {
    RECT rc; GetClientRect(hWnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(CLR_BG);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    if (g_banner) {
        HDC bd = CreateCompatibleDC(mem);
        HGDIOBJ o2 = SelectObject(bd, g_banner);
        BITMAP b; GetObjectW(g_banner, sizeof(b), &b);
        SetStretchBltMode(mem, HALFTONE);
        StretchBlt(mem, 0, 0, WIN_W, BANNER_H, bd, 0, 0, b.bmWidth, b.bmHeight, SRCCOPY);
        SelectObject(bd, o2);
        DeleteDC(bd);
    }

    SetBkMode(mem, TRANSPARENT);
    SelectObject(mem, g_font);
    SetTextColor(mem, CLR_DIM);
    RECT lr = {24, BANNER_H + 8, WIN_W - 24, BANNER_H + 26};
    DrawTextW(mem, L"Select the install to patch:", -1, &lr, DT_LEFT | DT_SINGLELINE);

    // The status/log area is a real child edit control (g_log); it paints itself.

    RECT br = {24, WIN_H - 72, WIN_W - 24, WIN_H - 56};
    HBRUSH barbg = CreateSolidBrush(CLR_BARBG);
    FillRect(mem, &br, barbg);
    DeleteObject(barbg);
    if (g_pct >= 0) {
        RECT fr = br;
        fr.right = br.left + (LONG)((br.right - br.left) * (g_pct / 100.0));
        HBRUSH fbar = CreateSolidBrush(CLR_BAR);
        FillRect(mem, &fr, fbar);
        DeleteObject(fbar);
    } else if (g_installing) {
        HBRUSH faint = CreateSolidBrush(RGB(74, 52, 52));
        FillRect(mem, &br, faint);
        DeleteObject(faint);
    }
    HBRUSH frame = CreateSolidBrush(CLR_FRAME);
    FrameRect(mem, &br, frame);
    DeleteObject(frame);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_list = CreateWindowW(L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            24, BANNER_H + 28, WIN_W - 48, 150, hWnd, (HMENU)IDC_LIST, g_inst, nullptr);
        g_btnBrowse = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            24, WIN_H - 50, 100, 34, hWnd, (HMENU)IDC_BROWSE, g_inst, nullptr);
        g_btnRefresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            130, WIN_H - 50, 100, 34, hWnd, (HMENU)IDC_REFRESH, g_inst, nullptr);
        // Browse and Refresh stay disabled until the update check + scan resolve,
        // so a browsed-in path can't be wiped by the scan that follows the check.
        EnableWindow(g_btnBrowse, FALSE);
        EnableWindow(g_btnRefresh, FALSE);
        g_btnCancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            WIN_W - 224, WIN_H - 50, 90, 34, hWnd, (HMENU)IDC_CANCEL, g_inst, nullptr);
        g_btnInstall = CreateWindowW(L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            WIN_W - 124, WIN_H - 50, 100, 34, hWnd, (HMENU)IDC_INSTALL, g_inst, nullptr);
        EnableWindow(g_btnInstall, FALSE);   // until something is selected
        g_listBrush = CreateSolidBrush(CLR_LIST);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            24, LOG_TOP, WIN_W - 48, (WIN_H - 80) - LOG_TOP, hWnd, (HMENU)IDC_LOG, g_inst, nullptr);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_fontLog, TRUE);
        cleanupOldSelf();   // remove the previous exe a self-update left behind
        logAppend(L"Checking for installer updates...");
        if (HANDLE t = CreateThread(nullptr, 0, UpdateCheckThread, nullptr, 0, nullptr)) CloseHandle(t);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);
        paintWindow(hWnd, dc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lParam;
        if (mi->CtlID == IDC_LIST) mi->itemHeight = 24;
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wParam;
        SetBkColor(dc, CLR_LIST);
        SetTextColor(dc, CLR_TEXT);
        return (LRESULT)g_listBrush;
    }
    case WM_CTLCOLORSTATIC:   // read-only edit (the log) paints via this message
        if ((HWND)lParam == g_log) {
            HDC dc = (HDC)wParam;
            SetBkColor(dc, CLR_LIST);
            SetTextColor(dc, CLR_TEXT);
            return (LRESULT)g_listBrush;
        }
        break;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        if (di->CtlType == ODT_LISTBOX) { paintListItem(di); return TRUE; }
        if (di->CtlType == ODT_BUTTON) { paintButton(di); return TRUE; }
        break;
    }
    case WM_FOUND: {
        GameInstall* gi = (GameInstall*)lParam;
        if (gi) {
            addInstall(*gi);
            const wchar_t* tag = gi->steam ? L"Steam" : L"non-Steam";
            const wchar_t* st = gi->state == GameState::Packed ? L"packed"
                              : gi->state == GameState::Unpacked ? L"unpacked" : L"unknown";
            logAppend(std::wstring(L"Found [") + tag + L", " + st + L"]: " + widen(gi->dataDir));
            delete gi;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_UPDATE_NONE:
        startScan();
        return 0;
    case WM_UPDATE_AVAIL: {
        wchar_t* v = (wchar_t*)lParam;
        std::wstring latest = v ? v : L"";
        if (v) delete[] v;
        logAppend(L"Installer update available: v" + widen(kInstallerVersion) + L" -> v" + latest);
        if (askInstallerUpdate(hWnd, latest)) {
            g_updating = true;
            g_pct = -1;
            EnableWindow(g_btnInstall, FALSE);
            EnableWindow(g_btnRefresh, FALSE);
            EnableWindow(g_btnBrowse, FALSE);
            logAppend(L"Downloading the latest installer...");
            if (HANDLE t = CreateThread(nullptr, 0, SelfUpdateThread, nullptr, 0, nullptr)) CloseHandle(t);
        } else {
            logAppend(L"Skipped the update; using this installer.");
            startScan();
        }
        return 0;
    }
    case WM_UPDATE_DONE: {
        g_updating = false;
        wchar_t* m = (wchar_t*)lParam;
        bool ok = (wParam == 1);
        if (m) { logAppend(m); g_lastLog = m; delete[] m; }
        if (ok) {
            DestroyWindow(hWnd);   // the freshly downloaded installer is taking over
        } else {
            g_pct = -1;
            startScan();           // fall back to running this version
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SCANDONE:
        g_scanning = false;
        if (!g_installing && !g_done) EnableWindow(g_btnRefresh, TRUE);
        logAppend(g_installs.empty()
            ? L"No Dark Souls install found. Click Browse to locate DARKSOULS.exe."
            : L"Found " + std::to_wstring(g_installs.size()) +
              L" install(s). Pick one and click Install.");
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_PROGRESS: {
        g_pct = (int)wParam;
        wchar_t* p = (wchar_t*)lParam;
        if (p) {
            if (g_lastLog != p) { logAppend(p); g_lastLog = p; }   // skip repeated % updates
            delete[] p;
        }
        RECT bar = {0, WIN_H - 76, WIN_W, WIN_H - 52};   // repaint just the progress bar
        InvalidateRect(hWnd, &bar, FALSE);
        return 0;
    }
    case WM_DONE: {
        g_installing = false;
        wchar_t* m = (wchar_t*)lParam;
        bool ok = (wParam == 1);
        if (m) { logAppend(m); g_lastLog = m; delete[] m; }
        g_pct = ok ? 100 : -1;
        g_done = ok;
        EnableWindow(g_btnInstall, TRUE);
        EnableWindow(g_btnCancel, TRUE);
        if (!ok) {
            EnableWindow(g_btnBrowse, TRUE);
            EnableWindow(g_list, TRUE);
            if (!g_scanning) EnableWindow(g_btnRefresh, TRUE);
        }
        SetWindowTextW(g_btnInstall, ok ? L"Play" : L"Install");
        SetWindowTextW(g_btnCancel, ok ? L"Close" : L"Cancel");
        InvalidateRect(hWnd, nullptr, FALSE);
        if (ok) offerShortcuts(hWnd);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE && !g_installing && !g_done)
                EnableWindow(g_btnInstall, SendMessageW(g_list, LB_GETCURSEL, 0, 0) != LB_ERR);
            return 0;
        case IDC_INSTALL:
            if (g_done) { launchAndExit(); return 0; }
            {
                int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
                if (sel < 0 || sel >= (int)g_installs.size()) return 0;
                bool doP = false, doU = false;
                BackupChoice c = showBackupChoice(hWnd, g_installs[sel].state == GameState::Packed, doP, doU);
                if (c == BackupChoice::Cancel) return 0;
                g_backupPacked   = (c == BackupChoice::BackUp) && doP;
                g_backupUnpacked = (c == BackupChoice::BackUp) && doU;
                startInstall();
            }
            return 0;
        case IDC_BROWSE:
            if (!g_installing) {
                std::string d = browseForData(hWnd);
                if (!d.empty()) {
                    std::string low = d;
                    for (char& c : low) c = (char)tolower((unsigned char)c);
                    addInstall(GameInstall{d, low.find("steamapps") != std::string::npos,
                                           detectGameState(d)});
                    for (size_t i = 0; i < g_installs.size(); ++i)   // select the browsed one
                        if (g_installs[i].dataDir == d) SendMessageW(g_list, LB_SETCURSEL, i, 0);
                    EnableWindow(g_btnInstall, TRUE);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            return 0;
        case IDC_REFRESH:
            if (!g_installing && !g_scanning && !g_updating && !g_done) startScan();
            return 0;
        case IDC_CANCEL:
            if (!g_installing && !g_updating) DestroyWindow(hWnd);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        if (g_installing || g_updating) return 0;
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (g_listBrush) DeleteObject(g_listBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    g_inst = hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf = {0};               // monospace font for the console-style log
    lf.lfHeight = -13;
    lstrcpynW(lf.lfFaceName, L"Consolas", 32);
    g_fontLog = CreateFontIndirectW(&lf);
    g_banner = (HBITMAP)LoadImageW(hInst, MAKEINTRESOURCEW(IDB_BANNER), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(CLR_BG);
    wc.lpszClassName = L"mctdeInstaller";
    RegisterClassW(&wc);

    RECT rc = {0, 0, WIN_W, WIN_H};
    DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    g_hwnd = CreateWindowW(L"mctdeInstaller", L"mctde Installer", style,
                           sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_banner) DeleteObject(g_banner);
    if (g_fontLog) DeleteObject(g_fontLog);
    return (int)msg.wParam;
}
