// WindowsProject1.cpp
// Composition root: startup, authentication and application lifetime only.
#include "modules/app/app.hpp"

// ---------------------------------------------------------------------------
int WINAPI wWinMain(
    _In_     HINSTANCE hInst,
    _In_opt_ HINSTANCE,
    _In_     PWSTR commandLine,
    _In_     int)
{
    if (GetEnvironmentVariableW(L"ALBION_GPU_TIMING", nullptr, 0) == 0)
        SetEnvironmentVariableW(L"ALBION_GPU_TIMING",L"0");
    // Stable release defaults live in the executable, not a launcher script.
    SetEnvironmentVariableW(L"ALBION_LEGACY_WAIT",L"0");
    SetEnvironmentVariableW(L"ALBION_ORT_CPU",L"0");
    SetEnvironmentVariableW(L"ALBION_DISABLE_INTEROP",L"0");
    SetEnvironmentVariableW(L"ALBION_IO_GRAPH",L"0");
    SetEnvironmentVariableW(L"ALBION_COPY_OUTPUT",L"0");
    SetEnvironmentVariableW(L"ALBION_OUTPUT_GRAPH",L"0");
    SetEnvironmentVariableW(L"ALBION_WGC",L"0");
    SetEnvironmentVariableW(L"ALBION_CUDA_POLL",L"0");
    SetEnvironmentVariableW(L"ALBION_PERF_LOG_MS",L"0");
    struct LogLease { ~LogLease() { DevShutdownLog(); } } logLease;
    // This build does not create a diagnostic console; file logs remain enabled.
    DevLog("START", "Stable no-prediction 20261112: F8 diagnostics, F9 snapshot, optional F10 trace; automatic snapshots off by default.");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        // Explorer, shortcuts, and terminals can supply arbitrary working dirs.
        const std::wstring programDirectory=AlbionProgramDirectory();
        if(!SetCurrentDirectoryW(programDirectory.c_str()))
            throw std::runtime_error("Cannot set application working directory");
        int argumentCount=0;
        auto arguments=CommandLineToArgvW(GetCommandLineW(),&argumentCount);
        const bool startupCheck=arguments && argumentCount==2 &&
            std::wcscmp(arguments[1],L"--startup-check")==0;
        if(arguments)LocalFree(arguments);
        if(startupCheck) {
            // Noninteractive dependency/model test only. Does not enter App::Run,
            // authenticate, connect HID, capture the desktop or send input.
            std::string error;auto backend=CreateInferenceBackend(PickModelPath(),640,error);
            if(!backend)throw std::runtime_error("Startup backend check: "+error);
            const int side=backend->InputSide();
            if(side<64 || side>640)throw std::runtime_error("Invalid startup input shape");
            std::vector<float> tensor(size_t(3)*side*side,0.0f),scratch;
            InferenceOutputView view{};int channels=0,anchors=0;
            if(!backend->RunView(tensor.data(),scratch,view,channels,anchors,error) ||
                !view.data || view.count==0)throw std::runtime_error("Startup inference check: "+error);
            const std::string report="PASS direct-exe-startup\n"+backend->Diagnostics()+
                "\nModel: "+WideToUtf8(PickModelPath())+
                "\nWorking directory anchored to EXE; no CMD required.\n";
            if(!WriteTextFileUtf8(programDirectory+L"startup-check.txt",report))
                throw std::runtime_error("Cannot save startup-check.txt");
            DevLog("STARTUP CHECK",report.c_str());return 0;
        }
        ShowAlbionStartupSplash(hInst);
        if (!RunAlbionAuthGate(hInst)) { DevLog("AUTH", "Authentication cancelled or failed."); return 1; }
        DevLog("AUTH", "Online license verification succeeded.");
        App app; app.Run(hInst); DevLog("STOP", "Normal application shutdown");
    }
    catch (const winrt::hresult_error& e)
    {
        DevLog("FATAL", WideToUtf8(std::wstring(e.message().c_str())).c_str());
        AlbionAuthFinishHandoff(nullptr);
        MessageBoxW(nullptr, e.message().c_str(), RU_ERROR, MB_ICONERROR);
        return 1;
    }
    catch (const std::exception& e)
    {
        // Convert the narrow exception text instead of reinterpreting char*
        // as wchar_t*, which can read past the string and display garbage.
        DevLog("FATAL", e.what());
        const std::wstring message = Utf8ToWide(e.what());
        AlbionAuthFinishHandoff(nullptr);
        MessageBoxW(nullptr, message.c_str(), RU_ERROR, MB_ICONERROR);
        return 1;
    }
    return 0;
}