// modules/config/configuration.cpp
#include "modules/app/app.hpp"

void App::ConfigTransfer(bool importing)
{
        const auto file=PickFile(mHwnd,!importing,L"Конфиги (*.cfg)\0*.cfg\0Все файлы\0*.*\0");
        if (!file) return;
        if (!importing) {
            const auto text=BuildConfigText();
            const bool ok=!text.empty() && WriteTextFileUtf8(*file,text);
            SetConfigStatus(ok?L"Снимок настроек формы экспортирован.":L"Не удалось экспортировать настройки.",!ok); return;
        }
        std::string text; albion::ConfigDocument config;
        if (!ReadTextFileUtf8(*file,text) || !config.Parse(text)) { SetConfigStatus(L"Импорт отклонён: "+Utf8ToWide(config.error),true); return; }
        const auto name=std::filesystem::path(*file).stem().wstring();
        if (!IsValidConfigName(name)) { SetConfigStatus(L"Недопустимое имя импортируемого конфига.",true); return; }
        const auto destination=ConfigPath(name);
        if (GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES &&
            MessageBoxW(mHwnd,L"Заменить существующий профиль? Его предыдущая версия останется в истории.",L"Импорт",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
        const bool ok=albion::SaveConfigWithHistory(destination,text);
        if (ok) RefreshConfigList(name);
        SetConfigStatus(ok?L"Профиль импортирован и выбран, но ещё НЕ применён.":L"Не удалось сохранить импортируемый профиль.",!ok);
    }

void App::RestoreConfigHistory()
{
        auto name=SelectedConfigName();
        const auto initial=name.empty()?std::filesystem::path(ConfigDirectory())/L".history":albion::HistoryDirectory(ConfigPath(name));
        const auto file=PickFile(mHwnd,false,L"Версии конфигов (*.cfg)\0*.cfg\0",initial.wstring());
        if (!file) return;
        // The file picker may navigate to a DELETED profile even while another
        // list row is selected. Derive the target from its named history folder,
        // then verify that exact folder is inside this repository below.
        name=std::filesystem::path(*file).parent_path().stem().wstring();
        if (!IsValidConfigName(name)) { SetConfigStatus(L"Выберите версию внутри папки истории профиля.",true); return; }
        const auto destination=ConfigPath(name);
        if (!albion::IsHistorySnapshot(*file, destination)) {
            SetConfigStatus(L"Выберите обычный файл версии из истории этого профиля. Для других файлов используйте импорт.",true); return;
        }
        std::string text; albion::ConfigDocument config;
        if (!ReadTextFileUtf8(*file,text) || !config.Parse(text)) { SetConfigStatus(L"Версия повреждена или несовместима: "+Utf8ToWide(config.error),true); return; }
        const auto question=L"Восстановить профиль «"+name+L"»? Текущая версия файла будет сохранена в истории. Работающие настройки не изменятся до нажатия «Загрузить».";
        if (MessageBoxW(mHwnd,question.c_str(),L"История",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
        const bool ok=albion::SaveConfigWithHistory(destination,text);
        if (ok) RefreshConfigList(name);
        SetConfigStatus(ok?L"Файл восстановлен. Нажмите «Загрузить», чтобы применить его.":L"Восстановление не удалось.",!ok);
    }

void App::ApplyMovementPath()
{
        if (!mPathCombo) return;
        int sel = int(SendMessageW(mPathCombo, CB_GETCURSEL, 0, 0));
        if (sel == CB_ERR) return;
        mAimCfg.movementPath =
            static_cast<AimMovementPath>(std::clamp<int>(sel, 0, 4));
        QueueSettings();
    }

void App::ApplyBodyPart(int selection)
{
        const int selected = std::clamp(selection, 0, 3);
        mAimCfg.targetBodyPart = static_cast<AimBodyPart>(selected);
        QueueSettings();
        for (HWND button : mBodyPartButtons)
            if (button) InvalidateRect(button, nullptr, FALSE);
    }

std::string App::BuildConfigText() const
{
        const BasicAimConfig& c = mAimCfg;
        char buf[4096];
        std::string modelUtf8 = WideToUtf8(FileNameOf(CurrentModelPath()));
        int n = std::snprintf(buf, sizeof(buf),
            "version=1\r\n"
            "radius=%u\r\n"
            "confidence=%.4f\r\n"
            "aimHeight=%.4f\r\n"
            "targetBodyPart=%d\r\n"
            "headYOffsetPixels=%d\r\n"
            "smoothing=%.4f\r\n"
            "deadZone=%.4f\r\n"
            "targetClass=%d\r\n"
            "sendThreshold=%.4f\r\n"
            "maxDelta=%.4f\r\n"

            "latencySeconds=%.4f\r\n"

            "velocityAlpha=%.4f\r\n"
            "velocityJitterPx=%.4f\r\n"
            "targetSmoothAlpha=%.4f\r\n"
            "maxGain=%.4f\r\n"
            "graceSeconds=%.4f\r\n"
            "matchRadius=%.4f\r\n"
            "bangBang=%d\r\n"
            "hardMode=%d\r\n"
            "smoothTracking=%d\r\n"
            "trackingOutputScale=%.4f\r\n"
            "bangGain=%.4f\r\n"
            "movementPath=%d\r\n"
            "settleSeconds=%.4f\r\n"
            "deadZoneRadius=%.4f\r\n"
            "deadZoneVelCancel=%.4f\r\n"
            "deadZoneExit=%.4f\r\n"
            "autoSensitivity=%d\r\n"
            "asyncMover=%d\r\n"
            "moverHz=%.4f\r\n"
            "calibMaxTargetSpeed=%.4f\r\n"
            "aimMethod=%d\r\n"
            "captureMethod=%d\r\n"
            "fpsLimit=%d\r\n"
            "showRegion=%d\r\n"
            "showBoxes=%d\r\n"
            "showStats=%d\r\n"
            "showConfidence=%d\r\n"
            "activationKey=%d\r\n"
            "model=%s\r\n",
            c.radius, double(c.confidence), double(c.aimHeight), int(c.targetBodyPart), c.headYOffsetPixels,
            double(c.smoothing), double(c.deadZone), c.targetClass, double(c.sendThreshold),
            double(c.maxDelta),
            double(c.latencySeconds), double(c.velocityAlpha),
            double(c.velocityJitterPx), double(c.targetSmoothAlpha), double(c.maxGain),
            double(c.graceSeconds), double(c.matchRadius), c.bangBang ? 1 : 0, c.hardMode ? 1 : 0, c.smoothTracking ? 1 : 0,
            double(c.trackingOutputScale),
            double(c.bangGain), int(c.movementPath), double(c.settleSeconds),
            double(c.deadZoneRadius), double(c.deadZoneVelCancel), double(c.deadZoneExit),
            c.autoSensitivity ? 1 : 0, c.asyncMover ? 1 : 0, double(c.moverHz),
            double(c.calibMaxTargetSpeed),
            int(mAimMethod), int(mCaptureMethod), mFpsLimit.load(std::memory_order_relaxed),
            mShowRegion ? 1 : 0, mShowBoxes ? 1 : 0, mShowStats ? 1 : 0,
            mShowConfidence ? 1 : 0,
            mActivationKey,
            modelUtf8.c_str());
        if (n <= 0 || n >= int(sizeof(buf))) return std::string();
        return std::string(buf, size_t(std::min<int>(n, int(sizeof(buf)) - 1)));
    }

bool App::ApplyConfigText(const std::string& text, std::wstring* why)
{
        albion::ConfigDocument document;
        if (!document.Parse(text)) { if (why) *why = Utf8ToWide(document.error); return false; }
        const auto& kv = document.values;
        const auto ConfigInt = [&](const char* key, int fallback) { return int(document.Number(key, double(fallback))); };
        const std::wstring wanted = Utf8ToWide(kv.at("model"));
        int modelIndex = -1;
        if (!wanted.empty()) for (size_t i = 0; i < mModelFiles.size(); ++i)
            if (ConfigNameEqual(FileNameOf(mModelFiles[i]), wanted)) {
                const DWORD attrs = GetFileAttributesW(mModelFiles[i].c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) modelIndex = int(i);
                break;
            }
        if (modelIndex < 0) { if (why) *why = L"Model is missing or filename is not valid UTF-8"; return false; }
        // Optional v1 fields restore schema defaults, never values from a
        // different previously loaded profile. Validation above has no side effects.
        mApplyingConfig = true;
        struct ApplyLease { bool& flag; ~ApplyLease() { flag = false; } } lease{mApplyingConfig};
        mAimCfg = BasicAimConfig{};
        mAimMethod = AimMethod::WinApi; mCaptureMethod = CaptureMethod::DirectXFull;
        mFpsLimit.store(0); mActivationKey = VK_RBUTTON;
        mShowRegion = mShowBoxes = mShowStats = mShowConfidence = false;
        BasicAimConfig& c = mAimCfg;
        c.aimHeight = float(document.Number("aimHeight", double(c.aimHeight)));
        c.deadZone = float(document.Number("deadZone", double(c.deadZone)));
        c.targetClass = ConfigInt("targetClass", c.targetClass);
        c.sendThreshold = float(document.Number("sendThreshold", double(c.sendThreshold)));
        c.maxDelta = float(document.Number("maxDelta", double(c.maxDelta)));

        c.latencySeconds = float(document.Number("latencySeconds", double(c.latencySeconds)));

        c.velocityAlpha = float(document.Number("velocityAlpha", double(c.velocityAlpha)));
        c.velocityJitterPx = float(document.Number("velocityJitterPx", double(c.velocityJitterPx)));
        c.targetSmoothAlpha = float(document.Number("targetSmoothAlpha", double(c.targetSmoothAlpha)));
        c.maxGain = float(document.Number("maxGain", double(c.maxGain)));
        c.graceSeconds = float(document.Number("graceSeconds", double(c.graceSeconds)));
        c.matchRadius = float(document.Number("matchRadius", double(c.matchRadius)));
        c.bangBang = ConfigInt("bangBang", c.bangBang ? 1 : 0) != 0;
        // Rezhim vsegda vklyuchen; staryy config ne mozhet ego vyklyuchit.
        c.hardMode = true;
        c.smoothTracking = true;
        c.trackingOutputScale = float(
            std::clamp(document.Number("trackingOutputScale", 1.0), 0.05, 2.0));
        c.bangGain = float(document.Number("bangGain", double(c.bangGain)));
        c.settleSeconds = float(document.Number("settleSeconds", double(c.settleSeconds)));
        c.deadZoneRadius = 0.f;
        c.deadZone = 0.f;
        c.deadZoneVelCancel = float(document.Number("deadZoneVelCancel", double(c.deadZoneVelCancel)));
        c.deadZoneExit = float(document.Number("deadZoneExit", double(c.deadZoneExit)));
        SyncMouseScaleControl();
        c.autoSensitivity = ConfigInt("autoSensitivity", c.autoSensitivity ? 1 : 0) != 0;
        c.asyncMover = ConfigInt("asyncMover", c.asyncMover ? 1 : 0) != 0;
        c.moverHz = float(document.Number("moverHz", double(c.moverHz)));
        c.calibMaxTargetSpeed = float(document.Number("calibMaxTargetSpeed", double(c.calibMaxTargetSpeed)));

        // UI-svyazannye polya - stavim polozheniya kontrolov, potom vyzyvaem
        // sushchestvuyushchie Apply*, chtoby vse pobochnye effekty (perepodklyuchenie
        // Arduino, zapusk modeli, pereschet regiona) otrabotali kak obychno.
        int spd = std::clamp<int>(int(document.Number("smoothing", double(c.smoothing)) * 50.0 + .5),
            kMinAimSpeed, kMaxAimSpeed);
        if (mSpeedSlider) SendMessageW(mSpeedSlider, TBM_SETPOS, TRUE, spd);

        // FIX-V11-01: document validated radius<=600 BEFORE multiplying/narrowing.
        int radius = ConfigInt("radius", int(c.radius));
        int sz = std::clamp<int>(radius * 2, kMinCaptureSize, kMaxCaptureSize);
        // The trackbar range is expressed directly in pixels, so restore the
        // saved FOV size itself (not the old step-based slider index).
        if (mCaptureSlider) SendMessageW(mCaptureSlider, TBM_SETPOS, TRUE, sz);

        int conf = std::clamp<int>(int(document.Number("confidence", double(c.confidence)) * 100.0 + .5),
            kMinConfidence, kMaxConfidence);
        if (mConfSlider) SendMessageW(mConfSlider, TBM_SETPOS, TRUE, conf);
        // Old configurations without this new field must restore a zero offset.
        const int headOffset = std::clamp(ConfigInt("headYOffsetPixels", 0),
            kMinHeadYOffset, kMaxHeadYOffset);
        if (mHeadYSlider) SendMessageW(mHeadYSlider, TBM_SETPOS, TRUE, headOffset);
        ApplyControlValues();

        ApplyBodyPart(std::clamp<int>(
            ConfigInt("targetBodyPart", int(c.targetBodyPart)), 0, 3));

        if (mPathCombo) SendMessageW(mPathCombo, CB_SETCURSEL,
            WPARAM(std::clamp<int>(ConfigInt("movementPath", int(c.movementPath)), 0, 4)), 0);
        ApplyMovementPath();

        if (mMethodCombo) SendMessageW(mMethodCombo, CB_SETCURSEL,
            WPARAM(ConfigInt("aimMethod", int(mAimMethod)) == 1 ? 1 : 0), 0);
        ApplyAimMethod();

        if (mFpsCombo)
        {
            int fpsWanted = ConfigInt("fpsLimit", mFpsLimit.load(std::memory_order_relaxed));
            int sel = 0;
            for (int i = 0; i < int(_countof(kFpsOptions)); ++i)
                if (kFpsOptions[i] == fpsWanted) { sel = i; break; }
            SendMessageW(mFpsCombo, CB_SETCURSEL, WPARAM(sel), 0);
        }
        ApplyFpsLimit();

        if (mCapCombo) SendMessageW(mCapCombo, CB_SETCURSEL,
            WPARAM(ConfigInt("captureMethod", int(mCaptureMethod)) == 1 ? 1 : 0), 0);
        ApplyCaptureMethod();

        mShowRegion = ConfigInt("showRegion", mShowRegion ? 1 : 0) != 0;
        mShowBoxes = ConfigInt("showBoxes", mShowBoxes ? 1 : 0) != 0;
        mShowStats = ConfigInt("showStats", mShowStats ? 1 : 0) != 0;
        mShowConfidence = ConfigInt("showConfidence", mShowConfidence ? 1 : 0) != 0;
        SetActivationKey(ConfigInt("activationKey", mActivationKey));
        ApplyVisuals();

        SendMessageW(mModelCombo, CB_SETCURSEL, WPARAM(modelIndex), 0);
        mApplyingConfig = false;
        QueueSettings(true); // publish profile+model together, not as two interleavable commands
        return true;
    }

void App::SetConfigStatus(const std::wstring& text, bool error)
{
        mConfigStatusError = error;
        if (mConfigStatusLabel) {
            SetWindowTextW(mConfigStatusLabel, text.c_str());
            InvalidateRect(mConfigStatusLabel, nullptr, TRUE);
        }
    }

bool App::IsValidConfigName(const std::wstring& name)
{
        if (name.empty() || name.size() > 120 || name == L"." || name == L"..") return false;
        if (name.back() == L' ' || name.back() == L'.') return false;
        if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
        for (wchar_t ch : name) if (ch < 32) return false;

        std::wstring stem = name.substr(0, name.find(L'.'));
        std::transform(stem.begin(), stem.end(), stem.begin(),
            [](wchar_t ch) { return wchar_t(towupper(ch)); });
        static const wchar_t* reserved[] = {
            L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
            L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
            L"LPT6", L"LPT7", L"LPT8", L"LPT9"
        };
        return std::none_of(std::begin(reserved), std::end(reserved),
            [&](const wchar_t* value) { return stem == value; });
    }

std::wstring App::ConfigDirectory() const
{
        const std::filesystem::path dir = AlbionDataPath(L"configs");
        static std::once_flag migration;
        std::call_once(migration, [&] {
            const bool existed = std::filesystem::exists(dir);
            std::filesystem::create_directories(dir);
            if (!existed) albion::ImportLegacyConfigs(ExeRelativePath(L"configs"), dir);
        });
        return dir.wstring();
    }

std::wstring App::ConfigPath(const std::wstring& name) const
{
        return ConfigDirectory() + L"\\" + name + L".cfg";
    }

std::wstring App::ConfigNameFromEdit() const
{
        if (!mConfigNameEdit) return std::wstring();
        int len = GetWindowTextLengthW(mConfigNameEdit);
        std::wstring name(size_t(std::max(0, len)) + 1, L'\0');
        if (len > 0) GetWindowTextW(mConfigNameEdit, name.data(), len + 1);
        name.resize(size_t(std::max(0, len)));
        size_t first = name.find_first_not_of(L" \t\r\n");
        size_t last = name.find_last_not_of(L" \t\r\n");
        return first == std::wstring::npos ? std::wstring() : name.substr(first, last - first + 1);
    }

std::wstring App::SelectedConfigName() const
{
        if (!mConfigList) return std::wstring();
        int sel = int(SendMessageW(mConfigList, LB_GETCURSEL, 0, 0));
        if (sel == LB_ERR) return std::wstring();
        int len = int(SendMessageW(mConfigList, LB_GETTEXTLEN, WPARAM(sel), 0));
        if (len < 0) return std::wstring();
        std::wstring name(size_t(len) + 1, L'\0');
        SendMessageW(mConfigList, LB_GETTEXT, WPARAM(sel), reinterpret_cast<LPARAM>(name.data()));
        name.resize(size_t(len));
        return name;
    }

bool App::ConfigNameEqual(const std::wstring& a, const std::wstring& b)
{
        if (a.empty() || b.empty()) return a.empty() && b.empty();
        return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
    }

void App::UpdateConfigTitle()
{
        const std::wstring title = mConfigEditing ? (mConfigRenameMode ? L"\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433" : L"\u0421\u043e\u0437\u0434\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433") :
            L"\u0412\u0430\u0448\u0438 \u043a\u043e\u043d\u0444\u0438\u0433\u0438 \xb7 " + std::to_wstring(mConfigEntries.size());
        SetWindowTextW(mConfigTitle, title.c_str());
    }

void App::UpdateConfigEditorVisibility()
{
        const bool visible = mActiveTab == 3;
        const HWND library[] = {mConfigList, mDeleteConfigBtn, mLoadConfigBtn, mSaveConfigBtn, mRenameConfigBtn, mConfigNewBtn};
        const HWND editor[] = {mConfigNameLabel, mConfigNameEdit, mConfigConfirmBtn, mConfigCancelBtn};
        for (HWND control : library) if (control) ShowWindow(control, visible && !mConfigEditing ? SW_SHOWNA : SW_HIDE);
        for (HWND control : editor) if (control) ShowWindow(control, visible && mConfigEditing ? SW_SHOWNA : SW_HIDE);
    }

void App::RebuildConfigList(const std::wstring& preferred)
{
        if (!mConfigList) return;
        const std::wstring selected = preferred.empty() ? SelectedConfigName() : preferred;
        const int top = int(SendMessageW(mConfigList, LB_GETTOPINDEX, 0, 0));
        mConfigRebuilding = true;
        SendMessageW(mConfigList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(mConfigList, LB_RESETCONTENT, 0, 0);
        mConfigVisible.clear();
        int nextSelection = -1;
        for (size_t i = 0; i < mConfigEntries.size(); ++i) {
            const auto& entry = mConfigEntries[i];
            const LRESULT row = SendMessageW(mConfigList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name.c_str()));
            if (row < 0) continue;
            if (ConfigNameEqual(entry.name, selected)) nextSelection = int(row);
            mConfigVisible.push_back(i);
        }
        SendMessageW(mConfigList, LB_SETCURSEL, WPARAM(nextSelection), 0);
        if (nextSelection < 0 && !mConfigVisible.empty())
            SendMessageW(mConfigList, LB_SETTOPINDEX, WPARAM(std::clamp(top, 0, int(mConfigVisible.size()) - 1)), 0);
        SendMessageW(mConfigList, WM_SETREDRAW, TRUE, 0);
        if (mActiveTab != 3 || mConfigEditing) ShowWindow(mConfigList, SW_HIDE);
        mConfigRebuilding = false;
        UpdateConfigTitle();
        InvalidateRect(mConfigList, nullptr, TRUE);
        UpdateConfigButtons();
    }

void App::RefreshConfigList(const std::wstring& selectName)
{
        if (!mConfigList) return;
        std::vector<ConfigEntry> entries;
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = ConfigDirectory() + L"\\*.cfg";
        HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) {
            SetConfigStatus(L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043f\u0440\u043e\u0447\u0438\u0442\u0430\u0442\u044c \u043f\u0430\u043f\u043a\u0443 configs.", true); return;
        }
        if (find != INVALID_HANDLE_VALUE) {
            DWORD scanError = ERROR_NO_MORE_FILES;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = fd.cFileName;
                if (name.size() < 5 || !ConfigNameEqual(name.substr(name.size() - 4), L".cfg")) continue;
                name.resize(name.size() - 4);
                FILETIME local{}; SYSTEMTIME date{};
                std::wstring detail = L"\u0424\u0430\u0439\u043b .cfg";
                if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &date)) {
                    wchar_t stamp[64]{};
                    swprintf_s(stamp, L".cfg \xb7 %02u.%02u.%04u  %02u:%02u",
                        unsigned(date.wDay), unsigned(date.wMonth), unsigned(date.wYear), unsigned(date.wHour), unsigned(date.wMinute));
                    detail = stamp;
                }
                entries.push_back({name, detail});
            } while (FindNextFileW(find, &fd));
            scanError = GetLastError(); FindClose(find);
            if (scanError != ERROR_NO_MORE_FILES) { SetConfigStatus(L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043e\u0431\u043d\u043e\u0432\u0438\u0442\u044c \u0432\u0435\u0441\u044c \u0441\u043f\u0438\u0441\u043e\u043a \u043f\u0440\u043e\u0444\u0438\u043b\u0435\u0439.", true); return; }
        }
        std::sort(entries.begin(), entries.end(), [](const ConfigEntry& a, const ConfigEntry& b) {
            const int order = CompareStringOrdinal(a.name.c_str(), int(a.name.size()), b.name.c_str(), int(b.name.size()), TRUE);
            return order == CSTR_LESS_THAN || (order == CSTR_EQUAL && a.name < b.name);
        });
        mConfigEntries = std::move(entries);
        RebuildConfigList(selectName);
        if (!selectName.empty()) SetWindowTextW(mConfigNameEdit, selectName.c_str());
        UpdateConfigButtons();
    }

void App::UpdateConfigButtons()
{
        const std::wstring selected = SelectedConfigName(), name = ConfigNameFromEdit();
        const bool hasSelection = !selected.empty() && !mConfigEditing;
        const std::pair<HWND, bool> states[] = {
            {mLoadConfigBtn, hasSelection}, {mDeleteConfigBtn, hasSelection},
            {mRenameConfigBtn, hasSelection}, {mSaveConfigBtn, hasSelection},
            {mConfigNewBtn, !mConfigEditing},
            {mConfigConfirmBtn, mConfigEditing && IsValidConfigName(name) &&
                (!mConfigRenameMode || !ConfigNameEqual(selected, name))}
        };
        for (const auto& state : states) if (state.first) {
            EnableWindow(state.first, state.second); InvalidateRect(state.first, nullptr, FALSE);
        }
    }

void App::OnConfigSelectionChanged()
{
        if (mConfigRebuilding || mConfigEditing) return;
        const std::wstring name = SelectedConfigName();
        if (!name.empty()) SetWindowTextW(mConfigNameEdit, name.c_str());
        UpdateConfigButtons();
        SetConfigStatus(L"\u0413\u0430\u043b\u043e\u0447\u043a\u0430 \u043e\u0442\u043c\u0435\u0447\u0430\u0435\u0442 \u0432\u044b\u0431\u043e\u0440. \u0414\u0432\u043e\u0439\u043d\u043e\u0439 \u0449\u0435\u043b\u0447\u043e\u043a \u0438\u043b\u0438 Enter \u2014 \u0437\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c.");
    }

void App::BeginConfigEdit(bool rename)
{
        const std::wstring selected = SelectedConfigName();
        if (rename && selected.empty()) return;
        mConfigEditing = true; mConfigRenameMode = rename;
        SetWindowTextW(mConfigNameEdit, rename ? selected.c_str() : L"");
        SetWindowTextW(mConfigConfirmBtn, rename ? L"\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c" : L"\u0421\u043e\u0437\u0434\u0430\u0442\u044c");
        UpdateConfigTitle(); UpdateConfigEditorVisibility(); UpdateConfigButtons();
        RedrawWindow(mHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        SetFocus(mConfigNameEdit); SendMessageW(mConfigNameEdit, EM_SETSEL, 0, -1);
        SetConfigStatus(rename ? L"Enter \u2014 \u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c, Esc \u2014 \u043e\u0442\u043c\u0435\u043d\u0438\u0442\u044c." : L"Enter \u2014 \u0441\u043e\u0437\u0434\u0430\u0442\u044c, Esc \u2014 \u043e\u0442\u043c\u0435\u043d\u0438\u0442\u044c. \u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u043d\u0435 \u0441\u0431\u0440\u0430\u0441\u044b\u0432\u0430\u044e\u0442\u0441\u044f.");
    }

void App::FinishConfigEdit()
{
        mConfigEditing = false; mConfigRenameMode = false;
        UpdateConfigTitle(); UpdateConfigEditorVisibility(); UpdateConfigButtons();
        RedrawWindow(mHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        SetFocus(mConfigList);
    }

void App::NewConfigDraft()
{ BeginConfigEdit(false); }

void App::CommitConfigEdit()
{
        if (!mConfigEditing) return;
        if (mConfigRenameMode) RenameSelectedConfig(); else SaveNamedConfig();
    }

void App::CancelConfigEdit()
{
        if (!mConfigEditing) return;
        FinishConfigEdit(); SetConfigStatus(L"\u0418\u0437\u043c\u0435\u043d\u0435\u043d\u0438\u044f \u043e\u0442\u043c\u0435\u043d\u0435\u043d\u044b. \u0424\u0430\u0439\u043b\u044b \u043d\u0435 \u0438\u0437\u043c\u0435\u043d\u044f\u043b\u0438\u0441\u044c.");
    }

void App::SaveNamedConfig()
{
        if (!mConfigEditing && SelectedConfigName().empty()) {
            SetConfigStatus(L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u043a\u043e\u043d\u0444\u0438\u0433 \u0438\u043b\u0438 \u043d\u0430\u0436\u043c\u0438\u0442\u0435 \xab\u0421\u043e\u0437\u0434\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433\xbb.");
            SetFocus(mConfigNewBtn); return;
        }
        const std::wstring name = mConfigEditing ? ConfigNameFromEdit() : SelectedConfigName();
        if (name.empty()) { SetConfigStatus(RU_CONFIG_ENTER_NAME, true); SetFocus(mConfigNameEdit); return; }
        if (!IsValidConfigName(name)) { SetConfigStatus(RU_CONFIG_BAD_NAME, true); SetFocus(mConfigNameEdit); return; }
        const std::wstring path = ConfigPath(name);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring question = L"\u041f\u0435\u0440\u0435\u0437\u0430\u043f\u0438\u0441\u0430\u0442\u044c \u043f\u0440\u043e\u0444\u0438\u043b\u044c \xab" + name + L"\xbb \u0442\u0435\u043a\u0443\u0449\u0438\u043c\u0438 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0430\u043c\u0438?";
            if (MessageBoxW(mHwnd, question.c_str(), RU_TAB_CONFIG,
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        }
        std::string text;
        { text = BuildConfigText(); }
        const bool ok = !text.empty() && albion::SaveConfigWithHistory(path, text);
        if (ok) {
            RefreshConfigList(name); if (mConfigEditing) FinishConfigEdit();
            mLoadedConfigName = name; mConfigDirty = false;
            mLoadedConfigRevision = mSettingsRevision; mLastProfileStatus.clear();
        }
        SetConfigStatus(ok ? std::wstring(RU_CONFIG_SAVED) + name : std::wstring(RU_CONFIG_SAVE_FAILED), !ok);
    }

void App::LoadSelectedConfig()
{
        if (mConfigEditing) return;
        const std::wstring name = SelectedConfigName();
        if (name.empty()) return;
        std::string text;
        std::wstring why;
        const bool read = ReadTextFileUtf8(ConfigPath(name), text);
        const bool ok = read && ApplyConfigText(text, &why);
        if (ok) {
            mLoadedConfigName = name; mConfigDirty = false; mLoadedConfigRevision = mSettingsRevision;
            mLastProfileStatus.clear();
            SetConfigStatus(L"Настройки приняты; применяется модель и профиль: " + name);
        } else SetConfigStatus(std::wstring(RU_CONFIG_LOAD_FAILED) + (why.empty() ? L"" : L": " + why), true);
    }

void App::DeleteSelectedConfig()
{
        if (mConfigEditing) return;
        const std::wstring name = SelectedConfigName();
        if (name.empty()) return;
        const int oldIndex = int(SendMessageW(mConfigList, LB_GETCURSEL, 0, 0));
        const std::wstring question = L"Удалить профиль «" + name + L"»?\nФайл будет сохранён в истории (F8 → Восстановить). Работающие настройки не изменятся.";
        if (MessageBoxW(mHwnd, question.c_str(), RU_TAB_CONFIG,
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        const bool ok = albion::DeleteConfigWithHistory(ConfigPath(name));
        if (ok) {
            if (ConfigNameEqual(mLoadedConfigName, name)) mConfigDirty = true;
            RefreshConfigList();
            const int count = int(SendMessageW(mConfigList, LB_GETCOUNT, 0, 0));
            if (count > 0) {
                SendMessageW(mConfigList, LB_SETCURSEL, WPARAM(std::min(oldIndex, count - 1)), 0);
                OnConfigSelectionChanged();
            } else SetWindowTextW(mConfigNameEdit, L"");
            UpdateConfigButtons();
        }
        SetConfigStatus(ok ? std::wstring(RU_CONFIG_DELETED) + name : L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0443\u0434\u0430\u043b\u0438\u0442\u044c \u043f\u0440\u043e\u0444\u0438\u043b\u044c. \u041f\u0440\u043e\u0432\u0435\u0440\u044c\u0442\u0435 \u0434\u043e\u0441\u0442\u0443\u043f \u043a \u0444\u0430\u0439\u043b\u0443.", !ok);
    }

void App::RenameSelectedConfig()
{
        if (!mConfigEditing || !mConfigRenameMode) return;
        const std::wstring oldName = SelectedConfigName(), newName = ConfigNameFromEdit();
        if (oldName.empty()) return;
        if (newName.empty()) { SetConfigStatus(RU_CONFIG_ENTER_NAME, true); return; }
        if (!IsValidConfigName(newName)) { SetConfigStatus(RU_CONFIG_BAD_NAME, true); return; }
        if (ConfigNameEqual(oldName, newName)) return;
        const std::wstring newPath = ConfigPath(newName);
        if (GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetConfigStatus(L"\u041f\u0440\u043e\u0444\u0438\u043b\u044c \u0441 \u0442\u0430\u043a\u0438\u043c \u0438\u043c\u0435\u043d\u0435\u043c \u0443\u0436\u0435 \u0441\u0443\u0449\u0435\u0441\u0442\u0432\u0443\u0435\u0442. \u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u0434\u0440\u0443\u0433\u043e\u0435 \u0438\u043c\u044f.", true); return;
        }
        bool historyMoved = false;
        const bool ok = albion::RenameConfigWithHistory(ConfigPath(oldName), newPath, historyMoved);
        if (ok) {
            if (ConfigNameEqual(mLoadedConfigName, oldName)) mLoadedConfigName = newName;
            RefreshConfigList(newName); FinishConfigEdit();
            if (!historyMoved) {
                SetConfigStatus(L"Профиль переименован; история осталась под старым именем. Открыть её можно через F8.",true); return;
            }
        }
        SetConfigStatus(ok ? std::wstring(RU_CONFIG_RENAMED) + newName : L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c \u043f\u0440\u043e\u0444\u0438\u043b\u044c. \u041f\u0440\u043e\u0432\u0435\u0440\u044c\u0442\u0435 \u0434\u043e\u0441\u0442\u0443\u043f \u043a \u0444\u0430\u0439\u043b\u0443.", !ok);
    }

bool App::HandleConfigKey(const MSG& message)
{
        if (mActiveTab != 3 || message.message != WM_KEYDOWN ||
            (message.hwnd != mHwnd && !IsChild(mHwnd, message.hwnd))) return false;
        const HWND focus = GetFocus();
        const bool control = GetKeyState(VK_CONTROL) < 0;
        const bool repeat = (message.lParam & (LPARAM(1) << 30)) != 0;
        const WPARAM key = message.wParam;
        if (key == VK_TAB) {
            const HWND order[] = {mConfigList, mDeleteConfigBtn, mLoadConfigBtn, mSaveConfigBtn,
                mRenameConfigBtn, mConfigNewBtn, mConfigNameEdit, mConfigConfirmBtn, mConfigCancelBtn,
                mTabSettings, mTabVisuals, mTabAi, mTabConfig, mWinMinimize, mWinClose};
            int index = -1;
            for (int i = 0; i < int(_countof(order)); ++i) if (order[i] == focus) index = i;
            const int step = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
            if (index < 0) index = step > 0 ? -1 : 0;
            for (int i = 0; i < int(_countof(order)); ++i) {
                index = (index + step + int(_countof(order))) % int(_countof(order));
                HWND target = order[index];
                if (target && IsWindowVisible(target) && IsWindowEnabled(target)) { SetFocus(target); break; }
            }
            return true;
        }
        if (key == VK_ESCAPE) { if (mConfigEditing) CancelConfigEdit(); else SetFocus(mConfigList); return true; }
        if (key == VK_F2) { if (!repeat && !mConfigEditing) BeginConfigEdit(true); return true; }
        if (key == VK_F5 || (control && key == 'R')) { if (!repeat && !mConfigEditing) RefreshConfigList(); return true; }
        if (control && key == 'N') { if (!repeat && !mConfigEditing) NewConfigDraft(); return true; }
        if (control && key == 'S') {
            if (!repeat) { if (mConfigEditing) CommitConfigEdit(); else SaveNamedConfig(); } return true;
        }
        if (key == VK_DELETE && focus == mConfigList && !mConfigEditing) { if (!repeat) DeleteSelectedConfig(); return true; }
        if (key == VK_RETURN) {
            if (!repeat) {
                if (focus == mConfigNameEdit && mConfigEditing) CommitConfigEdit();
                else if (focus == mConfigList && !mConfigEditing) LoadSelectedConfig();
                else if (focus && focus != mHwnd && IsWindowEnabled(focus)) SendMessageW(focus, BM_CLICK, 0, 0);
            }
            return true;
        }
        return false;
    }

double App::NowMs()
{
        static const double ticksToMs = []
            {
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                return frequency.QuadPart ? 1000.0 / double(frequency.QuadPart) : 0.0;
            }();

        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return double(counter.QuadPart) * ticksToMs;
    }
