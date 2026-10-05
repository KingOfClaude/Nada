// Nada - sweep away empty folders.
//
// A small Win32 GUI tool that scans a directory for empty folders (including
// folders that only contain other empty folders) and deletes them.
//
//  * Scanning and deleting run on a background thread: the window stays
//    responsive, shows live progress, and can be stopped.
//  * Any folder in the results can be "protected" (right-click, double-click,
//    Space, or the Protect button) and will then be kept when you click Delete.
//    A folder that contains a protected folder is automatically kept as well,
//    because it can't be removed without removing the protected one.
//
//  * Options (button at the top right): skip Windows folders, skip program
//    folders (Program Files, ProgramData, AppData), and a custom skip list.
//    Skipped folders are never scanned, listed or deleted. Settings are
//    remembered in %APPDATA%\Nada\settings.ini.
//
// Files in this project:
//   Nada.cpp       this file
//   Nada.rc        resources: icon, version info, manifest (themed controls + DPI)
//   Nada.manifest  enables modern visual styles
//   nada.ico       application icon
//
// Build with build_msvc.bat (Visual Studio) or build_mingw.sh (MSYS2/MinGW-w64).

#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>   // progress bar, list view, EM_SETCUEBANNER

#include <atomic>
#include <cwchar>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

enum {
    ID_EDIT_PATH = 100,
    ID_BTN_BROWSE,
    ID_BTN_SCAN,
    ID_BTN_DELETE,
    ID_BTN_PROTECT,
    ID_BTN_OPTIONS,
    ID_LIST,
    ID_STATUS,
    ID_BAR,

    ID_CTX_PROTECT = 200,
    ID_CTX_UNPROTECT,
    ID_CTX_CLEARALL,

    ID_OPT_WIN = 300,
    ID_OPT_PROG,
    ID_OPT_LIST,
    ID_OPT_ADD,
    ID_OPT_REMOVE
};

enum WorkType { WORK_SCAN, WORK_DELETE };

static const int  BANNER_H  = 92;           // design pixels at 96 DPI
static const UINT TIMER_ID  = 1;
static const UINT WM_WORK_DONE = WM_APP + 1;

static HWND  g_edit, g_browse, g_scan, g_delete, g_protect, g_options, g_list, g_status, g_bar;
static HFONT g_fontUI, g_fontTitle, g_fontSub;
static HICON g_iconBanner;
static int   g_dpi = 96;

// Results. The UI thread only modifies these while no worker is running; the
// worker only reads them. During work the UI reads just the atomic counters.
static fs::path                g_root;
static std::vector<fs::path>   g_empty;       // results, deepest folders first
static std::vector<std::wstring> g_display;   // same order: path shown in the list
static std::vector<char>       g_explicit;    // 1 = user protected this folder
static std::vector<char>       g_effective;   // 1 = kept (protected, or holds a protected folder)
static size_t                  g_protectedCount = 0;  // number of effective == 1
static std::unordered_map<std::wstring, size_t> g_index;  // path -> position in g_empty
static int                     g_pathColW = 0;

static std::vector<fs::path>   g_results;     // scan output
static std::vector<size_t>     g_keep;        // delete output: indices that were NOT deleted

static std::atomic<bool>   g_cancel{ false };
static std::atomic<size_t> g_scanned{ 0 };    // folders checked so far
static std::atomic<size_t> g_found{ 0 };      // empty folders found so far
static std::atomic<size_t> g_done{ 0 };       // delete progress
static std::atomic<size_t> g_deleted{ 0 };
static std::atomic<size_t> g_failed{ 0 };
static size_t              g_deleteTotal = 0;

static WorkType g_work = WORK_SCAN;
static HANDLE   g_thread = nullptr;
static bool     g_busy = false;

static int S(int px) { return MulDiv(px, g_dpi, 96); }  // scale for DPI

// ---------------------------------------------------------------------------
// Settings and skip rules
// ---------------------------------------------------------------------------

struct Settings {
    bool ignoreWindows = true;          // skip Windows system folders
    bool ignoreProgram = true;          // skip Program Files, ProgramData, AppData
    std::vector<std::wstring> custom;   // extra folders the user wants skipped
};
static Settings g_settings;

// Rules for the current scan. Built on the UI thread before the worker starts
// and only read by the worker.
static std::vector<std::wstring> g_ignorePaths;   // exact folders to skip
static bool g_skipDriveRootSystem = false;        // $Recycle.Bin, System Volume Information, ...
static bool g_skipUsersAppData = false;           // <drive>\Users\<name>\AppData

// Tidy a path for comparing: backslashes, no "." / ".." parts, no trailing slash.
static std::wstring Norm(const std::wstring& in)
{
    std::wstring s = fs::path(in).lexically_normal().make_preferred().wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

static std::wstring GetEnv(const wchar_t* name)
{
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    if (n == 0 || n >= 2048) return L"";
    return std::wstring(buf, n);
}

static std::wstring SettingsPath()
{
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata)))
        return L"";
    std::wstring dir = std::wstring(appdata) + L"\\Nada";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

// Windows' INI functions write ANSI unless the file starts with a UTF-16 BOM.
static void EnsureUnicodeIni(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(h, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(h);
    }
}

static void LoadSettings()
{
    const std::wstring ini = SettingsPath();
    if (ini.empty()) return;

    g_settings.ignoreWindows = GetPrivateProfileIntW(L"Skip", L"Windows", 1, ini.c_str()) != 0;
    g_settings.ignoreProgram = GetPrivateProfileIntW(L"Skip", L"Program", 1, ini.c_str()) != 0;

    const int count = (int)GetPrivateProfileIntW(L"Skip", L"Count", 0, ini.c_str());
    g_settings.custom.clear();
    for (int i = 0; i < count && i < 1000; ++i) {
        wchar_t key[32], buf[2048];
        wsprintfW(key, L"Folder%d", i);
        GetPrivateProfileStringW(L"Skip", key, L"", buf, 2048, ini.c_str());
        if (buf[0]) g_settings.custom.push_back(buf);
    }
}

static void SaveSettings()
{
    const std::wstring ini = SettingsPath();
    if (ini.empty()) return;
    EnsureUnicodeIni(ini);

    const int oldCount = (int)GetPrivateProfileIntW(L"Skip", L"Count", 0, ini.c_str());
    const int count = (int)g_settings.custom.size();

    WritePrivateProfileStringW(L"Skip", L"Windows", g_settings.ignoreWindows ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Skip", L"Program", g_settings.ignoreProgram ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Skip", L"Count", std::to_wstring(count).c_str(), ini.c_str());

    for (int i = 0; i < count; ++i) {
        wchar_t key[32];
        wsprintfW(key, L"Folder%d", i);
        WritePrivateProfileStringW(L"Skip", key, g_settings.custom[(size_t)i].c_str(), ini.c_str());
    }
    for (int i = count; i < oldCount && i < 1000; ++i) {  // remove leftovers from a longer list
        wchar_t key[32];
        wsprintfW(key, L"Folder%d", i);
        WritePrivateProfileStringW(L"Skip", key, nullptr, ini.c_str());
    }
}

// Collects the folders to skip for the next scan, from the current settings.
static void BuildIgnoreRules()
{
    g_ignorePaths.clear();
    auto add = [](const std::wstring& path) { if (!path.empty()) g_ignorePaths.push_back(Norm(path)); };

    if (g_settings.ignoreWindows) {
        wchar_t win[MAX_PATH];
        if (GetWindowsDirectoryW(win, MAX_PATH)) add(win);
    }
    if (g_settings.ignoreProgram) {
        add(GetEnv(L"ProgramFiles"));
        add(GetEnv(L"ProgramFiles(x86)"));
        add(GetEnv(L"ProgramW6432"));
        add(GetEnv(L"ProgramData"));
        add(GetEnv(L"APPDATA"));
        add(GetEnv(L"LOCALAPPDATA"));
        const std::wstring profile = GetEnv(L"USERPROFILE");
        if (!profile.empty()) add(profile + L"\\AppData");
    }
    for (const auto& c : g_settings.custom) add(c);

    g_skipDriveRootSystem = g_settings.ignoreWindows;
    g_skipUsersAppData = g_settings.ignoreProgram;
}

// True if this exact folder is on the skip list. Skipped folders are never
// entered, so there's no need to test their contents.
static bool IsIgnored(const fs::path& p)
{
    const std::wstring& s = p.native();
    for (const auto& ig : g_ignorePaths)
        if (_wcsicmp(s.c_str(), ig.c_str()) == 0) return true;

    if (g_skipDriveRootSystem || g_skipUsersAppData) {
        const size_t rootLen = p.root_path().native().size();
        if (rootLen > 0 && s.size() > rootLen) {
            const wchar_t* rest = s.c_str() + rootLen;       // e.g. "Users\bob\AppData"
            const wchar_t* sep1 = wcschr(rest, L'\\');

            if (g_skipDriveRootSystem && !sep1) {            // direct child of a drive root
                static const wchar_t* const names[] = {
                    L"$Recycle.Bin", L"System Volume Information", L"Recovery", L"Windows.old",
                    L"$WinREAgent", L"$SysReset", L"Config.Msi", L"$Windows.~BT", L"$Windows.~WS",
                    L"$GetCurrent"
                };
                for (const wchar_t* n : names)
                    if (_wcsicmp(rest, n) == 0) return true;
            }
            if (g_skipUsersAppData && sep1 && (sep1 - rest) == 5 && _wcsnicmp(rest, L"Users", 5) == 0) {
                const wchar_t* sep2 = wcschr(sep1 + 1, L'\\');
                if (sep2 && !wcschr(sep2 + 1, L'\\') && _wcsicmp(sep2 + 1, L"AppData") == 0)
                    return true;
            }
        }
    }
    return false;
}

// True if the chosen scan folder is itself skipped, or sits inside a skipped folder.
static bool IsIgnoredOrInside(const std::wstring& rootNorm)
{
    fs::path q(rootNorm);
    for (;;) {
        if (IsIgnored(q)) return true;
        fs::path parent = q.parent_path();
        if (parent.empty() || parent == q) return false;
        q = parent;
    }
}

// ---------------------------------------------------------------------------
// Core logic (runs on the worker thread)
// ---------------------------------------------------------------------------

// Returns true if `dir` contains nothing except effectively-empty directories.
// Every such non-root directory is appended to `out` in post-order (children
// before parents) so deleting in list order always works.
// Symlinks/junctions are never followed and count as content.
static bool CollectEmpty(const fs::path& dir, std::vector<fs::path>& out, bool isRoot)
{
    if (g_cancel) return false;
    ++g_scanned;

    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;  // unreadable -> treat as non-empty, never touch it

    bool hasContent = false;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (g_cancel) return false;
        if (ec) { hasContent = true; break; }

        const fs::directory_entry& entry = *it;
        std::error_code e2;

        if (entry.is_symlink(e2) || e2) { hasContent = true; continue; }

        if (entry.is_directory(e2) && !e2) {
            // Skipped folders are never entered or listed, and they count as content,
            // so a folder that contains one is never treated as empty.
            if (IsIgnored(entry.path())) { hasContent = true; continue; }
            if (!CollectEmpty(entry.path(), out, false))
                hasContent = true;
        } else {
            hasContent = true;
        }
    }

    if (hasContent || g_cancel) return false;
    if (!isRoot) {
        out.push_back(dir);
        ++g_found;
    }
    return true;
}

static DWORD WINAPI WorkerProc(LPVOID param)
{
    HWND hwnd = (HWND)param;

    if (g_work == WORK_SCAN) {
        g_results.clear();
        CollectEmpty(g_root, g_results, true);
    } else {
        // Protected folders are skipped. fs::remove on a directory only
        // succeeds if it is truly empty, so this can never delete anything
        // that has content.
        std::vector<size_t> keep;
        const size_t n = g_empty.size();
        for (size_t i = 0; i < n; ++i) {
            if (g_cancel) {
                for (size_t j = i; j < n; ++j) keep.push_back(j);
                break;
            }
            if (g_effective[i]) { keep.push_back(i); continue; }

            std::error_code ec;
            if (fs::remove(g_empty[i], ec) && !ec) {
                ++g_deleted;
            } else {
                keep.push_back(i);
                ++g_failed;
            }
            ++g_done;
        }
        g_keep = std::move(keep);
    }

    PostMessageW(hwnd, WM_WORK_DONE, 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

static void SetStatus(const std::wstring& text) { SetWindowTextW(g_status, text.c_str()); }

// 12345 -> "12,345"
static std::wstring Num(size_t n)
{
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}

static std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many)
{
    return Num(n) + L" " + (n == 1 ? one : many);
}

static std::wstring GetEditText(HWND h)
{
    int len = GetWindowTextLengthW(h);
    std::wstring s(len, L'\0');
    if (len > 0) GetWindowTextW(h, &s[0], len + 1);
    const wchar_t* ws = L" \t\r\n\"";  // trim whitespace and quotes
    size_t a = s.find_first_not_of(ws);
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static void UpdateDeleteButton()
{
    const size_t del = g_empty.size() - g_protectedCount;
    if (del == 0) {
        SetWindowTextW(g_delete, L"Delete");
        EnableWindow(g_delete, FALSE);
    } else {
        SetWindowTextW(g_delete, (L"Delete " + Plural(del, L"folder", L"folders")).c_str());
        EnableWindow(g_delete, g_busy ? FALSE : TRUE);
    }
}

// Button label follows the first selected row: protect it, or undo that.
static void UpdateProtectButton()
{
    const int sel = ListView_GetSelectedCount(g_list);
    if (g_busy || sel == 0) {
        SetWindowTextW(g_protect, L"Protect selected");
        EnableWindow(g_protect, FALSE);
        return;
    }
    int first = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    bool isProtected = (first >= 0 && (size_t)first < g_explicit.size() && g_explicit[(size_t)first]);
    SetWindowTextW(g_protect, isProtected ? L"Unprotect selected" : L"Protect selected");
    EnableWindow(g_protect, TRUE);
}

static void ApplyColumnWidths()
{
    RECT rc;
    GetClientRect(g_list, &rc);
    const int statusW = S(190);
    ListView_SetColumnWidth(g_list, 0, statusW);

    int avail = rc.right - statusW - GetSystemMetrics(SM_CXVSCROLL);
    if (avail < 0) avail = 0;
    ListView_SetColumnWidth(g_list, 1, g_pathColW > avail ? g_pathColW : avail);
}

// Works out which rows are effectively protected: the ones the user protected,
// plus every listed parent folder that contains one (it can't be deleted
// without also deleting the protected folder).
static void RecomputeProtection()
{
    const size_t n = g_empty.size();
    g_effective = g_explicit;

    for (size_t i = 0; i < n; ++i) {
        if (!g_explicit[i]) continue;
        fs::path p = g_empty[i].parent_path();
        for (;;) {
            auto it = g_index.find(p.native());
            if (it == g_index.end()) break;          // not an empty folder -> stop climbing
            if (g_effective[it->second]) break;      // already protected (its parents are handled)
            g_effective[it->second] = 1;
            p = p.parent_path();
        }
    }

    g_protectedCount = 0;
    for (char c : g_effective) if (c) ++g_protectedCount;
}

static void RefreshProtectionUI()
{
    InvalidateRect(g_list, nullptr, FALSE);
    UpdateDeleteButton();
    UpdateProtectButton();
}

static void UpdateSummaryStatus()
{
    if (g_empty.empty()) return;
    const size_t del = g_empty.size() - g_protectedCount;

    std::wstring s = Plural(del, L"folder", L"folders") + (del == 1 ? L" will be deleted" : L" will be deleted");
    if (g_protectedCount)
        s += L" \u00B7 " + Plural(g_protectedCount, L"protected", L"protected") + L" (kept)";
    else
        s += L". Right-click or double-click a folder to protect it.";
    SetStatus(s);
}

// Replaces the results list. `expl` carries protection flags (same length as paths).
static void ApplyResults(std::vector<fs::path>&& paths, std::vector<char>&& expl)
{
    g_empty = std::move(paths);
    g_explicit = std::move(expl);
    const size_t n = g_empty.size();
    if (g_explicit.size() != n) g_explicit.assign(n, 0);

    g_display.clear();
    g_display.reserve(n);
    g_index.clear();
    g_index.reserve(n);

    HDC dc = GetDC(g_list);
    HGDIOBJ oldFont = SelectObject(dc, g_fontUI);
    int maxW = 0;

    for (size_t i = 0; i < n; ++i) {
        fs::path rel = g_empty[i].lexically_relative(g_root);  // no disk access
        std::wstring text = rel.empty() ? g_empty[i].wstring() : rel.wstring();

        SIZE sz;
        if (GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz))
            maxW = (sz.cx > maxW) ? sz.cx : maxW;

        g_display.push_back(std::move(text));
        g_index.emplace(g_empty[i].native(), i);
    }
    SelectObject(dc, oldFont);
    ReleaseDC(g_list, dc);
    g_pathColW = maxW + S(24);

    RecomputeProtection();

    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);   // clear selection
    ListView_SetItemCount(g_list, (int)n);                 // virtual list: instant for any size
    ApplyColumnWidths();
    InvalidateRect(g_list, nullptr, TRUE);

    UpdateDeleteButton();
    UpdateProtectButton();
}

// Protect (or unprotect) every selected row.
static void SetProtectionForSelection(bool protect)
{
    bool changed = false;
    int i = -1;
    while ((i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) != -1) {
        if ((size_t)i < g_explicit.size() && g_explicit[(size_t)i] != (protect ? 1 : 0)) {
            g_explicit[(size_t)i] = protect ? 1 : 0;
            changed = true;
        }
    }
    if (!changed) return;

    RecomputeProtection();
    RefreshProtectionUI();
    UpdateSummaryStatus();

    if (!protect) {  // folders that hold a protected subfolder can't be unprotected directly
        size_t stuck = 0;
        i = -1;
        while ((i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) != -1)
            if ((size_t)i < g_effective.size() && g_effective[(size_t)i]) ++stuck;
        if (stuck)
            SetStatus(L"Some selected folders contain protected subfolders, so they stay protected "
                      L"until those are unprotected too.");
    }
}

static void ToggleSelectionProtection()
{
    int first = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (first < 0 || (size_t)first >= g_explicit.size()) return;
    SetProtectionForSelection(!g_explicit[(size_t)first]);
}

// Marquee (indeterminate) for scanning, normal 0..total bar for deleting.
static void SetProgressMode(bool marquee, size_t total)
{
    LONG_PTR st = GetWindowLongPtrW(g_bar, GWL_STYLE);
    if (marquee) {
        SetWindowLongPtrW(g_bar, GWL_STYLE, st | PBS_MARQUEE);
        SendMessageW(g_bar, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g_bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(g_bar, GWL_STYLE, st & ~(LONG_PTR)PBS_MARQUEE);
        SendMessageW(g_bar, PBM_SETRANGE32, 0, (LPARAM)total);
        SendMessageW(g_bar, PBM_SETPOS, 0, 0);
    }
}

static void SetBusy(bool busy)
{
    g_busy = busy;
    if (busy) SetFocus(g_scan);  // move focus before disabling other controls

    EnableWindow(g_edit, !busy);
    EnableWindow(g_browse, !busy);
    EnableWindow(g_list, !busy);
    EnableWindow(g_options, !busy);
    SetWindowTextW(g_scan, busy ? (g_work == WORK_SCAN ? L"Stop Scanning" : L"Stop Deleting") : L"Scan");
    ShowWindow(g_bar, busy ? SW_SHOW : SW_HIDE);

    UpdateDeleteButton();
    UpdateProtectButton();
}

static void UpdateProgressUI()
{
    if (g_work == WORK_SCAN) {
        SetStatus(L"Scanning\u2026 " + Num(g_scanned) + L" folders checked \u00B7 " +
                  Num(g_found) + L" empty so far");
    } else {
        size_t done = g_done;
        SendMessageW(g_bar, PBM_SETPOS, (WPARAM)done, 0);
        SetStatus(L"Deleting\u2026 " + Num(done) + L" of " + Num(g_deleteTotal));
    }
}

static void StartWork(HWND hwnd, WorkType type)
{
    g_work = type;
    g_cancel = false;
    g_scanned = 0;
    g_found = 0;
    g_done = 0;
    g_deleted = 0;
    g_failed = 0;
    if (type == WORK_SCAN) g_deleteTotal = 0;

    SetProgressMode(type == WORK_SCAN, g_deleteTotal);
    SetBusy(true);

    g_thread = CreateThread(nullptr, 0, WorkerProc, hwnd, 0, nullptr);
    if (!g_thread) {
        SetBusy(false);
        SetStatus(L"Couldn't start the background task.");
        return;
    }
    UpdateProgressUI();
    SetTimer(hwnd, TIMER_ID, 100, nullptr);
}

static void OnWorkDone(HWND hwnd)
{
    KillTimer(hwnd, TIMER_ID);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    const bool cancelled = g_cancel;

    if (g_work == WORK_SCAN) {
        // Folders are only added once verified empty, so partial results are safe to keep.
        const size_t n = g_results.size();
        ApplyResults(std::move(g_results), std::vector<char>(n, 0));
        g_results.clear();
        SetBusy(false);

        if (cancelled && g_empty.empty())
            SetStatus(L"Scan stopped. No empty folders found before stopping.");
        else if (cancelled)
            SetStatus(L"Scan stopped early \u2014 " + Plural(g_empty.size(), L"empty folder", L"empty folders") +
                      L" found so far. The rest wasn't checked.");
        else if (g_empty.empty())
            SetStatus(L"Nothing to sweep \u2014 no empty folders found (" +
                      Num(g_scanned) + L" folders checked).");
        else
            SetStatus(L"Found " + Plural(g_empty.size(), L"empty folder", L"empty folders") +
                      L". Right-click or double-click any you want to keep, then click Delete.");
    } else {
        // Keep whatever wasn't deleted (protected, failed, or not reached), with its protection flags.
        std::vector<fs::path> paths;
        std::vector<char> expl;
        paths.reserve(g_keep.size());
        expl.reserve(g_keep.size());
        for (size_t idx : g_keep) {
            paths.push_back(std::move(g_empty[idx]));
            expl.push_back(g_explicit[idx]);
        }
        g_keep.clear();

        ApplyResults(std::move(paths), std::move(expl));
        SetBusy(false);

        std::wstring result = cancelled ? L"Stopped. " : L"Swept! ";
        result += L"Deleted " + Plural(g_deleted, L"folder", L"folders") + L".";
        if (g_protectedCount)
            result += L"  " + Plural(g_protectedCount, L"protected folder", L"protected folders") + L" kept.";
        if (g_failed)
            result += L"  " + Num(g_failed) + L" couldn't be removed (in use or access denied).";
        const size_t left = g_empty.size() - g_protectedCount;
        if (cancelled && left)
            result += L"  " + Num(left) + L" not processed.";
        SetStatus(result);
    }
}

static bool BrowseForFolder(HWND owner, const wchar_t* title, std::wstring& out)
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH];
    const bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (ok) out = path;
    return ok;
}

static void DoBrowse(HWND hwnd)
{
    std::wstring path;
    if (BrowseForFolder(hwnd, L"Choose a folder to sweep", path))
        SetWindowTextW(g_edit, path.c_str());
}

static void DoScan(HWND hwnd)
{
    if (g_busy) return;

    std::wstring root = GetEditText(g_edit);
    std::error_code ec;

    if (root.empty() || !fs::is_directory(root, ec)) {
        MessageBoxW(hwnd, L"Please choose a valid folder first.", L"Nada",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    BuildIgnoreRules();
    const std::wstring rootNorm = Norm(root);
    if (IsIgnoredOrInside(rootNorm)) {
        MessageBoxW(hwnd,
                    L"That folder is covered by your skip settings, so there's nothing to scan.\n\n"
                    L"If you really want to scan it, change the settings under Options\u2026",
                    L"Nada", MB_OK | MB_ICONINFORMATION);
        return;
    }

    g_root = fs::path(rootNorm);
    ApplyResults(std::vector<fs::path>(), std::vector<char>());  // clear the list (and protections)
    StartWork(hwnd, WORK_SCAN);
}

static void DoDelete(HWND hwnd)
{
    if (g_busy) return;
    const size_t del = g_empty.size() - g_protectedCount;
    if (del == 0) return;

    std::wstring msg = L"Permanently delete " + Plural(del, L"empty folder", L"empty folders") + L"?";
    if (g_protectedCount)
        msg += L"\n\n" + Plural(g_protectedCount, L"protected folder", L"protected folders") + L" will be kept.";
    msg += L"\n\nThis can't be undone (nothing goes to the Recycle Bin).";

    if (g_root.relative_path().empty())  // a whole drive such as C:\ was scanned
        msg += L"\n\nYou scanned an entire drive. Windows and some programs rely on "
               L"intentionally empty folders, so please review the list carefully first.";

    if (MessageBoxW(hwnd, msg.c_str(), L"Nada", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    g_deleteTotal = del;
    StartWork(hwnd, WORK_DELETE);
}

// ---------------------------------------------------------------------------
// Results list notifications and context menu
// ---------------------------------------------------------------------------

static LRESULT OnListNotify(NMHDR* nh, LPARAM lParam)
{
    switch (nh->code) {

    case LVN_GETDISPINFOW: {  // virtual list asks us for the text of each visible cell
        NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
        if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 &&
            (size_t)di->item.iItem < g_empty.size() && di->item.pszText) {
            const size_t i = (size_t)di->item.iItem;
            const wchar_t* t = L"";
            if (di->item.iSubItem == 0) {
                if (g_explicit[i])       t = L"Protected";
                else if (g_effective[i]) t = L"Kept (has protected subfolder)";
                else                     t = L"Will delete";
            } else {
                t = g_display[i].c_str();
            }
            lstrcpynW(di->item.pszText, t, di->item.cchTextMax);
        }
        return 0;
    }

    case NM_CUSTOMDRAW: {  // tint protected rows green
        NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            const size_t i = (size_t)cd->nmcd.dwItemSpec;
            if (i < g_effective.size() && g_effective[i] && !(cd->nmcd.uItemState & CDIS_SELECTED))
                cd->clrText = RGB(0, 125, 108);
            return CDRF_DODEFAULT;
        }
        }
        return CDRF_DODEFAULT;
    }

    case NM_DBLCLK: {
        NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lParam;
        if (ia->iItem >= 0) ToggleSelectionProtection();
        return 0;
    }

    case LVN_KEYDOWN: {
        NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lParam;
        if (kd->wVKey == VK_SPACE) ToggleSelectionProtection();
        return 0;
    }

    case LVN_ITEMCHANGED: {
        NMLISTVIEW* nl = (NMLISTVIEW*)lParam;
        if ((nl->uChanged & LVIF_STATE) && ((nl->uNewState ^ nl->uOldState) & LVIS_SELECTED))
            UpdateProtectButton();
        return 0;
    }

    case LVN_ODSTATECHANGED:  // range selection in a virtual list
        UpdateProtectButton();
        return 0;
    }
    return 0;
}

static void ShowListContextMenu(HWND hwnd, LPARAM lParam)
{
    POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };

    if (pt.x == -1 && pt.y == -1) {  // opened with the keyboard (Shift+F10 / menu key)
        int f = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED);
        RECT r;
        if (f >= 0 && ListView_GetItemRect(g_list, f, &r, LVIR_LABEL)) {
            pt.x = r.left + S(8);
            pt.y = r.bottom;
            ClientToScreen(g_list, &pt);
        } else {
            RECT wr;
            GetWindowRect(g_list, &wr);
            pt.x = wr.left + S(24);
            pt.y = wr.top + S(24);
        }
    } else {  // right-click: if the row isn't already selected, select just that row
        POINT cp = pt;
        ScreenToClient(g_list, &cp);
        LVHITTESTINFO ht = {};
        ht.pt = cp;
        int item = ListView_HitTest(g_list, &ht);
        if (item >= 0 && !(ListView_GetItemState(g_list, item, LVIS_SELECTED) & LVIS_SELECTED)) {
            ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(g_list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    const UINT selFlags = ListView_GetSelectedCount(g_list) ? MF_STRING : (MF_STRING | MF_GRAYED);
    const UINT anyFlags = (g_protectedCount > 0) ? MF_STRING : (MF_STRING | MF_GRAYED);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, selFlags, ID_CTX_PROTECT, L"Protect selected (keep)");
    AppendMenuW(menu, selFlags, ID_CTX_UNPROTECT, L"Unprotect selected");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, anyFlags, ID_CTX_CLEARALL, L"Unprotect all");

    int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case ID_CTX_PROTECT:   SetProtectionForSelection(true);  break;
    case ID_CTX_UNPROTECT: SetProtectionForSelection(false); break;
    case ID_CTX_CLEARALL:
        g_explicit.assign(g_explicit.size(), 0);
        RecomputeProtection();
        RefreshProtectionUI();
        UpdateSummaryStatus();
        break;
    }
}

// ---------------------------------------------------------------------------
// Options dialog (modal, built in code)
// ---------------------------------------------------------------------------

static struct OptState {
    HWND dlg = nullptr, chkWin = nullptr, chkProg = nullptr, list = nullptr, btnRemove = nullptr;
    std::vector<std::wstring> custom;   // working copy of the custom skip list
    bool open = false, accepted = false;
} g_opt;

static void OptUpdateRemoveButton()
{
    EnableWindow(g_opt.btnRemove, SendMessageW(g_opt.list, LB_GETCURSEL, 0, 0) != LB_ERR);
}

static LRESULT CALLBACK OptionsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCTW)lParam)->hInstance;
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id,
                        int x, int y, int w, int h) -> HWND {
            HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                     S(x), S(y), S(w), S(h), hwnd, (HMENU)(INT_PTR)id, hi, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
            return c;
        };

        g_opt.chkWin = make(L"BUTTON",
            L"Skip Windows folders (the Windows folder, Recycle Bin, System Volume Information, "
            L"Recovery, Windows.old)",
            BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 0, ID_OPT_WIN, 16, 14, 438, 38);
        g_opt.chkProg = make(L"BUTTON",
            L"Skip program folders (Program Files, Program Files (x86), ProgramData, AppData)",
            BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 0, ID_OPT_PROG, 16, 58, 438, 38);

        make(L"STATIC", L"Also skip these folders:", SS_LEFT, 0, 0, 16, 108, 438, 18);
        g_opt.list = make(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                          WS_EX_CLIENTEDGE, ID_OPT_LIST, 16, 130, 438, 140);
        make(L"BUTTON", L"Add folder\u2026", BS_PUSHBUTTON | WS_TABSTOP, 0, ID_OPT_ADD, 16, 278, 120, 28);
        g_opt.btnRemove = make(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 0,
                               ID_OPT_REMOVE, 144, 278, 100, 28);

        make(L"STATIC",
             L"Skipped folders are never scanned, listed or deleted. A folder that contains a skipped "
             L"folder is never treated as empty either.",
             SS_LEFT, 0, 0, 16, 318, 438, 36);

        make(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK, 266, 364, 90, 28);
        make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, 364, 364, 90, 28);

        SendMessageW(g_opt.chkWin, BM_SETCHECK, g_settings.ignoreWindows ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_opt.chkProg, BM_SETCHECK, g_settings.ignoreProgram ? BST_CHECKED : BST_UNCHECKED, 0);
        for (const auto& c : g_opt.custom)
            SendMessageW(g_opt.list, LB_ADDSTRING, 0, (LPARAM)c.c_str());
        SendMessageW(g_opt.list, LB_SETHORIZONTALEXTENT, S(900), 0);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_OPT_ADD: {
            std::wstring picked;
            if (!BrowseForFolder(hwnd, L"Choose a folder to skip", picked)) break;

            const std::wstring n = Norm(picked);
            if (fs::path(n).relative_path().empty()) {
                MessageBoxW(hwnd, L"Please choose a folder, not a whole drive.", L"Nada",
                            MB_OK | MB_ICONINFORMATION);
                break;
            }
            bool duplicate = false;
            for (const auto& e : g_opt.custom)
                if (_wcsicmp(e.c_str(), n.c_str()) == 0) duplicate = true;
            if (!duplicate) {
                g_opt.custom.push_back(n);
                SendMessageW(g_opt.list, LB_ADDSTRING, 0, (LPARAM)n.c_str());
            }
            break;
        }
        case ID_OPT_REMOVE: {
            LRESULT sel = SendMessageW(g_opt.list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && (size_t)sel < g_opt.custom.size()) {
                g_opt.custom.erase(g_opt.custom.begin() + sel);
                SendMessageW(g_opt.list, LB_DELETESTRING, (WPARAM)sel, 0);
                OptUpdateRemoveButton();
            }
            break;
        }
        case ID_OPT_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE) OptUpdateRemoveButton();
            break;
        case IDOK:
            g_opt.accepted = true;
            g_opt.open = false;
            break;
        case IDCANCEL:
            g_opt.open = false;
            break;
        }
        return 0;

    case WM_CLOSE:   // closing with the X counts as Cancel; ShowOptions destroys the window
        g_opt.open = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowOptions(HWND owner)
{
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OptionsProc;
        wc.hInstance = hi;
        wc.hIcon = (HICON)GetClassLongPtrW(owner, GCLP_HICON);
        wc.hIconSm = (HICON)GetClassLongPtrW(owner, GCLP_HICONSM);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"NadaOptionsWindow";
        RegisterClassExW(&wc);
        registered = true;
    }

    g_opt.custom = g_settings.custom;
    g_opt.open = true;
    g_opt.accepted = false;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex = WS_EX_DLGMODALFRAME;
    RECT rc = { 0, 0, S(470), S(404) };
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;

    RECT orc;
    GetWindowRect(owner, &orc);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    g_opt.dlg = CreateWindowExW(ex, L"NadaOptionsWindow", L"Options", style, x, y, w, h,
                                owner, nullptr, hi, nullptr);
    if (!g_opt.dlg) return;

    EnableWindow(owner, FALSE);  // modal: block the main window while this is open
    ShowWindow(g_opt.dlg, SW_SHOW);
    SetFocus(g_opt.chkWin);

    MSG m;
    while (g_opt.open) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)m.wParam);  // app is quitting: pass it on
            break;
        }
        if (!IsDialogMessageW(g_opt.dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (g_opt.accepted) {  // read the controls before the window is destroyed
        g_settings.ignoreWindows = SendMessageW(g_opt.chkWin, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.ignoreProgram = SendMessageW(g_opt.chkProg, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.custom = g_opt.custom;
        SaveSettings();
    }

    EnableWindow(owner, TRUE);
    DestroyWindow(g_opt.dlg);
    g_opt.dlg = nullptr;
    SetForegroundWindow(owner);

    if (g_opt.accepted)
        SetStatus(L"Options saved \u2014 they apply to the next scan.");
}

// ---------------------------------------------------------------------------
// Painting the banner
// ---------------------------------------------------------------------------

static void PaintBanner(HDC hdc, int width)
{
    const int h = S(BANNER_H);
    const int r1 = 12, g1 = 84, b1 = 100;    // left colour
    const int r2 = 24, g2 = 170, b2 = 156;   // right colour

    // horizontal gradient, drawn in small vertical strips
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(DC_BRUSH));
    const int step = 4;
    int denom = (width > 1) ? width - 1 : 1;
    for (int x = 0; x < width; x += step) {
        int r = r1 + (r2 - r1) * x / denom;
        int g = g1 + (g2 - g1) * x / denom;
        int b = b1 + (b2 - b1) * x / denom;
        SetDCBrushColor(hdc, RGB(r, g, b));
        RECT strip = { x, 0, x + step, h };
        FillRect(hdc, &strip, (HBRUSH)GetStockObject(DC_BRUSH));
    }
    SelectObject(hdc, oldBrush);

    // thin sparkle-yellow accent line under the banner
    SetDCBrushColor(hdc, RGB(254, 240, 138));
    RECT line = { 0, h, width, h + S(3) };
    FillRect(hdc, &line, (HBRUSH)GetStockObject(DC_BRUSH));

    // icon
    const int icon = S(52);
    if (g_iconBanner)
        DrawIconEx(hdc, S(20), (h - icon) / 2, g_iconBanner, icon, icon, 0, nullptr, DI_NORMAL);

    // title + tagline
    SetBkMode(hdc, TRANSPARENT);
    const int tx = S(20) + icon + S(14);

    HGDIOBJ oldFont = SelectObject(hdc, g_fontTitle);
    TEXTMETRICW tmT = {};
    GetTextMetricsW(hdc, &tmT);
    SelectObject(hdc, g_fontSub);
    TEXTMETRICW tmS = {};
    GetTextMetricsW(hdc, &tmS);

    int blockH = tmT.tmHeight + tmS.tmHeight;
    int y = (h - blockH) / 2;

    SelectObject(hdc, g_fontTitle);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, tx, y, L"Nada", 4);

    SelectObject(hdc, g_fontSub);
    SetTextColor(hdc, RGB(214, 245, 240));
    const wchar_t* tag = L"Sweep away empty folders.";
    TextOutW(hdc, tx, y + tmT.tmHeight, tag, (int)wcslen(tag));

    SelectObject(hdc, oldFont);
}

// ---------------------------------------------------------------------------
// Layout + window procedure
// ---------------------------------------------------------------------------

static void Layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;

    const int m = S(16), gap = S(8), rowH = S(30), browseW = S(100);
    const int statusH = S(22);

    int y = S(BANNER_H) + S(3) + m;

    MoveWindow(g_browse, w - m - browseW, y, browseW, rowH, TRUE);
    MoveWindow(g_edit, m, y, w - 2 * m - browseW - gap, rowH, TRUE);

    y += rowH + gap;
    const int scanW = S(130), delW = S(160), protW = S(140), optW = S(100);
    int x = m;
    MoveWindow(g_scan, x, y, scanW, rowH, TRUE);       x += scanW + gap;
    MoveWindow(g_delete, x, y, delW, rowH, TRUE);      x += delW + gap;
    MoveWindow(g_protect, x, y, protW, rowH, TRUE);    x += protW + gap * 2;
    MoveWindow(g_options, w - m - optW, y, optW, rowH, TRUE);

    // progress bar fills the space between Protect and Options
    int barW = (w - m - optW - gap * 2) - x;
    if (barW < 0) barW = 0;
    int barH = S(14);
    MoveWindow(g_bar, x, y + (rowH - barH) / 2, barW, barH, TRUE);

    y += rowH + gap;
    int listH = h - y - statusH - m - gap;
    if (listH < 0) listH = 0;
    MoveWindow(g_list, m, y, w - 2 * m, listH, TRUE);
    ApplyColumnWidths();

    MoveWindow(g_status, m, h - statusH - m / 2 - gap / 2, w - 2 * m, statusH, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCTW)lParam)->hInstance;
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id) {
            return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                   hwnd, (HMENU)(INT_PTR)id, hi, nullptr);
        };

        g_edit    = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_EDIT_PATH);
        g_browse  = make(L"BUTTON", L"Browse\u2026", BS_PUSHBUTTON | WS_TABSTOP, 0, ID_BTN_BROWSE);
        g_scan    = make(L"BUTTON", L"Scan", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, ID_BTN_SCAN);
        g_delete  = make(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 0, ID_BTN_DELETE);
        g_protect = make(L"BUTTON", L"Protect selected", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 0, ID_BTN_PROTECT);
        g_options = make(L"BUTTON", L"Options\u2026", BS_PUSHBUTTON | WS_TABSTOP, 0, ID_BTN_OPTIONS);
        g_status  = make(L"STATIC", L"Pick a folder (or drop one here) and click Scan.",
                         SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0, ID_STATUS);

        // results: a virtual (owner-data) report-style list, so it stays fast with huge result sets
        g_list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP,
                      WS_EX_CLIENTEDGE, ID_LIST);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                     LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = S(190);
        col.pszText = (LPWSTR)L"Status";
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = S(400);
        col.pszText = (LPWSTR)L"Empty folder";
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

        // progress bar starts hidden; shown while a scan/delete is running
        g_bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | PBS_SMOOTH,
                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BAR, hi, nullptr);

        for (HWND c : {g_edit, g_browse, g_scan, g_delete, g_protect, g_options, g_list, g_status})
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUI, TRUE);

        SendMessageW(g_edit, EM_SETCUEBANNER, FALSE,
                     (LPARAM)L"Drop a folder here, or click Browse\u2026");
        SetFocus(g_edit);
        return 0;
    }

    case WM_SIZE:
        if (g_bar) Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lParam)->ptMinTrackSize = { S(660), S(440) };
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintBanner(hdc, rc.right);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {  // status line: grey text on the white window background
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(80, 90, 100));
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lParam;
        if (nh->hwndFrom == g_list) return OnListNotify(nh, lParam);
        return 0;
    }

    case WM_CONTEXTMENU:
        if ((HWND)wParam == g_list && !g_busy) {
            ShowListContextMenu(hwnd, lParam);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == TIMER_ID && g_busy) UpdateProgressUI();
        return 0;

    case WM_WORK_DONE:
        OnWorkDone(hwnd);
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        wchar_t buf[MAX_PATH];
        if (!g_busy && DragQueryFileW(drop, 0, buf, MAX_PATH)) {
            std::error_code ec;
            fs::path p(buf);
            if (!fs::is_directory(p, ec)) p = p.parent_path();
            SetWindowTextW(g_edit, p.c_str());
            DoScan(hwnd);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_BROWSE: DoBrowse(hwnd); break;
        case ID_BTN_SCAN:
            if (g_busy) {                    // the button reads "Stop Scanning" while working
                g_cancel = true;
                SetStatus(L"Stopping\u2026");
            } else {
                DoScan(hwnd);
            }
            break;
        case ID_BTN_DELETE:  DoDelete(hwnd); break;
        case ID_BTN_PROTECT: ToggleSelectionProtection(); SetFocus(g_list); break;
        case ID_BTN_OPTIONS: ShowOptions(hwnd); break;
        case IDOK:           // Enter: only rescan from the path box, never from the results list
            if (GetFocus() == g_edit) DoScan(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void CreateFonts()
{
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

    g_fontUI = CreateFontIndirectW(&ncm.lfMessageFont);  // the user's system UI font (Segoe UI)

    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = title.lfHeight * 5 / 2;
    title.lfWeight = FW_BOLD;
    lstrcpyW(title.lfFaceName, L"Segoe UI Semibold");
    g_fontTitle = CreateFontIndirectW(&title);

    LOGFONTW sub = ncm.lfMessageFont;
    sub.lfHeight = sub.lfHeight * 11 / 10;
    g_fontSub = CreateFontIndirectW(&sub);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // folder picker needs COM

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);

    CreateFonts();
    LoadSettings();

    // Icons come from Nada.rc (resource id 1); fall back to the default if missing.
    HICON iconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON iconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    g_iconBanner = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(52), S(52), LR_DEFAULTCOLOR);
    if (!iconBig) iconBig = LoadIcon(nullptr, IDI_APPLICATION);
    if (!iconSmall) iconSmall = iconBig;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = iconBig;
    wc.hIconSm = iconSmall;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NadaWindow";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, S(740), S(560) };  // desired client size
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_ACCEPTFILES);

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName,
                                L"Nada \u2014 Empty Folder Sweeper", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    Layout(hwnd);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {  // Tab / Enter keyboard navigation
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    // Window closed while a scan/delete was still running: ask it to stop, give
    // it a moment, then exit without running static destructors under its feet.
    if (g_thread) {
        g_cancel = true;
        WaitForSingleObject(g_thread, 2000);
        ExitProcess((UINT)m.wParam);
    }

    CoUninitialize();
    return (int)m.wParam;
}
