// modules/ui/controls.cpp
#include "modules/app/app.hpp"

void App::ApplyTab(int tab)
{
        const int nextTab = std::clamp(tab, 0, 3);
        const bool animateTab = mHwnd && nextTab != mActiveTab &&
            IsWindowVisible(mHwnd) && !IsIconic(mHwnd) && !AlbionReducedMotion() && BeginTabFade();
        mActiveTab = nextTab;
        const bool settings = (mActiveTab == 0);
        const bool visuals = (mActiveTab == 1);
        const bool ai = (mActiveTab == 2);
        const bool config = (mActiveTab == 3);

        // Freeze painting while controls are hidden/shown. This prevents the
        // intermediate frames that caused flashing and stale artifacts.
        if (mHwnd) SendMessageW(mHwnd, WM_SETREDRAW, FALSE, 0);

        HWND settingsCtrls[] = { mSpeedLabel, mSpeedSlider, mSpeedValue,
                                 mCaptureLabel, mCaptureSlider, mCaptureValue,
                                 mBodyPartLabel,
                                 mHeadYLabel, mHeadYSlider, mHeadYValue, mHeadYReset,
                                 mMouseScaleLabel, mMouseScaleSlider, mMouseScaleValue,
                                 mMethodLabel, mMethodCombo,
                                 mBindLabel, mBindButton };
        for (HWND h : settingsCtrls) if (h) ShowWindow(h, settings ? SW_SHOWNA : SW_HIDE);
        for (HWND h : mBodyPartButtons) if (h) ShowWindow(h, settings ? SW_SHOWNA : SW_HIDE);

        HWND visualCtrls[] = { mVisRegion, mVisBoxes, mVisStats, mVisConf };
        for (HWND h : visualCtrls) if (h) ShowWindow(h, visuals ? SW_SHOWNA : SW_HIDE);

        HWND aiCtrls[] = { mConfLabel, mConfSlider, mConfValue, mModelLabel, mModelCombo,
                           mConvertButton,mConvertStatus,mConvertLogButton,mOnnxFolderButton,
                           mCapLabel, mCapCombo, mFpsLabel, mFpsCombo,
                           mDiagnosticsLabel, mDiagnosticsButton };
        for (HWND h : aiCtrls) if (h) ShowWindow(h, ai ? SW_SHOWNA : SW_HIDE);
        if(mConvertProgress)ShowWindow(mConvertProgress,ai&&mConversionRequested.load()?SW_SHOWNA:SW_HIDE);

        HWND configCtrls[] = { mConfigTitle, mConfigList, mConfigNameLabel, mConfigNameEdit,
                               mSaveConfigBtn, mLoadConfigBtn, mDeleteConfigBtn,
                               mRenameConfigBtn, mConfigStatusLabel,
                               mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn };
        for (HWND h : configCtrls) if (h) ShowWindow(h, config ? SW_SHOWNA : SW_HIDE);
        UpdateConfigEditorVisibility();
        if (mHwnd)
        {
            SendMessageW(mHwnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(mHwnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        if (animateTab) StartTabFade();
    }

void App::EndTabFade()
{
        if (mTabFadeWnd) { HWND w = mTabFadeWnd; mTabFadeWnd = nullptr; DestroyWindow(w); }
        if (mTabFadeDc) { if (mTabFadeOld) SelectObject(mTabFadeDc, mTabFadeOld); DeleteDC(mTabFadeDc); }
        if (mTabFadeBmp) DeleteObject(mTabFadeBmp);
        mTabFadeDc = nullptr; mTabFadeBmp = nullptr; mTabFadeOld = nullptr;
    }

void App::PushTabFade(BYTE alpha, int dy)
{
        if (!mTabFadeWnd || !mTabFadeDc) return;
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, 0 };
        POINT dst{ mTabFadeRc.left, mTabFadeRc.top + dy }, src{ 0, 0 };
        SIZE size{ mTabFadeRc.right - mTabFadeRc.left, mTabFadeRc.bottom - mTabFadeRc.top };
        UpdateLayeredWindow(mTabFadeWnd, nullptr, &dst, &size, mTabFadeDc, &src, 0, &bf, ULW_ALPHA);
    }

bool App::BeginTabFade()
{
        EndTabFade();
        RECT client{};
        if (!GetClientRect(mHwnd, &client)) return false;
        // Content area only: the sidebar and the accent frame stay crisp. The
        // inset leaves room for the 8 px upward slide inside the window.
        RECT area{ 94, 14, client.right - 14, client.bottom - 14 };
        const int w = area.right - area.left, hgt = area.bottom - area.top;
        if (w <= 0 || hgt <= 0) return false;
        HDC wndDc = GetDC(mHwnd);
        if (!wndDc) return false;
        HDC full = CreateCompatibleDC(wndDc);
        HBITMAP fullBmp = full ? CreateCompatibleBitmap(wndDc, client.right, client.bottom) : nullptr;
        mTabFadeDc = CreateCompatibleDC(wndDc);
        mTabFadeBmp = mTabFadeDc ? CreateCompatibleBitmap(wndDc, w, hgt) : nullptr;
        bool ok = full && fullBmp && mTabFadeDc && mTabFadeBmp;
        if (ok) {
            HGDIOBJ oldFull = SelectObject(full, fullBmp);
            mTabFadeOld = SelectObject(mTabFadeDc, mTabFadeBmp);
            // PW_CLIENTONLY | PW_RENDERFULLCONTENT; falls back to the visible pixels.
            if (!PrintWindow(mHwnd, full, 3))
                BitBlt(full, 0, 0, client.right, client.bottom, wndDc, 0, 0, SRCCOPY);
            BitBlt(mTabFadeDc, 0, 0, w, hgt, full, area.left, area.top, SRCCOPY);
            SelectObject(full, oldFull);
        }
        if (fullBmp) DeleteObject(fullBmp);
        if (full) DeleteDC(full);
        ReleaseDC(mHwnd, wndDc);
        if (!ok) { EndTabFade(); return false; }

        static bool registered = false;
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(mHwnd, GWLP_HINSTANCE));
        if (!registered) {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = &App::TabFadeProc; wc.hInstance = inst;
            wc.lpszClassName = L"AlbionTabFade";
            registered = RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            if (!registered) { EndTabFade(); return false; }
        }
        POINT origin{ area.left, area.top };
        ClientToScreen(mHwnd, &origin);
        mTabFadeRc = RECT{ origin.x, origin.y, origin.x + w, origin.y + hgt };
        mTabFadeWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"AlbionTabFade", L"", WS_POPUP, origin.x, origin.y, w, hgt, mHwnd, nullptr, inst, this);
        if (!mTabFadeWnd) { EndTabFade(); return false; }
        SetWindowLongPtrW(mTabFadeWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        PushTabFade(255, 0);
        ShowWindow(mTabFadeWnd, SW_SHOWNOACTIVATE);
        return true;
    }

std::wstring App::AimMethodStatus() const
{
        if (mAimMethod == AimMethod::WinApi) return RU_METHOD_WINAPI;
        return std::wstring(L"Arduino: ") + mArduino.Status();
    }

void App::ApplyAimMethod()
{
        if (!mMethodCombo) return;
        const int sel = int(SendMessageW(mMethodCombo, CB_GETCURSEL, 0, 0));
        if (sel == CB_ERR) return;
        mAimMethod = sel == 1 ? AimMethod::ArduinoHid : AimMethod::WinApi;
        QueueSettings();
    }

std::wstring App::UiImagePath(const wchar_t* fileName) const
{
        // FIX-V11-PATH: identical long-path-safe asset root as fonts/models.
        return ExeRelativePath(L"extras\\images\\") + fileName;
    }

std::unique_ptr<Gdiplus::Image> App::LoadUiImage(const wchar_t* fileName) const
{
        auto image = std::make_unique<Gdiplus::Image>(UiImagePath(fileName).c_str(), FALSE);
        if (image->GetLastStatus() != Gdiplus::Ok) return {};
        return image;
    }

void App::LoadUiImages()
{
        mImgLogo = LoadUiImage(L"albionai.png");
        mImgSettings = LoadUiImage(L"settings.png");
        mImgVisual = LoadUiImage(L"visual.png");
        mImgAi = LoadUiImage(L"ai.png");
        mImgConfigs = LoadUiImage(L"configs.png");
        mImgDelete = LoadUiImage(L"delete.png");
        mImgRename = LoadUiImage(L"rename.png");
        mImgTrigger = LoadUiImage(L"triggerbot.png");
        mImgConvert = LoadUiImage(L"convert-white.png");
        // The load action is text-only, so its former icon is not loaded.
    }

void App::ResetUiImages()
{
        mImgLogo.reset(); mImgSettings.reset(); mImgVisual.reset(); mImgAi.reset();
        mImgConfigs.reset(); mImgDelete.reset(); mImgRename.reset();
        mImgTrigger.reset(); mImgUpdate.reset();mImgConvert.reset();
    }

Gdiplus::Image* App::ButtonImage(HWND button) const
{
        if (button == mTabSettings) return mImgSettings.get();
        if (button == mTabVisuals) return mImgVisual.get();
        if (button == mTabAi) return mImgAi.get();
        if (button == mTabConfig) return mImgConfigs.get();
        if (button == mDeleteConfigBtn) return mImgDelete.get();
        if (button == mRenameConfigBtn) return mImgRename.get();
        if (button == mBindButton) return mImgTrigger.get();
        // The load button is intentionally text-only.
        return nullptr;
    }

void App::DrawUiImage(HDC dc, Gdiplus::Image* image, RECT bounds, int padding) const
{
        if (!image || image->GetLastStatus() != Gdiplus::Ok) return;
        const int availableW = bounds.right - bounds.left - padding * 2;
        const int availableH = bounds.bottom - bounds.top - padding * 2;
        const int boxW = availableW > 1 ? availableW : 1;
        const int boxH = availableH > 1 ? availableH : 1;
        const UINT sourceW = image->GetWidth(), sourceH = image->GetHeight();
        if (!sourceW || !sourceH) return;
        const double scaleW = double(boxW) / double(sourceW);
        const double scaleH = double(boxH) / double(sourceH);
        const double scale = scaleW < scaleH ? scaleW : scaleH;
        const int scaledW = int(sourceW * scale + 0.5);
        const int scaledH = int(sourceH * scale + 0.5);
        const int drawW = scaledW > 1 ? scaledW : 1;
        const int drawH = scaledH > 1 ? scaledH : 1;
        const int x = bounds.left + (bounds.right - bounds.left - drawW) / 2;
        const int y = bounds.top + (bounds.bottom - bounds.top - drawH) / 2;
        Gdiplus::Graphics graphics(dc);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(image, x, y, drawW, drawH);
    }

void App::DrawUiImageMonochrome(HDC dc, Gdiplus::Image* image, RECT bounds,
        COLORREF color) const
{
        if (!image || image->GetLastStatus() != Gdiplus::Ok) return;
        const UINT sourceW = image->GetWidth(), sourceH = image->GetHeight();
        if (!sourceW || !sourceH) return;
        const int boxW = std::max<int>(1,
            static_cast<int>(bounds.right - bounds.left));
        const int boxH = std::max<int>(1,
            static_cast<int>(bounds.bottom - bounds.top));
        const double scale = std::min(double(boxW) / double(sourceW),
            double(boxH) / double(sourceH));
        const int drawW = std::max<int>(1,
            static_cast<int>(double(sourceW) * scale + 0.5));
        const int drawH = std::max<int>(1,
            static_cast<int>(double(sourceH) * scale + 0.5));
        const int x = bounds.left + (boxW - drawW) / 2;
        const int y = bounds.top + (boxH - drawH) / 2;

        const Gdiplus::REAL red = Gdiplus::REAL(GetRValue(color)) / 255.0f;
        const Gdiplus::REAL green = Gdiplus::REAL(GetGValue(color)) / 255.0f;
        const Gdiplus::REAL blue = Gdiplus::REAL(GetBValue(color)) / 255.0f;
        Gdiplus::ColorMatrix matrix = {
            0, 0, 0, 0, 0,
            0, 0, 0, 0, 0,
            0, 0, 0, 0, 0,
            0, 0, 0, 1, 0,
            red, green, blue, 0, 1
        };
        Gdiplus::ImageAttributes attributes;
        attributes.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault,
            Gdiplus::ColorAdjustTypeBitmap);
        Gdiplus::Graphics graphics(dc);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::Rect destination(x, y, drawW, drawH);
        graphics.DrawImage(image, destination, 0, 0, int(sourceW), int(sourceH),
            Gdiplus::UnitPixel, &attributes);
    }

void App::CreateSettingsWindow(HINSTANCE hInst)
{
        // FIX-V11.1-DPI: 1.0.0 forced DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED
        // here. Windows then bitmap-stretched GDI+ painting and the layered
        // animation snapshots (blurry text, cards misaligned with controls).
        // The UI again runs per-monitor aware like the original build.
        INITCOMMONCONTROLSEX icc{ sizeof(icc),ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
        if (!InitCommonControlsEx(&icc))
            throw std::runtime_error("InitCommonControlsEx failed");

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = &App::WndProcStatic;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!mBrBg) mBrBg = CreateSolidBrush(kThBg);
        wc.hbrBackground = mBrBg;
        wc.lpszClassName = L"AimSettingsWnd";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("RegisterClassExW failed");

        // Borderless window: the standard caption is replaced by our own title bar.
        // Fixed layout: no resize or maximize, so controls stay in place.
        constexpr DWORD windowStyle = WS_POPUP | WS_MINIMIZEBOX |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        RECT r{ 0,0,820,540 };
        AdjustWindowRect(&r, windowStyle, FALSE);

        mHwnd = CreateWindowExW(0, wc.lpszClassName,
            RU_WIN_TITLE,
            windowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT,
            r.right - r.left, r.bottom - r.top,
            nullptr, nullptr, hInst, this);
        if (!mHwnd) throw std::runtime_error("CreateWindow failed");
        // Tyomnyy zagolovok okna (Windows 10 1809+).
        {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(mHwnd, 20, &dark, sizeof(dark));
            DwmSetWindowAttribute(mHwnd, 19, &dark, sizeof(dark));
        }
        ApplyRoundedWindowRegion();
        // The window is shown at the end of CreateControls, once every child
        // control has its final position. See RevealSettingsWindow().
    }

void App::CenterSettingsWindow()
{
        if (!mHwnd) return;
        POINT cursor{};
        if (!GetCursorPos(&cursor)) cursor = POINT{ 0, 0 };
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        RECT window{};
        if (!monitor || !GetMonitorInfoW(monitor, &info) || !GetWindowRect(mHwnd, &window)) return;
        const int width = window.right - window.left;
        const int height = window.bottom - window.top;
        const RECT& work = info.rcWork;
        const int workWidth = work.right - work.left;
        const int workHeight = work.bottom - work.top;
        if (width <= 0 || height <= 0 || workWidth <= 0 || workHeight <= 0) return;
        const int x = work.left + (width < workWidth ? (workWidth - width) / 2 : 0);
        const int y = work.top + (height < workHeight ? (workHeight - height) / 2 : 0);
        SetWindowPos(mHwnd, nullptr, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

void App::RevealSettingsWindow()
{
        if (!mHwnd) return;
        ApplyRoundedWindowRegion();
        // Coming from the license screen: open exactly where it was, then let
        // its snapshot cross-fade away on top of the settings page.
        RECT handoff{};
        const bool fromAuth = AlbionAuthHandoffRect(&handoff);
        if (fromAuth)
            SetWindowPos(mHwnd, nullptr, handoff.left, handoff.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        else
            CenterSettingsWindow();
        ShowWindow(mHwnd, SW_SHOW);
        RedrawWindow(mHwnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (fromAuth) AlbionAuthFinishHandoff(mHwnd);
    }

void App::CreateControls()
{
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!mBrPanel)  mBrPanel = CreateSolidBrush(kThPanel);
        if (!mBrInput)  mBrInput = CreateSolidBrush(kThInput);
        if (!mBrStatus) mBrStatus = CreateSolidBrush(kThStatus);
        // Montserrat from extras\fonts (falls back to Segoe UI).
        if (!mFontUi)    mFontUi = AlbionFont(15, 500);
        if (!mFontBold)  mFontBold = AlbionFont(15, 700);
        if (!mFontTitle) mFontTitle = AlbionFont(28, 700);
        if (!mFontTab)   mFontTab = AlbionFont(20, 700);
        if (!mFontSmall) mFontSmall = AlbionFont(13, 500);
        if (!mFontMono)
            mFontMono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        HFONT font = mFontUi;
        LoadUiImages();

        auto mkStatic = [&](const wchar_t* t, int x, int y, int w = 158)->HWND {
            return CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE,
                x, y, w, 22, mHwnd, nullptr, inst, nullptr); };

        mSpeedLabel = mkStatic(RU_AIM_SPEED, 152, 50);
        mCaptureLabel = mkStatic(RU_CAPTURE, 152, 86);
        mBodyPartLabel = mkStatic(RU_BODY_PART, 152, 122);
        mMethodLabel = mkStatic(RU_AIM_METHOD, 152, 194);
        mFpsLabel = mkStatic(RU_FPS_LIMIT, 152, 230);
        mCapLabel = mkStatic(RU_CAP_METHOD, 152, 266);
        mConfLabel = mkStatic(RU_CONFIDENCE, 152, 14);
        mModelLabel = mkStatic(RU_MODEL, 152, 50);

        mSpeedSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
            314, 46, 374, 28, mHwnd, reinterpret_cast<HMENU>(IDC_AIM_SPEED), inst, nullptr);
        mSpeedValue = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            694, 50, 84, 22, mHwnd, reinterpret_cast<HMENU>(IDC_SPEED_VALUE), inst, nullptr);

        mCaptureSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
            314, 82, 374, 28, mHwnd, reinterpret_cast<HMENU>(IDC_CAPTURE_SIZE), inst, nullptr);
        mCaptureValue = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            694, 86, 84, 22, mHwnd, reinterpret_cast<HMENU>(IDC_CAPTURE_VALUE), inst, nullptr);

        mConfSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
            314, 10, 374, 28, mHwnd, reinterpret_cast<HMENU>(IDC_CONFIDENCE), inst, nullptr);
        mConfValue = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            694, 14, 84, 22, mHwnd, reinterpret_cast<HMENU>(IDC_CONF_VALUE), inst, nullptr);

        // Horizontal, mutually exclusive body-part selectors. They are drawn
        // manually so the selected marker uses the red accent instead of green.
        const wchar_t* bodyPartNames[] = {
            RU_HEAD_SHORT, RU_NECK_SHORT, RU_BODY_SHORT, RU_CLOSEST_SHORT
        };
        const int bodyPartIds[] = {
            IDC_BODY_HEAD, IDC_BODY_NECK, IDC_BODY_TORSO, IDC_BODY_CLOSEST
        };
        const int bodyPartX[] = { 314, 406, 480, 554 };
        const int bodyPartW[] = { 88, 70, 70, 124 };
        for (size_t i = 0; i < mBodyPartButtons.size(); ++i)
        {
            mBodyPartButtons[i] = CreateWindowExW(0, L"BUTTON", bodyPartNames[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                bodyPartX[i], 116, bodyPartW[i], 30, mHwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(bodyPartIds[i])), inst, nullptr);
        }

        mHeadYLabel = mkStatic(RU_HEAD_Y_OFFSET, 152, 158);
        mHeadYSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
            314, 154, 310, 28, mHwnd, reinterpret_cast<HMENU>(IDC_HEAD_Y_OFFSET), inst, nullptr);
        mHeadYValue = CreateWindowExW(0, L"STATIC", L"0 px",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            630, 158, 78, 22, mHwnd, reinterpret_cast<HMENU>(IDC_HEAD_Y_VALUE), inst, nullptr);
        mHeadYReset = CreateWindowExW(0, L"BUTTON", RU_RESET_OFFSET,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            714, 154, 64, 28, mHwnd, reinterpret_cast<HMENU>(IDC_HEAD_Y_RESET), inst, nullptr);
        mHeadYHint = mkStatic(RU_HEAD_Y_HINT, 152, 302, 626);
        ShowWindow(mHeadYHint, SW_HIDE); // hint removed from the UI

        mMouseScaleLabel = mkStatic(RU_MOUSE_SCALE, 152, 338);
        mMouseScaleSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
            314, 334, 374, 28, mHwnd, reinterpret_cast<HMENU>(IDC_MOUSE_SCALE), inst, nullptr);
        mMouseScaleValue = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            694, 338, 84, 22, mHwnd, reinterpret_cast<HMENU>(IDC_MOUSE_SCALE_VALUE), inst, nullptr);

        mMethodCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
            314, 190, 240, 200, mHwnd, reinterpret_cast<HMENU>(IDC_AIM_METHOD), inst, nullptr);
        const wchar_t* methods[] = { RU_METHOD_WINAPI, RU_METHOD_ARDUINO };
        for (const wchar_t* item : methods)
            SendMessageW(mMethodCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        SendMessageW(mMethodCombo, CB_SETCURSEL, WPARAM(int(mAimMethod)), 0);

        mModelCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
            314, 46, 340, 240, mHwnd, reinterpret_cast<HMENU>(IDC_MODEL), inst, nullptr);
        FillModelCombo();
        mFpsCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
            314, 226, 200, 220, mHwnd, reinterpret_cast<HMENU>(IDC_FPS_LIMIT), inst, nullptr);
        for (int fps : kFpsOptions)
        {
            wchar_t item[32];
            if (fps <= 0) wcscpy_s(item, RU_FPS_UNLIM);
            else          swprintf_s(item, L"%d", fps);
            SendMessageW(mFpsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        {
            int sel = 0;
            for (int i = 0; i < int(_countof(kFpsOptions)); ++i)
                if (kFpsOptions[i] == mFpsLimit.load(std::memory_order_relaxed)) { sel = i; break; }
            SendMessageW(mFpsCombo, CB_SETCURSEL, WPARAM(sel), 0);
        }

        mCapCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
            314, 262, 240, 200, mHwnd, reinterpret_cast<HMENU>(IDC_CAPTURE_METHOD), inst, nullptr);
        {
            const wchar_t* capItems[] = { L"DXGI · ROI", L"DXGI · полный кадр" };
            for (const wchar_t* item : capItems)
                SendMessageW(mCapCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            SendMessageW(mCapCombo, CB_SETCURSEL, WPARAM(int(mCaptureMethod)), 0);
        }

        mPathLabel = CreateWindowExW(0, L"STATIC", RU_PATH, WS_CHILD,
            570, 204, 208, 20, mHwnd, nullptr, inst, nullptr);
        mPathCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS | CBS_OWNERDRAWFIXED,
            570, 226, 208, 220, mHwnd, reinterpret_cast<HMENU>(IDC_MOVEMENT_PATH), inst, nullptr);
        {
            const wchar_t* pathItems[] = { L"Linear", L"Quadratic Bezier", L"Cubic Bezier",
                                           L"Exponential", L"Adaptive" };
            for (const wchar_t* item : pathItems)
                SendMessageW(mPathCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            SendMessageW(mPathCombo, CB_SETCURSEL,
                WPARAM(std::clamp<int>(int(mAimCfg.movementPath), 0, 4)), 0);
        }

        // Compact icon-only navigation, inspired by the reference sidebar.
        // Positions are shifted down later together with the other controls.
        mTabSettings = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            34, 80, 48, 48, mHwnd, reinterpret_cast<HMENU>(IDC_TAB_SETTINGS), inst, nullptr);
        mTabVisuals = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            34, 138, 48, 48, mHwnd, reinterpret_cast<HMENU>(IDC_TAB_VISUALS), inst, nullptr);
        mTabAi = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            34, 196, 48, 48, mHwnd, reinterpret_cast<HMENU>(IDC_TAB_AI), inst, nullptr);
        mTabConfig = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            34, 254, 48, 48, mHwnd, reinterpret_cast<HMENU>(IDC_TAB_CONFIG), inst, nullptr);

        // Top tabs removed: navigation lives only in the sidebar.

        // Vkladka "Vizualy" - sozdaem skrytymi, pokazyvaet ApplyTab.
        mVisRegion = CreateWindowExW(0, L"BUTTON", RU_VIS_REGION,
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            152, 14, 420, 26, mHwnd, reinterpret_cast<HMENU>(IDC_VIS_REGION), inst, nullptr);
        mVisBoxes = CreateWindowExW(0, L"BUTTON", RU_VIS_BOXES,
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            152, 50, 420, 26, mHwnd, reinterpret_cast<HMENU>(IDC_VIS_BOXES), inst, nullptr);
        mVisStats = CreateWindowExW(0, L"BUTTON", RU_VIS_STATS,
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            152, 86, 420, 26, mHwnd, reinterpret_cast<HMENU>(IDC_VIS_STATS), inst, nullptr);
        mVisConf = CreateWindowExW(0, L"BUTTON", RU_VIS_CONF,
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            152, 122, 420, 26, mHwnd, reinterpret_cast<HMENU>(IDC_VIS_CONF), inst, nullptr);

        // Reference layout: large list, four actions, full-width create button.
        auto configControl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 1, 1,
                mHwnd, id ? reinterpret_cast<HMENU>(INT_PTR(id)) : nullptr, inst, nullptr);
        };
        mConfigTitle = configControl(L"STATIC", L"\u0412\u0430\u0448\u0438 \u043a\u043e\u043d\u0444\u0438\u0433\u0438", SS_LEFT, 0);
        mConfigList = configControl(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
            LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED, IDC_CONFIG_LIST);
        mConfigNameLabel = configControl(L"STATIC", L"\u0418\u043c\u044f \u043a\u043e\u043d\u0444\u0438\u0433\u0430 (\u0431\u0435\u0437 .cfg)", SS_LEFT, 0);
        mConfigNameEdit = configControl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_CONFIG_NAME);
        const DWORD actionStyle = WS_TABSTOP | BS_OWNERDRAW;
        mConfigNewBtn = configControl(L"BUTTON", L"\u0421\u043e\u0437\u0434\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433", actionStyle, IDC_CONFIG_NEW);
        mSaveConfigBtn = configControl(L"BUTTON", L"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c", actionStyle, IDC_SAVE_CONFIG);
        mLoadConfigBtn = configControl(L"BUTTON", L"\u0417\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c", actionStyle, IDC_LOAD_CONFIG);
        // Accessible control names remain available for both icon-only actions.
        mRenameConfigBtn = configControl(L"BUTTON", L"\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c", actionStyle, IDC_RENAME_CONFIG);
        mDeleteConfigBtn = configControl(L"BUTTON", L"\u0423\u0434\u0430\u043b\u0438\u0442\u044c", actionStyle, IDC_DELETE_CONFIG);
        mConfigConfirmBtn = configControl(L"BUTTON", L"\u0421\u043e\u0437\u0434\u0430\u0442\u044c", actionStyle, IDC_CONFIG_CONFIRM);
        mConfigCancelBtn = configControl(L"BUTTON", L"\u041e\u0442\u043c\u0435\u043d\u0430", actionStyle, IDC_CONFIG_CANCEL);
        mConfigStatusLabel = configControl(L"STATIC", L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u043a\u043e\u043d\u0444\u0438\u0433 \u0438\u043b\u0438 \u0441\u043e\u0437\u0434\u0430\u0439\u0442\u0435 \u043d\u043e\u0432\u044b\u0439.", SS_LEFT | SS_ENDELLIPSIS, 0);
        SendMessageW(mConfigNameEdit, EM_LIMITTEXT, 120, 0);
        for (HWND input : { mConfigNameEdit, mConfigList })
            SetWindowSubclass(input, &App::ConfigInputSubclass, 8, reinterpret_cast<DWORD_PTR>(this));

        HWND ctrls[] = { mSpeedLabel,mSpeedSlider,mSpeedValue,
                      mCaptureLabel,mCaptureSlider,mCaptureValue,
                      mConfLabel,mConfSlider,mConfValue,
                      mBodyPartLabel,
                      mHeadYLabel,mHeadYSlider,mHeadYValue,mHeadYReset,mHeadYHint,
                      mMouseScaleLabel,mMouseScaleSlider,mMouseScaleValue,
                      mMethodLabel,mMethodCombo,
                      mModelLabel,mModelCombo,
                      mFpsLabel,mFpsCombo,
                      mCapLabel,mCapCombo,
                      mPathLabel,mPathCombo,
                      mTabSettings,mTabVisuals,mTabAi,mTabConfig,
                      mVisRegion,mVisBoxes,mVisStats,mVisConf,
                      mConfigTitle,mConfigList,mConfigNameLabel,mConfigNameEdit,
                      mSaveConfigBtn,mLoadConfigBtn,mDeleteConfigBtn,mRenameConfigBtn,
                      mConfigStatusLabel, mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn };
        for (HWND h : ctrls) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        for (HWND h : mBodyPartButtons)
            if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        SendMessageW(mHeadYSlider, TBM_SETRANGEMIN, FALSE, LPARAM(kMinHeadYOffset));
        SendMessageW(mHeadYSlider, TBM_SETRANGEMAX, FALSE, LPARAM(kMaxHeadYOffset));
        SendMessageW(mHeadYSlider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(mHeadYSlider, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(mHeadYSlider, TBM_SETPOS, TRUE, std::clamp(mAimCfg.headYOffsetPixels, kMinHeadYOffset, kMaxHeadYOffset));
        SendMessageW(mHeadYHint, WM_SETFONT, reinterpret_cast<WPARAM>(mFontSmall), TRUE);
        SendMessageW(mMouseScaleSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinMouseScale, kMaxMouseScale));
        SendMessageW(mMouseScaleSlider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(mMouseScaleSlider, TBM_SETPAGESIZE, 0, 5);
        SyncMouseScaleControl();

        SendMessageW(mSpeedSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinAimSpeed, kMaxAimSpeed));
        SendMessageW(mSpeedSlider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(mSpeedSlider, TBM_SETPAGESIZE, 0, 1);
        int spd = std::clamp<int>(int(mAimCfg.smoothing * 50.f + .5f), kMinAimSpeed, kMaxAimSpeed);
        SendMessageW(mSpeedSlider, TBM_SETPOS, TRUE, spd);

        // The capture slider now uses the actual pixel value, so dragging is smooth.
        SendMessageW(mCaptureSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinCaptureSize, kMaxCaptureSize));
        SendMessageW(mCaptureSlider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(mCaptureSlider, TBM_SETPAGESIZE, 0, 10);
        int sz = std::clamp<int>(int(mAimCfg.radius * 2), kMinCaptureSize, kMaxCaptureSize);
        SendMessageW(mCaptureSlider, TBM_SETPOS, TRUE, sz);

        SendMessageW(mConfSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kMinConfidence, kMaxConfidence));
        SendMessageW(mConfSlider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(mConfSlider, TBM_SETPAGESIZE, 0, 1);
        int conf = std::clamp<int>(int(mAimCfg.confidence * 100.f + .5f),
            kMinConfidence, kMaxConfidence);
        SendMessageW(mConfSlider, TBM_SETPOS, TRUE, conf);

        // Vybor klavishi aktivacii navedeniya - vkladka "Nastroyki".
        mBindLabel = CreateWindowExW(0, L"STATIC", RU_BIND_LABEL,
            WS_CHILD | WS_VISIBLE,
            152, 14, 158, 22, mHwnd, nullptr, inst, nullptr);
        mBindButton = CreateWindowExW(0, L"BUTTON", L"RMouse",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            314, 10, 180, 30, mHwnd, reinterpret_cast<HMENU>(IDC_AIM_BIND), inst, nullptr);
        SendMessageW(mBindLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(mBindButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        UpdateBindButton();

        // Tyomnye sistemnye chasti (strelka kombo, polosy prokrutki).
        if (mConfigNameEdit) SetWindowTheme(mConfigNameEdit, L"DarkMode_CFD", nullptr);
        HWND comboCtrls[] = { mMethodCombo, mModelCombo, mFpsCombo,
                              mCapCombo, mPathCombo };
        for (HWND hc : comboCtrls)
        {
            if (!hc) continue;
            // The themed combo painted its own grey drop-down button over our
            // pill on hover/click. Unthemed + fully custom WM_PAINT fixes it.
            SetWindowTheme(hc, L" ", L" ");
            COMBOBOXINFO info{ sizeof(info) };
            if (GetComboBoxInfo(hc, &info) && info.hwndList)
                SetWindowTheme(info.hwndList, L"DarkMode_Explorer", nullptr);
            SetWindowSubclass(hc, &App::ComboSubclass, 1,
                reinterpret_cast<DWORD_PTR>(this));
        }
        HWND sliderCtrls[] = { mSpeedSlider, mCaptureSlider, mConfSlider, mHeadYSlider, mMouseScaleSlider };
        for (HWND hs : sliderCtrls)
        {
            if (!hs) continue;
            SetWindowTheme(hs, L" ", L" ");
            SetWindowSubclass(hs, &App::TrackbarSubclass, 2,
                reinterpret_cast<DWORD_PTR>(this));
        }
        if (mConfigList) SetWindowTheme(mConfigList, L"DarkMode_Explorer", nullptr);
        if (mConfigTitle) SendMessageW(mConfigTitle, WM_SETFONT, reinterpret_cast<WPARAM>(mFontBold), TRUE);
        SendMessageW(mConfigStatusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(mFontSmall), TRUE);
        SendMessageW(mConfigList, LB_SETITEMHEIGHT, 0, 52);

        ApplyControlValues();
        RefreshConfigList();
        UpdateConfigButtons();

        ApplyTab(0);

        const DWORD captionButtonStyle = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_FLAT;
        mWinMinimize = CreateWindowExW(0, L"BUTTON", L"", captionButtonStyle,
            674, 7, 34, 28, mHwnd, reinterpret_cast<HMENU>(IDC_WINDOW_MINIMIZE), inst, nullptr);
        mWinClose = CreateWindowExW(0, L"BUTTON", L"", captionButtonStyle,
            750, 7, 40, 28, mHwnd, reinterpret_cast<HMENU>(IDC_WINDOW_CLOSE), inst, nullptr);
        HWND captionButtons[] = { mWinMinimize, mWinClose };
        for (HWND button : captionButtons)
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(mFontUi), TRUE);
        HWND hoverButtons[] = { mTabSettings, mTabVisuals, mTabAi, mTabConfig,
            mHeaderTabs[0], mHeaderTabs[1], mHeaderTabs[2], mHeaderTabs[3],
            mWinMinimize, mWinClose, mBindButton, mHeadYReset,
            mSaveConfigBtn, mLoadConfigBtn, mDeleteConfigBtn, mRenameConfigBtn,
            mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn,
            mVisRegion, mVisBoxes, mVisStats, mVisConf };
        for (HWND button : hoverButtons)
            if (button) SetWindowSubclass(button, &App::UiButtonSubclass, 3,
                reinterpret_cast<DWORD_PTR>(this));
        HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, mHwnd, nullptr, inst, nullptr);
        if (tooltip)
        {
            SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 300);
            const HWND tools[] = { mTabSettings, mTabVisuals, mTabAi, mTabConfig,
                mDeleteConfigBtn, mRenameConfigBtn };
            const wchar_t* tips[] = { RU_TAB_SETTINGS, RU_TAB_VISUALS, RU_TAB_AI, RU_TAB_CONFIG,
                L"\u0423\u0434\u0430\u043b\u0438\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433\u0443\u0440\u0430\u0446\u0438\u044e", L"\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433\u0443\u0440\u0430\u0446\u0438\u044e" };
            for (size_t i = 0; i < _countof(tools); ++i)
            {
                TOOLINFOW ti{ sizeof(ti) };
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd = mHwnd;
                ti.uId = reinterpret_cast<UINT_PTR>(tools[i]);
                ti.lpszText = const_cast<wchar_t*>(tips[i]);
                SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            }
        }
        // FIX-V11.1-UI-STYLE: diagnostics is a regular row of the AI tab, drawn
        // as the same owner-drawn pill as the other buttons (1.0.x placed a
        // system push button on the title bar of every tab).
        mDiagnosticsLabel=mkStatic(L"Диагностика:",152,266);
        mDiagnosticsButton=CreateWindowExW(0,L"BUTTON",L"Открыть · F8",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,
            0,0,220,32,mHwnd,reinterpret_cast<HMENU>(INT_PTR(1080)),inst,nullptr);
        for(HWND h : {mDiagnosticsLabel,mDiagnosticsButton})
            if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        if(mDiagnosticsButton) SetWindowSubclass(mDiagnosticsButton,&App::UiButtonSubclass,3,reinterpret_cast<DWORD_PTR>(this));
        if(mDiagnosticsLabel) ShowWindow(mDiagnosticsLabel,mActiveTab==2?SW_SHOWNA:SW_HIDE);
        if(mDiagnosticsButton) ShowWindow(mDiagnosticsButton,mActiveTab==2?SW_SHOWNA:SW_HIDE);
        LayoutControls();
        RevealSettingsWindow();
    }

void App::DestroySettingsBuffer()
{
        if (!mSettingsMemDc) return;
        if (mSettingsOldBitmap) SelectObject(mSettingsMemDc, mSettingsOldBitmap);
        mSettingsOldBitmap = nullptr;
        if (mSettingsBitmap) DeleteObject(mSettingsBitmap);
        mSettingsBitmap = nullptr;
        DeleteDC(mSettingsMemDc);
        mSettingsMemDc = nullptr;
        mSettingsBufferW = mSettingsBufferH = 0;
    }

bool App::EnsureSettingsBuffer(HDC target, int width, int height)
{
        if (width <= 0 || height <= 0) return false;
        if (mSettingsMemDc && mSettingsBitmap &&
            mSettingsBufferW == width && mSettingsBufferH == height) return true;

        DestroySettingsBuffer();
        mSettingsMemDc = CreateCompatibleDC(target);
        if (!mSettingsMemDc) return false;
        mSettingsBitmap = CreateCompatibleBitmap(target, width, height);
        if (!mSettingsBitmap)
        {
            DeleteDC(mSettingsMemDc);
            mSettingsMemDc = nullptr;
            return false;
        }
        mSettingsOldBitmap = SelectObject(mSettingsMemDc, mSettingsBitmap);
        mSettingsBufferW = width;
        mSettingsBufferH = height;
        return true;
    }

void App::PaintSettingsBackground(HDC dc)
{
        RECT rc{}; GetClientRect(mHwnd, &rc);
        constexpr int L = 124, R = 788;
        FillRect(dc, &rc, mBrPanel);
        HBRUSH line = CreateSolidBrush(kThInput);
        RECT divider{ 92, 24, 93, rc.bottom - 24 }; FillRect(dc, &divider, line);
        RECT logoLine{ 26, 98, 66, 99 }; FillRect(dc, &logoLine, line);
        RECT bottomLine{ 18, rc.bottom - 92, 74, rc.bottom - 91 }; FillRect(dc, &bottomLine, line);
        DeleteObject(line);
        SetBkMode(dc, TRANSPARENT);

        RECT logo{ 22, 28, 70, 76 };
        if (mImgLogo) {if(RectVisible(dc,&logo))DrawUiImage(dc, mImgLogo.get(), logo);}
        else {
            HGDIOBJ old = SelectObject(dc, mFontTitle);
            SetTextColor(dc, kThAccent);
            DrawTextW(dc, L"A", -1, &logo, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }

        // Page title in place of the removed top tabs.
        {
            const wchar_t* titles[] = { RU_TAB_SETTINGS, RU_TAB_VISUALS, RU_TAB_AI, RU_TAB_CONFIG };
            HGDIOBJ old = SelectObject(dc, mFontTab ? mFontTab : mFontBold);
            RECT t{ L, 24, R - 80, 64 };
            SetTextColor(dc, kThTextBright);
            DrawTextW(dc, titles[std::clamp(mActiveTab, 0, 3)], -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }

        if (mActiveTab == 3) PaintConfigWells(dc);

        // Sidebar corner: status (dot + on/off) and FPS.
        {
            const std::wstring prefix = RU_HUD_STATUS;
            std::wstring value = mHudStatus;
            if (value.compare(0, prefix.size(), prefix) == 0) value = value.substr(prefix.size());
            const COLORREF stateColor = mHudReady ? RGB(0, 255, 0) : kThAccent;
            HGDIOBJ old = SelectObject(dc, mFontBold);
            SIZE ext{};
            GetTextExtentPoint32W(dc, value.c_str(), int(value.size()), &ext);
            const int rowW = 14 + ext.cx, x0 = 46 - rowW / 2, y = rc.bottom - 74;
            {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::SolidBrush dot(GpColor(stateColor));
                g.FillEllipse(&dot, float(x0), float(y + 6), 8.f, 8.f);
            }
            RECT st{ x0 + 14, y, x0 + 14 + ext.cx + 2, y + 20 };
            SetTextColor(dc, stateColor);
            DrawTextW(dc, value.c_str(), -1, &st, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, mFontSmall);
            RECT fps{ 4, y + 24, 88, y + 44 };
            SetTextColor(dc, kThTextMuted);
            DrawTextW(dc, mHudFps.c_str(), -1, &fps, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, old);
        }

        // Footer: one quiet centered line.
        {
            HGDIOBJ old = SelectObject(dc, mFontSmall);
            RECT f{ L, rc.bottom - 44, R, rc.bottom - 18 };
            SetTextColor(dc, RGB(104, 105, 114));
            DrawTextW(dc, mHudInfo.c_str(), -1, &f, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, old);
        }

        // Rounded accent frame, as before.
        HPEN pen = CreatePen(PS_SOLID, 1, kThAccent);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, 0, 0, rc.right - 1, rc.bottom - 1, 18, 18);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    }

void App::UpdateBindButton()
{
        if (!mBindButton) return;
        const std::wstring text = mBindCapture ? std::wstring(RU_BIND_PRESS)
            : VkToName(mActivationKey);
        SetWindowTextW(mBindButton, text.c_str());
        InvalidateRect(mBindButton, nullptr, FALSE);
    }

void App::SetActivationKey(int vk)
{
        mActivationKey = std::clamp(vk, 1, 254);
        {
                mAimCfg.activationKey = mActivationKey;
        }
        QueueSettings();
        UpdateBindButton();
    }

void App::BeginBindCapture()
{
        if (mBindCapture) return;
        mBindCapture = true;
        mBindWaitRelease = true;
        UpdateBindButton();
        SetTimer(mHwnd, kBindTimerId, 10, nullptr);
    }

void App::EndBindCapture()
{
        if (!mBindCapture) return;
        mBindCapture = false;
        KillTimer(mHwnd, kBindTimerId);
        UpdateBindButton();
    }

void App::PollBindCapture()
{
        if (!mBindCapture) return;
        if (mBindWaitRelease)
        {
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
                mBindWaitRelease = false;
            return;
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { EndBindCapture(); return; }
        for (int vk = 1; vk < 0xFF; ++vk)
        {
            if (vk == VK_ESCAPE) continue;
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                EndBindCapture();
                SetActivationKey(vk);
                return;
            }
        }
    }

void App::SyncMouseScaleControl()
{
        if (!mMouseScaleSlider || !mMouseScaleValue) return;
        float pct = mAimCfg.trackingOutputScale * 100.f;
        if (!std::isfinite(pct)) pct = 100.f;
        const int sliderPos = int(std::clamp(pct, float(kMinMouseScale), float(kMaxMouseScale)) + 0.5f);
        SendMessageW(mMouseScaleSlider, TBM_SETPOS, TRUE, sliderPos);
        wchar_t label[64];
        swprintf_s(label, L"%d%%", sliderPos);
        SetWindowTextW(mMouseScaleValue, label);
    }

void App::ApplyMouseScaleControl()
{
        if (!mMouseScaleSlider) return;
        const int value = std::clamp<int>(int(SendMessageW(mMouseScaleSlider, TBM_GETPOS, 0, 0)),
            kMinMouseScale, kMaxMouseScale);
        mAimCfg.trackingOutputScale = float(value) / 100.f;
        SyncMouseScaleControl();
        QueueSettings();
    }

void App::ApplyControlValues()
{
        if (!mSpeedSlider || !mCaptureSlider || !mConfSlider || !mHeadYSlider) return;
        int speed = std::clamp<int>(int(SendMessageW(mSpeedSlider, TBM_GETPOS, 0, 0)),
            kMinAimSpeed, kMaxAimSpeed);
        int size = std::clamp<int>(int(SendMessageW(mCaptureSlider, TBM_GETPOS, 0, 0)),
            kMinCaptureSize, kMaxCaptureSize);

        int confPct = std::clamp<int>(int(SendMessageW(mConfSlider, TBM_GETPOS, 0, 0)),
            kMinConfidence, kMaxConfidence);
        const float confidence = float(confPct) / 100.f;

        const int headOffset = std::clamp<int>(int(SendMessageW(mHeadYSlider, TBM_GETPOS, 0, 0)),
            kMinHeadYOffset, kMaxHeadYOffset);
        mAimCfg.headYOffsetPixels = headOffset;
        mAimCfg.smoothing = float(speed) * 2.f / 100.f;
        mAimCfg.radius = size / 2;
        mAimCfg.confidence = confidence;
        // Tot zhe porog primenyaem i k dekoderu, inache slabye detektsii otsekayutsya ranshe.
        // Decode threshold changes atomically with the owner snapshot.
        QueueSettings(); // the capture owner recomputes the region

        wchar_t buf[64];
        swprintf_s(buf, L"%+d " RU_PX, headOffset); SetWindowTextW(mHeadYValue, buf);
        swprintf_s(buf, L"%d%%", speed);   SetWindowTextW(mSpeedValue, buf);
        swprintf_s(buf, L"%d " RU_PX, size); SetWindowTextW(mCaptureValue, buf);
        swprintf_s(buf, L"%.2f", confidence);  SetWindowTextW(mConfValue, buf);
    }

