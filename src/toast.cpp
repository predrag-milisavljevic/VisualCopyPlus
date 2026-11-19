#include "app.h"
#include <algorithm>

// Custom-drawn toast: GDI draws content onto an opaque DIB, then a per-pixel pass applies
// the rounded-rect mask, a 1px border and a soft drop shadow (premultiplied alpha).

static HWND s_wnd;
static HDC s_dc;
static HBITMAP s_bmp, s_oldBmp;
enum Phase { P_HIDDEN, P_IN, P_HOLD, P_OUT };
static Phase s_phase = P_HIDDEN;
static ULONGLONG s_t0;
static DWORD s_hold;
static BYTE s_alpha, s_outFrom;
static const DWORD FADE_IN = 110, FADE_OUT = 220;
static const DWORD HOLD_MS[] = {1500, 2500, 4000, 6000};

static void FreeSurface() {
    if (s_dc) {
        SelectObject(s_dc, s_oldBmp);
        DeleteObject(s_bmp);
        DeleteDC(s_dc);
        s_dc = nullptr; s_bmp = nullptr;
    }
}

static void SetAlpha(BYTE a) {
    s_alpha = a;
    BLENDFUNCTION bf{AC_SRC_OVER, 0, a, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_wnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

static void Hide() {
    KillTimer(s_wnd, 1);
    ShowWindow(s_wnd, SW_HIDE);
    FreeSurface();
    s_phase = P_HIDDEN;
}

static void BeginFadeOut() {
    if (s_phase == P_HIDDEN || s_phase == P_OUT) return;
    s_phase = P_OUT;
    s_outFrom = s_alpha;
    s_t0 = GetTickCount64();
}

static void Tick() {
    ULONGLONG now = GetTickCount64();
    DWORD el = (DWORD)(now - s_t0);
    switch (s_phase) {
    case P_IN: {
        float u = std::min(1.f, el / (float)FADE_IN);
        SetAlpha((BYTE)(255 * (1 - (1 - u) * (1 - u))));
        if (u >= 1) { s_phase = P_HOLD; s_t0 = now; }
        break;
    }
    case P_HOLD: {
        POINT pt; RECT rc;
        GetCursorPos(&pt);
        GetWindowRect(s_wnd, &rc);
        if (PtInRect(&rc, pt)) s_t0 = now;          // keep visible while hovered
        else if (el >= s_hold) BeginFadeOut();
        break;
    }
    case P_OUT: {
        float u = std::min(1.f, el / (float)FADE_OUT);
        SetAlpha((BYTE)(s_outFrom * (1 - u)));
        if (u >= 1) Hide();
        break;
    }
    default: break;
    }
}

static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER: Tick(); return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        BeginFadeOut();
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void EnsureWindow() {
    if (s_wnd) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VisualCopyPlus.Toast";
    RegisterClassExW(&wc);
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
}

static int CALLBACK FontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *(bool*)lp = true;
    return 0;
}

static bool HasFont(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static const wchar_t* IconFontFace() {
    static const wchar_t* face = nullptr;
    if (!face) face = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return face;
}

static HFONT MakeFont(int px, int weight, const wchar_t* face) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static int LineHeight(HDC dc, HFONT f) {
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    return tm.tmHeight;
}

// Word-wraps text into at most maxLines lines. The last line carries the remainder so the
// caller can draw it with an end ellipsis.
static std::vector<std::wstring> WrapLines(HDC dc, const std::wstring& text, int maxW, int maxLines) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    const size_t n = text.size();
    while (pos < n && (int)out.size() < maxLines) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = n;
        std::wstring para = text.substr(pos, eol - pos);
        pos = eol < n ? eol + 1 : n;
        if (para.empty()) { out.push_back(L""); continue; }
        size_t p = 0;
        while (p < para.size() && (int)out.size() < maxLines) {
            if ((int)out.size() == maxLines - 1) {
                std::wstring rest = para.substr(p);
                if (pos < n) {
                    std::wstring more = text.substr(pos, 200);
                    for (auto& c : more) if (c == L'\n') c = L' ';
                    rest += L"  ↵ " + more;
                }
                out.push_back(rest);
                return out;
            }
            int fit = 0;
            SIZE sz;
            int remain = (int)(para.size() - p);
            GetTextExtentExPointW(dc, para.c_str() + p, remain, maxW, &fit, nullptr, &sz);
            if (fit >= remain) { out.push_back(para.substr(p)); break; }
            if (fit < 1) fit = 1;
            int brk = fit;
            while (brk > 0 && para[p + brk] != L' ') brk--;
            if (brk <= 0) brk = fit;
            out.push_back(para.substr(p, brk));
            p += brk;
            while (p < para.size() && para[p] == L' ') p++;
        }
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

static std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return FormatNumber(n) + L" " + (n == 1 ? one : many);
}

void Toast_Show(const ClipInfo& ci, const RECT* anchorIn) {
    RECT anchor;
    MONITORINFO mi{sizeof(mi)};
    if (anchorIn) {
        anchor = *anchorIn;
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    } else {
        POINT pt; GetCursorPos(&pt);
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        anchor = mi.rcWork;
    }
    const UINT dpi = DpiForRect(anchor);
    auto S = [&](double v) { return (int)std::lround(v * dpi / 96.0); };

    const bool dark = IsDarkMode();
    const COLORREF bg     = dark ? RGB(43, 43, 43) : RGB(251, 251, 251);
    const COLORREF fg     = dark ? RGB(255, 255, 255) : RGB(27, 27, 27);
    const COLORREF fg2    = dark ? RGB(168, 168, 168) : RGB(96, 96, 96);
    const COLORREF border = dark ? RGB(72, 72, 72) : RGB(214, 214, 214);
    const COLORREF accent = ci.kind == CLIP_FAILED ? WARNING_COLOR : GetFlashColor();
    const wchar_t* verb = ci.isCut ? L" cut" : L" copied";
    const COLORREF accentFg = dark ? MixColor(accent, RGB(255, 255, 255), 0.35f) : MixColor(accent, RGB(0, 0, 0), 0.12f);
    const COLORREF tile   = MixColor(bg, accent, dark ? 0.24f : 0.14f);

    const bool preview = g_settings.toastPreview;
    const int W = S(344), P = S(14), gap = S(12), margin = S(16), radius = S(8);

    // ---- content strings
    std::wstring title, meta, glyph;
    std::vector<std::wstring> fileLines;
    switch (ci.kind) {
    case CLIP_TEXT:
        title = std::wstring(L"Text") + verb;
        glyph = ci.isCut ? L"" : L"";
        meta = ci.charCount ? Plural(ci.charCount, L"character", L"characters") + L"  ·  " +
                                  Plural(ci.lineCount, L"line", L"lines")
                            : L"Empty text";
        break;
    case CLIP_IMAGE:
        title = std::wstring(L"Image") + verb;
        glyph = ci.isCut ? L"" : L"";
        meta = FormatNumber(ci.imgW) + L" × " + FormatNumber(ci.imgH) + L" px";
        break;
    case CLIP_FILES: {
        glyph = L"";
        size_t n = ci.fileCount;
        if (ci.typesKnown && ci.folderCount == 0) title = Plural(n, L"file", L"files");
        else if (ci.typesKnown && ci.plainFileCount == 0) title = Plural(n, L"folder", L"folders");
        else title = Plural(n, L"item", L"items");
        title += verb;
        if (ci.isCut) glyph = L"";
        if (ci.typesKnown && ci.folderCount && ci.plainFileCount)
            meta = Plural(ci.plainFileCount, L"file", L"files") + L", " + Plural(ci.folderCount, L"folder", L"folders");
        if (preview) {
            size_t show = n <= 3 ? n : 2;
            for (size_t i = 0; i < show && i < ci.files.size(); i++) fileLines.push_back(FileNameOf(ci.files[i]));
            if (n > show) fileLines.push_back(L"+ " + FormatNumber(n - show) + L" more");
        }
        break;
    }
    case CLIP_FAILED:
        title = ci.isCut ? L"Nothing was cut" : L"Nothing was copied";
        glyph = L"";
        meta = ci.isCut ? L"Ctrl+X didn't change the clipboard" : L"Ctrl+C didn't change the clipboard";
        break;
    case CLIP_SENSITIVE:
        title = ci.isCut ? L"Cut" : L"Copied";
        glyph = L"";
        meta = L"Preview hidden (sensitive)";
        break;
    default:
        title = ci.isCut ? L"Cut" : L"Copied";
        glyph = ci.isCut ? L"" : L"";
        meta = ci.formatName;
        break;
    }
    if (!ci.appName.empty()) meta = meta.empty() ? ci.appName : meta + L"  ·  " + ci.appName;

    // ---- fonts & measuring
    HFONT fTitle = MakeFont(S(14.5), FW_SEMIBOLD, L"Segoe UI");
    HFONT fMeta  = MakeFont(S(12), FW_NORMAL, L"Segoe UI");
    HFONT fBody  = MakeFont(S(13), FW_NORMAL, L"Segoe UI");
    HFONT fGlyph = MakeFont(S(18), FW_NORMAL, IconFontFace());

    bool thumb = ci.kind == CLIP_IMAGE && preview && !ci.dib.empty() && ci.imgW > 0 && ci.imgH > 0;
    int iconW = S(36), iconH = S(36);
    if (thumb) {
        double sc = std::min(S(96) / (double)ci.imgW, S(72) / (double)ci.imgH);
        sc = std::min(sc, 4.0);
        iconW = std::max(1, (int)std::lround(ci.imgW * sc));
        iconH = std::max(1, (int)std::lround(ci.imgH * sc));
    }
    const int textX = P + std::max(iconW, S(36)) + gap;
    const int textW = W - textX - P;

    HDC screen = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(screen);
    const int titleH = LineHeight(mdc, fTitle);
    const int metaH = meta.empty() ? 0 : LineHeight(mdc, fMeta);
    const int bodyLH = LineHeight(mdc, fBody);

    std::vector<std::wstring> bodyLines;
    if (ci.kind == CLIP_TEXT && preview && !ci.snippet.empty()) {
        HGDIOBJ old = SelectObject(mdc, fBody);
        bodyLines = WrapLines(mdc, ci.snippet, textW, 3);
        SelectObject(mdc, old);
    } else if (!fileLines.empty()) {
        bodyLines = fileLines;
    }
    const int bodyH = (int)bodyLines.size() * bodyLH;
    const int contentH = titleH + (metaH ? S(1) + metaH : 0) + (bodyH ? S(7) + bodyH : 0);
    const int H = P * 2 + std::max(contentH, iconH);

    // ---- surface
    const int BW = W + 2 * margin, BH = H + 2 * margin;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = BW;
    bi.bmiHeader.biHeight = -BH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bitsV = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bitsV, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bitsV) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mdc);
        DeleteObject(fTitle); DeleteObject(fMeta); DeleteObject(fBody); DeleteObject(fGlyph);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(mdc, bmp);

    const int ox = margin, oy = margin;
    RECT card{ox, oy, ox + W, oy + H};
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(mdc, &card, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(mdc, TRANSPARENT);

    const int top = oy + P;
    const int iconBoxW = std::max(iconW, S(36));
    int iconY = contentH >= iconH ? top : oy + (H - iconH) / 2;
    int textY = contentH >= iconH ? top : oy + (H - contentH) / 2;

    if (thumb) {
        int ix = ox + P + (iconBoxW - iconW) / 2;
        SetStretchBltMode(mdc, HALFTONE);
        SetBrushOrgEx(mdc, 0, 0, nullptr);
        StretchDIBits(mdc, ix, iconY, iconW, iconH, 0, 0, ci.imgW, ci.imgH,
                      ci.dib.data() + ci.dibBitsOffset, (const BITMAPINFO*)ci.dib.data(), DIB_RGB_COLORS, SRCCOPY);
        RECT fr{ix, iconY, ix + iconW, iconY + iconH};
        HBRUSH bb = CreateSolidBrush(border);
        FrameRect(mdc, &fr, bb);
        DeleteObject(bb);
    } else {
        RECT ir{ox + P, iconY, ox + P + S(36), iconY + S(36)};
        HBRUSH tb = CreateSolidBrush(tile);
        HGDIOBJ oldBrush = SelectObject(mdc, tb);
        HGDIOBJ oldPen = SelectObject(mdc, GetStockObject(NULL_PEN));
        RoundRect(mdc, ir.left, ir.top, ir.right + 1, ir.bottom + 1, S(10), S(10));
        SelectObject(mdc, oldPen);
        SelectObject(mdc, oldBrush);
        DeleteObject(tb);
        SelectObject(mdc, fGlyph);
        SetTextColor(mdc, accentFg);
        DrawTextW(mdc, glyph.c_str(), -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    const UINT lineFlags = DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_LEFT | DT_TOP;
    int y = textY;
    RECT r{ox + textX, y, ox + textX + textW, y + titleH};
    SelectObject(mdc, fTitle);
    SetTextColor(mdc, fg);
    DrawTextW(mdc, title.c_str(), -1, &r, lineFlags);
    y += titleH;
    if (metaH) {
        y += S(1);
        r = {ox + textX, y, ox + textX + textW, y + metaH};
        SelectObject(mdc, fMeta);
        SetTextColor(mdc, fg2);
        DrawTextW(mdc, meta.c_str(), -1, &r, lineFlags);
        y += metaH;
    }
    if (bodyH) {
        y += S(7);
        SelectObject(mdc, fBody);
        SetTextColor(mdc, dark ? RGB(222, 222, 222) : RGB(48, 48, 48));
        for (size_t i = 0; i < bodyLines.size(); i++) {
            bool moreLine = ci.kind == CLIP_FILES && i + 1 == bodyLines.size() && bodyLines[i].rfind(L"+ ", 0) == 0;
            if (moreLine) SetTextColor(mdc, fg2);
            r = {ox + textX, y, ox + textX + textW, y + bodyLH};
            DrawTextW(mdc, bodyLines[i].c_str(), -1, &r, lineFlags | DT_EXPANDTABS);
            y += bodyLH;
        }
    }
    GdiFlush();

    // ---- alpha: rounded mask + 1px border + drop shadow
    DWORD* px = (DWORD*)bitsV;
    const float hx = W * 0.5f, hy = H * 0.5f, cx = ox + hx, cy = oy + hy;
    const float shadowOff = (float)S(3), shadowSpread = (float)margin;
    const float bR = GetRValue(border), bG = GetGValue(border), bB = GetBValue(border);
    for (int yy = 0; yy < BH; yy++) {
        for (int xx = 0; xx < BW; xx++) {
            float fx = xx + 0.5f - cx, fy = yy + 0.5f - cy;
            float d = SdRoundRect(fx, fy, hx, hy, (float)radius);
            float cov = Clamp01(0.5f - d);
            float ds = SdRoundRect(fx, fy - shadowOff, hx, hy, (float)radius);
            float sa = ds <= 0 ? 0.26f : 0.26f * std::pow(Clamp01(1 - ds / shadowSpread), 2.f);
            if (dark) sa *= 1.4f;
            DWORD& p = px[(size_t)yy * BW + xx];
            float r_ = 0, g_ = 0, b_ = 0;
            if (cov > 0) {
                r_ = (float)((p >> 16) & 0xFF); g_ = (float)((p >> 8) & 0xFF); b_ = (float)(p & 0xFF);
                float bm = Clamp01(d + 1.5f);   // 1px border ring just inside the edge
                r_ += (bR - r_) * bm; g_ += (bG - g_) * bm; b_ += (bB - b_) * bm;
            }
            float a = cov + sa * (1 - cov);
            p = ((DWORD)(Clamp01(a) * 255 + 0.5f) << 24) | ((DWORD)(r_ * cov + 0.5f) << 16) |
                ((DWORD)(g_ * cov + 0.5f) << 8) | (DWORD)(b_ * cov + 0.5f);
        }
    }

    DeleteObject(fTitle); DeleteObject(fMeta); DeleteObject(fBody); DeleteObject(fGlyph);

    // ---- placement: bottom-centre inside the source window, else below it; clamp to work area
    const int gapEdge = S(24), pad = S(8);
    int x = (anchor.left + anchor.right) / 2 - W / 2;
    int yPos;
    if (!anchorIn) yPos = anchor.bottom - H - gapEdge;
    else if (anchor.bottom - anchor.top >= H + 2 * gapEdge) yPos = anchor.bottom - H - gapEdge;
    else yPos = anchor.bottom + pad;
    const RECT& wa = mi.rcWork;
    x = std::max((int)wa.left + pad, std::min(x, (int)wa.right - W - pad));
    if (yPos + H > wa.bottom - pad) yPos = std::max((int)wa.top + pad, (int)anchor.top - H - pad);
    yPos = std::max((int)wa.top + pad, std::min(yPos, (int)wa.bottom - H - pad));

    // ---- show
    EnsureWindow();
    KillTimer(s_wnd, 1);
    FreeSurface();
    s_dc = mdc; s_bmp = bmp; s_oldBmp = oldBmp;

    bool wasVisible = s_phase == P_HOLD || s_phase == P_IN;
    POINT dst{x - margin, yPos - margin}, src{0, 0};
    SIZE sz{BW, BH};
    BYTE a0 = wasVisible ? 255 : 0;
    BLENDFUNCTION bf{AC_SRC_OVER, 0, a0, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_wnd, nullptr, &dst, &sz, s_dc, &src, 0, &bf, ULW_ALPHA);
    s_alpha = a0;
    SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    s_phase = wasVisible ? P_HOLD : P_IN;
    s_hold = HOLD_MS[std::min(3, std::max(0, g_settings.toastDuration))];
    s_t0 = GetTickCount64();
    SetTimer(s_wnd, 1, 15, nullptr);
}
