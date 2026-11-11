// modules/ui/drawing.cpp
#include "modules/app/app.hpp"

void App::LayoutControls()
{
        if (!mHwnd) return;
        auto put = [](HWND h, int x, int y, int w, int height) {
            if (h) SetWindowPos(h, nullptr, x, y, w, height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        };
        constexpr int L = 124, R = 788, slot = 72, trackR = R - slot;
        put(mWinClose, R - 32, 26, 32, 32);
        put(mWinMinimize, R - 70, 26, 32, 32);
        HWND nav[] = { mTabSettings, mTabVisuals, mTabAi, mTabConfig };
        for (int i = 0; i < 4; ++i) put(nav[i], 24, 124 + i * 62, 44, 44);
        // Settings.
        put(mBodyPartLabel, 0, 0, 0, 0); // radios read on their own, as in the reference
        const int bw[] = { 96, 72, 72, 128 };
        for (int i = 0, x = L; i < 4; ++i) { put(mBodyPartButtons[i], x, 88, bw[i], 30); x += bw[i] + 14; }
        struct SliderRow { HWND label, value, slider; int y; };
        const SliderRow rows[] = {
            { mSpeedLabel, mSpeedValue, mSpeedSlider, 138 },
            { mCaptureLabel, mCaptureValue, mCaptureSlider, 196 },
            { mHeadYLabel, mHeadYValue, mHeadYSlider, 254 },
            { mMouseScaleLabel, mMouseScaleValue, mMouseScaleSlider, 312 },
            { mConfLabel, mConfValue, mConfSlider, 88 } };
        for (const SliderRow& r : rows) {
            put(r.label, L, r.y, 260, 22);
            put(r.value, trackR - 110, r.y, 110, 22);
            put(r.slider, L - 9, r.y + 24, trackR - L + 18, 24);
        }
        put(mHeadYReset, R - 60, 270, 60, 30);
        auto pillRow = [&](HWND label, HWND pill, int y, int w, bool combo) {
            put(label, L, y + 6, 300, 22);
            put(pill, R - w, y, w, combo ? 220 : 32);
        };
        pillRow(mBindLabel, mBindButton, 376, 220, false);
        pillRow(mMethodLabel, mMethodCombo, 416, 220, true);
        // AI: confidence slider, model, capture method and FPS limit.
        put(mModelLabel,L,162,140,22);
        put(mModelCombo,280,156,508,220);
        put(mConvertButton,608,156,180,32);
        put(mConvertStatus,L,328,R-L,44);
        put(mConvertProgress,L,380,R-L,6);
        put(mConvertLogButton,L,404,240,32);
        put(mOnnxFolderButton,L+252,404,180,32);
        pillRow(mCapLabel, mCapCombo, 196, 220, true);
        pillRow(mFpsLabel, mFpsCombo, 236, 220, true);
        pillRow(mDiagnosticsLabel, mDiagnosticsButton, 276, 220, false);
        // Visuals.
        HWND checks[] = { mVisRegion, mVisBoxes, mVisStats, mVisConf };
        for (int i = 0; i < 4; ++i) put(checks[i], L, 84 + i * 52, R - L, 40);
        // Reference card: 24px inset, list, one action row and full-width create.
        put(mConfigTitle, 148, 104, 592, 24);
        put(mConfigList, 156, 152, 600, 156);
        put(mDeleteConfigBtn, 148, 336, 44, 44);
        put(mLoadConfigBtn, 204, 336, 246, 44);
        put(mSaveConfigBtn, 462, 336, 246, 44);
        put(mRenameConfigBtn, 720, 336, 44, 44);
        put(mConfigNewBtn, 148, 392, 616, 48);
        // Name entry is a separate in-tab form, not a permanently visible side column.
        put(mConfigNameLabel, 232, 228, 448, 22);
        put(mConfigNameEdit, 244, 272, 424, 22);
        put(mConfigConfirmBtn, 232, 332, 218, 44);
        put(mConfigCancelBtn, 462, 332, 218, 44);
        put(mConfigStatusLabel, L, 472, R - L, 20);
    }

int App::SwitchIndex(HWND h) const
{
        const HWND checks[] = { mVisRegion, mVisBoxes, mVisStats, mVisConf };
        for (int i = 0; i < 4; ++i) if (checks[i] && checks[i] == h) return i;
        return -1;
    }

bool App::SwitchTarget(int i) const
{
        return i == 0 ? mShowRegion : i == 1 ? mShowBoxes : i == 2 ? mShowStats : mShowConfidence;
    }

LRESULT CALLBACK App::UiButtonSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref)
{
        App* self = reinterpret_cast<App*>(ref);
        if (m == WM_ERASEBKGND) return 1; // owner draw fills everything; avoids hover flicker
        // Owner-draw buttons turn a fast second click into BN_DOUBLECLICKED,
        // which the app ignores. Treat it as a normal press so toggles never miss.
        if (m == WM_LBUTTONDBLCLK) m = WM_LBUTTONDOWN;
        if (self && m == WM_TIMER && w == kSwitchAnimTimer) {
            const int i = self->SwitchIndex(h);
            if (i < 0) { KillTimer(h, kSwitchAnimTimer); return 0; }
            const float target = self->mSwitchGoal[i] > 0 ? 1.f : 0.f;
            const float k = std::min(1.f, float(GetTickCount64() - self->mSwitchStart[i]) / 200.f);
            const float e = 1.f - (1.f - k) * (1.f - k) * (1.f - k); // OutCubic, 200 ms
            float& pos = self->mSwitchAnim[i];
            pos = self->mSwitchFrom[i] + (target - self->mSwitchFrom[i]) * e;
            if (k >= 1.f) { pos = target; KillTimer(h, kSwitchAnimTimer); }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (self && m == WM_MOUSEMOVE && self->mUiHovered != h) {
            HWND previous = self->mUiHovered;
            self->mUiHovered = h;
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, h, 0 };
            TrackMouseEvent(&track);
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (self && m == WM_MOUSELEAVE && self->mUiHovered == h) {
            self->mUiHovered = nullptr;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        if (m == WM_NCDESTROY) {
            KillTimer(h, kSwitchAnimTimer);
            if (self && self->mUiHovered == h) self->mUiHovered = nullptr;
            RemoveWindowSubclass(h, &App::UiButtonSubclass, id);
        }
        return DefSubclassProc(h, m, w, l);
    }

void App::ApplyRoundedWindowRegion()
{
        if (!mHwnd) return;
        if (IsZoomed(mHwnd))
        {
            SetWindowRgn(mHwnd, nullptr, FALSE);
            return;
        }
        RECT wr{};
        GetWindowRect(mHwnd, &wr);
        const int width = wr.right - wr.left;
        const int height = wr.bottom - wr.top;
        if (width <= 1 || height <= 1) return;
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 18, 18);
        if (region && !SetWindowRgn(mHwnd, region, FALSE))
            DeleteObject(region);
    }

Gdiplus::Color App::GpColor(COLORREF c, BYTE a)
{
        return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
    }

void App::GpPill(Gdiplus::Graphics& g, Gdiplus::Brush* brush, float x, float y, float w, float h)
{
        Gdiplus::GraphicsPath path;
        const float d = h;
        path.AddArc(x, y, d, d, 90.f, 180.f);
        path.AddArc(x + w - d, y, d, d, 270.f, 180.f);
        path.CloseFigure();
        g.FillPath(brush, &path);
    }

void App::DrawThemedButton(DRAWITEMSTRUCT* di)
{
        const RECT item = di->rcItem;
        const int w = item.right - item.left, hgt = item.bottom - item.top;
        HDC target = di->hDC;
        HDC mem = (w > 0 && hgt > 0) ? CreateCompatibleDC(target) : nullptr;
        HBITMAP bmp = mem ? CreateCompatibleBitmap(target, w, hgt) : nullptr;
        if (!mem || !bmp) { if (mem) DeleteDC(mem); DrawThemedButtonContent(di); return; }
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        DRAWITEMSTRUCT local = *di;
        local.hDC = mem;
        local.rcItem = RECT{ 0, 0, w, hgt };
        DrawThemedButtonContent(&local);
        BitBlt(target, item.left, item.top, w, hgt, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
    }

void App::DrawThemedButtonContent(DRAWITEMSTRUCT* di)
{
        HDC dc = di->hDC;
        RECT rc = di->rcItem;
        const bool pressed = (di->itemState & ODS_SELECTED) != 0;
        const bool hot = mUiHovered == di->hwndItem;
        const COLORREF kIdle = RGB(104, 105, 114);
        wchar_t text[160]{};
        GetWindowTextW(di->hwndItem, text, 159);
        FillRect(dc, &rc, mBrPanel);
        SetBkMode(dc, TRANSPARENT);

        if(di->hwndItem==mConvertButton) {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush bg(GpColor(pressed?kThSelect:(hot?kThInput:kThStatus)));
            GpPill(g,&bg,float(rc.left),float(rc.top),float(rc.right-rc.left),float(rc.bottom-rc.top));
            RECT icon{rc.left+9,rc.top+5,rc.left+31,rc.top+27};
            if(mImgConvert)DrawUiImageMonochrome(dc,mImgConvert.get(),icon,kThTextBright);
            else { // Visible vector fallback when the PNG is missing.
                Gdiplus::Pen arrow(GpColor(kThTextBright),2.f);
                g.DrawArc(&arrow,float(rc.left+12),float(rc.top+8),16.f,16.f,205.f,265.f);
                g.DrawLine(&arrow,float(rc.left+23),float(rc.top+6),float(rc.left+28),float(rc.top+12));
            }
            RECT label{rc.left+36,rc.top,rc.right-7,rc.bottom};
            const auto old=SelectObject(dc,mFontSmall?mFontSmall:mFontUi);
            SetTextColor(dc,hot?kThTextBright:kThText);DrawTextW(dc,text,-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(dc,old);return;
        }

        // Caption buttons: bare glyphs, as in the reference.
        if (di->hwndItem == mWinMinimize || di->hwndItem == mWinMaximize ||
            di->hwndItem == mWinClose)
        {
            const bool closeButton = di->hwndItem == mWinClose;
            const COLORREF glyph = pressed ? (closeButton ? kThAccent : kThTextBright)
                : (hot ? kThTextBright : kThTextMuted);
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(GpColor(glyph), 2.f);
            const float cx = (rc.left + rc.right) / 2.f, cy = (rc.top + rc.bottom) / 2.f;
            if (di->hwndItem == mWinMinimize) g.DrawLine(&pen, cx - 7, cy + 1, cx + 7, cy + 1);
            else if (di->hwndItem == mWinMaximize) g.DrawRectangle(&pen, cx - 6, cy - 6, 12.f, 12.f);
            else {
                g.DrawLine(&pen, cx - 7, cy - 7, cx + 7, cy + 7);
                g.DrawLine(&pen, cx + 7, cy - 7, cx - 7, cy + 7);
            }
            return;
        }

        // Top text tabs: bold, active white with a short accent underline.
        const auto header = std::find(mHeaderTabs.begin(), mHeaderTabs.end(), di->hwndItem);
        if (header != mHeaderTabs.end()) {
            const bool active = int(std::distance(mHeaderTabs.begin(), header)) == mActiveTab;
            HGDIOBJ oldFont = SelectObject(dc, mFontTab ? mFontTab : mFontBold);
            SIZE ext{};
            GetTextExtentPoint32W(dc, text, int(wcslen(text)), &ext);
            SetTextColor(dc, active ? kThTextBright : (hot ? kThText : kIdle));
            RECT tr{ rc.left, rc.top, rc.right, rc.bottom - 8 };
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            if (active) {
                const int w = std::min<int>(ext.cx, rc.right - rc.left);
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::SolidBrush line(GpColor(kThAccent));
                GpPill(g, &line, float(rc.left), float(rc.bottom - 5), float(w), 3.f);
            }
            return;
        }

        // Body-part radios: dark filled dots, the selected one is accent.
        const auto bodyIt = std::find(mBodyPartButtons.begin(), mBodyPartButtons.end(), di->hwndItem);
        if (bodyIt != mBodyPartButtons.end())
        {
            const int index = int(std::distance(mBodyPartButtons.begin(), bodyIt));
            const bool active = index == std::clamp<int>(int(mAimCfg.targetBodyPart), 0, 3);
            const float cy = (rc.top + rc.bottom) / 2.f, cx = rc.left + 11.f;
            {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::SolidBrush well(GpColor(hot && !active ? kThSelect : kThInput));
                g.FillEllipse(&well, cx - 11, cy - 11, 22.f, 22.f);
                if (active) {
                    Gdiplus::SolidBrush dot(GpColor(kThAccent));
                    g.FillEllipse(&dot, cx - 7, cy - 7, 14.f, 14.f);
                }
            }
            RECT tr{ rc.left + 30, rc.top, rc.right, rc.bottom };
            SetTextColor(dc, active ? kThTextBright : (hot ? kThText : kThTextMuted));
            HGDIOBJ of = SelectObject(dc, mFontUi);
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
            return;
        }

        // Visual options: text on the left, switch on the right.
        if (di->hwndItem == mVisRegion || di->hwndItem == mVisBoxes ||
            di->hwndItem == mVisStats || di->hwndItem == mVisConf)
        {
            bool on = mShowConfidence;
            if (di->hwndItem == mVisRegion) on = mShowRegion;
            else if (di->hwndItem == mVisBoxes) on = mShowBoxes;
            else if (di->hwndItem == mVisStats) on = mShowStats;

            RECT tr{ rc.left, rc.top, rc.right - 70, rc.bottom };
            SetTextColor(dc, on || hot ? kThTextBright : kThText);
            HGDIOBJ of = SelectObject(dc, mFontUi);
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);

            const int si = SwitchIndex(di->hwndItem);
            float t = on ? 1.f : 0.f;
            if (si >= 0) {
                const int goal = on ? 1 : 0;
                if (mSwitchAnim[si] < 0.f) { mSwitchAnim[si] = t; mSwitchGoal[si] = goal; } // first paint: no animation
                if (mSwitchGoal[si] != goal) {
                    mSwitchGoal[si] = goal; mSwitchFrom[si] = mSwitchAnim[si];
                    mSwitchStart[si] = GetTickCount64();
                    SetTimer(di->hwndItem, kSwitchAnimTimer, 10, nullptr);
                }
                t = mSwitchAnim[si];
            }
            auto mix = [](COLORREF a, COLORREF b, float k) {
                return RGB(int(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * k),
                           int(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * k),
                           int(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * k));
            };
            const float sw = 46.f, sh = 24.f;
            const float sx = rc.right - sw - 2.f, sy = (rc.top + rc.bottom) / 2.f - sh / 2.f;
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush track(GpColor(mix(hot ? kThSelect : kThInput, kThAccent, t)));
            GpPill(g, &track, sx, sy, sw, sh);
            Gdiplus::SolidBrush knob(GpColor(mix(kThTextMuted, kThTextBright, t)));
            const float kd = 16.f, kx = sx + 4.f + (sw - kd - 8.f) * t;
            g.FillEllipse(&knob, kx, sy + 4.f, kd, kd);
            return;
        }

        // Sidebar: bare monochrome icons, active is white.
        if (di->hwndItem == mTabSettings || di->hwndItem == mTabVisuals ||
            di->hwndItem == mTabAi || di->hwndItem == mTabConfig)
        {
            const int idx = (di->hwndItem == mTabSettings) ? 0
                : (di->hwndItem == mTabVisuals) ? 1
                : (di->hwndItem == mTabAi) ? 2 : 3;
            const bool active = (idx == mActiveTab);
            const COLORREF glyph = active ? kThTextBright : ((hot || pressed) ? kThText : kIdle);
            Gdiplus::Image* icon = ButtonImage(di->hwndItem);
            const int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
            if (icon)
            {
                RECT iconRc{ cx - 13, cy - 13, cx + 13, cy + 13 };
                DrawUiImageMonochrome(dc, icon, iconRc, glyph);
            }
            else // Packaged archive has no PNGs: draw filled vector icons.
            {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::SolidBrush b(GpColor(glyph));
                Gdiplus::SolidBrush hole(GpColor(kThPanel));
                Gdiplus::Pen p(GpColor(glyph), 2.2f);
                const float x = float(cx), y = float(cy);
                if (idx == 0) { // sliders
                    for (int i = 0; i < 3; ++i) {
                        const float yy = y - 8.f + i * 8.f, kx = x + (i == 1 ? 5.f : -5.f);
                        g.DrawLine(&p, x - 11.f, yy, x + 11.f, yy);
                        g.FillEllipse(&b, kx - 4.f, yy - 4.f, 8.f, 8.f);
                        g.FillEllipse(&hole, kx - 1.6f, yy - 1.6f, 3.2f, 3.2f);
                    }
                } else if (idx == 1) { // eye
                    Gdiplus::GraphicsPath eye;
                    eye.AddBezier(x - 12.f, y, x - 6.f, y - 9.f, x + 6.f, y - 9.f, x + 12.f, y);
                    eye.AddBezier(x + 12.f, y, x + 6.f, y + 9.f, x - 6.f, y + 9.f, x - 12.f, y);
                    g.FillPath(&b, &eye);
                    g.FillEllipse(&hole, x - 5.f, y - 5.f, 10.f, 10.f);
                    g.FillEllipse(&b, x - 2.5f, y - 2.5f, 5.f, 5.f);
                } else if (idx == 2) { // chip
                    for (int i = -1; i <= 1; ++i) {
                        const float o = i * 5.f;
                        g.DrawLine(&p, x + o, y - 12.f, x + o, y + 12.f);
                        g.DrawLine(&p, x - 12.f, y + o, x + 12.f, y + o);
                    }
                    g.FillRectangle(&b, x - 8.f, y - 8.f, 16.f, 16.f);
                    g.FillRectangle(&hole, x - 4.f, y - 4.f, 8.f, 8.f);
                } else { // folder
                    Gdiplus::GraphicsPath f;
                    f.AddLine(x - 12.f, y - 8.f, x - 3.f, y - 8.f);
                    f.AddLine(x - 3.f, y - 8.f, x, y - 5.f);
                    f.AddLine(x, y - 5.f, x + 12.f, y - 5.f);
                    f.AddLine(x + 12.f, y - 5.f, x + 12.f, y + 9.f);
                    f.AddLine(x + 12.f, y + 9.f, x - 12.f, y + 9.f);
                    f.CloseFigure();
                    g.FillPath(&b, &f);
                }
            }
            return;
        }

        // Reference action row: trash | load | save | pencil; no favourite/star action.
        if (di->hwndItem == mSaveConfigBtn || di->hwndItem == mLoadConfigBtn ||
            di->hwndItem == mRenameConfigBtn || di->hwndItem == mDeleteConfigBtn ||
            di->hwndItem == mConfigNewBtn || di->hwndItem == mConfigConfirmBtn || di->hwndItem == mConfigCancelBtn)
        {
            const bool enabled = (di->itemState & ODS_DISABLED) == 0 && IsWindowEnabled(di->hwndItem);
            const bool primary = di->hwndItem == mConfigNewBtn || di->hwndItem == mConfigConfirmBtn;
            const bool danger = di->hwndItem == mDeleteConfigBtn;
            const bool rename = di->hwndItem == mRenameConfigBtn;
            if (mConfigEditing && (di->hwndItem == mConfigConfirmBtn || di->hwndItem == mConfigCancelBtn))
                FillRect(dc, &rc, mBrStatus);
            COLORREF fill = enabled && primary ? kThAccentDim : (enabled && danger ? kThHover : kThInput);
            COLORREF ink = enabled ? (danger ? kThAccent : kThTextBright) : kThTextMuted;
            if (enabled && hot) fill = primary ? kThAccent : (danger ? kThPressed : kThSelect);
            if (enabled && pressed) fill = primary ? kThAccentHot : kThPressed;
            COLORREF border = enabled && (di->itemState & ODS_FOCUS) ? kThBorder : fill;
            ThemeRoundRect(dc, rc, 8, fill, border, 1);
            if (danger || rename) {
                DrawConfigActionGlyph(dc, (rc.left + rc.right) / 2.f, (rc.top + rc.bottom) / 2.f,
                    danger ? 0 : 3, ink);
            } else {
                HGDIOBJ previousFont = SelectObject(dc, mFontBold);
                SetTextColor(dc, ink);
                RECT label = rc;
                if (di->hwndItem == mLoadConfigBtn || di->hwndItem == mSaveConfigBtn) {
                    SIZE extent{}; GetTextExtentPoint32W(dc, text, int(wcslen(text)), &extent);
                    const int groupLeft = (rc.left + rc.right - extent.cx - 28) / 2;
                    DrawConfigActionGlyph(dc, float(groupLeft + 8), (rc.top + rc.bottom) / 2.f,
                        di->hwndItem == mLoadConfigBtn ? 1 : 2, ink);
                    label.left = groupLeft + 28; label.right = label.left + extent.cx + 2;
                }
                DrawTextW(dc, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dc, previousFont);
            }
            return;
        }

        // Pills: bind and reset.
        const bool isBind = (di->hwndItem == mBindButton);
        const bool isSave = (di->hwndItem == mSaveConfigBtn);
        const bool isDelete = (di->hwndItem == mDeleteConfigBtn);
        COLORREF fill = kThInput, border = kThInput, txt = kThText;
        if (isSave) { fill = kThAccentDim; border = fill; txt = kThTextBright; }
        if (hot) { fill = isSave ? kThAccent : (isDelete ? kThHover : kThSelect); border = fill; txt = kThTextBright; }
        if (isBind && mBindCapture) { fill = kThActive; border = kThAccent; txt = kThTextBright; }
        if (pressed) { fill = isSave ? kThAccentHot : kThPressed; border = fill; txt = kThTextBright; }
        ThemeRoundRect(dc, rc, 10, fill, border, 1);
        RECT tr = rc;
        Gdiplus::Image* icon = ButtonImage(di->hwndItem);
        const bool iconOnly = (di->hwndItem == mDeleteConfigBtn ||
            di->hwndItem == mRenameConfigBtn);
        if (icon && iconOnly)
        {
            const int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
            RECT iconRc{ cx - 10, cy - 10, cx + 10, cy + 10 };
            DrawUiImage(dc, icon, iconRc);
        }
        else if (icon)
        {
            RECT iconRc{ rc.left + 10, rc.top + 7, rc.left + 28, rc.bottom - 7 };
            DrawUiImage(dc, icon, iconRc);
            tr.left += 22;
        }
        if (iconOnly && !icon)
        {
            const float x = (rc.left + rc.right) / 2.f, y = (rc.top + rc.bottom) / 2.f;
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen p(GpColor(hot && isDelete ? kThAccent : txt), 1.8f);
            if (isDelete) {
                g.DrawRectangle(&p, x - 5.f, y - 4.f, 10.f, 12.f);
                g.DrawLine(&p, x - 8.f, y - 7.f, x + 8.f, y - 7.f);
                g.DrawLine(&p, x - 3.f, y - 10.f, x + 3.f, y - 10.f);
            } else {
                const Gdiplus::PointF pencil[] = { {x - 7.f, y + 7.f}, {x - 5.f, y + 1.f},
                    {x + 5.f, y - 9.f}, {x + 9.f, y - 5.f}, {x - 1.f, y + 5.f}, {x - 7.f, y + 7.f} };
                g.DrawLines(&p, pencil, _countof(pencil));
            }
        }
        if (!iconOnly)
        {
            SetTextColor(dc, txt);
            HGDIOBJ of = SelectObject(dc, isBind || di->hwndItem == mHeadYReset ||
                di->hwndItem == mDiagnosticsButton ? mFontUi : mFontBold);
            DrawTextW(dc, text, -1, &tr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
    }

void App::PaintComboControl(HWND hc, HDC dc)
{
        RECT rc{};
        GetClientRect(hc, &rc);
        FillRect(dc, &rc, mBrPanel);
        const bool dropped = SendMessageW(hc, CB_GETDROPPEDSTATE, 0, 0) != 0;
        const bool hot = (mUiHovered == hc);
        const COLORREF fill = dropped ? kThSelect : (hot ? kThSelect : kThInput);
        ThemeRoundRect(dc, rc, 10, fill, dropped ? kThAccent : fill, 1);

        wchar_t text[256]{};
        const LRESULT sel = SendMessageW(hc, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR)
            SendMessageW(hc, CB_GETLBTEXT, WPARAM(sel), reinterpret_cast<LPARAM>(text));

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, hot || dropped ? kThTextBright : kThText);
        HGDIOBJ of = SelectObject(dc, mFontUi ? mFontUi : GetStockObject(DEFAULT_GUI_FONT));
        RECT tr{ rc.left + 28, rc.top, rc.right - 28, rc.bottom };
        DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);

        // Own chevron: the native drop-down button is no longer painted.
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GpColor(dropped ? kThAccent : kThTextMuted), 1.8f);
        const float cx = rc.right - 16.f, cy = (rc.top + rc.bottom) / 2.f;
        const float d = dropped ? -1.f : 1.f;
        g.DrawLine(&pen, cx - 4.f, cy - 2.f * d, cx, cy + 2.f * d);
        g.DrawLine(&pen, cx, cy + 2.f * d, cx + 4.f, cy - 2.f * d);
    }

LRESULT CALLBACK App::ComboSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref)
{
        App* self = reinterpret_cast<App*>(ref);
        switch (m)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (self && self->mUiHovered != h) {
                HWND previous = self->mUiHovered;
                self->mUiHovered = h;
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, h, 0 };
                TrackMouseEvent(&track);
                if (previous) InvalidateRect(previous, nullptr, FALSE);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (self && self->mUiHovered == h) { self->mUiHovered = nullptr; InvalidateRect(h, nullptr, FALSE); }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED:
        case WM_KEYDOWN:
        case WM_MOUSEWHEEL:
        case CB_SETCURSEL:
        case CB_SHOWDROPDOWN:
        {
            // Unthemed combos draw a pushed button straight to the screen on
            // clicks; repaint our pill right after the default handling.
            const LRESULT result = DefSubclassProc(h, m, w, l);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            return result;
        }
        case WM_PRINTCLIENT:
            if (self) { self->PaintComboControl(h, reinterpret_cast<HDC>(w)); return 0; }
            break;
        case WM_PAINT:
        {
            if (!self) break;
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT rc{};
            GetClientRect(h, &rc);
            const int width = rc.right - rc.left;
            const int height = rc.bottom - rc.top;
            HDC bufferDc = CreateCompatibleDC(dc);
            HBITMAP bufferBitmap = CreateCompatibleBitmap(dc, width, height);
            HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
            self->PaintComboControl(h, bufferDc);
            BitBlt(dc, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
            SelectObject(bufferDc, oldBitmap);
            DeleteObject(bufferBitmap);
            DeleteDC(bufferDc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORLISTBOX:
            if (self && self->mBrInput)
            {
                SetTextColor(reinterpret_cast<HDC>(w), kThText);
                SetBkColor(reinterpret_cast<HDC>(w), kThInput);
                SetBkMode(reinterpret_cast<HDC>(w), OPAQUE);
                return reinterpret_cast<LRESULT>(self->mBrInput);
            }
            break;
        case WM_NCDESTROY:
            if (self && self->mUiHovered == h) self->mUiHovered = nullptr;
            RemoveWindowSubclass(h, &App::ComboSubclass, id);
            break;
        default:
            break;
        }
        return DefSubclassProc(h, m, w, l);
    }

void App::DrawThemedComboItem(DRAWITEMSTRUCT* di)
{
        HDC dc = di->hDC;
        RECT rc = di->rcItem;
        if (di->itemID == UINT(-1)) return;
        wchar_t text[256]{};
        SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, reinterpret_cast<LPARAM>(text));
        const bool inEdit = (di->itemState & ODS_COMBOBOXEDIT) != 0;
        const bool selected = (di->itemState & ODS_SELECTED) != 0;

        if (inEdit)
        {
            FillRect(dc, &rc, mBrPanel);
            ThemeRoundRect(dc, rc, 10, kThInput, kThInput, 1);
            // Arrow is drawn once by the native drop-down button.
        }
        else
        {
            HBRUSH bg = CreateSolidBrush(selected ? kThSelect : kThInput);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
        }

        RECT tr{ rc.left + 10, rc.top, rc.right - (inEdit ? 10 : 22), rc.bottom };
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kThText);
        HGDIOBJ of = SelectObject(dc, mFontUi);
        DrawTextW(dc, text, -1, &tr, (inEdit ? DT_CENTER : DT_LEFT) |
            DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }

void App::DrawConfigActionGlyph(HDC dc, float x, float y, int kind, COLORREF ink)
{
        Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(GpColor(ink), 1.8f);
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        if (kind == 0) {
            g.DrawRectangle(&pen, x - 5, y - 4, 10.f, 12.f);
            g.DrawLine(&pen, x - 8, y - 7, x + 8, y - 7);
            g.DrawLine(&pen, x - 3, y - 10, x + 3, y - 10);
            g.DrawLine(&pen, x - 2, y - 1, x - 2, y + 5);
            g.DrawLine(&pen, x + 2, y - 1, x + 2, y + 5);
        } else if (kind == 3) {
            g.DrawLine(&pen, x - 8, y + 8, x - 5, y + 1);
            g.DrawLine(&pen, x - 5, y + 1, x + 5, y - 9);
            g.DrawLine(&pen, x + 5, y - 9, x + 9, y - 5);
            g.DrawLine(&pen, x + 9, y - 5, x - 1, y + 5);
            g.DrawLine(&pen, x - 1, y + 5, x - 8, y + 8);
            g.DrawLine(&pen, x + 2, y - 6, x + 6, y - 2);
        } else {
            const float direction = kind == 1 ? -1.f : 1.f;
            g.DrawLine(&pen, x, y - 7 * direction, x, y + 4 * direction);
            g.DrawLine(&pen, x, y + 4 * direction, x - 4, y);
            g.DrawLine(&pen, x, y + 4 * direction, x + 4, y);
            g.DrawLine(&pen, x - 7, y + 4, x - 7, y + 8);
            g.DrawLine(&pen, x - 7, y + 8, x + 7, y + 8);
            g.DrawLine(&pen, x + 7, y + 8, x + 7, y + 4);
        }
    }

void App::DrawThemedListItem(DRAWITEMSTRUCT* di)
{
        HDC dc = di->hDC; RECT rc = di->rcItem;
        FillRect(dc, &rc, mBrInput);
        if (di->itemID >= mConfigVisible.size()) return;
        const ConfigEntry& entry = mConfigEntries[mConfigVisible[di->itemID]];
        const bool selected = (di->itemState & ODS_SELECTED) != 0;
        RECT row{rc.left + 1, rc.top + 3, rc.right - 1, rc.bottom - 3};
        if (selected) ThemeRoundRect(dc, row, 8, kThSelect, kThSelect, 0);
        const COLORREF ink = selected ? kThTextBright : kThTextMuted;
        {
            Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(GpColor(ink), 1.5f);
            const float x = float(rc.left + 18), y = float(rc.top + 17);
            g.DrawLine(&pen, x, y, x + 9, y);
            g.DrawLine(&pen, x + 9, y, x + 14, y + 5);
            g.DrawLine(&pen, x + 14, y + 5, x + 14, y + 18);
            g.DrawLine(&pen, x + 14, y + 18, x, y + 18);
            g.DrawLine(&pen, x, y + 18, x, y);
            g.DrawLine(&pen, x + 9, y, x + 9, y + 5);
            g.DrawLine(&pen, x + 9, y + 5, x + 14, y + 5);
            if (selected) {
                const float cx = float(rc.right - 24), cy = (rc.top + rc.bottom) / 2.f;
                g.DrawLine(&pen, cx - 5, cy, cx - 1, cy + 4);
                g.DrawLine(&pen, cx - 1, cy + 4, cx + 6, cy - 4);
            }
        }
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ink);
        HGDIOBJ previousFont = SelectObject(dc, mFontUi);
        RECT name{rc.left + 48, rc.top, rc.right - 44, rc.bottom};
        const std::wstring filename = entry.name + L".cfg";
        DrawTextW(dc, filename.c_str(), -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, previousFont);
    }

void App::PaintConfigWells(HDC dc)
{
        ThemeRoundRect(dc, RECT{124,84,788,464}, 10, kThPanel, kThInput, 1);
        if (!mConfigEditing) {
            ThemeRoundRect(dc, RECT{148,144,764,320}, 10, kThInput,
                GetFocus() == mConfigList ? kThBorder : kThInput, 1);
            return;
        }
        ThemeRoundRect(dc, RECT{208,154,704,420}, 12, kThStatus, kThStatus, 0);
        ThemeRoundRect(dc, RECT{232,260,680,304}, 8, kThInput,
            GetFocus() == mConfigNameEdit ? kThBorder : kThInput, 1);
        HGDIOBJ previousFont = SelectObject(dc, mFontUi);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, kThTextMuted);
        RECT hint{232,178,680,218};
        DrawTextW(dc, mConfigRenameMode ? L"\u0418\u0437\u043c\u0435\u043d\u0438\u0442\u0441\u044f \u0442\u043e\u043b\u044c\u043a\u043e \u0438\u043c\u044f \u0432\u044b\u0431\u0440\u0430\u043d\u043d\u043e\u0433\u043e \u0444\u0430\u0439\u043b\u0430.\n\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u043e\u0441\u0442\u0430\u043d\u0443\u0442\u0441\u044f \u0431\u0435\u0437 \u0438\u0437\u043c\u0435\u043d\u0435\u043d\u0438\u0439." :
            L"\u041d\u043e\u0432\u044b\u0439 \u043a\u043e\u043d\u0444\u0438\u0433 \u0441\u043e\u0445\u0440\u0430\u043d\u0438\u0442 \u0442\u0435\u043a\u0443\u0449\u0438\u0435 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438.\n\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u0438\u043c\u044f \u0431\u0435\u0437 \u0440\u0430\u0441\u0448\u0438\u0440\u0435\u043d\u0438\u044f .cfg.", -1, &hint, DT_LEFT | DT_WORDBREAK);
        SelectObject(dc, previousFont);
    }

LRESULT CALLBACK App::ConfigInputSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR data)
{
        App* self = reinterpret_cast<App*>(data);
        if (!self) return DefSubclassProc(hwnd, message, w, l);
        const bool edit = hwnd != self->mConfigList;
        if (edit && message == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) < 0)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0;
        }
        if (edit && message == WM_CHAR && w == 1) return 0;
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            RECT wells{124,84,788,464}; InvalidateRect(self->mHwnd, &wells, FALSE);
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, &App::ConfigInputSubclass, id);
        LRESULT result = DefSubclassProc(hwnd, message, w, l);
        if (message == WM_PAINT || message == WM_PRINTCLIENT)
        {
            const bool empty = edit ? GetWindowTextLengthW(hwnd) == 0 :
                SendMessageW(hwnd, LB_GETCOUNT, 0, 0) == 0;
            if (empty) {
                HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(w) : GetDC(hwnd);
                if (dc) {
                    RECT rc{}; GetClientRect(hwnd, &rc);
                    HGDIOBJ previousFont = SelectObject(dc, edit ? self->mFontUi : self->mFontSmall);
                    SetTextColor(dc, kThTextMuted); SetBkMode(dc, TRANSPARENT);
                    const wchar_t* text = edit ? L"\u041d\u0430\u043f\u0440\u0438\u043c\u0435\u0440, \u043e\u0441\u043d\u043e\u0432\u043d\u043e\u0439" :
                        L"\u041a\u043e\u043d\u0444\u0438\u0433\u043e\u0432 \u043f\u043e\u043a\u0430 \u043d\u0435\u0442\n\u041d\u0430\u0436\u043c\u0438\u0442\u0435 \xab\u0421\u043e\u0437\u0434\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433\xbb \u043d\u0438\u0436\u0435.";
                    if (!edit) { rc.left += 12; rc.right -= 12; rc.top += 48; }
                    DrawTextW(dc, text, -1, &rc, edit ? DT_LEFT | DT_VCENTER | DT_SINGLELINE : DT_CENTER | DT_WORDBREAK);
                    SelectObject(dc, previousFont);
                    if (message != WM_PRINTCLIENT) ReleaseDC(hwnd, dc);
                }
            }
        }
        if (message == WM_SETTEXT) InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    }

void App::PaintTrackbar(HWND slider, HDC target)
{
        RECT client{};
        GetClientRect(slider, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) return;

        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

        FillRect(dc, &client, mBrPanel ? mBrPanel : mBrBg);

        const int minimum = int(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0));
        const int maximum = int(SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
        const int position = int(SendMessageW(slider, TBM_GETPOS, 0, 0));
        const double ratio = (maximum > minimum)
            ? double(position - minimum) / double(maximum - minimum)
            : 0.0;

        const float thumbRadius = 8.f;
        const float trackLeft = client.left + 9.f;
        const float trackRight = client.right - 9.f;
        const float centerY = (client.top + client.bottom) / 2.f;
        const float thumbX = trackLeft + float((trackRight - trackLeft) * ratio);
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush track(GpColor(kThInput));
            GpPill(g, &track, trackLeft, centerY - 3.5f, trackRight - trackLeft, 7.f);
            if (thumbX > trackLeft + 1.f) {
                Gdiplus::SolidBrush progress(GpColor(kThAccent));
                GpPill(g, &progress, trackLeft, centerY - 3.5f, thumbX - trackLeft, 7.f);
            }
            const bool focused = (GetFocus() == slider);
            if (focused) {
                Gdiplus::SolidBrush ring(GpColor(kThAccent, 90));
                g.FillEllipse(&ring, thumbX - thumbRadius - 3.f, centerY - thumbRadius - 3.f,
                    (thumbRadius + 3.f) * 2.f, (thumbRadius + 3.f) * 2.f);
            }
            Gdiplus::SolidBrush thumb(GpColor(kThTextBright));
            g.FillEllipse(&thumb, thumbX - thumbRadius, centerY - thumbRadius,
                thumbRadius * 2.f, thumbRadius * 2.f);
        }

        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

LRESULT CALLBACK App::TrackbarSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref)
{
        App* self = reinterpret_cast<App*>(ref);
        switch (m)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            if (!self) break;
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            self->PaintTrackbar(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_KEYDOWN:
        case WM_MOUSEWHEEL:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        {
            const LRESULT result = DefSubclassProc(h, m, w, l);
            InvalidateRect(h, nullptr, FALSE);
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, &App::TrackbarSubclass, id);
            break;
        default:
            break;
        }
        return DefSubclassProc(h, m, w, l);
    }
