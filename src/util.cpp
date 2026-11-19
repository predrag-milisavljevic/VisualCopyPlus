#include "app.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <map>
#include <cwchar>

COLORREF GetAccentColor() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentColorMenu", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v & 0x00FFFFFF;  // stored as 0xAABBGGRR, same channel order as COLORREF
    DWORD c = 0; BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&c, &opaque)))
        return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    return RGB(0, 120, 215);
}

COLORREF GetFlashColor() {
    return g_settings.useAccent ? GetAccentColor() : g_settings.customColor;
}

bool IsDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

bool IsWin11() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
        auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW vi{}; vi.dwOSVersionInfoSize = sizeof(vi);
        if (fn && fn(&vi) == 0) cached = (vi.dwMajorVersion > 10 || vi.dwBuildNumber >= 22000) ? 1 : 0;
    }
    return cached == 1;
}

std::wstring GetProcessExePath(DWORD pid) {
    if (!pid) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 4];
    DWORD n = ARRAYSIZE(buf);
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(h);
    return r;
}

std::wstring FileNameOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring GetFriendlyAppName(const std::wstring& exePath) {
    static std::map<std::wstring, std::wstring> cache;
    if (exePath.empty()) return {};
    auto it = cache.find(exePath);
    if (it != cache.end()) return it->second;

    std::wstring name;
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (sz) {
        std::vector<BYTE> buf(sz);
        if (GetFileVersionInfoW(exePath.c_str(), 0, sz, buf.data())) {
            struct LangCp { WORD lang, cp; } *tr = nullptr;
            UINT len = 0;
            wchar_t query[80] = L"\\StringFileInfo\\040904B0\\FileDescription";
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && tr && len >= sizeof(LangCp))
                swprintf(query, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
            wchar_t* val = nullptr;
            if (VerQueryValueW(buf.data(), query, (void**)&val, &len) && val && len > 1) name = val;
        }
    }
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (name.empty()) {
        name = FileNameOf(exePath);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    }
    cache[exePath] = name;
    return name;
}

std::wstring FormatNumber(size_t n) {
    wchar_t sep[8] = L",";
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8)) wcscpy(sep, L",");
    std::wstring d = std::to_wstring(n), out;
    int c = 0;
    for (int i = (int)d.size() - 1; i >= 0; --i) {
        out.insert(out.begin(), d[i]);
        if (++c % 3 == 0 && i > 0) out.insert(0, sep);
    }
    return out;
}

UINT DpiForRect(const RECT& rc) {
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y))) x = 96;
    return x ? x : 96;
}

bool GetVisualRect(HWND hwnd, RECT* rc) {
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, rc, sizeof(*rc))))
        if (!GetWindowRect(hwnd, rc)) return false;
    return rc->right > rc->left && rc->bottom > rc->top;
}

bool IsDesktopWindowClass(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}
