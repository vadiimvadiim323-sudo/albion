// modules/diagnostics/diagnostics.cpp
#include "modules/app/app.hpp"

std::wstring App::QuoteArgument(const std::wstring& value)
{
        // Windows CommandLineToArgvW-compatible quoting, including trailing '\'.
        std::wstring quoted = L"\""; size_t backslashes = 0;
        for (wchar_t ch : value) {
            if (ch == L'\\') { ++backslashes; continue; }
            if (ch == L'"') { quoted.append(backslashes * 2 + 1, L'\\'); quoted += ch; }
            else { quoted.append(backslashes, L'\\'); quoted += ch; }
            backslashes = 0;
        }
        quoted.append(backslashes * 2, L'\\'); quoted += L'"'; return quoted;
    }

std::optional<std::wstring> App::PickFile(HWND owner, bool save, const wchar_t* filter,
        const std::wstring& initial, const wchar_t* extension)
{
        std::vector<wchar_t> buffer(32768, L'\0');
        OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner=owner; dialog.lpstrFilter=filter;
        dialog.lpstrFile=buffer.data(); dialog.nMaxFile=DWORD(buffer.size()); dialog.lpstrDefExt=extension;
        dialog.lpstrInitialDir=initial.empty()?nullptr:initial.c_str();
        dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
        if (!(save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog))) return std::nullopt;
        return std::wstring(buffer.data());
    }

std::wstring App::FindPythonLauncher(bool& isPyLauncher)
{
        // FIX-V11.1-PYTHON: resolve the interpreter explicitly. ShellExecute of
        // a missing "python.exe" could open the Microsoft Store alias or show
        // its own error box. The WindowsApps alias is deliberately skipped.
        for (const wchar_t* candidate : {L"py.exe", L"python.exe"}) {
            std::vector<wchar_t> found(32768, L'\0');
            const DWORD n=SearchPathW(nullptr,candidate,nullptr,DWORD(found.size()),found.data(),nullptr);
            if (!n || n>=found.size()) continue;
            std::wstring path(found.data(),n), lower=path;
            for (auto& c : lower) c=wchar_t(towlower(c));
            if (lower.find(L"\\windowsapps\\")!=std::wstring::npos) continue;
            isPyLauncher=candidate[0]==L'p' && candidate[1]==L'y' && candidate[2]==L'.';
            return path;
        }
        return {};
    }

void App::LaunchPython(const std::wstring& script, const std::wstring& arguments)
{
        const std::wstring path=ExeRelativePath((L"tools\\"+script).c_str());
        if (GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(mHwnd,L"Не найден скрипт tools. Повторите сборку или скопируйте папку tools рядом с EXE.",L"Диагностика",MB_ICONERROR); return;
        }
        bool pyLauncher=false;
        const std::wstring interpreter=FindPythonLauncher(pyLauncher);
        if (interpreter.empty()) {
            MessageBoxW(mHwnd,L"Для этой дополнительной функции нужен Python 3.10+ (py.exe или python.exe в PATH). Ничего автоматически не устанавливалось.",L"Диагностика",MB_ICONERROR); return;
        }
        // --pause keeps the separate console open so results/errors are readable
        // (1.0.0 closed the window immediately after the tool finished).
        std::wstring command=QuoteArgument(interpreter)+(pyLauncher?L" -3 ":L" ")+QuoteArgument(path)+L" "+arguments+L" --pause";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        const std::wstring workingDirectory=AlbionDataDirectory();
        if (!CreateProcessW(interpreter.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE|CREATE_UNICODE_ENVIRONMENT,
            nullptr,workingDirectory.c_str(),&startup,&process)) {
            const auto message=L"Не удалось запустить Python (ошибка Win32 "+std::to_wstring(GetLastError())+L").";
            MessageBoxW(mHwnd,message.c_str(),L"Диагностика",MB_ICONERROR); return;
        }
        CloseHandle(process.hThread); CloseHandle(process.hProcess); // not awaited: never blocks the UI
    }

void App::DiagnosticAction(albion::DiagnosticAction action)
{
        using Action=albion::DiagnosticAction;
        if(action==Action::ReloadModel) { ApplyModel(true); return; }
        if(action==Action::RecoverCapture) {
            { std::lock_guard<std::mutex> lock(mCommandMx); mPendingCaptureRetry=true; }
            mDet.NotifyActivity(); return;
        }
        if(action==Action::OpenData || action==Action::OpenHistory) {
            const auto folder=action==Action::OpenData?AlbionDataDirectory():ConfigDirectory()+L"\\.history";
            std::filesystem::create_directories(folder); ShellExecuteW(mHwnd,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL); return;
        }
        if(action==Action::RestoreConfig) { RestoreConfigHistory(); return; }
        if(action==Action::ImportConfig || action==Action::ExportConfig) { ConfigTransfer(action==Action::ImportConfig); return; }
        if(action==Action::ForgetKey) {
            if(MessageBoxW(mHwnd,L"Удалить сохранённый ключ с этого Windows-аккаунта? Текущая онлайн-проверенная сессия останется действующей.",L"Ключ",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES) {
                const bool ok=albion::ForgetLicenseCache(); MessageBoxW(mHwnd,ok?L"Сохранённый ключ удалён.":L"Не удалось удалить ключ. Закройте другие экземпляры и повторите.",L"Ключ",ok?MB_ICONINFORMATION:MB_ICONERROR);
            } return;
        }
        if(action==Action::OfflineCheck) {
            const auto model=PickFile(mHwnd,false,L"ONNX (*.onnx)\0*.onnx\0",ExeRelativePath(L"models"),L"onnx");
            if(!model) return;
            const auto input=PickFile(mHwnd,false,L"Изображения/видео\0*.png;*.jpg;*.jpeg;*.bmp;*.mp4;*.avi;*.mkv;*.mov;*.webm\0Все файлы\0*.*\0",{},nullptr);
            if(!input) return;
            const auto output=AlbionDataPath((L"offline-check-"+std::to_wstring(GetTickCount64())+L".json").c_str());
            LaunchPython(L"offline_model_check.py",L"--model "+QuoteArgument(*model)+L" --input "+QuoteArgument(*input)+L" --output "+QuoteArgument(output)); return;
        }
        const auto args=(action==Action::Doctor?L"doctor":L"support-prepare")+std::wstring(L" --app-dir ")+QuoteArgument(AlbionProgramDirectory())+
            L" --data-dir "+QuoteArgument(AlbionDataDirectory())+L" --open-folder";
        // Support export makes a REVIEW FOLDER first, never uploads or bundles
        // credentials/configs/models; a separate explicit confirmation makes ZIP.
        LaunchPython(L"diagnostics.py",args);
    }

void App::ShowPerformance()
{
        mPerformance.Show(mHwnd,[this] { mDiagnostics.Show(mHwnd,[this](albion::DiagnosticAction a) { DiagnosticAction(a); }); UpdateDiagnostics(); });
    }

void App::PollDiagnosticsHotkey()
{
        const bool down=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
        // FIX-V11.1-F8-FOCUS: GetAsyncKeyState is global. Opening a window and
        // calling SetForegroundWindow while ANOTHER fullscreen application has
        // focus stole input from it. F8 now works only when this process is in
        // the foreground; the title-bar "Diagnostics" button always works.
        DWORD foregroundProcess=0;
        if (const HWND foreground=GetForegroundWindow()) GetWindowThreadProcessId(foreground,&foregroundProcess);
        if (down && !mF8WasDown && foregroundProcess==GetCurrentProcessId())
            ShowPerformance();
        mF8WasDown=down;
    }

void App::UpdateDiagnostics()
{
        UiSnapshot ui; { std::lock_guard<std::mutex> lock(mUiMx); ui=mUiSnap; }
        std::wstring info=L"stable-no-prediction-20261112\r\n";
        info+=L"Модель: "+Utf8ToWide(ui.backend)+L"; "+(ui.ready?L"готова":L"не готова")+L"\r\n";
        info+=L"Ошибка: "+(ui.error.empty()?std::wstring(L"нет"):Utf8ToWide(ui.error))+L"\r\n";
        info+=L"Устройство: "+mArduino.Status()+L"\r\n";
        info+=L"Захват: "+Utf8ToWide(ui.captureDevice)+L"\r\n";
        info+=L"Выбранный файл: "+SelectedConfigName()+L"\r\nПринятый профиль: "+mLoadedConfigName+(mConfigDirty?L" (есть изменения)":L"")+L"\r\n";
        info+=L"Ревизия настроек UI / конвейера: "+std::to_wstring(mSettingsRevision)+L" / "+std::to_wstring(ui.revision)+L"\r\n";
        info+=L"Папка данных: "+AlbionDataDirectory()+L"\r\n";
        info+=L"Ключ: "+std::wstring(GetFileAttributesW(albion::LicenseCachePath().c_str())==INVALID_FILE_ATTRIBUTES?L"не сохранён":L"DPAPI-файл есть (значение скрыто)")+L"\r\n";
        info+=L"Журнал: потерян�� в очереди="+std::to_wstring(gDevLogDropped.load())+L"; ошибок записи="+std::to_wstring(gDevLogFailures.load())+L"\r\n";
        info+=L"F8: производительность; Diagnostics: состояние; F9: снимок; F10: трасса. Внешние диагностические инструменты ничего не загружают в сеть.\r\n";
        mDiagnostics.Update(info);
        if (!mLoadedConfigName.empty()) {
            // Selected, saved, queued and owner-applied are different states.
            std::wstring status;
            if (ui.revision < mSettingsRevision || ui.revision < mLoadedConfigRevision)
                status=L"Ожидается применение настроек: "+mLoadedConfigName;
            else if (!ui.error.empty())
                status=L"Настройки приняты, но подсистема сообщила ошибку: "+Utf8ToWide(ui.error);
            else if (mConfigDirty)
                status=L"Изменения применены, но не сохранены в профиль: "+mLoadedConfigName;
            else status=(ui.ready?L"Профиль применён: ":L"Настройки применены; модель загружается: ")+mLoadedConfigName;
            if(status!=mLastProfileStatus && !mConfigEditing) { mLastProfileStatus=status; SetConfigStatus(status,!ui.error.empty()); }
        }
    }

bool App::QueryDxgiMemory(double& usedMb, double& budgetMb)
{
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(mDev.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter.As(&adapter3))) return false;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return false;
        constexpr double bytesPerMb = 1024.0 * 1024.0;
        usedMb = double(info.CurrentUsage) / bytesPerMb;
        budgetMb = double(info.Budget) / bytesPerMb;
        return true;
    }

void App::SavePerformanceSnapshot()
{
        double gpuUtil = -1.0, gpuUsed = -1.0, gpuTotal = -1.0, gpuTemp = -1.0;
        mGpuSampler.Sample(gpuUtil, gpuUsed, gpuTotal, gpuTemp);
        UiSnapshot ui;
        { std::lock_guard<std::mutex> snapshotLock(mUiMx); ui = mUiSnap; }
        if (gpuUsed < 0.0) { gpuUsed = ui.captureUsedMb; gpuTotal = ui.captureBudgetMb; }
        const std::uint64_t timeouts = mDxgiTimeouts.load(std::memory_order_relaxed);
        const std::uint64_t timeoutDelta = timeouts - mLastLoggedDxgiTimeouts;
        mLastLoggedDxgiTimeouts = timeouts;
        SYSTEMTIME now{};
        GetLocalTime(&now);

        char gpuUtilText[32] = "N/A";
        char gpuMemoryText[64] = "N/A";
        char gpuTempText[32] = "N/A";
        if (gpuUtil >= 0.0) snprintf(gpuUtilText, sizeof(gpuUtilText), "%.1f %%", gpuUtil);
        if (gpuUsed >= 0.0 && gpuTotal > 0.0)
            snprintf(gpuMemoryText, sizeof(gpuMemoryText), "%.0f / %.0f MB", gpuUsed, gpuTotal);
        if (gpuTemp >= 0.0) snprintf(gpuTempText, sizeof(gpuTempText), "%.1f C", gpuTemp);

        const double sampleMs = PerfNowMs();
        const auto completed = mDet.CompletedCount();
        const double intervalSeconds = std::max(0.001, (sampleMs - mLastInferenceLogMs) * 0.001);
        const double detectionFps = double(completed - mLastLoggedInferenceCount) / intervalSeconds;
        mLastLoggedInferenceCount = completed; mLastInferenceLogMs = sampleMs;
        const auto latency = mDet.RecentLatency();
        char record[1536]{};
        const int length = snprintf(record, sizeof(record),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] F9 snapshot\r\n"
            "GPU utilization: %s\r\n"
            "GPU memory usage: %s\r\n"
            "GPU temperature: %s\r\n"
            "Capture ms: %.3f\r\n"
            "Preprocess GPU ms: %.3f (-1 = unavailable)\r\n"
            "Inference/backend ms: %.3f\r\n"
            "Decode/NMS ms: %.3f\r\n"
            "Capture-to-result ms: %.3f\r\n"
            "Latest result age ms: %.3f\r\n"
            "Capture FPS: %.1f\r\n"
            "Completed inference FPS (since previous F9/start): %.2f over %.3f s\r\n"
            "Completed inferences (lifetime): %llu\r\n"
            "Capture-to-result p50/p95 ms (last %zu valid results): %.3f / %.3f\r\n"
            "DXGI timeouts: %llu (+%llu since previous F9)\r\n"
            "Backend: %s\r\n---\r\n",
            unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
            unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond),
            unsigned(now.wMilliseconds), gpuUtilText, gpuMemoryText, gpuTempText,
            mCaptureMs.load(std::memory_order_relaxed),
            mPreprocessMs.load(std::memory_order_relaxed), ui.inferMs,
            mDecodeMs.load(std::memory_order_relaxed), ui.pipeMs,
            mResultAgeMs.load(std::memory_order_relaxed), ui.capFps,
            detectionFps, intervalSeconds, static_cast<unsigned long long>(completed),
            latency.count, latency.Quantile(0.50), latency.Quantile(0.95),
            static_cast<unsigned long long>(timeouts),
            static_cast<unsigned long long>(timeoutDelta), ui.backend);
        if (length > 0) {
            char stages[768]{};
            const auto& st=ui.stageTiming;
            auto measured=[](double ms) {
                if (!std::isfinite(ms) || ms<0) return std::string("N/A");
                char text[32]{}; std::snprintf(text,sizeof(text),"%.3f",ms); return std::string(text);
            };
            std::snprintf(stages,sizeof(stages),
                "Submit-to-backend ms: %s\r\nResult delivery ms: %s\r\nPresent-to-acquire ms: %s\r\nHost input copy ms: %s\r\nGPU H2D/compute/D2H ms: %s / %s / %s\r\nHost sync wait ms: %s\r\nHost output copy ms: %s\r\nShared map/unmap CPU ms: %s / %s\r\nShared D2D / interop stream gap GPU ms: %s / %s\r\n",
                measured(ui.submitToBackendMs).c_str(),measured(ui.deliveryMs).c_str(),measured(ui.presentToAcquireMs).c_str(),measured(st.hostInputMs).c_str(),
                measured(st.h2dGpuMs).c_str(),measured(st.computeGpuMs).c_str(),measured(st.d2hGpuMs).c_str(),measured(st.waitMs).c_str(),measured(st.hostOutputMs).c_str(),
                measured(st.mapHostMs).c_str(),measured(st.unmapHostMs).c_str(),measured(st.d2dGpuMs).c_str(),measured(st.interopGapGpuMs).c_str());
            // FIX-V11-OWNER: WGC recovery can replace mState; the service
            // thread must use the owner's snapshot, not race mWgc.Stats().
            const auto& captureSource = ui.captureSource;
            const auto writes = gDevHid.writes.load(std::memory_order_relaxed);
            const auto totalUs = gDevHid.totalUs.load(std::memory_order_relaxed);
            char hardware[1024]{};
            std::snprintf(hardware, sizeof(hardware),
                "Output selected: %s; aim enabled=%d; detection enabled=%d; speed=%.3f; mouse scale=%.3f; capture radius=%d; fps cap=%d\r\n"
                "Arduino connected=%d handshake=%d report-bytes=%d; connect=%llu disconnect=%llu reconnect=%llu\r\n"
                "HID writes=%llu failures=%llu short=%llu dropped=%llu last-winerror=%lu; WriteFile mean/max ms=%.3f / %.3f (since startup; not end-to-end)\r\n",
                ui.devOutputMethod ? "Arduino HID" : "WinAPI", int(ui.devAimEnabled), int(ui.devDetectEnabled),
                double(ui.devSpeed), double(ui.devMouseScale), ui.devRadius, ui.devFpsLimit,
                gDevHid.connected.load(std::memory_order_relaxed), gDevHid.handshake.load(std::memory_order_relaxed),
                gDevHid.reportBytes.load(std::memory_order_relaxed),
                gDevHid.connects.load(std::memory_order_relaxed), gDevHid.disconnects.load(std::memory_order_relaxed),
                gDevHid.reconnects.load(std::memory_order_relaxed), writes,
                gDevHid.failures.load(std::memory_order_relaxed), gDevHid.shortWrites.load(std::memory_order_relaxed),
                gDevHid.dropped.load(std::memory_order_relaxed), gDevHid.lastError.load(std::memory_order_relaxed),
                writes ? double(totalUs) / double(writes) / 1000.0 : 0.0,
                double(gDevHid.maxUs.load(std::memory_order_relaxed)) / 1000.0);
            const std::string details = std::string(stages) + "Detector ready: " + (ui.ready ? "true" : "false") +
                "\r\nDetector error: " + (ui.error.empty() ? "none" : ui.error) +
                "\r\nCapture source: " + captureSource + "\r\nCapture adapter: " + ui.captureDevice +
                "\r\nBackend detail: " + mDet.BackendDetails() + "\r\n" + hardware +
                "Source revision: stable-no-prediction-20261112; preprocess timing: sampled 1/16 (full with ALBION_GPU_TIMING=1)\r\n---\r\n";
            const std::string snapshot = std::string(record, size_t(std::min<int>(length, int(sizeof(record)) - 1))) + details;
            if (!AppendTextFileUtf8(AlbionDataPath(L"performance_measurements.txt"), snapshot))
                DevLog("SNAPSHOT", "Could not save performance snapshot");
            // Do not duplicate the entire snapshot in the event log.
            DevLog("SNAPSHOT", "Performance snapshot saved/request completed");
        }
    }

void App::PollPerformanceSnapshotHotkey()
{
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const double now=NowMs();
        if ((down && !mF9WasDown) ||
            (mAutoPerfIntervalMs>0 && now-mLastAutoPerfMs>=mAutoPerfIntervalMs)) {
            if (!mPerfSnapshotQueued.exchange(true)) {
                if (!mServiceTasks.Post([this] {
                    albion::GuardWorker([this] { SavePerformanceSnapshot(); }, [](const char* e) { DevLog("SNAPSHOT", e); },
                        [this] { mPerfSnapshotQueued = false; });
                })) mPerfSnapshotQueued = false;
            }
            mLastAutoPerfMs=now;
        }
        mF9WasDown = down;
    }

void App::StopJitterTrace()
{
        std::vector<JitterRow> rows;
        std::uint64_t dropped=0;
        {
            std::lock_guard<std::mutex> guard(mJitterMx);
            mJitterRecording.store(false, std::memory_order_relaxed);
            rows.swap(mJitterRows); dropped=mJitterDropped; mJitterDropped=0;
        }
        SYSTEMTIME t{}; GetLocalTime(&t);
        wchar_t name[128]{};
        std::swprintf(name, sizeof(name)/sizeof(name[0]),
            L"albion-jitter-%04u%02u%02u-%02u%02u%02u-%03u.csv",
            unsigned(t.wYear),unsigned(t.wMonth),unsigned(t.wDay),unsigned(t.wHour),
            unsigned(t.wMinute),unsigned(t.wSecond),unsigned(t.wMilliseconds));
        std::string csv;
        csv.reserve(512 + rows.size()*320);
        csv += "# jitter-trace-v2; QPC milliseconds, absolute desktop pixels; dropped=" + std::to_string(dropped) + "\r\n";
        csv += "frame_id,observed_ms,captured_ms,completed_ms,detections,active,found,passive_candidate,async_mode,command_known,center_x,center_y,aim_x,aim_y,box_x,box_y,box_w,box_h,confidence,err_x,err_y,step_x,step_y,move_x,move_y,residual_before_x,residual_before_y,residual_after_x,residual_after_y,px_per_count,scale,speed,hard_mode,smooth_tracking,image_ms,acquired_ms,submitted_ms,worker_started_ms,result_age_ms,acquire_delay_ms,inference_ms,decode_ms,accumulated_frames,roi_x,roi_y,roi_w,roi_h,letterbox_ratio,pad_x,pad_y,net_side,processed_x,processed_y,command_begin_ms,command_end_ms,send_attempted,dxgi_timeouts,activation_key,target_class\r\n";
        for (const auto& r : rows) {
            const auto& a=r.trace; const auto& b=a.selected.detection;
            char line[1024]{};
            const int n=std::snprintf(line,sizeof(line),
                "%llu,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.4f,%.4f,%.4f,%.4f,%ld,%ld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d\r\n",
                static_cast<unsigned long long>(r.frameId),r.observedMs,r.capturedMs,r.completedMs,
                r.detectionCount,int(a.active),int(a.found),int(!a.active && a.found),
                int(a.asyncMode),int(a.commandKnown),double(r.centerX),double(r.centerY),
                double(a.selected.x),double(a.selected.y),double(b.x),double(b.y),double(b.w),double(b.h),double(b.score),
                double(a.errX),double(a.errY),double(a.stepX),double(a.stepY),long(a.moveX),long(a.moveY),
                double(a.residualBeforeX),double(a.residualBeforeY),double(a.residualAfterX),double(a.residualAfterY),
                double(a.pxPerCount),double(r.scale),double(r.speed),int(r.hardMode),int(r.smoothTracking));
            if (n>2 && size_t(n)<sizeof(line)) {
                csv.append(line,size_t(n)-2); // replace the CRLF with v2 fields
                const auto& t=r.timing;
                const double age=t.presentedMs>0?r.observedMs-t.presentedMs:-1;
                const double acquire=t.presentedMs>0&&t.acquiredMs>0?t.acquiredMs-t.presentedMs:-1;
                char extra[1536]{};
                const int m=std::snprintf(extra,sizeof(extra),
                    ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%.4f,%.4f,%.4f,%.4f,%.8f,%.4f,%.4f,%d,%.4f,%.4f,%.6f,%.6f,%d,%llu,%d,%d\r\n",
                    t.presentedMs,t.acquiredMs,t.submittedMs,t.workerStartedMs,age,acquire,t.inferenceMs,t.decodeMs,
                    unsigned(t.accumulatedFrames),double(t.roiX),double(t.roiY),double(t.roiWidth),double(t.roiHeight),
                    double(t.letterboxRatio),double(t.padX),double(t.padY),t.netSide,
                    double(a.processedX),double(a.processedY),
                    a.commandBeginMs,a.commandEndMs,int(a.sendAttempted),static_cast<unsigned long long>(r.dxgiTimeouts),
                    r.activationKey,r.targetClass);
                if(m>0&&size_t(m)<sizeof(extra))csv.append(extra,size_t(m));
                else csv+="\r\n";
            }
        }
        const bool ok=WriteTextFileUtf8(AlbionDataPath(name),csv);
        const std::string msg = std::string(ok ? "Trace saved: " : "ERROR saving trace: ") +
            WideToUtf8(name) + "; rows=" + std::to_string(rows.size()) + "; dropped=" + std::to_string(dropped);
        DevLog("JITTER",msg.c_str());
    }

void App::PollJitterTraceHotkey()
{
        const bool down=(GetAsyncKeyState(VK_F10) & 0x8000)!=0;
        if (down && !mF10WasDown) {
            if (mJitterRecording.load(std::memory_order_relaxed)) StopJitterTrace();
            else {
                { std::lock_guard<std::mutex> guard(mJitterMx);
                  mJitterRows.clear(); mJitterRows.reserve(kJitterMaxRows); mJitterDropped=0;
                  mJitterStartedMs=PerfNowMs();
                  mJitterRecording.store(true, std::memory_order_relaxed); }
                DevLog("JITTER","Recording v2 started. F10 to save; automatic stop after 30 seconds. Up to 8192 detections. No movement is enabled by recording.");
            }
        }
        mF10WasDown=down;
        if(mJitterRecording.load(std::memory_order_relaxed)&&PerfNowMs()-mJitterStartedMs>=30000.0)StopJitterTrace();
    }
