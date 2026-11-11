// modules/ui/theme.hpp
#pragma once
#include "modules/core/platform.hpp"
inline constexpr COLORREF kThBg = RGB(0x0b, 0x0c, 0x10);   // #0B0C10
inline constexpr COLORREF kThPanel = RGB(0x11, 0x12, 0x17);   // #111217
inline constexpr COLORREF kThStatus = RGB(0x15, 0x16, 0x1c);   // #15161C
inline constexpr COLORREF kThInput = RGB(0x1c, 0x1d, 0x23);   // #1C1D23
inline constexpr COLORREF kThBorder = RGB(0x30, 0x31, 0x3a);   // #30313A
inline constexpr COLORREF kThAccent = RGB(0xff, 0x35, 0x48);   // #FF3548
inline constexpr COLORREF kThAccentDim = RGB(0xbd, 0x26, 0x26);   // #BD2626
inline constexpr COLORREF kThAccentHot = RGB(0xff, 0x33, 0x33);   // #FF3333
inline constexpr COLORREF kThHover = RGB(0x2d, 0x1b, 0x1b);   // #2D1B1B
inline constexpr COLORREF kThActive = RGB(0x3d, 0x1b, 0x1b);   // #3D1B1B
inline constexpr COLORREF kThPressed = RGB(0x4d, 0x15, 0x15);   // #4D1515
inline constexpr COLORREF kThText = RGB(0xE0, 0xE0, 0xE0);   // #E0E0E0
inline constexpr COLORREF kThTextMuted = RGB(0xA0, 0xA0, 0xA0);   // #A0A0A0
inline constexpr COLORREF kThTextBright = RGB(0xFF, 0xFF, 0xFF);
inline constexpr COLORREF kThSelect = RGB(0x2d, 0x2f, 0x38);   // #2D2F38

// Skruglyonnaya zalivka - analog border-radius iz Qt-stiley.
inline void ThemeRoundRect(HDC dc, RECT rc, int radius,
    COLORREF fill, COLORREF border, int borderWidth)
{
    if (radius > 0)
    {
        // Antialiased corners (plain GDI RoundRect gives jagged pills).
        Gdiplus::Graphics g(dc);
        if (g.GetLastStatus() == Gdiplus::Ok)
        {
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float x = rc.left + 0.5f, y = rc.top + 0.5f;
            const float w = float(rc.right - rc.left - 1), h = float(rc.bottom - rc.top - 1);
            const float d = std::min(float(radius), std::min(w, h));
            if (w > 0.f && h > 0.f)
            {
                Gdiplus::GraphicsPath path;
                path.AddArc(x, y, d, d, 180.f, 90.f);
                path.AddArc(x + w - d, y, d, d, 270.f, 90.f);
                path.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
                path.AddArc(x, y + h - d, d, d, 90.f, 90.f);
                path.CloseFigure();
                Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
                g.FillPath(&brush, &path);
                const COLORREF line = borderWidth > 0 ? border : fill;
                Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(line), GetGValue(line), GetBValue(line)),
                    float(borderWidth > 0 ? borderWidth : 1));
                g.DrawPath(&pen, &path);
            }
            return;
        }
    }
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = (borderWidth > 0) ? CreatePen(PS_SOLID, borderWidth, border)
        : CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBr = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (radius > 0) RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    else            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

inline void DrawCornerBox(HDC dc, int l, int t, int r, int b, int cornerLen)
{
    if (r <= l || b <= t) return;
    int maxLen = std::max(1, std::min(r - l, b - t) / 2);
    int len = std::min(cornerLen, maxLen);

    MoveToEx(dc, l, t + len, nullptr); LineTo(dc, l, t); LineTo(dc, l + len, t);
    MoveToEx(dc, r - len, t, nullptr); LineTo(dc, r, t); LineTo(dc, r, t + len);
    MoveToEx(dc, r, b - len, nullptr); LineTo(dc, r, b); LineTo(dc, r - len, b);
    MoveToEx(dc, l + len, b, nullptr); LineTo(dc, l, b); LineTo(dc, l, b - len);
}

// Гладкая одноцветная окружность: несколько контуров с субпиксельным сдвигом
// закрывают ступеньки, при этом цвет остаётся ровно один.
inline void DrawSmoothCircle(HDC dc, int cx, int cy, int radius)
{
    if (radius <= 1) return;
    // Overlay center/radius normally remain constant for thousands of paints.
    // Cache exactly the old integer vertices; do not approximate the circle.
    struct CircleCache {
        int cx = 0, cy = 0, radius = -1;
        std::array<std::vector<POINT>, 7> contours;
    };
    static thread_local CircleCache cache;
    if (cache.radius != radius || cache.cx != cx || cache.cy != cy)
    {
        const int segments = std::clamp(radius * 8, 256, 2048);
        const double step = 6.283185307179586 / double(segments);
        static const double offsets[] = { -0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75 };
        for (size_t n = 0; n < cache.contours.size(); ++n)
        {
            auto& pts = cache.contours[n];
            pts.resize(size_t(segments) + 1);
            const double rr = double(radius) + offsets[n];
            for (int i = 0; i <= segments; ++i)
            {
                const double a = step * double(i);
                pts[size_t(i)].x = cx + LONG(std::lround(std::cos(a) * rr));
                pts[size_t(i)].y = cy + LONG(std::lround(std::sin(a) * rr));
            }
        }
        cache.cx = cx; cache.cy = cy; cache.radius = radius;
    }
    for (const auto& pts : cache.contours)
        Polyline(dc, pts.data(), int(pts.size()));
}

// Имя виртуальной клавиши для кнопки бинда.
inline std::wstring VkToName(int vk)
{
    switch (vk)
    {
    case VK_LBUTTON:  return L"LMouse";
    case VK_RBUTTON:  return L"RMouse";
    case VK_MBUTTON:  return L"MMouse";
    case VK_XBUTTON1: return L"XButton1";
    case VK_XBUTTON2: return L"XButton2";
    case VK_SPACE:    return L"Space";
    case VK_TAB:      return L"Tab";
    case VK_RETURN:   return L"Enter";
    case VK_BACK:     return L"Backspace";
    case VK_CAPITAL:  return L"CapsLock";
    case VK_SHIFT:
    case VK_LSHIFT:   return L"Shift";
    case VK_RSHIFT:   return L"RShift";
    case VK_CONTROL:
    case VK_LCONTROL: return L"Ctrl";
    case VK_RCONTROL: return L"RCtrl";
    case VK_MENU:
    case VK_LMENU:    return L"Alt";
    case VK_RMENU:    return L"RAlt";
    case VK_INSERT:   return L"Insert";
    case VK_DELETE:   return L"Delete";
    case VK_HOME:     return L"Home";
    case VK_END:      return L"End";
    case VK_PRIOR:    return L"PageUp";
    case VK_NEXT:     return L"PageDown";
    case VK_LEFT:     return L"Left";
    case VK_RIGHT:    return L"Right";
    case VK_UP:       return L"Up";
    case VK_DOWN:     return L"Down";
    default: break;
    }
    if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A))
    {
        const wchar_t b[2] = { wchar_t(vk), 0 };
        return std::wstring(b);
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        wchar_t b[8]; swprintf_s(b, L"F%d", vk - VK_F1 + 1); return std::wstring(b);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    {
        wchar_t b[16]; swprintf_s(b, L"Num%d", vk - VK_NUMPAD0); return std::wstring(b);
    }
    wchar_t b[24]; swprintf_s(b, L"Key_%d", vk); return std::wstring(b);
}

