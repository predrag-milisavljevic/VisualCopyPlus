#include "app.h"
#include <algorithm>

// A click-through, top-most layered window drawn over the source window's visual bounds.
// The shape is rendered once into a premultiplied 32bpp DIB; the fade only changes the
// window's constant alpha, so animation is cheap even for 4K windows.

static HWND s_wnd;
static HDC s_dc;
static HBITMAP s_bmp, s_oldBmp;
static ULONGLONG s_t0;
static DWORD s_dur;
static bool s_anim;
static BYTE s_peak;
static const DWORD DURATIONS[] = {320, 560, 950};

static void FreeSurface() {
    if (s_dc) {
        SelectObject(s_dc, s_oldBmp);
        DeleteObject(s_bmp);
        DeleteDC(s_dc);
        s_dc = nullptr; s_bmp = nullptr;
    }
}

static void SetAlpha(BYTE a) {
    BLENDFUNCTION bf{AC_SRC_OVER, 0, a, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_wnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

static void Tick() {
    float t = (float)(GetTickCount64() - s_t0) / (float)s_dur;
    if (t >= 1.f) {
        KillTimer(s_wnd, 1);
        ShowWindow(s_wnd, SW_HIDE);
        FreeSurface();
        return;
    }
    float f = 1.f;
    if (s_anim) {
        const float fadeIn = 0.18f, hold = 0.38f;
        if (t < fadeIn) { float u = t / fadeIn; f = 1 - (1 - u) * (1 - u); }
        else if (t > hold) { float u = (t - hold) / (1 - hold); f = (1 - u) * (1 - u); }
    }
    SetAlpha((BYTE)(s_peak * f + 0.5f));
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER: Tick(); return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void EnsureWindow() {
    if (s_wnd) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"VisualCopyPlus.Flash";
    RegisterClassExW(&wc);
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
}

static void Render(DWORD* bits, int w, int h, int style, COLORREF col, float s, float radius) {
    const float R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    auto pack = [&](float v) -> DWORD {
        v = Clamp01(v);
        DWORD a = (DWORD)(v * 255 + 0.5f);
        return (a << 24) | ((DWORD)(R * v + 0.5f) << 16) | ((DWORD)(G * v + 0.5f) << 8) | (DWORD)(B * v + 0.5f);
    };
    const float hx = w * 0.5f, hy = h * 0.5f;
    const float thick = std::max(2.f, 5.f * s);
    float band;
    switch (style) {
    case FLASH_GLOW: band = 34.f * s; break;
    case FLASH_FILL: band = 3.f * s + 2; break;
    case FLASH_TOPBAR:
    case FLASH_BOTTOMBAR: band = 30.f * s; break;
    default:         band = thick + 2; break;
    }
    const float cornerLen = std::min(std::max(std::min(w, h) * 0.16f, 22.f * s), 90.f * s);
    const int ib = std::min((int)std::ceil(std::max(band, radius + 2)) + 1, std::max(w, h));
    const DWORD fillPix = pack(0.36f);

    auto value = [&](int x, int y) -> float {
        float d = -SdRoundRect(x + 0.5f - hx, y + 0.5f - hy, hx, hy, radius);   // distance inside
        if (d <= -0.5f) return 0;
        float cov = std::min(1.f, d + 0.5f);
        float v = 0;
        switch (style) {
        case FLASH_BORDER:
            v = Clamp01(thick - d + 0.5f);
            break;
        case FLASH_GLOW: {
            float core = Clamp01(2.f * s - d + 0.5f);
            float g = 1.f - d / band;
            g = g > 0 ? g * g * 0.9f : 0;
            v = std::max(core, g);
            break;
        }
        case FLASH_FILL:
            v = std::max(Clamp01(3.f * s - d + 0.5f), 0.36f);
            break;
        case FLASH_CORNERS: {
            bool inCorner = (x < cornerLen || x >= w - cornerLen) && (y < cornerLen || y >= h - cornerLen);
            v = inCorner ? Clamp01(thick - d + 0.5f) : 0;
            break;
        }
        case FLASH_TOPBAR:
        case FLASH_BOTTOMBAR: {
            // Solid strip on one edge fading into the window, like a light bar.
            float e = style == FLASH_TOPBAR ? y + 0.5f : h - (y + 0.5f);
            float solid = 4.f * s;
            float g = e < solid ? 1.f : 1.f - (e - solid) / (band - solid);
            v = g > 0 ? g * g : 0;
            break;
        }
        }
        return v * cov;
    };

    for (int y = 0; y < h; y++) {
        DWORD* row = bits + (size_t)y * w;
        if (y < ib || y >= h - ib) {
            for (int x = 0; x < w; x++) { float v = value(x, y); if (v > 0) row[x] = pack(v); }
        } else {
            int left = std::min(ib, w), right = std::max(w - ib, left);
            for (int x = 0; x < left; x++) { float v = value(x, y); if (v > 0) row[x] = pack(v); }
            if (style == FLASH_FILL) std::fill(row + left, row + right, fillPix);
            for (int x = right; x < w; x++) { float v = value(x, y); if (v > 0) row[x] = pack(v); }
        }
    }
}

void Overlay_Flash(HWND target, const COLORREF* overrideColor) {
    if (!target) return;
    RECT rc;
    HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    bool desktop = IsDesktopWindowClass(target);
    if (desktop) {
        POINT pt; GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    bool maximized = IsZoomed(target) != FALSE;
    if (desktop) rc = mi.rcWork;
    else if (!GetVisualRect(target, &rc)) return;
    if (maximized) IntersectRect(&rc, &rc, &mi.rcMonitor);
    bool fullscreen = EqualRect(&rc, &mi.rcMonitor) != FALSE;

    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 8 || h < 8) return;
    float s = DpiForRect(rc) / 96.f;
    float radius = (IsWin11() && !maximized && !desktop && !fullscreen) ? 8.f * s : 0.f;

    EnsureWindow();
    KillTimer(s_wnd, 1);
    FreeSurface();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    s_dc = CreateCompatibleDC(screen);
    s_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!s_bmp || !bits) { if (s_bmp) DeleteObject(s_bmp); DeleteDC(s_dc); s_dc = nullptr; s_bmp = nullptr; return; }
    s_oldBmp = (HBITMAP)SelectObject(s_dc, s_bmp);

    Render((DWORD*)bits, w, h, g_settings.flashStyle, overrideColor ? *overrideColor : GetFlashColor(), s, radius);

    s_peak = (BYTE)(std::min(100, std::max(10, g_settings.opacity)) * 255 / 100);
    s_anim = g_settings.animate;
    s_dur = DURATIONS[std::min(2, std::max(0, g_settings.flashDuration))];
    s_t0 = GetTickCount64();

    POINT dst{rc.left, rc.top}, src{0, 0};
    SIZE sz{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)(s_anim ? 0 : s_peak), AC_SRC_ALPHA};
    UpdateLayeredWindow(s_wnd, nullptr, &dst, &sz, s_dc, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(s_wnd, 1, 10, nullptr);
}
