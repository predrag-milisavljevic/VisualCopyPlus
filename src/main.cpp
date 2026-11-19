#include "app.h"
#include "../res/resource.h"
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <cwchar>

HINSTANCE    g_hInst;
HWND         g_hwndMain;
std::wstring g_lastSourceExe;

static UINT s_cfExclude, s_cfViewerIgnore, s_cfHistory;
static DWORD s_lastSeq;
static int s_openRetries;
static UINT s_msgTaskbarCreated;
static NOTIFYICONDATAW s_nid;

enum { TIMER_CLIP = 1 };
enum { IDM_SETTINGS = 100, IDM_PAUSE, IDM_IGNORE_LAST, IDM_AUTOSTART, IDM_EXIT };

// ---------------------------------------------------------------- tray

static void TrayUpdateTip() {
    s_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy(s_nid.szTip, g_settings.paused ? APP_NAME L" (paused)" : APP_NAME);
    Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

static void TrayAdd() {
    if (s_nid.hIcon) DestroyIcon(s_nid.hIcon);
    s_nid = {};
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = g_hwndMain;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    s_nid.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(g_hInst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &s_nid.hIcon);
    wcscpy(s_nid.szTip, g_settings.paused ? APP_NAME L" (paused)" : APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &s_nid);
    s_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
}

static void TrayRemove() {
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    if (s_nid.hIcon) DestroyIcon(s_nid.hIcon);
    s_nid.hIcon = nullptr;
}

static void ShowTrayMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_settings.paused ? MF_CHECKED : 0), IDM_PAUSE, L"&Pause");
    std::wstring ign = g_lastSourceExe.empty() ? std::wstring(L"Ignore last source app")
                                               : L"Ignore copies from " + g_lastSourceExe;
    bool canIgnore = !g_lastSourceExe.empty() && !IsIgnored(g_lastSourceExe);
    AppendMenuW(m, MF_STRING | (canIgnore ? 0 : MF_GRAYED), IDM_IGNORE_LAST, ign.c_str());
    AppendMenuW(m, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with &Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);

    SetForegroundWindow(g_hwndMain);
    UINT flags = TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    TrackPopupMenuEx(m, flags, x, y, g_hwndMain, nullptr);
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- source window

static bool IsCandidateWindow(HWND h) {
    if (!IsWindowVisible(h) || IsIconic(h)) return false;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    RECT rc;
    return GetWindowRect(h, &rc) && rc.right > rc.left && rc.bottom > rc.top;
}

struct FindCtx { DWORD pid; HWND found; };
static BOOL CALLBACK FindTopWindowOfPid(HWND h, LPARAM lp) {
    auto c = (FindCtx*)lp;
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p == c->pid && IsCandidateWindow(h)) { c->found = h; return FALSE; }
    return TRUE;
}

// The window the user copied from: the foreground window if it belongs to the clipboard
// owner, otherwise the owner's top-most visible window, otherwise the foreground window
// (e.g. UWP apps, where the frame is owned by ApplicationFrameHost).
static HWND FindSourceWindow(DWORD ownerPid) {
    HWND fg = GetForegroundWindow();
    if (fg) fg = GetAncestor(fg, GA_ROOT);
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    if (!ownerPid || fgPid == ownerPid) return fg;
    FindCtx ctx{ownerPid, nullptr};
    EnumWindows(FindTopWindowOfPid, (LPARAM)&ctx);
    return ctx.found ? ctx.found : fg;
}

// ---------------------------------------------------------------- clipboard reading

static std::wstring MakeSnippet(const wchar_t* p, size_t len) {
    std::wstring s;
    s.reserve(640);
    size_t i = 0;
    // skip leading whitespace / blank lines
    while (i < len && (p[i] == L' ' || p[i] == L'\t' || p[i] == L'\r' || p[i] == L'\n')) i++;
    int newlines = 0;
    for (; i < len && s.size() < 600; i++) {
        wchar_t c = p[i];
        if (c == L'\r') continue;
        if (c == L'\n') { if (++newlines <= 2) s += c; continue; }
        newlines = 0;
        if (c == L'\t') { s += L"    "; continue; }
        if (c < 0x20 || c == 0x7F) continue;
        s += c;
    }
    while (!s.empty() && (s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s;
}

static void ReadText(ClipInfo& ci, const wchar_t* p, size_t len) {
    ci.kind = CLIP_TEXT;
    size_t chars = 0, nl = 0;
    for (size_t i = 0; i < len; i++) {
        wchar_t c = p[i];
        if (c == L'\n') nl++;
        if (c == L'\r' && i + 1 < len && p[i + 1] == L'\n') continue;
        if (c >= 0xDC00 && c <= 0xDFFF) continue;   // low surrogate: counted with its pair
        chars++;
    }
    ci.charCount = chars;
    ci.lineCount = len == 0 ? 0 : nl + 1;
    if (len > 0 && p[len - 1] == L'\n') ci.lineCount--;
    if (ci.lineCount == 0 && chars > 0) ci.lineCount = 1;
    ci.snippet = MakeSnippet(p, len);
}

static bool ReadDib(ClipInfo& ci, HANDLE h) {
    SIZE_T size = GlobalSize(h);
    const BYTE* p = (const BYTE*)GlobalLock(h);
    if (!p) return false;
    bool ok = false;
    if (size >= sizeof(BITMAPINFOHEADER)) {
        auto bih = (const BITMAPINFOHEADER*)p;
        if (bih->biSize >= sizeof(BITMAPINFOHEADER) && bih->biSize <= size) {
            size_t off = bih->biSize;
            if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) off += 12;
            DWORD colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? (1u << bih->biBitCount) : 0);
            off += colors * sizeof(RGBQUAD);
            ci.kind = CLIP_IMAGE;
            ci.imgW = bih->biWidth;
            ci.imgH = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
            if (off < size && (bih->biCompression == BI_RGB || bih->biCompression == BI_BITFIELDS)) {
                ci.dib.assign(p, p + size);
                ci.dibBitsOffset = off;
            }
            ok = true;
        }
    }
    GlobalUnlock(h);
    return ok;
}

static std::wstring FormatDisplayName(UINT f) {
    switch (f) {
    case CF_BITMAP: case CF_DIB: case CF_DIBV5: return L"Bitmap";
    case CF_ENHMETAFILE: case CF_METAFILEPICT: return L"Picture (metafile)";
    case CF_WAVE: case CF_RIFF: return L"Audio";
    case CF_TEXT: case CF_OEMTEXT: case CF_UNICODETEXT: return L"Text";
    case CF_HDROP: return L"Files";
    }
    wchar_t buf[128];
    if (GetClipboardFormatNameW(f, buf, 128) > 0) return buf;
    return L"Format " + std::to_wstring(f);
}

static void ReadContent(ClipInfo& ci) {
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
        if (hd) {
            UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            ci.kind = CLIP_FILES;
            ci.fileCount = n;
            ci.typesKnown = true;
            for (UINT i = 0; i < n && i < 64; i++) {
                UINT len = DragQueryFileW(hd, i, nullptr, 0);
                std::wstring path(len + 1, L'\0');
                DragQueryFileW(hd, i, &path[0], len + 1);
                path.resize(len);
                if (ci.files.size() < 4) ci.files.push_back(path);
                // Avoid touching network paths (could block on offline shares).
                if (path.size() > 1 && path[0] == L'\\' && path[1] == L'\\') { ci.typesKnown = false; continue; }
                DWORD a = GetFileAttributesW(path.c_str());
                if (a == INVALID_FILE_ATTRIBUTES) ci.typesKnown = false;
                else if (a & FILE_ATTRIBUTE_DIRECTORY) ci.folderCount++;
                else ci.plainFileCount++;
            }
            if (n > 64) ci.typesKnown = false;
            return;
        }
    }
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) {
                size_t len = wcsnlen(p, GlobalSize(h) / sizeof(wchar_t));
                bool useText = len > 0 || !IsClipboardFormatAvailable(CF_DIB);
                if (useText) ReadText(ci, p, len);
                GlobalUnlock(h);
                if (useText) return;
            }
        }
    }
    if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE h = GetClipboardData(CF_DIB);
        if (h && ReadDib(ci, h)) return;
    }
    ci.kind = CLIP_OTHER;
    UINT f = EnumClipboardFormats(0);
    ci.formatName = f ? FormatDisplayName(f) : L"Unknown";
}

// True if the (open) clipboard holds anything besides OLE bookkeeping and marker formats.
static bool HasRealContent() {
    static const wchar_t* noise[] = {L"DataObject", L"Ole Private Data", L"OleClipboardPersistOnFlush",
                                     L"CanUploadToCloudClipboard"};
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        if (f == CF_LOCALE || f == s_cfExclude || f == s_cfViewerIgnore || f == s_cfHistory) continue;
        wchar_t name[128];
        bool isNoise = false;
        if (f >= 0xC000 && GetClipboardFormatNameW(f, name, 128) > 0)
            for (auto n : noise) if (wcscmp(name, n) == 0) { isNoise = true; break; }
        if (!isNoise) return true;
    }
    return false;
}

static void ProcessClipboard(HWND hwnd) {
    DWORD seq = GetClipboardSequenceNumber();
    if (seq == s_lastSeq) return;

    HWND owner = GetClipboardOwner();
    DWORD ownerPid = 0;
    if (owner) GetWindowThreadProcessId(owner, &ownerPid);
    if (ownerPid == GetCurrentProcessId()) { s_lastSeq = seq; return; }

    HWND src = FindSourceWindow(ownerPid);
    DWORD srcPid = 0;
    if (src) GetWindowThreadProcessId(src, &srcPid);
    std::wstring exePath = GetProcessExePath(ownerPid ? ownerPid : srcPid);
    std::wstring exe = FileNameOf(exePath);

    if (g_settings.paused) { s_lastSeq = seq; return; }

    // Clipboard was just emptied: nothing to show.
    if (CountClipboardFormats() == 0) { s_lastSeq = seq; return; }

    if (!exe.empty() && exe != g_lastSourceExe) {
        g_lastSourceExe = exe;
        if (HWND dlg = GetSettingsWindow()) PostMessageW(dlg, WM_APP_LASTSOURCE, 0, 0);
    }
    if (IsIgnored(exe)) { s_lastSeq = seq; return; }

    // Sensitive markers. Per the docs, when ExcludeClipboardContentFromMonitorProcessing is
    // present a monitor should not read the content at all.
    bool sensitive = IsClipboardFormatAvailable(s_cfExclude) || IsClipboardFormatAvailable(s_cfViewerIgnore);

    if (!OpenClipboard(hwnd)) {
        if (++s_openRetries < 8) SetTimer(hwnd, TIMER_CLIP, 40, nullptr);
        return;
    }
    if (!HasRealContent()) {   // e.g. OleSetClipboard(NULL) leaves only OLE bookkeeping formats
        CloseClipboard();
        s_lastSeq = GetClipboardSequenceNumber();
        return;
    }
    ClipInfo ci;
    if (!sensitive && IsClipboardFormatAvailable(s_cfHistory)) {
        HANDLE h = GetClipboardData(s_cfHistory);
        if (h && GlobalSize(h) >= sizeof(DWORD)) {
            const DWORD* v = (const DWORD*)GlobalLock(h);
            if (v) { if (*v == 0) sensitive = true; GlobalUnlock(h); }
        }
    }
    if (sensitive) ci.kind = CLIP_SENSITIVE;
    else ReadContent(ci);
    CloseClipboard();
    s_lastSeq = GetClipboardSequenceNumber();

    ci.appName = GetFriendlyAppName(exePath);

    RECT anchor;
    bool haveAnchor = false;
    if (src && IsCandidateWindow(src) && !IsDesktopWindowClass(src)) haveAnchor = GetVisualRect(src, &anchor);

    if (g_settings.flashEnabled && src && (IsCandidateWindow(src) || IsDesktopWindowClass(src))) Overlay_Flash(src);
    if (g_settings.toastEnabled) Toast_Show(ci, haveAnchor ? &anchor : nullptr);
    PlayCopySound();
}

void RunDemo(HWND target) {
    ClipInfo ci;
    const wchar_t* sample = L"VisualCopy+ test — this is what a copied text preview looks like.\n"
                            L"Second line of the sample snippet.\nThird line.\nFourth line that gets cut.";
    ReadText(ci, sample, wcslen(sample));
    ci.appName = APP_NAME;
    if (g_settings.flashEnabled) Overlay_Flash(target);
    RECT rc;
    if (g_settings.toastEnabled) Toast_Show(ci, GetVisualRect(target, &rc) ? &rc : nullptr);
    PlayCopySound();
}

// ---------------------------------------------------------------- main window

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwndMain = hwnd;
        AddClipboardFormatListener(hwnd);
        TrayAdd();
        return 0;

    case WM_CLIPBOARDUPDATE:
        s_openRetries = 0;
        SetTimer(hwnd, TIMER_CLIP, 60, nullptr);   // debounce multi-step clipboard writes
        return 0;

    case WM_TIMER:
        if (wp == TIMER_CLIP) { KillTimer(hwnd, TIMER_CLIP); ProcessClipboard(hwnd); }
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowSettingsWindow();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu((short)LOWORD(wp), (short)HIWORD(wp));
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS: ShowSettingsWindow(); break;
        case IDM_PAUSE:
            g_settings.paused = !g_settings.paused;
            SaveSettings();
            TrayUpdateTip();
            break;
        case IDM_IGNORE_LAST:
            if (!g_lastSourceExe.empty()) AddIgnored(g_lastSourceExe);
            break;
        case IDM_AUTOSTART: SetAutostart(!IsAutostartEnabled()); break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        }
        if (HWND dlg = GetSettingsWindow()) PostMessageW(dlg, WM_APP_LASTSOURCE, 0, 0);
        return 0;

    case WM_APP_SHOWSETTINGS:
        ShowSettingsWindow();
        return 0;

    case WM_APP_EXIT:
        DestroyWindow(hwnd);
        return 0;

    case WM_DISPLAYCHANGE:
        TrayRemove();
        TrayAdd();
        return 0;

    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        TrayRemove();
        if (HWND dlg = GetSettingsWindow()) DestroyWindow(dlg);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == s_msgTaskbarCreated && msg) { TrayAdd(); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    g_hInst = hInst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // normally set by manifest

    bool wantSettings = cmdLine && wcsstr(cmdLine, L"--settings");
    bool wantExit = cmdLine && wcsstr(cmdLine, L"--exit");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VisualCopyPlus.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(MAIN_WND_CLASS, nullptr);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, wantExit ? WM_APP_EXIT : WM_APP_SHOWSETTINGS, 0, 0);
        }
        return 0;
    }
    if (wantExit) return 0;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    LoadSettings();
    RefreshAutostartPath();

    s_cfExclude      = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    s_cfViewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    s_cfHistory      = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    s_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    s_lastSeq = GetClipboardSequenceNumber();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.lpszClassName = MAIN_WND_CLASS;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);
    CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_WND_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0,
                    nullptr, nullptr, hInst, nullptr);
    if (!g_hwndMain) return 1;

    if (g_firstRun) SaveSettings();
    if (wantSettings || g_firstRun) ShowSettingsWindow();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND dlg = GetSettingsWindow();
        if (dlg && IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
