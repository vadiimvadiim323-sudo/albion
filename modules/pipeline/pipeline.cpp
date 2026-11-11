// modules/pipeline/pipeline.cpp
#include "modules/app/app.hpp"

bool App::WantsPreprocess()
{
        if (!mCapturedFrameId || mCapturedFrameMs<=0 || mLastPreprocessFrameId==mCapturedFrameId) return false;
        return HasPreprocessCapacity();
    }

bool App::HasPreprocessCapacity()
{
        const bool shared = mDet.SharedInput();
        mPre.SetSharedMode(shared);
        if (shared)
        {
            // At most one active and one prepared frame. Replacing the pending
            // tensor at 240/unlimited FPS competes with the inference itself.
            if (mSharedFrames.HasPending()) return false;
            const int slot = mSharedFrames.WriteSlot(mDet.WantsFrame(), mDet.SharedSlotCount());
            return slot >= 0 && mPre.CanWriteSharedSlot(mCtx.Get(), size_t(slot));
        }
        if (mPre.HasPending()) return false;
        // Keep one ready CPU tensor, not an unbounded stream of discarded
        // readbacks while the detector is still busy.
        return !mHasTensor;
    }

void App::SubmitPreprocess(ID3D11ShaderResourceView* srv, UINT texW, UINT texH,
        const CaptureRegion& region, LONG outLeft, LONG outTop)
{
        if (!mDet.Ready()) return;
        const bool shared = mDet.SharedInput();
        int slot = 0;
        if (shared)
        {
            slot = mSharedFrames.WriteSlot(mDet.WantsFrame(), mDet.SharedSlotCount());
            if (slot < 0 || !mPre.CanWriteSharedSlot(mCtx.Get(), size_t(slot))) return;
        }
        mPre.SetSharedMode(shared);
        mSubmitTime = NowMs();
        LetterboxInfo lb = mPre.Submit(mCtx.Get(), srv, texW, texH,
            region, outLeft, outTop, mCapturedFrameId, mCapturedFrameMs, mSubmitTime, mPresentedFrameMs, size_t(slot), gNetSize.load(std::memory_order_relaxed),mAcquiredFrameMs,mAccumulatedFrames);
        lb.presentedMs=mPresentedFrameMs;
        if(lb.net>0) mLastPreprocessFrameId=mCapturedFrameId;
        if (lb.net > 0 && shared)
        {
            // Replaces only the unconsumed pending slot, never the active slot.
            mSharedFrames.Commit(slot, lb);
            DispatchSharedFrame();
        }
    }

void App::DispatchSharedFrame()
{
        if (!mDet.SharedInput()) return;
        LetterboxInfo lb{};
        const int slot = mSharedFrames.Take(mDet.WantsFrame(), lb);
        if (slot >= 0) mDet.PushShared(lb, size_t(slot));
    }

bool App::PumpDetection(bool allowPreprocess)
{
        mPre.PollGpuTiming(mCtx.Get());
        mPreprocessMs.store(mPre.GpuMs(), std::memory_order_relaxed);
        if (!mDetectEnabled || !mDet.Ready() || (!mFrameSRV && !mDirectCapture))
        {
            const bool changed = !mDets.empty();
            mDets.clear();
            mAim.Reset();
            mAimTarget.reset();
            return changed;
        }
        LetterboxInfo lb{};
        // Zabiraem gotovyy tenzor vsegda, a ne tolko kogda detektor svoboden.
        if (!mDet.SharedInput() && mPre.TryFetch(mCtx.Get(), mTensorCpu.data(), lb))
        {
            mHasTensor = true; mTensorLb = lb;
        }
        if (!mDet.SharedInput() && mHasTensor && mDet.WantsFrame())
        {
            mHasTensor = false;
            // Obmen buferami vmesto kopirovaniya tenzora.
            mDet.Push(mTensorCpu, mTensorLb);
            if (mTensorCpu.size() < kTensorFloatsMax) mTensorCpu.resize(kTensorFloatsMax);
        }
        // Derzhim odin kadr preprotsessa v polete, chtoby k momentu osvobozhdeniya
        // detektora svezhiy tenzor uzhe lezhal gotovym.
        // Pri pryamom SRV kadr uzhe otpravlen vnutri CaptureFrame.
        if (allowPreprocess && !mDirectCapture && mFrameSRV && WantsPreprocess())
        {
            CaptureRegion localRegion = mCaptureRegion;
            localRegion.left = 0;
            localRegion.top = 0;
            SubmitPreprocess(mFrameSRV.Get(), mFrameW, mFrameH, localRegion,
                mCaptureRegion.left, mCaptureRegion.top);
        }
        // Pending shared frames can start between FPS-limited capture ticks.
        DispatchSharedFrame();
        const bool nd = mDet.CopyLatestIfChanged(mLastDetVer, mDets, mLatestTiming,mLatestStages);
        const double now = NowMs();
        if (nd)
        {
            mDeliveryMs=std::max(0.0,now-mLatestTiming.completedMs);
            if(mLatestTiming.frameId && mLatestTiming.completedMs>0) albion::performance::telemetry.Delivery(mDeliveryMs);
            mPipelineMs = mLatestTiming.capturedMs > 0.0
                ? std::max(0.0, mLatestTiming.completedMs - mLatestTiming.capturedMs) : 0.0;
            mDecodeMs.store(mLatestTiming.decodeMs, std::memory_order_relaxed);
        }
        const double ageMs = mLatestTiming.capturedMs > 0.0
            ? std::max(0.0, now - mLatestTiming.capturedMs) : 0.0;
        mResultAgeMs.store(ageMs, std::memory_order_relaxed);
        // The frame age already includes inference. Never add inference again.
        mAim.Config().latencySeconds = float(std::clamp(ageMs * 0.001, 0.0, 0.12));
        const bool traceNow = nd && mJitterRecording.load(std::memory_order_relaxed);
        AimTraceObservation trace{};
        mAimTarget = mAim.Update(mDets, mCaptureRegion, mAimEnabled && mDetectEnabled, nd,
            mLatestTiming.capturedMs > 0.0 ? mLatestTiming.capturedMs * 0.001 : -1.0,
            traceNow ? &trace : nullptr);
        if (traceNow) {
            // Passive measurements only: nearest candidate, not a controller selection.
            if (!trace.active) {
                const auto candidate = SelectNearestTarget(mDets, mCaptureRegion, mAim.Config());
                if (candidate) { trace.found = true; trace.selected = *candidate; }
            }
            JitterRow row{};
            row.observedMs=now; row.capturedMs=mLatestTiming.capturedMs;
            row.completedMs=mLatestTiming.completedMs; row.frameId=mLatestTiming.frameId;
            row.timing=mLatestTiming; row.dxgiTimeouts=mDxgiTimeouts.load(std::memory_order_relaxed);
            row.activationKey=mAim.Config().activationKey; row.targetClass=mAim.Config().targetClass;
            row.detectionCount=int(mDets.size()); row.centerX=mCaptureRegion.CenterX();
            row.centerY=mCaptureRegion.CenterY(); row.scale=mAim.Config().trackingOutputScale;
            row.speed=mAim.Config().smoothing; row.hardMode=mAim.Config().hardMode;
            row.smoothTracking=mAim.Config().smoothTracking; row.trace=trace;
            std::lock_guard<std::mutex> guard(mJitterMx);
            if (mJitterRecording.load(std::memory_order_relaxed)) {
                if (mJitterRows.size() < kJitterMaxRows) mJitterRows.push_back(row);
                else ++mJitterDropped;
            }
        }
        return nd;
    }

void App::ReportPipelineFailure(const std::string& error)
{
        DevLog("PIPELINE ERROR", error.c_str());
        std::lock_guard<std::recursive_mutex> lock(mPipeMx);
        mAim.Reset();mAimTarget.reset();mDets.clear();
        std::lock_guard<std::mutex> snapshotLock(mUiMx);
        mUiSnap.ready=false;mUiSnap.error=error;mUiSnap.dets.clear();mUiSnap.target.reset();
    }

void App::StartPipeline()
{
        bool expected = false;
        if (!mPipeRunning.compare_exchange_strong(expected, true)) return;
        try { mPipeThread = std::thread([this]() {
            albion::GuardWorker([this] { PipelineThread(); }, [this](const char* e) { ReportPipelineFailure(e); },
                [this] { mPipeRunning = false; });
        }); } catch (...) { mPipeRunning = false; throw; }
    }

void App::StopPipeline()
{
        mPipeRunning.store(false);
        mDet.NotifyActivity();
        if (mPipeThread.joinable()) mPipeThread.join();
    }

std::optional<App::UiSnapshot> App::CapturePipelineSnapshot(unsigned& frames, bool& detectionsChanged, bool captureDue)
{
        std::lock_guard<std::recursive_mutex> pipelineLock(mPipeMx);
        // Service completed results before entering the non-cancellable DXGI wait.
        detectionsChanged = PumpDetection(!captureDue);
        // PumpDetection can fill a staging tensor; recheck capacity after it.
        if (captureDue && (!mDetectEnabled || !mDet.Ready() || HasPreprocessCapacity())) {
            const double begin=NowMs();
            if (CaptureFrame(CaptureTimeoutMs())) {
                ++frames; albion::performance::telemetry.Capture((std::max)(0.0,NowMs()-begin));
            }
        }
        // A completion arriving during acquisition is consumed in this pass.
        detectionsChanged = PumpDetection(captureDue) || detectionsChanged;

        mUiDetsDirty = mUiDetsDirty || detectionsChanged;
        const double snapshotNow = NowMs();
        const bool readyNow = mDet.Ready();
        if(snapshotNow < mNextUiPublishMs && readyNow == mLastUiReady && mAppliedRevision == mLastUiRevision)
            return std::nullopt;
        mNextUiPublishMs = snapshotNow + 1000.0/30.0;
        mLastUiReady = readyNow; mLastUiRevision = mAppliedRevision;
        detectionsChanged = mUiDetsDirty; mUiDetsDirty = false;
        UiSnapshot snapshot;
        if (detectionsChanged) snapshot.dets = mDets;
        snapshot.target = mAimTarget;
        snapshot.region = mCaptureRegion;
        snapshot.inferMs = mDet.InferenceMs();
        snapshot.pipeMs = mPipelineMs;
        snapshot.deliveryMs=mDeliveryMs;
        snapshot.submitToBackendMs=mLatestTiming.workerStartedMs>0 ? std::max(0.0,mLatestTiming.workerStartedMs-mLatestTiming.submittedMs) : -1;
        snapshot.presentToAcquireMs=mLatestTiming.presentedMs>0 ? std::max(0.0,mLatestTiming.capturedMs-mLatestTiming.presentedMs) : -1;
        snapshot.stageTiming=mLatestStages;
        snapshot.confidence = mAim.Config().confidence;
        snapshot.ready = mDet.Ready(); snapshot.revision = mAppliedRevision;
        // DXGI sampling runs on the device owner, at most once per second.
        if (NowMs() - mLastMemorySampleMs >= 1000) {
            QueryDxgiMemory(mCaptureUsedMb, mCaptureBudgetMb); mLastMemorySampleMs = NowMs();
        }
        snapshot.captureUsedMb = mCaptureUsedMb; snapshot.captureBudgetMb = mCaptureBudgetMb;
        snapshot.captureDevice = mCaptureDeviceDetails;
#ifdef AIM_ENABLE_WGC
        if (mUseWgc) {
            const auto counts = mWgc.Stats();
            snapshot.captureSource = "WGC latest mailbox; published=" + std::to_string(counts.published) +
                "; taken=" + std::to_string(counts.taken) + "; replaced=" + std::to_string(counts.replaced);
        }
#endif
        snapshot.backend = mDet.Backend();
        snapshot.layout = mDet.LayoutName();
        snapshot.error = snapshot.ready ? std::string() : mDet.Error();
        snapshot.devOutputMethod = mWorkerAimMethod == AimMethod::ArduinoHid ? 1 : 0;
        snapshot.devFpsLimit = mFpsLimit.load(std::memory_order_relaxed);
        snapshot.devRadius = mAim.Config().radius;
        snapshot.devSpeed = mAim.Config().smoothing;
        snapshot.devMouseScale = mAim.Config().trackingOutputScale;
        snapshot.devAimEnabled = mAimEnabled; snapshot.devDetectEnabled = mDetectEnabled;
        return snapshot;
    }

void App::PublishPipelineSnapshot(UiSnapshot snapshot, bool detectionsChanged)
{
        std::lock_guard<std::mutex> snapshotLock(mUiMx);
        if (detectionsChanged) mUiSnap.dets = std::move(snapshot.dets);
        mUiSnap.target = std::move(snapshot.target);
        mUiSnap.region = snapshot.region;
        mUiSnap.inferMs = snapshot.inferMs;
        mUiSnap.pipeMs = snapshot.pipeMs;
        mUiSnap.deliveryMs=snapshot.deliveryMs;
        mUiSnap.submitToBackendMs=snapshot.submitToBackendMs;
        mUiSnap.presentToAcquireMs=snapshot.presentToAcquireMs;
        mUiSnap.stageTiming=snapshot.stageTiming;
        mUiSnap.confidence = snapshot.confidence;
        mUiSnap.ready = snapshot.ready; mUiSnap.revision = snapshot.revision;
        mUiSnap.captureUsedMb = snapshot.captureUsedMb; mUiSnap.captureBudgetMb = snapshot.captureBudgetMb;
        mUiSnap.captureDevice = std::move(snapshot.captureDevice);
        mUiSnap.captureSource = std::move(snapshot.captureSource);
        mUiSnap.backend = std::move(snapshot.backend);
        mUiSnap.layout = std::move(snapshot.layout);
        mUiSnap.error = std::move(snapshot.error);
        mUiSnap.devOutputMethod = snapshot.devOutputMethod; mUiSnap.devFpsLimit = snapshot.devFpsLimit;
        mUiSnap.devRadius = snapshot.devRadius; mUiSnap.devSpeed = snapshot.devSpeed;
        mUiSnap.devMouseScale = snapshot.devMouseScale; mUiSnap.devAimEnabled = snapshot.devAimEnabled;
        mUiSnap.devDetectEnabled = snapshot.devDetectEnabled;
    }

void App::PublishCaptureFps(double fps)
{
        std::lock_guard<std::mutex> snapshotLock(mUiMx);
        mUiSnap.capFps = fps;
    }

void App::PipelineThread()
{
#ifdef AIM_ENABLE_WGC
        // Capture recovery creates WinRT resources on this owner thread too.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct ApartmentLease { ~ApartmentLease() { winrt::uninit_apartment(); } } apartment;
#endif
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        PipelineActivityWaiter waiter(!AimPerfEnvFlag(L"ALBION_LEGACY_WAIT"));
        DevLog("PIPELINE", waiter.HighResolution()
            ? "High-resolution event/timer waits; preprocess timing sampled 1/16 (full with ALBION_GPU_TIMING=1)"
            : "Legacy event timeouts; preprocess timing sampled 1/16 (full with ALBION_GPU_TIMING=1)");
        double window = NowMs(), nextCaptureMs = 0.0;
        int previousFpsLimit = -1;
        unsigned frames = 0;
        while (mPipeRunning.load(std::memory_order_relaxed))
        {
            try {
            if(PauseForModelConversion(waiter))continue;
            ProcessCommands();
            if (mCaptureFailed) { waiter.Wait(mDet.ActivityEvent(), 250); continue; }
            const int fpsLimit = mFpsLimit.load(std::memory_order_relaxed);
            const double now = NowMs();
            if (fpsLimit != previousFpsLimit)
            {
                previousFpsLimit = fpsLimit; nextCaptureMs = now;
            }
            // Backpressure BEFORE DXGI acquisition/copy, not only before Dispatch.
            // DXGI retains the newest desktop image while inference is saturated.
            const bool capacity = !mDetectEnabled || !mDet.Ready() || HasPreprocessCapacity();
            const bool captureDue = capacity && (fpsLimit <= 0 || now >= nextCaptureMs);
            if (captureDue && fpsLimit > 0)
            {
                const double interval = 1000.0 / double(fpsLimit);
                // A cap is a maximum rate, not a debt to repay in bursts after\n                // backpressure or scheduler delays.
                nextCaptureMs = now + interval;
            }
            bool detectionsChanged = false;
            auto snapshot = CapturePipelineSnapshot(frames, detectionsChanged, captureDue);
            if(snapshot) PublishPipelineSnapshot(std::move(*snapshot), detectionsChanged);
            const double after = NowMs();
            if (after - window >= 500.0)
            {
                mCapFps = double(frames) * 1000.0 / (after - window);
                PublishCaptureFps(mCapFps); frames = 0; window = after;
            }
            // Completion wakes us immediately even at a low capture FPS cap.
            // A bounded wait also services CPU staging and activation polling.
            // In unlimited mode AcquireNextFrame already supplies a bounded wait.
#ifdef AIM_ENABLE_WGC
            if(mUseWgc){waiter.Wait(mDet.ActivityEvent(), PipelineWaitMs(fpsLimit, NowMs(), nextCaptureMs));continue;}
#endif
            if (fpsLimit > 0) waiter.Wait(mDet.ActivityEvent(), capacity ? PipelineWaitMs(fpsLimit, NowMs(), nextCaptureMs) : 1.0);
            else waiter.Wait(mDet.ActivityEvent(), 1.0);
            }
#ifdef AIM_ENABLE_WGC
            catch (const winrt::hresult_error& e) {
                mCaptureFailed = true;
                try { const auto message=winrt::to_string(e.message()); ReportPipelineFailure(message.c_str()); }
                catch (...) { ReportPipelineFailure("WinRT capture failure; use Capture recovery in F8"); }
            }
#endif
            catch (const std::exception& e) {
                mCaptureFailed = true; ReportPipelineFailure(e.what());
            } catch (...) {
                mCaptureFailed = true; ReportPipelineFailure("Unknown pipeline failure; use Capture recovery in F8");
            }
        }
    }
