#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cmath>

#define APP_NAME        L"VisualCopy+"
#define APP_REG_KEY     L"Software\\VisualCopyPlus"
#define MAIN_WND_CLASS  L"VisualCopyPlus.Main"

#define WM_APP_TRAY          (WM_APP + 1)
#define WM_APP_SHOWSETTINGS  (WM_APP + 2)
#define WM_APP_EXIT          (WM_APP + 3)
#define WM_APP_LASTSOURCE    (WM_APP + 4)   // sent to the settings dialog
#define WM_APP_COPYKEY       (WM_APP + 5)   // wParam: 1 = cut, 0 = copy; lParam: clipboard sequence at key press

// Values are persisted: append new styles at the end.
enum FlashStyle { FLASH_BORDER = 0, FLASH_GLOW, FLASH_FILL, FLASH_CORNERS, FLASH_TOPBAR, FLASH_BOTTOMBAR, FLASH_STYLE_COUNT };
enum SoundKind  { SOUND_TICK = 0, SOUND_SYSTEM, SOUND_CUSTOM, SOUND_KIND_COUNT };

struct Settings {
    bool flashEnabled   = true;
    int  flashStyle     = FLASH_GLOW;
    int  flashDuration  = 1;            // 0 short, 1 normal, 2 long
    bool useAccent      = true;
    COLORREF customColor = RGB(255, 160, 0);
    int  opacity        = 70;           // percent, 10..100
    bool animate        = true;
    bool toastEnabled   = true;
    bool toastPreview   = true;
    int  toastDuration  = 1;            // index into toast hold table
    bool soundEnabled   = false;
    int  soundKind      = SOUND_TICK;
    std::wstring soundFile;
    std::vector<std::wstring> ignoreList;   // exe file names, case-insensitive
    bool paused         = false;
    bool labelCut       = true;   // say "cut" when Ctrl+X was pressed
    bool warnNoCopy     = true;   // warn when Ctrl+C / Ctrl+Insert left the clipboard unchanged
    bool quietFullscreen = true;  // no cues while a full-screen app or presentation is running
    bool skipRepeats    = true;   // ignore identical re-writes and bursts not triggered by a key press
};

enum ClipKind { CLIP_TEXT, CLIP_IMAGE, CLIP_FILES, CLIP_SENSITIVE, CLIP_OTHER, CLIP_FAILED };

#define WARNING_COLOR RGB(240, 120, 20)

struct ClipInfo {
    ClipKind kind = CLIP_OTHER;
    bool isCut = false;
    unsigned long long hash = 0;   // content fingerprint, 0 = unknown
    // text
    std::wstring snippet;
    size_t charCount = 0, lineCount = 0;
    // image
    int imgW = 0, imgH = 0;
    std::vector<BYTE> dib;     // packed DIB (header + color table/masks + bits)
    size_t dibBitsOffset = 0;
    // files
    std::vector<std::wstring> files;   // first few full paths
    size_t fileCount = 0;
    int folderCount = 0, plainFileCount = 0; bool typesKnown = false;
    // other
    std::wstring formatName;
    // source
    std::wstring appName;
};

// globals (main.cpp / settings.cpp)
extern HINSTANCE    g_hInst;
extern HWND         g_hwndMain;
extern Settings     g_settings;
extern std::wstring g_lastSourceExe;
extern bool         g_firstRun;

// settings.cpp
void LoadSettings();
void SaveSettings();
bool IsAutostartEnabled();
void SetAutostart(bool on);
void RefreshAutostartPath();
void ShowSettingsWindow();
HWND GetSettingsWindow();
bool IsIgnored(const std::wstring& exe);
void AddIgnored(const std::wstring& exe);

// main.cpp
void RunDemo(HWND target);
void UpdateKeyHook();

// overlay.cpp
void Overlay_Flash(HWND target, const COLORREF* overrideColor = nullptr);

// toast.cpp
void Toast_Show(const ClipInfo& info, const RECT* anchor);

// sound.cpp
void PlayCopySound();
void PlaySoundKind(int kind);

// util.cpp
COLORREF     GetAccentColor();
COLORREF     GetFlashColor();
bool         IsDarkMode();
bool         IsWin11();
std::wstring GetProcessExePath(DWORD pid);
std::wstring FileNameOf(const std::wstring& path);
std::wstring GetFriendlyAppName(const std::wstring& exePath);
std::wstring FormatNumber(size_t n);
UINT         DpiForRect(const RECT& rc);
bool         GetVisualRect(HWND hwnd, RECT* rc);
bool         IsDesktopWindowClass(HWND hwnd);

// Signed distance from point (px,py) (relative to the rect centre) to a rounded
// rectangle with half extents (hx,hy) and corner radius r. Negative inside.
inline float SdRoundRect(float px, float py, float hx, float hy, float r) {
    float qx = std::fabs(px) - (hx - r), qy = std::fabs(py) - (hy - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    if (inside > 0) inside = 0;
    return std::sqrt(ox * ox + oy * oy) + inside - r;
}

inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

inline COLORREF MixColor(COLORREF a, COLORREF b, float t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t + 0.5f),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t + 0.5f),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t + 0.5f));
}
