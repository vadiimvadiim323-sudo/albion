// modules/ui/overlay.cpp
#include "modules/app/app.hpp"

void App::StartTabFade()
{
        if (!mTabFadeWnd) return;
        mTabFadeStart = GetTickCount64();
        if (!SetTimer(mTabFadeWnd, kTabFadeTimer, 15, nullptr)) EndTabFade();
    }

LRESULT CALLBACK App::TabFadeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (w == kTabFadeTimer && self && self->mTabFadeWnd == h) {
                const double t = std::min(1.0, double(GetTickCount64() - self->mTabFadeStart) / kTabFadeMs);
                if (t >= 1.0) { KillTimer(h, kTabFadeTimer); self->EndTabFade(); return 0; }
                const double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t); // OutCubic
                self->PushTabFade(BYTE(255.0 * (1.0 - e) + 0.5), -int(8.0 * e + 0.5));
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(h, kTabFadeTimer);
            if (self && self->mTabFadeWnd == h) { self->mTabFadeWnd = nullptr; self->EndTabFade(); }
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

void App::ApplyVisuals()
{
        // Sostoyanie galochek hranitsya v flagakh - knopki risuyutsya vruchnuyu.
        HWND checks[] = { mVisRegion, mVisBoxes, mVisStats, mVisConf };
        for (HWND c : checks) if (c) InvalidateRect(c, nullptr, FALSE);
        EnsureOverlay();
    }

LRESULT CALLBACK App::OverlayProcStatic(HWND h, UINT m, WPARAM w, LPARAM l)
{
        App* self = nullptr;
        if (m == WM_NCCREATE)
        {
            self = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));

        if (m == WM_NCHITTEST) return HTTRANSPARENT;   // klik prokhodit skvoz overlay
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_PAINT && self)
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            try { self->PaintOverlayBuffered(dc, ps.rcPaint); }
            catch (...) { DevLog("OVERLAY", "Paint failed; frame skipped"); }
            EndPaint(h, &ps);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

void App::DestroyOverlayBuffer()
{
        if (mOverlayMemDc)
        {
            if (mOverlayOldBitmap) SelectObject(mOverlayMemDc, mOverlayOldBitmap);
            mOverlayOldBitmap = nullptr;
            if (mOverlayBitmap) DeleteObject(mOverlayBitmap);
            mOverlayBitmap = nullptr;
            DeleteDC(mOverlayMemDc);
            mOverlayMemDc = nullptr;
        }
    }

bool App::EnsureOverlayBuffer(HDC targetDc)
{
        if (mOverlayMemDc && mOverlayBitmap) return true;
        mOverlayMemDc = CreateCompatibleDC(targetDc);
        if (!mOverlayMemDc) return false;
        mOverlayBitmap = CreateCompatibleBitmap(targetDc, int(mDeskW), int(mDeskH));
        if (!mOverlayBitmap)
        {
            DeleteDC(mOverlayMemDc);
            mOverlayMemDc = nullptr;
            return false;
        }
        mOverlayOldBitmap = SelectObject(mOverlayMemDc, mOverlayBitmap);
        return true;
    }

void App::PaintOverlayBuffered(HDC targetDc, const RECT& paintRect)
{
        // Рисуем кадр целиком в память и только потом выводим его на экран.
        // Это устраняет мигание HUD/FOV при очистке color-key окна.
        if (!EnsureOverlayBuffer(targetDc))
        {
            DrawOverlay(targetDc);
            return;
        }
        const int saved = SaveDC(mOverlayMemDc);
        if (saved)
            IntersectClipRect(mOverlayMemDc, paintRect.left, paintRect.top, paintRect.right, paintRect.bottom);
        DrawOverlay(mOverlayMemDc);
        if (saved) RestoreDC(mOverlayMemDc, saved);
        const int width = paintRect.right - paintRect.left;
        const int height = paintRect.bottom - paintRect.top;
        if (width > 0 && height > 0)
            BitBlt(targetDc, paintRect.left, paintRect.top, width, height,
                mOverlayMemDc, paintRect.left, paintRect.top, SRCCOPY);
    }

void App::DestroyOverlayResources()
{
        for (HPEN* p : { &mOverlayFovPen, &mOverlayBoxPen, &mOverlayTargetPen, &mOverlayBorderPen })
        {
            if (*p) DeleteObject(*p);
            *p = nullptr;
        }
        if (mOverlayPanelBrush) DeleteObject(mOverlayPanelBrush);
        if (mOverlayFont) DeleteObject(mOverlayFont);
        mOverlayPanelBrush = nullptr; mOverlayFont = nullptr;
    }

void App::EnsureOverlayResources()
{
        if (!mOverlayFovPen) mOverlayFovPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        if (!mOverlayBoxPen) mOverlayBoxPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        if (!mOverlayTargetPen) mOverlayTargetPen = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
        if (!mOverlayBorderPen) mOverlayBorderPen = CreatePen(PS_SOLID, 2, RGB(255, 51, 51));
        if (!mOverlayPanelBrush) mOverlayPanelBrush = CreateSolidBrush(RGB(16, 16, 16));
        if (!mOverlayFont) mOverlayFont = CreateFontW(18, 0, 0, 0, FW_BOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    }

RECT App::OverlayBounds(const UiSnapshot& ui) const
{
        RECT bounds{};
        auto add = [&bounds](RECT r) { RECT joined{}; UnionRect(&joined, &bounds, &r); bounds = joined; };
        if (mShowRegion)
        {
            const LONG cx = ui.region.left + LONG(ui.region.width) / 2;
            const LONG cy = ui.region.top + LONG(ui.region.height) / 2;
            const LONG radius = LONG(std::min(ui.region.width, ui.region.height)) / 2;
            add(RECT{ cx - radius - 4, cy - radius - 4, cx + radius + 5, cy + radius + 5 });
        }
        if (mShowBoxes)
            for (const auto& d : ui.dets)
            {
                if (d.score < ui.confidence || d.w <= 0.f || d.h <= 0.f) continue;
                const LONG x = LONG(d.x), y = LONG(d.y);
                RECT r{ x - 4, y - 4, LONG(d.x + d.w) + 5, LONG(d.y + d.h) + 5 };
                // Confidence text can extend beyond a narrow detection box.
                if (mShowConfidence) { r.top = std::min<LONG>(r.top, y > 16 ? y - 20 : 0); r.right = std::max<LONG>(r.right, x + 96); }
                add(r);
            }
        if (mShowStats) add(RECT{ 16,16,198,80 });
        RECT screen{ 0,0,LONG(mDeskW),LONG(mDeskH) }, clipped{};
        IntersectRect(&clipped, &bounds, &screen);
        return clipped;
    }

void App::UpdateOverlayInvalidation(double now)
{
        UiSnapshot next;
        { std::lock_guard<std::mutex> lk(mUiMx); next = mUiSnap; }
        const unsigned flags = (mShowRegion ? 1u : 0u) | (mShowBoxes ? 2u : 0u) |
            (mShowStats ? 4u : 0u) | (mShowConfidence ? 8u : 0u);
        const bool status = (GetAsyncKeyState(mActivationKey) & 0x8000) != 0;
        // Numeric HUD refresh is throttled; key ON/OFF remains responsive.
        if (now - mOverlayStatsMs < 250.0 && mOverlaySnapshotValid)
            next.capFps = mOverlaySnapshot.capFps;
        else mOverlayStatsMs = now;
        const auto& old = mOverlaySnapshot;
        bool changed = !mOverlaySnapshotValid || flags != mOverlayFlags;
        if (mShowStats && (next.capFps != old.capFps || status != mOverlayStatusOn)) changed = true;
        if ((mShowRegion || mShowBoxes) &&
            (next.region.left != old.region.left || next.region.top != old.region.top ||
                next.region.width != old.region.width || next.region.height != old.region.height)) changed = true;
        if (mShowBoxes)
        {
            if (next.confidence != old.confidence || next.dets.size() != old.dets.size()) changed = true;
            if (!changed)
                for (size_t i = 0; i < next.dets.size(); ++i)
                {
                    const auto& a = next.dets[i]; const auto& b = old.dets[i];
                    if (a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h || a.score != b.score || a.cls != b.cls)
                    {
                        changed = true; break;
                    }
                }
        }
        if (!changed) return;
        const RECT current = OverlayBounds(next);
        RECT damage{}; UnionRect(&damage, &mOverlayPreviousBounds, &current);
        mOverlayPreviousBounds = current;
        mOverlaySnapshot = std::move(next);
        mOverlaySnapshotValid = true; mOverlayFlags = flags; mOverlayStatusOn = status;
        if (!IsRectEmpty(&damage)) InvalidateRect(mOverlay, &damage, FALSE);
    }

void App::EnsureOverlay()
{
        AlbionPhysicalDpi dpi; // monitor coordinates must not be DPI-virtualized
        mOverlaySnapshotValid = false;
        const bool need = mShowRegion || mShowBoxes || mShowStats;
        if (!need)
        {
            if (mOverlay) ShowWindow(mOverlay, SW_HIDE);
            return;
        }
        if (!mOverlay)
        {
            if (!mDeskW || !mDeskH) return;
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = &App::OverlayProcStatic;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = L"AimOverlayWnd";
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

            mOverlay = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                wc.lpszClassName, L"", WS_POPUP,
                mOutputLeft.load(), mOutputTop.load(), int(mDeskW), int(mDeskH), nullptr, nullptr, inst, this);
            if (!mOverlay) return;
            // Chernyy stanovitsya prozrachnym - vidny tolko linii i tekst.
            SetLayeredWindowAttributes(mOverlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
        }
        UpdateOverlayInvalidation(NowMs());
        SetWindowPos(mOverlay, HWND_TOPMOST, mOutputLeft.load(), mOutputTop.load(), int(mDeskW), int(mDeskH),
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

void App::RefreshOverlayGeometry()
{
        // FIX-V11-13: a successful recovery can change output origin/size.
        // Rebuild the cached bitmap on the UI owner, never on the D3D worker.
        if (!mOverlay || !(mShowRegion || mShowBoxes || mShowStats)) return;
        AlbionPhysicalDpi dpi;
        const int width=int(mDeskW.load()), height=int(mDeskH.load());
        const LONG left=mOutputLeft.load(), top=mOutputTop.load();
        if (width <= 0 || height <= 0) return;
        RECT actual{};
        if (GetWindowRect(mOverlay, &actual) && actual.left==left && actual.top==top &&
            actual.right-actual.left==width && actual.bottom-actual.top==height) return;
        DestroyOverlayBuffer();
        EnsureOverlay();
        if (mOverlay) InvalidateRect(mOverlay, nullptr, FALSE);
    }

void App::DrawOverlay(HDC dc)
{
        EnsureOverlayResources();
        const UiSnapshot& ui = mOverlaySnapshot;
        RECT damaged{}; GetClipBox(dc, &damaged);
        FillRect(dc, &damaged, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

        if (mShowRegion)
        {
            const int cx = int(ui.region.left + LONG(ui.region.width) / 2);
            const int cy = int(ui.region.top + LONG(ui.region.height) / 2);
            const int radius = int(std::min(ui.region.width, ui.region.height) / 2);
            HPEN fovPen = mOverlayFovPen;
            HGDIOBJ oldFovPen = SelectObject(dc, fovPen);
            DrawSmoothCircle(dc, cx, cy, radius);
            SelectObject(dc, oldFovPen);
            // Cached pen is released with the application.
        }

        if (mShowBoxes)
        {
            const float conf = ui.confidence;
            const int fovCx = int(ui.region.left + LONG(ui.region.width) / 2);
            const int fovCy = int(ui.region.top + LONG(ui.region.height) / 2);
            for (const Detection& d : ui.dets)
            {
                if (d.score < conf || d.w <= 0.f || d.h <= 0.f) continue;
                const int l = int(d.x), t = int(d.y);
                const int r = int(d.x + d.w), b = int(d.y + d.h);
                const bool target = (l <= fovCx && fovCx <= r && t <= fovCy && fovCy <= b);
                HPEN boxPen = target ? mOverlayTargetPen : mOverlayBoxPen;
                HGDIOBJ old = SelectObject(dc, boxPen);
                DrawCornerBox(dc, l, t, r, b, 6);
                SelectObject(dc, old);
                // Cached box pen.
                if (mShowConfidence)
                {
                    SetTextColor(dc, RGB(255, 255, 255));
                    wchar_t lbl[32];
                    swprintf_s(lbl, L"%d%%", int(d.score * 100.f));
                    TextOutW(dc, l, (t > 16 ? t - 16 : 0), lbl, int(wcslen(lbl)));
                }
            }

        }

        if (mShowStats)
        {
            RECT panel{ 20, 20, 190, 75 };
            HBRUSH bg = mOverlayPanelBrush;
            HPEN border = mOverlayBorderPen;
            HGDIOBJ oldPanelBrush = SelectObject(dc, bg);
            HGDIOBJ oldPanelPen = SelectObject(dc, border);
            RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 20, 20);
            SelectObject(dc, oldPanelPen);
            SelectObject(dc, oldPanelBrush);
            HFONT font = mOverlayFont;
            HGDIOBJ oldFont = SelectObject(dc, font);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, 34, 26, L"Status:", 7);
            const bool statusOn = mOverlayStatusOn;
            SetTextColor(dc, statusOn ? RGB(0, 255, 0) : RGB(255, 68, 68));
            TextOutW(dc, 112, 26, statusOn ? L"ON" : L"OFF", statusOn ? 2 : 3);
            wchar_t fps[32];
            swprintf_s(fps, L"%.0f FPS", ui.capFps);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, 34, 47, fps, int(wcslen(fps)));
            SelectObject(dc, oldFont);
            // Cached font.
        }

        SelectObject(dc, oldBrush);
    }

std::wstring App::CaptureMethodStatus() const
{
#ifdef AIM_ENABLE_WGC
        if(mUseWgc)return L"WGC / latest frame";
#endif
        return mCaptureMethod == CaptureMethod::DirectXFull ? RU_CAP_DIRECTX : RU_CAP_OURS;
    }

void App::ApplyCaptureMethod()
{
        if (!mCapCombo) return;
        int sel = int(SendMessageW(mCapCombo, CB_GETCURSEL, 0, 0));
        if (sel == CB_ERR) return;
        mCaptureMethod = (sel == 1) ? CaptureMethod::DirectXFull : CaptureMethod::DirtyRects;
        QueueSettings();
    }

std::wstring App::FpsLimitStatus() const
{
        const int fpsLimit = mFpsLimit.load(std::memory_order_relaxed);
        if (fpsLimit <= 0) return RU_FPS_UNLIM;
        wchar_t buf[32];
        swprintf_s(buf, L"%d", fpsLimit);
        return buf;
    }

void App::ApplyFpsLimit()
{
        if (!mFpsCombo) return;
        int sel = int(SendMessageW(mFpsCombo, CB_GETCURSEL, 0, 0));
        if (sel == CB_ERR) return;
        sel = std::clamp<int>(sel, 0, int(_countof(kFpsOptions)) - 1);
        mFpsLimit.store(kFpsOptions[sel], std::memory_order_relaxed);
    }
