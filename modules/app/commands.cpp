// modules/app/commands.cpp
#include "modules/app/app.hpp"

void App::ArduinoMouseSink(LONG dx, LONG dy)
{
        if (auto* sink = sArduinoSink.load(std::memory_order_acquire)) sink->MoveRelative(int(dx), int(dy));
    }

void App::QueueSettings(bool includeModel)
{
        if (mApplyingConfig) return;
        if (!mLoadedConfigName.empty()) mConfigDirty = true;
        SettingsCommand next{mAimCfg, int(mAimMethod), int(mCaptureMethod), ++mSettingsRevision};
        const auto path = includeModel ? CurrentModelPath() : std::wstring{};
        { std::lock_guard<std::mutex> lock(mCommandMx); mPendingSettings = next;
          if (includeModel) mPendingModel = ModelRequest{path, false}; }
        mDet.NotifyActivity();
    }

void App::ProcessCommands()
{
        std::optional<SettingsCommand> settings; std::optional<ModelRequest> model; bool retry = false;
        { std::lock_guard<std::mutex> lock(mCommandMx);
          settings.swap(mPendingSettings); model.swap(mPendingModel);
          retry = mPendingCaptureRetry; mPendingCaptureRetry = false; }
        if (settings) {
            mAim.Reset(); mAimTarget.reset(); mAim.Config() = settings->config;
            gDecodeConf.store(settings->config.confidence);
            mLastPreprocessFrameId=mCapturedFrameId; // discard cached ROI on settings revision
            mCaptureRegion = MakeCenteredRegion(mDeskW, mDeskH, settings->config.radius);
            mWorkerCaptureMethod = static_cast<CaptureMethod>(settings->capture); mCapFailures = 0;
            const auto method = static_cast<AimMethod>(settings->method);
            if (method != mWorkerAimMethod) {
                mAim.Shutdown(); // no old sender can outlive a device/sink switch
                if (method == AimMethod::ArduinoHid) {
                    sArduinoSink.store(&mArduino); gMouseMoveSink.store(&App::ArduinoMouseSink);
                    mArduino.Connect(); // queues enumeration/handshake, never falls back silently
                } else {
                    gMouseMoveSink.store(nullptr); sArduinoSink.store(nullptr); mArduino.Disconnect();
                }
                mWorkerAimMethod = method;
            }
            mAppliedRevision = settings->revision;
        }
        if (retry) {
            // Explicit recovery owns the full teardown: detector releases its
            // registrations BEFORE a replacement device/preprocessor is created.
            mDet.Stop(); mSharedFrames.Reset(); mHasTensor = false; mPre.DiscardCpuFrames();
            mFrameSRV.Reset(); mFrameTex.Reset(); mDupl.Reset(); mDesktopSrvs.Reset();
#ifdef AIM_ENABLE_WGC
            if (mUseWgc) mWgc.Stop();
#endif
            CreateDevice(); mPre = GpuPreprocessor{}; mPre.Init(mDev.Get(), AimPerfEnvFlag(L"ALBION_GPU_TIMING"));
            mDet.EnableSharedInput(mDev.Get(), mPre.TensorBuffer(0), mPre.TensorBuffer(1));
#ifdef AIM_ENABLE_WGC
            if (mUseWgc) {
                DXGI_OUTPUT_DESC output{}; HR(mOutput1->GetDesc(&output), "WGC output");
                mWgc.Start(mDev.Get(), output.Monitor, mDet.ActivityEvent());
                mDeskW = UINT(mWgc.Width()); mDeskH = UINT(mWgc.Height());
                mCaptureRegion = MakeCenteredRegion(mDeskW, mDeskH, mAim.Config().radius);
                EnsureCaptureTexture();
            }
            else
#endif
            StartDuplication();
            mFrameW = mFrameH = 0; mLastPreprocessFrameId = 0; mCapturedFrameId = 0;
            if (!model) model = ModelRequest{mDet.ModelPath(), true};
            mCaptureFailed = false;
        }
        if (model && (model->force || model->path != mDet.ModelPath() || (!mDet.Ready() && !mDet.Loading()))) {
            mDet.Stop(); // FIX-V11-04/08: owner join, never under UI/global lock
            mDets.clear(); mAimTarget.reset(); mAim.Reset(); mLastDetVer = 0;
            mSharedFrames.Reset(); mHasTensor = false; mPre.DiscardCpuFrames();
            mLatestTiming = {}; mLatestStages = {}; mPipelineMs = 0; mLastPreprocessFrameId = 0;
            mDet.Start(model->path);
        }
    }
