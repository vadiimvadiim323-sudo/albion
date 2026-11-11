// modules/ui/window.cpp
#include "modules/app/app.hpp"

void App::MessageLoop()
{
        LARGE_INTEGER freq, t0, now;
        QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
        unsigned capFrames = 0;
        MSG msg{};
        while (mRunning)
        {
            unsigned messageBudget=0;
            while (messageBudget++<128 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) { mRunning = false; break; }
                if (mPerformance.HandleMessage(msg)) continue;
                if (mDiagnostics.HandleMessage(msg)) continue;
                if (HandleConfigKey(msg)) continue;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (!mRunning) break;
            albion::performance::telemetry.MenuState((IsWindowVisible(mHwnd)&&!IsIconic(mHwnd)?1u:0u)|(mPerformance.Visible()?2u:0u));
            PollDiagnosticsHotkey();
            PollPerformanceSnapshotHotkey();
            PollJitterTraceHotkey();
            // Zakhvat, preprocess, inferens i navedenie rabotayut v PipelineThread.
            // Zdes ostayotsya tolko okno: soobshcheniya, overlay i statistika.
            MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            // Check for changed overlay contents at most about 60 times/second.
            if (mOverlay && IsWindowVisible(mOverlay))
            {
                const double overlayNow = NowMs();
                if (overlayNow - mOverlayLastMs >= 16.0)
                {
                    mOverlayLastMs = overlayNow;
                    UpdateOverlayInvalidation(overlayNow);
                }
            }

            QueryPerformanceCounter(&now);
            double dt = double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart);
            if (dt >= 0.5)
            {
                capFrames = 0; t0 = now;
                PollModelConversion();
                UiSnapshot ui;
                { std::lock_guard<std::mutex> lk(mUiMx); ui = mUiSnap; }
                // FIX-V11.1-UI-GUARD: optional status/overlay refresh must never
                // escape MessageLoop (e.g. LocalAppData lookup throwing).
                try { UpdateDiagnostics(); RefreshOverlayGeometry(); }
                catch (const std::exception& e) { DevLog("UI", e.what()); }
                catch (...) { DevLog("UI", "Diagnostics refresh failed"); }
                const double cfps = ui.capFps;
                wchar_t buf[512];
                {
                    wchar_t fpsBuf[64];
                    swprintf_s(fpsBuf, L"FPS: %.1f", cfps);
                    mHudFps = fpsBuf;
                    mHudReady = ui.ready;
                    mHudStatus = std::wstring(RU_HUD_STATUS) + (ui.ready ? RU_ON : RU_OFF);
                }
                if (ui.ready)
                {
                    const std::wstring method = AimMethodStatus();
                    const std::wstring fpsLimit = FpsLimitStatus();
                    const std::wstring capMethod = CaptureMethodStatus();
                    const int hudNet = gNetSize.load(std::memory_order_relaxed);
                    (void)method; (void)fpsLimit; (void)capMethod; (void)hudNet;
                    swprintf_s(buf, RU_FMT_HUD,
                        ui.backend, ui.inferMs,
                        ui.dets.size(),
                        VkToName(mActivationKey).c_str());
                }
                else
                {
                    // FIX-V11.1-HUD-UTF8: errors are UTF-8; %hs used the ANSI code
                    // page and printed Russian messages as mojibake.
                    std::wstring err = ui.error.empty() ? std::wstring(L"loading...") : Utf8ToWide(ui.error);
                    if (err.size() > 120) err.resize(120);
                    swprintf_s(buf, RU_FMT_HUD_LOAD, err.c_str());
                }
                mHudInfo = buf;
                // Only the two dynamic HUD areas changed. Repainting the whole
                // window every 500 ms made controls appear to twitch.
                RECT client{};
                GetClientRect(mHwnd, &client);
                // Sidebar readiness dot, settings status row and footer line.
                RECT leftStatus{ 2, client.bottom - 84, 92, client.bottom - 8 };
                RECT infoBar{ 124, client.bottom - 46, 788, client.bottom - 16 };
                if(IsWindowVisible(mHwnd) && !IsIconic(mHwnd)) {
                    InvalidateRect(mHwnd, &leftStatus, FALSE);
                    InvalidateRect(mHwnd, &infoBar, FALSE);
                }
            }
        }
        StopPipeline();
        mDet.Stop();
        DestroyOverlayBuffer();
        if (mOverlay) { DestroyWindow(mOverlay); mOverlay = nullptr; }
    }

LRESULT CALLBACK App::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l)
{
        App* self = nullptr;
        if (m == WM_NCCREATE)
        {
            self = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        try { return self ? self->WndProc(h, m, w, l) : DefWindowProcW(h, m, w, l); }
        catch (...) {
            DevLog("UI", "Unhandled UI operation error; shutting down cleanly");
            MessageBoxW(h,L"Ошибка операции интерфейса. Приложение будет закрыто; исходные файлы не удаляются.",L"Ошибка",MB_ICONERROR);
            PostQuitMessage(1); return m==WM_CREATE?-1:0; // 0 correctly rejects WM_NCCREATE
        }
    }

LRESULT App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
        switch (m)
        {
        case WM_NCHITTEST:
        {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &pt);
            RECT rc{};
            GetClientRect(h, &rc);
            if (pt.y < kTitleBarHeight && pt.x < rc.right - 250) return HTCAPTION;
            break;
        }
        case WM_NCLBUTTONDBLCLK:
            if (w == HTCAPTION) return 0; // fixed size even on title double-click
            break;
        case WM_SYSCOMMAND:
            // An Alt activation bind must not enter a modal system-menu loop
            // and freeze overlay/UI refresh until the next Alt press.
            if((w&0xfff0)==SC_KEYMENU &&
                (mActivationKey==VK_MENU||mActivationKey==VK_LMENU||mActivationKey==VK_RMENU))return 0;
            if ((w & 0xfff0) == SC_MAXIMIZE || (w & 0xfff0) == SC_SIZE) return 0;
            break;
        case WM_SIZE:
            LayoutControls();
            ApplyRoundedWindowRegion();
            if (mWinMaximize) InvalidateRect(mWinMaximize, nullptr, FALSE);
            RedrawWindow(h, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        case WM_COMMAND:
            if(LOWORD(w)==1080 && HIWORD(w)==BN_CLICKED) { ShowPerformance(); return 0; }
            // Closed/opened list: redraw the pill (chevron direction, outline).
            if ((HIWORD(w) == CBN_DROPDOWN || HIWORD(w) == CBN_CLOSEUP) && l)
                InvalidateRect(reinterpret_cast<HWND>(l), nullptr, FALSE);
            if (LOWORD(w) == IDC_HEAD_Y_RESET && HIWORD(w) == BN_CLICKED)
            {
                SendMessageW(mHeadYSlider, TBM_SETPOS, TRUE, 0);
                ApplyControlValues();
                SetFocus(mHwnd);
                return 0;
            }
            if (HIWORD(w) == BN_CLICKED)
            {
                if (LOWORD(w) == IDC_WINDOW_MINIMIZE) { ShowWindow(h, SW_MINIMIZE); return 0; }
                if (LOWORD(w) == IDC_WINDOW_CLOSE) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
            }
            if (HIWORD(w) == BN_CLICKED && LOWORD(w) >= IDC_BODY_HEAD && LOWORD(w) <= IDC_BODY_CLOSEST)
            {
                ApplyBodyPart(int(LOWORD(w) - IDC_BODY_HEAD));
                SetFocus(mHwnd);
                return 0;
            }
            if (LOWORD(w) == IDC_MODEL && HIWORD(w) == CBN_SELCHANGE) {
                ApplyModel();
                SetFocus(mHwnd);return 0;
            }
            if (LOWORD(w) == IDC_AIM_METHOD && HIWORD(w) == CBN_SELCHANGE) { ApplyAimMethod(); SetFocus(mHwnd); return 0; }
            if (LOWORD(w) == IDC_FPS_LIMIT && HIWORD(w) == CBN_SELCHANGE) { ApplyFpsLimit();  SetFocus(mHwnd); return 0; }
            if (LOWORD(w) == IDC_CAPTURE_METHOD && HIWORD(w) == CBN_SELCHANGE) { ApplyCaptureMethod(); SetFocus(mHwnd); return 0; }
            if (LOWORD(w) == IDC_MOVEMENT_PATH && HIWORD(w) == CBN_SELCHANGE) { ApplyMovementPath(); SetFocus(mHwnd); return 0; }
            if (LOWORD(w) == IDC_TAB_SETTINGS && HIWORD(w) == BN_CLICKED) { ApplyTab(0); return 0; }
            if (LOWORD(w) == IDC_TAB_VISUALS && HIWORD(w) == BN_CLICKED) { ApplyTab(1); return 0; }
            if (LOWORD(w) == IDC_TAB_AI && HIWORD(w) == BN_CLICKED) { ApplyTab(2); return 0; }
            if (LOWORD(w) == IDC_TAB_CONFIG && HIWORD(w) == BN_CLICKED) { RefreshConfigList(); ApplyTab(3); return 0; }
            if (LOWORD(w) == IDC_CONFIG_LIST && HIWORD(w) == LBN_SELCHANGE) { OnConfigSelectionChanged(); return 0; }
            if (LOWORD(w) == IDC_CONFIG_LIST && HIWORD(w) == LBN_DBLCLK) { LoadSelectedConfig(); return 0; }
            if (LOWORD(w) == IDC_CONFIG_NAME && HIWORD(w) == EN_CHANGE) { UpdateConfigButtons(); return 0; }
            if (LOWORD(w) == IDC_CONFIG_NEW && HIWORD(w) == BN_CLICKED) { NewConfigDraft(); return 0; }
            if (LOWORD(w) == IDC_CONFIG_CONFIRM && HIWORD(w) == BN_CLICKED) { CommitConfigEdit(); return 0; }
            if (LOWORD(w) == IDC_CONFIG_CANCEL && HIWORD(w) == BN_CLICKED) { CancelConfigEdit(); return 0; }
            if (LOWORD(w) == IDC_SAVE_CONFIG && HIWORD(w) == BN_CLICKED) { SaveNamedConfig(); return 0; }
            if (LOWORD(w) == IDC_LOAD_CONFIG && HIWORD(w) == BN_CLICKED) { LoadSelectedConfig(); return 0; }
            if (LOWORD(w) == IDC_DELETE_CONFIG && HIWORD(w) == BN_CLICKED) { DeleteSelectedConfig(); return 0; }
            if (LOWORD(w) == IDC_RENAME_CONFIG && HIWORD(w) == BN_CLICKED) { BeginConfigEdit(true); return 0; }
            if (LOWORD(w) == IDC_AIM_BIND && HIWORD(w) == BN_CLICKED) { BeginBindCapture(); return 0; }
            if ((LOWORD(w) == IDC_VIS_REGION || LOWORD(w) == IDC_VIS_BOXES ||
                LOWORD(w) == IDC_VIS_STATS || LOWORD(w) == IDC_VIS_CONF)
                && HIWORD(w) == BN_CLICKED) {
                switch (LOWORD(w))
                {
                case IDC_VIS_REGION: mShowRegion = !mShowRegion; break;
                case IDC_VIS_BOXES:  mShowBoxes = !mShowBoxes;  break;
                case IDC_VIS_STATS:  mShowStats = !mShowStats;  break;
                default:             mShowConfidence = !mShowConfidence; break;
                }
                ApplyVisuals(); return 0;
            }
            break;
        case WM_ERASEBKGND:
            if (!mBrBg) break;
            return 1;
        case WM_PAINT:
        {
            if (!mBrBg) break;
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT rc{};
            GetClientRect(h, &rc);
            const int width = rc.right - rc.left;
            const int height = rc.bottom - rc.top;
            if (EnsureSettingsBuffer(dc, width, height))
            {
                const int saved=SaveDC(mSettingsMemDc);
                IntersectClipRect(mSettingsMemDc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right,ps.rcPaint.bottom);
                PaintSettingsBackground(mSettingsMemDc);
                RestoreDC(mSettingsMemDc,saved);
                const int paintW = ps.rcPaint.right - ps.rcPaint.left;
                const int paintH = ps.rcPaint.bottom - ps.rcPaint.top;
                if (paintW > 0 && paintH > 0)
                    BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, paintW, paintH,
                        mSettingsMemDc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            }
            else
            {
                PaintSettingsBackground(dc);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            if (!mBrPanel) break;
            HDC dc = reinterpret_cast<HDC>(w);
            const HWND control = reinterpret_cast<HWND>(l);
            SetTextColor(dc, control == mConfigStatusLabel ? (mConfigStatusError ? kThAccent : kThTextMuted) : kThText);
            const bool configNameLabel = control == mConfigNameLabel && mConfigEditing;
            SetBkColor(dc, configNameLabel ? kThStatus : kThPanel);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(configNameLabel ? mBrStatus : mBrPanel);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        {
            if (!mBrInput) break;
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, kThText);
            SetBkColor(dc, kThInput);
            return reinterpret_cast<LRESULT>(mBrInput);
        }
        case WM_MEASUREITEM:
        {
            MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if (mi) { mi->itemHeight = mi->CtlID == IDC_CONFIG_LIST ? 52 : 26; return TRUE; }
            break;
        }
        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT* di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (di)
            {
                if (!mBrPanel || !mFontUi || !mFontBold) break;
                if (di->CtlType == ODT_BUTTON) { DrawThemedButton(di); return TRUE; }
                if (di->CtlType == ODT_COMBOBOX) { DrawThemedComboItem(di); return TRUE; }
                if (di->CtlType == ODT_LISTBOX) { DrawThemedListItem(di); return TRUE; }
            }
            break;
        }
        case WM_TIMER:
            if (w == kBindTimerId) { PollBindCapture(); return 0; }
            break;
        case WM_HSCROLL:
        {
            HWND c = reinterpret_cast<HWND>(l);
            if (c == mSpeedSlider || c == mCaptureSlider || c == mConfSlider || c == mHeadYSlider || c == mMouseScaleSlider)
            {
                if (c == mMouseScaleSlider) ApplyMouseScaleControl();
                else ApplyControlValues();
                // Repaint the complete track so the previous thumb position is
                // cleared immediately instead of accumulating partial drawings.
                RedrawWindow(c, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
            return 0;
        }
        case WM_KEYDOWN:
        {
            if (w == VK_ESCAPE) PostQuitMessage(0);
        }
        return 0;
        case WM_DESTROY:
            mRunning = false; PostQuitMessage(0); return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            if (mHwnd == h) mHwnd = nullptr;
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
