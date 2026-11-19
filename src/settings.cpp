#include "app.h"
#include "../res/resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>

Settings g_settings;
bool g_firstRun = false;

static HWND s_dlg;
static COLORREF s_customColors[16];
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"VisualCopyPlus";

// ---------------------------------------------------------------- persistence (HKCU registry)

static std::wstring ReadString(HKEY k, const wchar_t* name, DWORD type) {
    DWORD sz = 0;
    if (RegGetValueW(k, nullptr, name, type, nullptr, nullptr, &sz) != ERROR_SUCCESS || sz == 0) return {};
    std::wstring s(sz / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(k, nullptr, name, type, nullptr, &s[0], &sz) != ERROR_SUCCESS) return {};
    s.resize(sz / sizeof(wchar_t));
    return s;
}

void LoadSettings() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) { g_firstRun = true; return; }
    auto D = [&](const wchar_t* n, DWORD def) {
        DWORD v = def, sz = sizeof(v);
        if (RegGetValueW(k, nullptr, n, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) v = def;
        return v;
    };
    Settings& s = g_settings;
    s.flashEnabled  = D(L"FlashEnabled", s.flashEnabled) != 0;
    s.flashStyle    = std::min<int>(D(L"FlashStyle", s.flashStyle), FLASH_STYLE_COUNT - 1);
    s.flashDuration = std::min<int>(D(L"FlashDuration", s.flashDuration), 2);
    s.useAccent     = D(L"UseAccent", s.useAccent) != 0;
    s.customColor   = D(L"CustomColor", s.customColor) & 0xFFFFFF;
    s.opacity       = std::max(10, std::min<int>(D(L"Opacity", s.opacity), 100));
    s.animate       = D(L"Animate", s.animate) != 0;
    s.toastEnabled  = D(L"ToastEnabled", s.toastEnabled) != 0;
    s.toastPreview  = D(L"ToastPreview", s.toastPreview) != 0;
    s.toastDuration = std::min<int>(D(L"ToastDuration", s.toastDuration), 3);
    s.soundEnabled  = D(L"SoundEnabled", s.soundEnabled) != 0;
    s.soundKind     = std::min<int>(D(L"SoundKind", s.soundKind), SOUND_KIND_COUNT - 1);
    s.paused        = D(L"Paused", s.paused) != 0;
    s.soundFile     = ReadString(k, L"SoundFile", RRF_RT_REG_SZ);
    while (!s.soundFile.empty() && s.soundFile.back() == 0) s.soundFile.pop_back();

    std::wstring multi = ReadString(k, L"IgnoreList", RRF_RT_REG_MULTI_SZ);
    s.ignoreList.clear();
    for (size_t p = 0; p < multi.size();) {
        size_t e = multi.find(L'\0', p);
        if (e == std::wstring::npos) e = multi.size();
        if (e > p) s.ignoreList.push_back(multi.substr(p, e - p));
        p = e + 1;
    }
    RegCloseKey(k);
}

void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto D = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); };
    const Settings& s = g_settings;
    D(L"FlashEnabled", s.flashEnabled);
    D(L"FlashStyle", s.flashStyle);
    D(L"FlashDuration", s.flashDuration);
    D(L"UseAccent", s.useAccent);
    D(L"CustomColor", s.customColor);
    D(L"Opacity", s.opacity);
    D(L"Animate", s.animate);
    D(L"ToastEnabled", s.toastEnabled);
    D(L"ToastPreview", s.toastPreview);
    D(L"ToastDuration", s.toastDuration);
    D(L"SoundEnabled", s.soundEnabled);
    D(L"SoundKind", s.soundKind);
    D(L"Paused", s.paused);
    RegSetValueExW(k, L"SoundFile", 0, REG_SZ, (const BYTE*)s.soundFile.c_str(),
                   (DWORD)((s.soundFile.size() + 1) * sizeof(wchar_t)));
    std::wstring multi;
    for (auto& e : s.ignoreList) { multi += e; multi += L'\0'; }
    multi += L'\0';
    RegSetValueExW(k, L"IgnoreList", 0, REG_MULTI_SZ, (const BYTE*)multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
    RegCloseKey(k);
}

bool IsIgnored(const std::wstring& exe) {
    if (exe.empty()) return false;
    for (auto& e : g_settings.ignoreList)
        if (_wcsicmp(e.c_str(), exe.c_str()) == 0) return true;
    return false;
}

static void RefreshIgnoreList();

void AddIgnored(const std::wstring& exe) {
    if (exe.empty() || IsIgnored(exe)) return;
    g_settings.ignoreList.push_back(exe);
    SaveSettings();
    RefreshIgnoreList();
}

// ---------------------------------------------------------------- autostart

static std::wstring ExePath() {
    wchar_t b[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return std::wstring(b, n);
}

static std::wstring AutostartCommand() { return L"\"" + ExePath() + L"\""; }

bool IsAutostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, RUN_VALUE, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = AutostartCommand();
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
    if (s_dlg) CheckDlgButton(s_dlg, IDC_AUTOSTART, on ? BST_CHECKED : BST_UNCHECKED);
}

// If the exe was moved, keep the Run entry pointing at the current location.
void RefreshAutostartPath() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::wstring cur = ReadString(k, RUN_VALUE, RRF_RT_REG_SZ);
    RegCloseKey(k);
    while (!cur.empty() && cur.back() == 0) cur.pop_back();
    if (!cur.empty() && _wcsicmp(cur.c_str(), AutostartCommand().c_str()) != 0) SetAutostart(true);
}

// ---------------------------------------------------------------- settings dialog

HWND GetSettingsWindow() { return s_dlg; }

static bool Checked(HWND h, int id) { return IsDlgButtonChecked(h, id) == BST_CHECKED; }
static void Check(HWND h, int id, bool on) { CheckDlgButton(h, id, on ? BST_CHECKED : BST_UNCHECKED); }
static void Enable(HWND h, int id, bool on) { EnableWindow(GetDlgItem(h, id), on); }

static void RefreshIgnoreList() {
    if (!s_dlg) return;
    HWND lb = GetDlgItem(s_dlg, IDC_IGNORE_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (auto& e : g_settings.ignoreList) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)e.c_str());
    Enable(s_dlg, IDC_IGNORE_REMOVE, SendMessageW(lb, LB_GETCURSEL, 0, 0) != LB_ERR);
    std::wstring t = g_lastSourceExe.empty() ? std::wstring(L"Ignore last source") : L"Ignore " + g_lastSourceExe;
    SetDlgItemTextW(s_dlg, IDC_IGNORE_LAST, t.c_str());
    Enable(s_dlg, IDC_IGNORE_LAST, !g_lastSourceExe.empty() && !IsIgnored(g_lastSourceExe));
}

static void RefreshSoundCombo(HWND h) {
    HWND cb = GetDlgItem(h, IDC_SOUND_KIND);
    std::wstring custom = g_settings.soundFile.empty() ? std::wstring(L"Custom WAV file...")
                                                       : L"Custom: " + FileNameOf(g_settings.soundFile);
    SendMessageW(cb, CB_DELETESTRING, SOUND_CUSTOM, 0);
    SendMessageW(cb, CB_INSERTSTRING, SOUND_CUSTOM, (LPARAM)custom.c_str());
    SendMessageW(cb, CB_SETCURSEL, g_settings.soundKind, 0);
}

static void UpdateEnables(HWND h) {
    const Settings& s = g_settings;
    for (int id : {IDC_FLASH_STYLE, IDC_FLASH_DURATION, IDC_COLOR_ACCENT, IDC_COLOR_CUSTOM, IDC_COLOR_SWATCH,
                   IDC_FLASH_ANIMATE, IDC_OPACITY, IDC_OPACITY_LABEL})
        Enable(h, id, s.flashEnabled);
    Enable(h, IDC_TOAST_PREVIEW, s.toastEnabled);
    Enable(h, IDC_TOAST_DURATION, s.toastEnabled);
    Enable(h, IDC_SOUND_KIND, s.soundEnabled);
    Enable(h, IDC_SOUND_PLAY, s.soundEnabled);
    Enable(h, IDC_SOUND_BROWSE, s.soundEnabled && s.soundKind == SOUND_CUSTOM);
    InvalidateRect(GetDlgItem(h, IDC_COLOR_SWATCH), nullptr, TRUE);
}

static void UpdateOpacityLabel(HWND h) {
    wchar_t b[16];
    swprintf(b, 16, L"%d%%", g_settings.opacity);
    SetDlgItemTextW(h, IDC_OPACITY_LABEL, b);
}

static void InitDialog(HWND h) {
    const Settings& s = g_settings;
    HWND cb = GetDlgItem(h, IDC_FLASH_STYLE);
    for (const wchar_t* t : {L"Border", L"Glow", L"Fill (tint)", L"Corners"}) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(cb, CB_SETCURSEL, s.flashStyle, 0);
    cb = GetDlgItem(h, IDC_FLASH_DURATION);
    for (const wchar_t* t : {L"Short", L"Normal", L"Long"}) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(cb, CB_SETCURSEL, s.flashDuration, 0);
    cb = GetDlgItem(h, IDC_TOAST_DURATION);
    for (const wchar_t* t : {L"1.5 s", L"2.5 s", L"4 s", L"6 s"}) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(cb, CB_SETCURSEL, s.toastDuration, 0);
    cb = GetDlgItem(h, IDC_SOUND_KIND);
    for (const wchar_t* t : {L"Soft tick (built-in)", L"Windows notification", L"Custom WAV file..."})
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    RefreshSoundCombo(h);

    Check(h, IDC_FLASH_ENABLE, s.flashEnabled);
    Check(h, s.useAccent ? IDC_COLOR_ACCENT : IDC_COLOR_CUSTOM, true);
    Check(h, IDC_FLASH_ANIMATE, s.animate);
    Check(h, IDC_TOAST_ENABLE, s.toastEnabled);
    Check(h, IDC_TOAST_PREVIEW, s.toastPreview);
    Check(h, IDC_SOUND_ENABLE, s.soundEnabled);
    Check(h, IDC_AUTOSTART, IsAutostartEnabled());

    HWND tb = GetDlgItem(h, IDC_OPACITY);
    SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(tb, TBM_SETPOS, TRUE, s.opacity);
    UpdateOpacityLabel(h);

    SendDlgItemMessageW(h, IDC_IGNORE_EDIT, EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. KeePassXC.exe");
    RefreshIgnoreList();
    UpdateEnables(h);
}

static std::wstring NormalizeExeName(std::wstring s) {
    s = FileNameOf(s);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"')) s.pop_back();
    while (!s.empty() && (s.front() == L' ' || s.front() == L'"')) s.erase(s.begin());
    if (!s.empty() && s.find(L'.') == std::wstring::npos) s += L".exe";
    return s;
}

static void AddFromEdit(HWND h) {
    wchar_t buf[MAX_PATH];
    GetDlgItemTextW(h, IDC_IGNORE_EDIT, buf, MAX_PATH);
    std::wstring name = NormalizeExeName(buf);
    if (name.empty()) return;
    AddIgnored(name);
    SetDlgItemTextW(h, IDC_IGNORE_EDIT, L"");
}

static bool BrowseFile(HWND h, const wchar_t* filter, std::wstring& out) {
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = h;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = file;
    return true;
}

static void PreviewFlash(HWND h) { if (g_settings.flashEnabled) Overlay_Flash(h); }

static INT_PTR CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Settings& s = g_settings;
    switch (m) {
    case WM_INITDIALOG: {
        s_dlg = h;
        HICON big = nullptr, small = nullptr;
        LoadIconMetric(g_hInst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
        LoadIconMetric(g_hInst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &small);
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);
        InitDialog(h);
        return TRUE;
    }
    case WM_APP_LASTSOURCE:
        RefreshIgnoreList();
        Check(h, IDC_AUTOSTART, IsAutostartEnabled());
        return TRUE;

    case WM_HSCROLL:
        if ((HWND)l == GetDlgItem(h, IDC_OPACITY)) {
            s.opacity = (int)SendMessageW((HWND)l, TBM_GETPOS, 0, 0);
            UpdateOpacityLabel(h);
            if (LOWORD(w) == TB_ENDTRACK) { SaveSettings(); PreviewFlash(h); }
        }
        return TRUE;

    case WM_DRAWITEM: {
        auto di = (const DRAWITEMSTRUCT*)l;
        if (di->CtlID != IDC_COLOR_SWATCH) break;
        RECT r = di->rcItem;
        FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
        HBRUSH frame = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));
        FrameRect(di->hDC, &r, frame);
        InflateRect(&r, -3, -3);
        bool en = !(di->itemState & ODS_DISABLED);
        HBRUSH fill = CreateSolidBrush(en ? GetFlashColor() : GetSysColor(COLOR_GRAYTEXT));
        FillRect(di->hDC, &r, fill);
        DeleteObject(fill);
        DeleteObject(frame);
        if (di->itemState & ODS_FOCUS) { InflateRect(&r, 2, 2); DrawFocusRect(di->hDC, &r); }
        SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        bool clicked = code == BN_CLICKED;
        bool sel = code == CBN_SELCHANGE;
        switch (id) {
        case IDC_FLASH_ENABLE: if (clicked) { s.flashEnabled = Checked(h, id); SaveSettings(); UpdateEnables(h); PreviewFlash(h); } break;
        case IDC_FLASH_STYLE: if (sel) { s.flashStyle = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0); SaveSettings(); PreviewFlash(h); } break;
        case IDC_FLASH_DURATION: if (sel) { s.flashDuration = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0); SaveSettings(); PreviewFlash(h); } break;
        case IDC_COLOR_ACCENT:
        case IDC_COLOR_CUSTOM:
            if (clicked) { s.useAccent = Checked(h, IDC_COLOR_ACCENT); SaveSettings(); UpdateEnables(h); PreviewFlash(h); }
            break;
        case IDC_COLOR_SWATCH:
            if (clicked) {
                CHOOSECOLORW cc{sizeof(cc)};
                cc.hwndOwner = h;
                cc.rgbResult = s.customColor;
                cc.lpCustColors = s_customColors;
                cc.Flags = CC_RGBINIT | CC_FULLOPEN;
                if (ChooseColorW(&cc)) {
                    s.customColor = cc.rgbResult;
                    s.useAccent = false;
                    Check(h, IDC_COLOR_ACCENT, false);
                    Check(h, IDC_COLOR_CUSTOM, true);
                    SaveSettings();
                    UpdateEnables(h);
                    PreviewFlash(h);
                }
            }
            break;
        case IDC_FLASH_ANIMATE: if (clicked) { s.animate = Checked(h, id); SaveSettings(); PreviewFlash(h); } break;
        case IDC_TOAST_ENABLE: if (clicked) { s.toastEnabled = Checked(h, id); SaveSettings(); UpdateEnables(h); } break;
        case IDC_TOAST_PREVIEW: if (clicked) { s.toastPreview = Checked(h, id); SaveSettings(); } break;
        case IDC_TOAST_DURATION: if (sel) { s.toastDuration = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0); SaveSettings(); } break;
        case IDC_SOUND_ENABLE: if (clicked) { s.soundEnabled = Checked(h, id); SaveSettings(); UpdateEnables(h); } break;
        case IDC_SOUND_KIND:
            if (sel) {
                s.soundKind = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0);
                if (s.soundKind == SOUND_CUSTOM && s.soundFile.empty()) {
                    std::wstring f;
                    if (BrowseFile(h, L"WAV sounds (*.wav)\0*.wav\0All files\0*.*\0", f)) s.soundFile = f;
                    else s.soundKind = SOUND_TICK;
                    RefreshSoundCombo(h);
                }
                SaveSettings();
                UpdateEnables(h);
                PlaySoundKind(s.soundKind);
            }
            break;
        case IDC_SOUND_BROWSE:
            if (clicked) {
                std::wstring f;
                if (BrowseFile(h, L"WAV sounds (*.wav)\0*.wav\0All files\0*.*\0", f)) {
                    s.soundFile = f;
                    s.soundKind = SOUND_CUSTOM;
                    RefreshSoundCombo(h);
                    SaveSettings();
                    PlaySoundKind(s.soundKind);
                }
            }
            break;
        case IDC_SOUND_PLAY: if (clicked) PlaySoundKind(s.soundKind); break;
        case IDC_IGNORE_LIST:
            if (code == LBN_SELCHANGE)
                Enable(h, IDC_IGNORE_REMOVE, SendDlgItemMessageW(h, id, LB_GETCURSEL, 0, 0) != LB_ERR);
            break;
        case IDC_IGNORE_REMOVE:
            if (clicked) {
                HWND lb = GetDlgItem(h, IDC_IGNORE_LIST);
                int i = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
                if (i != LB_ERR) {
                    wchar_t buf[MAX_PATH] = L"";
                    if (SendMessageW(lb, LB_GETTEXTLEN, i, 0) < MAX_PATH) SendMessageW(lb, LB_GETTEXT, i, (LPARAM)buf);
                    auto& v = s.ignoreList;
                    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::wstring& e) { return _wcsicmp(e.c_str(), buf) == 0; }), v.end());
                    SaveSettings();
                    RefreshIgnoreList();
                }
            }
            break;
        case IDC_IGNORE_BROWSE:
            if (clicked) {
                std::wstring f;
                if (BrowseFile(h, L"Programs (*.exe)\0*.exe\0All files\0*.*\0", f)) AddIgnored(NormalizeExeName(f));
            }
            break;
        case IDC_IGNORE_LAST: if (clicked) AddIgnored(g_lastSourceExe); break;
        case IDC_IGNORE_ADD: if (clicked) AddFromEdit(h); break;
        case IDOK:   // Enter key
            if (GetFocus() == GetDlgItem(h, IDC_IGNORE_EDIT)) AddFromEdit(h);
            break;
        case IDC_AUTOSTART: if (clicked) SetAutostart(Checked(h, id)); break;
        case IDC_TEST: if (clicked) RunDemo(h); break;
        case IDCANCEL: DestroyWindow(h); break;
        }
        return TRUE;
    }
    case WM_DESTROY: {
        HICON big = (HICON)SendMessageW(h, WM_GETICON, ICON_BIG, 0);
        HICON small = (HICON)SendMessageW(h, WM_GETICON, ICON_SMALL, 0);
        if (big) DestroyIcon(big);
        if (small) DestroyIcon(small);
        s_dlg = nullptr;
        return TRUE;
    }
    }
    return FALSE;
}

void ShowSettingsWindow() {
    if (!s_dlg) {
        CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, SettingsProc, 0);
        if (!s_dlg) return;
    }
    if (IsIconic(s_dlg)) ShowWindow(s_dlg, SW_RESTORE);
    ShowWindow(s_dlg, SW_SHOW);
    SetForegroundWindow(s_dlg);
}
