// Startup intro, ported from IntroAnimation in the Python version (mainn.py):
//  1. the logo (extras\images\AlbionAi.png, 200 px) rises from below the screen
//     to the centre - 1200 ms, OutCubic;
//  2. 300 ms pause;
//  3. the logo slides 250 px left (800 ms, InOutQuad) while "Albion" + red "AI"
//     rises next to it (1000 ms, OutCubic);
//  4. "Initializing..." appears at the bottom;
//  5. the whole intro fades out (1000 ms, InOutQuad) and closes after 200 ms.
// The window is a transparent, click-through, per-pixel-alpha overlay, like the
// Qt window with WA_TranslucentBackground.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <memory>
#include <string>
#include "Resource.h"
#include "startup_splash.hpp"
#include "app_paths.hpp"
#include "ui_fonts.hpp"
#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr UINT_PTR kTimer = 7193;
// Timeline, ms (same durations as the Python intro).
constexpr double kRiseEnd = 1200, kMoveStart = 1500, kMoveEnd = 2300, kTextEnd = 2500,
                 kHoldEnd = 3300, kFadeEnd = 4300, kCloseAt = 4500;

double clamp01(double t) { return std::clamp(t, 0.0, 1.0); }
double outCubic(double t) { t = clamp01(t); const double u = 1.0 - t; return 1.0 - u * u * u; }
double inOutQuad(double t) { t = clamp01(t); return t < 0.5 ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) / 2; }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

struct Intro {
    HWND hwnd{};
    int x{}, y{}, width{}, height{};
    double scale = 1.0;
    ULONGLONG start{};
    bool done{};
    HDC memDc{}; HBITMAP dib{}; HGDIOBJ oldBmp{}; void* bits{};
    std::unique_ptr<Gdiplus::Bitmap> logo;
    std::unique_ptr<Gdiplus::PrivateFontCollection> fonts;
    std::unique_ptr<Gdiplus::Font> titleFont, statusFont;
    float logoW = 200, logoH = 200;
    int dirtyTop = 0, dirtyBottom = -1; // rows touched by the previous frame (-1: whole bitmap)
    ~Intro() {
        logo.reset(); titleFont.reset(); statusFont.reset(); fonts.reset();
        if (memDc) { if (oldBmp) SelectObject(memDc, oldBmp); DeleteDC(memDc); }
        if (dib) DeleteObject(dib);
    }

    void render(double t) {
        if (!memDc || !bits) return;
        // Clear only the rows the previous frame painted: a full 4K clear every
        // 15 ms was the main cost of the intro.
        {
            const int top = dirtyBottom < 0 ? 0 : std::clamp(dirtyTop, 0, height);
            const int bottom = dirtyBottom < 0 ? height : std::clamp(dirtyBottom, 0, height);
            if (bottom > top)
                std::fill_n(static_cast<UINT32*>(bits) + size_t(top) * size_t(width),
                    size_t(bottom - top) * size_t(width), 0u);
        }
        double paintTop = 1e9, paintBottom = -1e9;
        auto touch = [&](double y0, double y1) { paintTop = std::min(paintTop, y0); paintBottom = std::max(paintBottom, y1); };
        Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        const double below = height + 100.0 * scale;
        const double centerX = (width - logoW) / 2.0, centerY = (height - logoH) / 2.0;
        // Logo: rise, then slide left.
        const double ly = lerp(below, centerY, outCubic(t / kRiseEnd));
        const double lx = lerp(centerX, centerX - 250.0 * scale,
            inOutQuad((t - kMoveStart) / (kMoveEnd - kMoveStart)));
        if (logo) { g.DrawImage(logo.get(), Gdiplus::RectF(float(lx), float(ly), logoW, logoH)); touch(ly, ly + logoH); }

        // Title: "Albion" white + "AI" red, rising next to the final logo position.
        if (t >= kMoveStart && titleFont) {
            const std::wstring a = L"Albion", b = L"AI";
            Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
            fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
            Gdiplus::RectF ra{}, rb{};
            g.MeasureString(a.c_str(), int(a.size()), titleFont.get(), Gdiplus::PointF(0, 0), &fmt, &ra);
            g.MeasureString(b.c_str(), int(b.size()), titleFont.get(), Gdiplus::PointF(0, 0), &fmt, &rb);
            const double endLogoX = centerX - 250.0 * scale;
            const double tx = endLogoX + logoW + 20.0 * scale;
            const double tyEnd = centerY + (logoH - ra.Height) / 2.0;
            const double ty = lerp(below, tyEnd, outCubic((t - kMoveStart) / (kTextEnd - kMoveStart)));
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::SolidBrush red(Gdiplus::Color(255, 0xfc, 0x21, 0x31));
            g.DrawString(a.c_str(), int(a.size()), titleFont.get(), Gdiplus::PointF(float(tx), float(ty)), &fmt, &white);
            g.DrawString(b.c_str(), int(b.size()), titleFont.get(), Gdiplus::PointF(float(tx + ra.Width), float(ty)), &fmt, &red);
            touch(ty, ty + std::max(ra.Height, rb.Height));
        }

        // Status line while waiting, hidden when the fade starts (as in Python).
        if (t >= kTextEnd && t < kHoldEnd && statusFont) {
            const wchar_t* text = L"Initializing...";
            Gdiplus::StringFormat center;
            center.SetAlignment(Gdiplus::StringAlignmentCenter);
            center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush gray(Gdiplus::Color(255, 0x80, 0x80, 0x80));
            const float sw = float(400 * scale), sh = float(30 * scale);
            g.DrawString(text, -1, statusFont.get(),
                Gdiplus::RectF((width - sw) / 2.f, float(height - 100 * scale), sw, sh), &center, &gray);
            touch(height - 100 * scale, height - 100 * scale + sh);
        }

        if (paintBottom > paintTop) { dirtyTop = int(paintTop) - 4; dirtyBottom = int(paintBottom) + 6; }
        else { dirtyTop = 0; dirtyBottom = 0; }
        const double fade = 1.0 - inOutQuad((t - kHoldEnd) / (kFadeEnd - kHoldEnd));
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, BYTE(std::clamp(fade, 0.0, 1.0) * 255.0 + 0.5), AC_SRC_ALPHA };
        POINT dst{ x, y }, src{ 0, 0 }; SIZE size{ width, height };
        UpdateLayeredWindow(hwnd, nullptr, &dst, &size, memDc, &src, 0, &blend, ULW_ALPHA);
    }
};

LRESULT CALLBACK IntroProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* s = reinterpret_cast<Intro*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        s = reinterpret_cast<Intro*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (w == kTimer) {
            const double t = double(GetTickCount64() - s->start);
            if (t >= kCloseAt) { DestroyWindow(h); return 0; }
            s->render(t);
            return 0;
        }
        break;
    case WM_DESTROY: KillTimer(h, kTimer); s->hwnd = nullptr; s->done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}

void ShowAlbionStartupSplash(HINSTANCE instance) {
    AlbionPhysicalDpi dpi; // full-monitor bitmap uses physical coordinates
    if (AlbionReducedMotion()) return;

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return;
    {
        POINT cursor{}; GetCursorPos(&cursor);
        MONITORINFO info{ sizeof(info) };
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) { Gdiplus::GdiplusShutdown(token); return; }

        Intro s;
        s.x = info.rcMonitor.left; s.y = info.rcMonitor.top;
        s.width = info.rcMonitor.right - info.rcMonitor.left;
        s.height = info.rcMonitor.bottom - info.rcMonitor.top;
        HDC screen = GetDC(nullptr);
        s.scale = screen ? GetDeviceCaps(screen, LOGPIXELSY) / 96.0 : 1.0;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = s.width; bi.bmiHeader.biHeight = -s.height;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        s.memDc = CreateCompatibleDC(screen);
        s.dib = s.memDc ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &s.bits, nullptr, 0) : nullptr;
        if (screen) ReleaseDC(nullptr, screen);
        if (!s.memDc || !s.dib || !s.bits) { Gdiplus::GdiplusShutdown(token); return; }
        s.oldBmp = SelectObject(s.memDc, s.dib);

        // Logo: the same PNG as the Python intro, scaled to 200 px keeping aspect.
        const std::wstring logoPath = AlbionExeDir() + L"extras\\images\\AlbionAi.png";
        s.logo.reset(Gdiplus::Bitmap::FromFile(logoPath.c_str(), FALSE));
        if (s.logo && s.logo->GetLastStatus() != Gdiplus::Ok) s.logo.reset();
        if (!s.logo) {
            HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_WINDOWSPROJECT1), IMAGE_ICON, 256, 256, 0));
            if (icon) { s.logo.reset(Gdiplus::Bitmap::FromHICON(icon)); DestroyIcon(icon); }
        }
        const float box = float(200 * s.scale);
        if (s.logo && s.logo->GetWidth() && s.logo->GetHeight()) {
            const float k = std::min(box / s.logo->GetWidth(), box / s.logo->GetHeight());
            s.logoW = s.logo->GetWidth() * k; s.logoH = s.logo->GetHeight() * k;
        } else { s.logoW = s.logoH = box; }

        // Title font: Montserrat Bold from extras\fonts (the UI font), Arial Bold otherwise.
        // 72 pt like the Python QFont("Arial", 72, Bold).
        const float titlePx = float(72.0 * 96.0 / 72.0 * s.scale);
        s.fonts = std::make_unique<Gdiplus::PrivateFontCollection>();
        if (s.fonts->AddFontFile(AlbionFontPath(L"Montserrat-Bold.ttf").c_str()) == Gdiplus::Ok &&
            s.fonts->GetFamilyCount() > 0) {
            Gdiplus::FontFamily family; int found = 0;
            s.fonts->GetFamilies(1, &family, &found);
            if (found) s.titleFont = std::make_unique<Gdiplus::Font>(&family, titlePx, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        }
        if (!s.titleFont || s.titleFont->GetLastStatus() != Gdiplus::Ok)
            s.titleFont = std::make_unique<Gdiplus::Font>(L"Arial", titlePx, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        s.statusFont = std::make_unique<Gdiplus::Font>(L"Consolas", float(14.0 * 96.0 / 72.0 * s.scale),
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = IntroProc; wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"AlbionIntroPython";
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                wc.lpszClassName, L"AlbionAI", WS_POPUP, s.x, s.y, s.width, s.height, nullptr, nullptr, instance, &s);
            if (s.hwnd) {
                s.start = GetTickCount64();
                s.render(0.0);
                ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
                if (!SetTimer(s.hwnd, kTimer, 15, nullptr)) DestroyWindow(s.hwnd);
                MSG msg{};
                while (!s.done) {
                    const int ret = GetMessageW(&msg, nullptr, 0, 0);
                    if (ret <= 0) { if (ret == 0) PostQuitMessage(static_cast<int>(msg.wParam)); break; }
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                }
                if (s.hwnd) DestroyWindow(s.hwnd);
            }
        }
    } // GDI+ objects are released before shutdown
    Gdiplus::GdiplusShutdown(token);
}
