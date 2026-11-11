// modules/app/lifecycle.cpp
#include "modules/app/app.hpp"

App::~App()
{
        mConverter.Cancel();mConverter.Join();
        mPerformance.Close();
        mDiagnostics.Close();
        mDiscord.Stop();
        StopPipeline();
        mServiceTasks.Stop();
        mAim.Shutdown();
        gMouseMoveSink.store(nullptr); sArduinoSink.store(nullptr);
        mArduino.Disconnect();
        if (mJitterRecording.load(std::memory_order_relaxed)) {
            try { StopJitterTrace(); } catch (...) { DevLog("JITTER", "ERROR saving trace on exit"); }
        }
#ifdef AIM_ENABLE_WGC
        mWgc.Stop();
        if(mWgcApartment)winrt::uninit_apartment();
#endif
        mDet.Stop();
        // FIX-V11-LIFETIME: callbacks and children must die while App is alive,
        // including startup exceptions and Escape/PostQuitMessage exits.
        EndTabFade();
        if (mOverlay) { DestroyWindow(mOverlay); mOverlay = nullptr; }
        if (mHwnd) { DestroyWindow(mHwnd); mHwnd = nullptr; }
        DestroySettingsBuffer();
        DestroyOverlayBuffer();
        DestroyOverlayResources();
        ResetUiImages();
        HGDIOBJ owned[] = {mFontUi,mFontBold,mFontMono,mFontTitle,mFontSmall,mFontTab,
            mBrPanel,mBrInput,mBrStatus};
        for (auto object : owned) if (object) DeleteObject(object);
        // The registered class owns mBrBg; Windows releases that class brush.
        if (mGdiplusToken) Gdiplus::GdiplusShutdown(mGdiplusToken);
    }

void App::Run(HINSTANCE hInst)
{
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&mGdiplusToken, &startupInput, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("GDI+ startup failed");
SetEnvironmentVariableW(L"ALBION_WGC",L"0");
        wchar_t logInterval[16]{};
        const DWORD intervalLength=GetEnvironmentVariableW(L"ALBION_PERF_LOG_MS",logInterval,16);
        if(intervalLength>0 && intervalLength<16) {
            wchar_t* end=nullptr;const long ms=std::wcstol(logInterval,&end,10);
            if(end && *end==0 && ms>=1000 && ms<=60000)mAutoPerfIntervalMs=double(ms);
        }
        CreateDevice();
        mGpuSampler.Init();
        CreateSettingsWindow(hInst);
        // Same Discord Rich Presence settings as main.py. Connection runs on
        // its own thread and retries when Discord is launched or restarted.
        mDiscord.Start();
        mPre.Init(mDev.Get(), AimPerfEnvFlag(L"ALBION_GPU_TIMING"));
        // Otdayom detektoru bufer tenzora: esli backend umeet chitat ego
        // napryamuyu, readback na CPU voobshche ne ponadobitsya.
        mDet.EnableSharedInput(mDev.Get(), mPre.TensorBuffer(0), mPre.TensorBuffer(1));

        mAimCfg.radius = 320;
        mAimCfg.confidence = 0.50f;
        mAimCfg.aimHeight = 0.30f;
        mAimCfg.targetBodyPart = AimBodyPart::Body;
        mAimCfg.smoothing = 1.80f;
        mAimCfg.deadZone = 0.0f;
        mAimCfg.sendThreshold = 0.5f;
        mAimCfg.maxDelta = 127.0f;
        mAimCfg.tickSeconds = 0.008f;
        mAimCfg.activationKey = VK_RBUTTON;

#ifdef AIM_ENABLE_WGC
        mUseWgc=false; // DXGI-only release
        if(mUseWgc) {
            winrt::init_apartment(winrt::apartment_type::single_threaded);mWgcApartment=true;
            DXGI_OUTPUT_DESC output{};HR(mOutput1->GetDesc(&output),"WGC monitor description");
            mWgc.Start(mDev.Get(),output.Monitor,mDet.ActivityEvent());
            mDeskW=UINT(mWgc.Width());mDeskH=UINT(mWgc.Height());
            mCaptureRegion=MakeCenteredRegion(mDeskW,mDeskH,mAimCfg.radius);
            EnsureCaptureTexture();
        } else
#endif
        StartDuplication();
        CreateControls();
        LayoutControls();
        mAim.Config() = mAimCfg;
        mDet.Start(CurrentModelPath());
        StartPipeline();
        MessageLoop();
    }
