// modules/inference/detector.cpp
#include "modules/inference/detector.hpp"
#include "modules/core/utilities.hpp"
#include "modules/core/clock.hpp"
#include <cstring>

Detector::~Detector()
{ Stop(); if (mActivityEvent) CloseHandle(mActivityEvent); }

HANDLE Detector::ActivityEvent() const
{ return mActivityEvent; }

void Detector::NotifyActivity()
{ if (mActivityEvent) SetEvent(mActivityEvent); }

void Detector::WaitForActivity(DWORD timeoutMs)
{
        if (mActivityEvent) WaitForSingleObject(mActivityEvent, timeoutMs);
        else Sleep(timeoutMs);
    }

void Detector::Start(const std::wstring& modelPath)
{
        Stop();
        mModelPath = modelPath;
        const auto modelSlash = modelPath.find_last_of(L"\\/");
        const std::wstring modelFile = modelSlash == std::wstring::npos ? modelPath : modelPath.substr(modelSlash + 1);
        DevLog("MODEL", ("Loading: " + WideToUtf8(modelFile)).c_str());
        mInput.resize(kTensorFloatsMax);
        mQuit.store(false, std::memory_order_release);
        mReady.store(false, std::memory_order_release);
        mHasJob.store(false, std::memory_order_release);
        mSharedReady.store(false, std::memory_order_release);
        mSharedSlotCount.store(0, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(mMx); mSharedJob = false; mJobSlot = 0; }
        mBackend.store("loading"); mInferMs.store(0.0);
        // A new model may use different output semantics even when its tensor
        // dimensions match the previous model. Force layout detection again.
        mOutD1 = 0;
        mOutD2 = 0;
        mLayout = {};
        mLayoutName.store("?");
        gOutCh.store(0, std::memory_order_relaxed);
        gOutAnchors.store(0, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(mResMx); mError.clear(); mResults.clear(); mResultTiming = {}; mLatency = {}; mBackendDetails.clear(); mStageTiming={}; ++mResultVersion; }
        mContract = {};
        const std::wstring sidecar = modelPath + L".albion-model";
        if (GetFileAttributesW(sidecar.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string text, error;
            if (!ReadTextFileUtf8(sidecar, text) ||
                !ModelOutputContract::ParseText(text, mContract, error)) {
                SetError(error.empty() ? "Cannot read model contract" : error);
                mBackend.store("contract error"); return;
            }
        }
        mExited.store(false, std::memory_order_release);
        try { mWorker = std::thread([this] { WorkerEntry(); }); }
        catch (const std::exception& e) { WorkerFailed(e.what()); mExited = true; }
    }

const std::wstring& Detector::ModelPath() const
{ return mModelPath; }

bool Detector::Loading() const
{ return !mExited.load(std::memory_order_acquire) && !Ready(); }

void Detector::WorkerFailed(const char* error) noexcept
{
        mReady.store(false, std::memory_order_release);
        try { SetError(error ? error : "Unknown detector failure"); } catch (...) {}
        mBackend.store("failed"); NotifyActivity();
    }

void Detector::WorkerEntry() noexcept
{
        albion::GuardWorker([this] {
            try { Loop(); }
            catch (const winrt::hresult_error& e) { throw std::runtime_error(winrt::to_string(e.message())); }
        }, [this](const char* e) { WorkerFailed(e); }, [this] {
            mReady.store(false, std::memory_order_release);
            mExited.store(true, std::memory_order_release); NotifyActivity();
        });
    }

void Detector::Stop()
{
        mReady.store(false, std::memory_order_release);
        // Condition-variable predicates must change while holding mMx.
        // Otherwise Stop/Push can notify between predicate evaluation and wait.
        { std::lock_guard<std::mutex> lk(mMx); mQuit.store(true, std::memory_order_release); }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
        mExited.store(true, std::memory_order_release);
        mHasJob.store(false, std::memory_order_release);
    }

bool Detector::Ready()      const
{ return mReady.load(std::memory_order_acquire); }

bool Detector::WantsFrame() const
{ return Ready() && !mHasJob.load(std::memory_order_acquire); }

const char* Detector::Backend()  const
{ return mBackend.load(); }

const char* Detector::LayoutName() const
{ return mLayoutName.load(); }

double Detector::InferenceMs()   const
{ return mInferMs.load(); }

std::uint64_t Detector::CompletedCount() const
{ return mCompletedCount.load(std::memory_order_relaxed); }

LatencyWindow Detector::RecentLatency()
{
        std::lock_guard<std::mutex> lk(mResMx); return mLatency;
    }

std::string Detector::Error()
{ std::lock_guard<std::mutex> lk(mResMx); return mError; }

std::string Detector::BackendDetails()
{ std::lock_guard<std::mutex> lk(mResMx); return mBackendDetails; }

void Detector::Push(std::vector<float>& tensor, LetterboxInfo lb)
{
        {
            std::lock_guard<std::mutex> lk(mMx);
            mInput.swap(tensor); mLb = lb;
            mHasJob.store(true, std::memory_order_release);
        }
        mCv.notify_one();
    }

void Detector::EnableSharedInput(ID3D11Device* dev, ID3D11Buffer* first, ID3D11Buffer* second)
{
        mSharedDev = dev; mSharedBuf = { first, second };
    }

int Detector::SharedSlotCount() const
{ return mSharedSlotCount.load(std::memory_order_acquire); }

bool Detector::SharedInput() const
{ return mSharedReady.load(std::memory_order_acquire); }

void Detector::PushShared(LetterboxInfo lb, size_t slot)
{
        {
            std::lock_guard<std::mutex> lk(mMx);
            mLb = lb; mSharedJob = true; mJobSlot = slot;
            mHasJob.store(true, std::memory_order_release);
        }
        mCv.notify_one();
    }

bool Detector::CopyLatestIfChanged(std::uint64_t& version, std::vector<Detection>& out, DetectionTiming& timing, BackendStageTiming& stages)
{
        std::lock_guard<std::mutex> lk(mResMx);
        if (version == mResultVersion) return false;
        out = mResults;
        timing = mResultTiming; stages=mStageTiming;
        version = mResultVersion;
        return true;
    }

void Detector::SetError(std::string e)
{ DevLog("BACKEND ERROR", e.c_str()); std::lock_guard<std::mutex> lk(mResMx); mError = std::move(e); }

float Detector::IoU(const Detection& a, const Detection& b)
{
        float x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
        float x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
        float inter = std::max(0.f, x2 - x1) * std::max(0.f, y2 - y1);
        float uni = a.w * a.h + b.w * b.h - inter;
        return uni > 0.f ? inter / uni : 0.f;
    }

float Detector::OutAt(const float* d, const OutLayout& L, int c, int i)
{
        return L.transposed ? d[size_t(i) * size_t(L.ch) + size_t(c)]
            : d[size_t(c) * size_t(L.anchors) + size_t(i)];
    }

Detector::OutLayout Detector::DetectLayout(const float* d, int d1, int d2, int net)
{
        OutLayout L;
        L.transposed = (d1 > d2);
        L.ch = L.transposed ? d2 : d1;
        L.anchors = L.transposed ? d1 : d2;
        if (L.ch <= 0 || L.anchors <= 0) return L;

        const int sample = std::min(L.anchors, 256);

        // Vykhod posle vstroennogo NMS: 6 kolonok, klass - celoe, x2 > x1.
        if (L.ch == 6 && L.transposed && sample > 0)
        {
            int intCls = 0, ordered = 0;
            for (int i = 0; i < sample; ++i)
            {
                const float cls = OutAt(d, L, 5, i);
                if (std::fabs(cls - std::round(cls)) < 1e-3f && cls >= 0.f && cls < 1000.f) ++intCls;
                if (OutAt(d, L, 2, i) > OutAt(d, L, 0, i)) ++ordered;
            }
            if (intCls == sample && ordered > sample / 2) L.nms = true;
        }

        if (!L.nms)
        {
            // Chislo yakorey odnoznachno delit v8 i v5: u v5 ikh rovno vtroe bolshe.
            const int a8 = (net / 8) * (net / 8) + (net / 16) * (net / 16) + (net / 32) * (net / 32);
            if (a8 > 0 && L.anchors >= a8 * 2) L.objectness = true;
            else if (L.ch == 85 || L.ch - 5 == kNumClasses) L.objectness = true;

            float maxCoord = 0.f;
            for (int i = 0; i < sample; ++i)
                for (int c = 0; c < 4; ++c)
                    maxCoord = std::max(maxCoord, std::fabs(OutAt(d, L, c, i)));
            L.normalized = (maxCoord > 0.f && maxCoord <= 1.5f);
        }
        return L;
    }

Detector::OutLayout Detector::ContractLayout(const float* data,int d1,int d2,int net,
        const ModelOutputContract& contract)
{
        if(contract.format==ModelOutputContract::Format::Auto) return DetectLayout(data,d1,d2,net);
        OutLayout L;
        L.transposed=contract.anchorsFirst;
        L.ch=L.transposed ? d2 : d1;L.anchors=L.transposed ? d1 : d2;
        L.nms=contract.format==ModelOutputContract::Format::NmsXYXY;
        L.objectness=contract.format==ModelOutputContract::Format::Yolo5;
        L.normalized=contract.normalized;
        if(L.anchors<=0 || (L.nms ? L.ch!=6 : L.ch<(L.objectness ? 6 : 5))) return {};
        return L;
    }

void Detector::SuppressReference(const std::vector<Detection>& cand,
        std::vector<Detection>& keep, std::vector<char>& dead)
{
        dead.assign(cand.size(), 0);
        for (size_t i = 0; i < cand.size() && keep.size() < AlbionVariant::Get().maxDetections; ++i) {
            if (dead[i]) continue;
            keep.push_back(cand[i]);
            for (size_t j = i + 1; j < cand.size(); ++j)
                if (!dead[j] && cand[j].cls == cand[i].cls && IoU(cand[i], cand[j]) > AlbionVariant::Get().nmsIou)
                    dead[j] = 1;
        }
    }

void Detector::SuppressSorted(const std::vector<Detection>& cand,
        std::vector<Detection>& keep, std::vector<char>& dead)
{
#if defined(ALBION_REFERENCE_NMS)
        // A/B build: same diagnostics/tracking, original NMS only.
        SuppressReference(cand, keep, dead); return;
#endif
        const size_t n = cand.size();
        // Dense clusters suppress almost everything in the first few iterations:
        // precomputing all bounds costs more than it saves. Mixed classes also
        // favor the original short-circuit loop. This selects an algorithm only;
        // it never changes the score order, limit, or suppression criterion.
        bool reference = n > 300; // preserve safety if a future caller changes the cap
        if (n > 32 && !reference) {
            for (size_t i = 1; i < n; ++i)
                if (cand[i].cls != cand[0].cls) { reference = true; break; }
            if (!reference) {
                unsigned overlaps = 0;
                for (size_t i = 1; i < std::min<size_t>(n, 9); ++i)
                    overlaps += unsigned(IoU(cand[0], cand[i]) > AlbionVariant::Get().nmsIou);
                reference = overlaps >= 2;
            }
        }
        if (reference) { SuppressReference(cand, keep, dead); return; }
        std::array<float, 300> right, bottom, area;
        dead.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            right[i] = cand[i].x + cand[i].w;
            bottom[i] = cand[i].y + cand[i].h;
            area[i] = cand[i].w * cand[i].h;
        }
        for (size_t i = 0; i < n && keep.size() < AlbionVariant::Get().maxDetections; ++i) {
            if (dead[i]) continue;
            keep.push_back(cand[i]);
            for (size_t j = i + 1; j < n; ++j) {
                if (dead[j] || cand[j].cls != cand[i].cls) continue;
                const float iw = std::min(right[i], right[j]) - std::max(cand[i].x, cand[j].x);
                const float ih = std::min(bottom[i], bottom[j]) - std::max(cand[i].y, cand[j].y);
                const float inter = std::max(0.0f, iw) * std::max(0.0f, ih);
                const float uni = area[i] + area[j] - inter;
                if (uni > 0.0f && inter / uni > AlbionVariant::Get().nmsIou) dead[j] = 1;
            }
        }
    }

void Detector::Decode(const float* d, int d1, int d2, LetterboxInfo lb)
{
        // Bufery kandidatov pereispolzuyutsya mezhdu kadrami: ranshe kazhdyy
        // vyzov delal po tri kuchnye allokacii.
        std::vector<Detection>& cand = mCand;
        cand.clear();
        mDecodeOutput.clear();
        // Invalid metadata must not poison cached layout or publish NaN coordinates.
        if (!d || d1 <= 0 || d2 <= 0 || lb.net <= 0 ||
            !std::isfinite(lb.ratio) || lb.ratio <= 0.f ||
            !std::isfinite(lb.padX) || !std::isfinite(lb.padY) ||
            !std::isfinite(lb.offsetX) || !std::isfinite(lb.offsetY)) return;
        if (cand.capacity() < 512) cand.reserve(512);
        const float confThr = gDecodeConf.load(std::memory_order_relaxed);

        if (d1 != mOutD1 || d2 != mOutD2)
        {
            mOutD1 = d1; mOutD2 = d2;
            mLayout = ContractLayout(d, d1, d2, lb.net, mContract);
            mLayoutName.store(mLayout.nms ? "NMS [N,6]"
                : (mLayout.objectness ? (mLayout.transposed ? "v5 [A,C]" : "v5 [C,A]")
                    : (mLayout.transposed ? "v8 [A,C]" : "v8 [C,A]")));
            gOutCh.store(mLayout.ch, std::memory_order_relaxed);
            gOutAnchors.store(mLayout.anchors, std::memory_order_relaxed);
        }

        const OutLayout L = mLayout;
        mDecodeOutput.clear();
        if (L.ch <= 0 || L.anchors <= 0) return;

        // Shagi po pamyati schitaem odin raz - ubiraem vetvlenie na kazhdom chtenii.
        const size_t chStride = L.transposed ? size_t(1) : size_t(L.anchors);
        const size_t anStride = L.transposed ? size_t(L.ch) : size_t(1);
        auto At = [d, chStride, anStride](int c, int i) noexcept -> float {
            return d[size_t(c) * chStride + size_t(i) * anStride];
            };

        const float scale = L.normalized ? float(lb.net) : 1.f;
        const float maxSide = (lb.ratio > 0.f ? float(lb.net) / lb.ratio : float(lb.net)) * 1.3f;
        const int first = L.objectness ? 5 : 4;
        const int nc = std::min(L.ch - first, kNumClasses);

        for (int i = 0; i < L.anchors; ++i)
        {
            float bs = 0.f, x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            int best = 0;

            if (L.nms)
            {
                bs = At(4, i);
                if (bs < confThr) continue;
                const float cls = At(5, i);
                if (!std::isfinite(cls) || cls < 0.f || cls >= float(kNumClasses) ||
                    std::fabs(cls - std::round(cls)) > 0.001f) continue;
                best = int(cls);
                if (kDecodeClass >= 0 && best != kDecodeClass) continue;
                const float x1 = At(0, i) * scale, y1 = At(1, i) * scale;
                const float x2 = At(2, i) * scale, y2 = At(3, i) * scale;
                x = x1; y = y1; w = x2 - x1; h = y2 - y1;
            }
            else
            {
                if (nc <= 0) break;
                const float obj = L.objectness ? At(4, i) : 1.f;
                if (obj < confThr) continue;
                if (kDecodeClass >= 0 && kDecodeClass < nc)
                {
                    best = kDecodeClass;
                    bs = obj * At(first + kDecodeClass, i);
                }
                else
                {
                    for (int c = 0; c < nc; ++c)
                    {
                        const float s = At(first + c, i);
                        if (s > bs) { bs = s; best = c; }
                    }
                    bs *= obj;
                }
                if (bs < confThr) continue;
                const float cx = At(0, i) * scale, cy = At(1, i) * scale;
                w = At(2, i) * scale;
                h = At(3, i) * scale;
                x = cx - w * .5f; y = cy - h * .5f;
            }

            // Reject malformed model output before it reaches sorting, NMS,
            // drawing, or mouse movement. Comparisons alone do not reject NaN.
            if (!std::isfinite(bs) || !std::isfinite(x) || !std::isfinite(y) ||
                !std::isfinite(w) || !std::isfinite(h)) continue;

            // Strakhovka: uverennost vne 0..1 znachit, chto razmetka vykhoda ne ta.
            if (bs < confThr || bs > 1.001f) continue;
            if (w <= 1.f || h <= 1.f) continue;

            Detection det{ (x - lb.padX) / lb.ratio + lb.offsetX,
                           (y - lb.padY) / lb.ratio + lb.offsetY,
                           w / lb.ratio, h / lb.ratio, bs, best };
            if (!std::isfinite(det.x) || !std::isfinite(det.y) ||
                !std::isfinite(det.w) || !std::isfinite(det.h) ||
                det.w > maxSide || det.h > maxSide) continue;
            cand.push_back(det);

        }
        const auto byScore = [](const Detection& a, const Detection& b) {
            return a.score > b.score;
            };
        if (cand.size() > 300)
        {
            std::nth_element(cand.begin(), cand.begin() + 300, cand.end(), byScore);
            cand.resize(300);
        }
        std::sort(cand.begin(), cand.end(), byScore);
        auto& keep = mDecodeOutput;
        if (keep.capacity() < 100) keep.reserve(100);
        if ((kModelOutputHasVerifiedNms || mContract.verifiedNms) && L.nms)
        {
            keep.insert(keep.end(), cand.begin(), cand.begin() + std::min<size_t>(cand.size(), AlbionVariant::Get().maxDetections));
            return;
        }
        SuppressSorted(cand, keep, mDead);
    }

void Detector::PublishDetections(const LetterboxInfo& lb, double inferenceMs, double decodeMs,
        BackendStageTiming stages,double workerStarted)
{
        if(mLayout.ch<=0 || mLayout.anchors<=0) {
            SetError("Output shape does not match the model contract");
            mReady.store(false,std::memory_order_release);NotifyActivity();return;
        }
        DetectionTiming timing{ lb.frameId, lb.capturedMs, lb.submittedMs,
            PerfNowMs(), inferenceMs, decodeMs,workerStarted,lb.presentedMs };
        timing.acquiredMs=lb.acquiredMs; timing.accumulatedFrames=lb.accumulatedFrames;
        timing.roiX=lb.offsetX; timing.roiY=lb.offsetY; timing.roiWidth=lb.roiWidth; timing.roiHeight=lb.roiHeight;
        timing.letterboxRatio=lb.ratio; timing.padX=lb.padX; timing.padY=lb.padY; timing.netSide=lb.net;
        albion::performance::telemetry.Result(inferenceMs,
            timing.capturedMs>0 ? (std::max)(0.0,timing.completedMs-timing.capturedMs) : -1,
            stages.computeGpuMs,decodeMs,
            workerStarted>0 && lb.submittedMs>0 ? (std::max)(0.0,workerStarted-lb.submittedMs) : -1,
            double(mDecodeOutput.size()),timing.frameId,timing.presentedMs,timing.acquiredMs,timing.submittedMs,timing.workerStartedMs,timing.completedMs,timing.accumulatedFrames);
        mInferMs.store(inferenceMs, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mResMx);
        mResults.swap(mDecodeOutput); // recycle the previous result's storage
        mResultTiming = timing; mStageTiming=stages;
        if (timing.capturedMs > 0.0) mLatency.Add(timing.completedMs - timing.capturedMs);
        mCompletedCount.fetch_add(1, std::memory_order_relaxed);
        ++mResultVersion;
    }

bool Detector::WaitForJob(std::vector<float>& local, LetterboxInfo& lb, bool& shared, size_t& slot)
{
        std::unique_lock<std::mutex> lk(mMx);
        mCv.wait(lk, [this] {return mQuit.load() || mHasJob.load(); });
        if (mQuit) return false;
        lb = mLb;
        shared = mSharedJob;
        slot = mJobSlot;
        mSharedJob = false;
        if (!shared) local.swap(mInput);
        return true;
    }

void Detector::LoopGeneric(IInferenceBackend& backend)
{
        const bool borrowOutput = !AimPerfEnvFlag(L"ALBION_COPY_OUTPUT");
        mReady.store(true, std::memory_order_release);
        std::vector<float> local(kTensorFloatsMax), output;
        while (true)
        {
            LetterboxInfo lb{}; bool shared = false; size_t slot = 0;
            if (!WaitForJob(local, lb, shared, slot)) return;
            if (lb.net != mNetSize)
            {
                mHasJob.store(false, std::memory_order_release); NotifyActivity(); continue;
            }
            const double start = PerfNowMs();
            int ch = 0, anchors = 0; std::string err;
            InferenceOutputView view{};
            bool ok = false;
            if (borrowOutput) {
                ok = shared
                    ? backend.RunSharedSlotView(slot, output, view, ch, anchors, err)
                    : backend.RunView(local.data(), output, view, ch, anchors, err);
            } else {
                ok = shared
                    ? backend.RunSharedSlot(slot, output, ch, anchors, err)
                    : backend.Run(local.data(), output, ch, anchors, err);
                if (ok) view = { output.data(), output.size() };
            }
            if (ok && (!view.data || ch <= 0 || anchors <= 0 ||
                size_t(ch) > (std::numeric_limits<size_t>::max)() / size_t(anchors) ||
                view.count != size_t(ch) * size_t(anchors))) {
                err = "Backend output storage does not match its shape";
                ok = false;
            }
            const double inferenceMs = PerfNowMs() - start;
            if (ok)
            {
                const double decodeStart = PerfNowMs();
                Decode(view.data, ch, anchors, lb);
                if(mLayout.ch<=0 || mLayout.anchors<=0) {
                    SetError("Output shape does not match the explicit model contract");
                    mReady.store(false,std::memory_order_release);
                } else PublishDetections(lb, inferenceMs, PerfNowMs() - decodeStart,backend.LastStageTiming(),start);
            }
            else
            {
                SetError(err);
                mReady.store(false,std::memory_order_release);
                NotifyActivity();
                if (shared)
                {
                    // Never recycle a slot after a failed interop operation.
                    // Keep resources registered until Stop(), with the producer
                    // stopped/excluded, rather than unregistering underneath it.
                    mReady.store(false, std::memory_order_release);
                    NotifyActivity();
                    std::unique_lock<std::mutex> lk(mMx);
                    mCv.wait(lk, [this] { return mQuit.load(); });
                    return;
                }
            }
            // RunSharedSlot success guarantees the slot is no longer read.
            mHasJob.store(false, std::memory_order_release);
            NotifyActivity();
        }
    }

int Detector::ReadNetSize(const WinML::LearningModel& model)
{
        try
        {
            auto desc = model.InputFeatures().GetAt(0).try_as<WinML::TensorFeatureDescriptor>();
            if (desc)
            {
                auto shape = desc.Shape();
                if (shape.Size() == 4)
                {
                    const int h = int(shape.GetAt(2));
                    const int w = int(shape.GetAt(3));
                    const int n = std::max(h, w);
                    if (n >= 64 && n <= kNetMax) return n;
                }
            }
        }
        catch (...) {}
        return kNetMax;
    }

int Detector::ProbeNetSize(const std::wstring& path)
{
        if (AimIsEngineFile(path)) return kNetMax;   // backend resolves actual shape
#ifdef AIM_ENABLE_ORT
        return kNetMax; // ORT reads model metadata; never probe modern ONNX through old WinML.
#endif
        try { return ReadNetSize(WinML::LearningModel::LoadFromFilePath(path)); }
        catch (...) { return kNetMax; }
    }

void Detector::Loop()
{
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct ApartmentLease { ~ApartmentLease() { winrt::uninit_apartment(); } } apartment;
        std::string err;
        int netSize = ProbeNetSize(mModelPath);
        auto backend = CreateInferenceBackend(mModelPath, netSize, err);
        if (backend)
        {
            const int resolvedSide = backend->InputSide();
            if (resolvedSide > 0) netSize = resolvedSide;
            if (netSize < 64 || netSize > kNetMax)
            {
                SetError("Backend input size exceeds preprocessor limits"); return;
            }
            mNetSize = netSize;
            gNetSize.store(mNetSize, std::memory_order_relaxed);
            // Zero-copy: tenzor beryotsya pryamo iz buffera D3D11 preprocessora.
            // Esli backend ne umeet - prosto ostayotsya obychnyy put cherez CPU.
            std::string inputPath = "CPU-transfer fallback: backend has no shared-input support";
            if (mSharedDev && mSharedBuf[0] && backend->SupportsSharedInput() && !AimPerfEnvFlag(L"ALBION_DISABLE_INTEROP"))
            {
                std::string bindErr, twoSlotError;
                void* buffers[] = { mSharedBuf[0], mSharedBuf[1] };
                int count = 0;
                if (buffers[1] && backend->BindSharedInputs(mSharedDev, buffers, 2, bindErr)) count = 2;
                else {
                    twoSlotError = bindErr;
                    bindErr.clear();
                    if (backend->BindSharedInput(mSharedDev, buffers[0], bindErr)) count = 1;
                }
                mSharedSlotCount.store(count, std::memory_order_release);
                mSharedReady.store(count > 0, std::memory_order_release);
                if (count > 0) inputPath = "GPU-input slots=" + std::to_string(count) +
                    "; GPU D2D copy retained (not zero-copy)";
                else inputPath = "CPU-transfer fallback: " + bindErr;
                if (!twoSlotError.empty()) inputPath += "; two-slot attempt: " + twoSlotError;
            }
            else if (backend->SupportsSharedInput())
                inputPath = AimPerfEnvFlag(L"ALBION_DISABLE_INTEROP") ? "CPU-transfer fallback: disabled by environment" : "CPU-transfer fallback: capture input buffers unavailable";
#ifndef AIM_ENABLE_D3D_INTEROP
            inputPath += "; AIM_ENABLE_D3D_INTEROP not compiled";
#endif
            const std::string details = backend->Diagnostics() + "; " + inputPath +
                "; decoder output=" + (AimPerfEnvFlag(L"ALBION_COPY_OUTPUT") ? "owning copy" : "borrowed if supported") +
                "; output contract: " + mContract.Describe();
            { std::lock_guard<std::mutex> lk(mResMx); mBackendDetails = details; }
            OutputDebugStringA(("[Albion stage1] " + details + "\n").c_str());
            DevLog("BACKEND", details.c_str());
            mBackend.store(backend->Name());
            LoopGeneric(*backend);
            mSharedReady.store(false, std::memory_order_release);
            backend->UnbindSharedInput();
            return;
        }
        if (!err.empty()) SetError(err);
        if (AimIsEngineFile(mModelPath))
        {
            if (err.empty()) SetError(".engine requires TensorRT backend"); return;
        }
#ifdef AIM_ENABLE_ORT
        // Preserve the useful ORT error instead of overwriting it with WinML.
        if (err.empty()) SetError("ONNX Runtime initialization failed");
        return;
#else
        LoopWinMl();
#endif
    }

void Detector::RecordWinMlError(const char* stage, const winrt::hresult_error& e)
{
        char code[24]{};
        std::snprintf(code, sizeof(code), "0x%08X",
            static_cast<unsigned>(static_cast<int32_t>(e.code())));
        const std::string detail = std::string("WinML ") + code + " at " + stage +
            ": " + winrt::to_string(e.message());
        SetError(detail);
        // Also visible under Debug > Windows > Output in Visual Studio.
        const std::string record = detail + "\r\nModel: " + AimNarrow(mModelPath) +
            "\r\nBackend: " + Backend() + "\r\nInput shape: [1,3," +
            std::to_string(mNetSize) + "," + std::to_string(mNetSize) + "]\r\n---\r\n";
        OutputDebugStringA(record.c_str());
        AppendTextFileUtf8(AlbionDataPath(L"winml_errors.txt"), record);
    }

void Detector::LoopWinMl()
{
        const char* stage = "LearningModelSession initialization";
        WinML::LearningModelSession session{ nullptr };
        winrt::hstring inName, outName;
        try
        {
            stage = "LearningModel::LoadFromFilePath";
            auto model = WinML::LearningModel::LoadFromFilePath(mModelPath);
            stage = "Read model input/output descriptors";
            inName = model.InputFeatures().GetAt(0).Name();
            outName = model.OutputFeatures().GetAt(0).Name();
            // Vkhod beretsya iz samoy modeli - inache Bind brosaet hresult_error.
            mNetSize = ReadNetSize(model);
            gNetSize.store(mNetSize, std::memory_order_relaxed);
            try
            {
                stage = "Create WinML GPU session";
                session = WinML::LearningModelSession(model,
                    WinML::LearningModelDevice(WinML::LearningModelDeviceKind::DirectXHighPerformance));
                mBackend.store("WinML/GPU");
            }
            catch (const winrt::hresult_error& gpuError) {
                RecordWinMlError("Create WinML GPU session", gpuError);
                stage = "Create WinML CPU session";
                session = WinML::LearningModelSession(model,
                    WinML::LearningModelDevice(WinML::LearningModelDeviceKind::Cpu));
                mBackend.store("WinML/CPU");
            }
        }
        catch (winrt::hresult_error const& e) { RecordWinMlError(stage, e); return; }

        {
            std::lock_guard<std::mutex> lock(mResMx);
            mBackendDetails=std::string(mBackend.load())+"; original ONNX; fresh input/reused binding; CPU-visible output";
        }
        try
        {
            stage = "Initialize WinML frame buffers";
            const std::vector<int64_t> shape{ 1,3,mNetSize,mNetSize };
            WinML::LearningModelBinding bind(session);
            // Reuse binding, not input tensors: WinML may cache uploaded tensor contents.
            // Changing a mapped tensor in place failed the changed-input parity check.
            std::vector<float> local(kTensorFloatsMax);
            mReady.store(true, std::memory_order_release);
            while (true)
            {
                LetterboxInfo lb{}; bool shared = false; size_t slot = 0;
                if (!WaitForJob(local, lb, shared, slot)) return;
                if (shared || lb.net != mNetSize)
                {
                    mHasJob.store(false, std::memory_order_release); NotifyActivity(); continue;
                }
                try
                {
                    const double inferenceStart = PerfNowMs();
                    stage = "Create and bind fresh WinML input";
                    auto input=WinML::TensorFloat::CreateFromArray(shape,
                        winrt::array_view<const float>(local.data(),local.data()+TensorFloatsFor(mNetSize)));
                    bind.Clear();
                    bind.Bind(inName,input);
                    stage = "LearningModelSession::Evaluate";
                    auto res = session.Evaluate(bind, L"");
                    stage = "Read WinML output tensor";
                    auto out = res.Outputs().Lookup(outName).as<WinML::TensorFloat>();
                    auto os = out.Shape();
                    // Materialization/readback belongs to inference, not NMS.
                    stage = "Access WinML output buffer";
                    auto ref = out.CreateReference();
                    auto acc = ref.as<IMemoryBufferByteAccess>();
                    uint8_t* raw = nullptr; uint32_t cap = 0;
                    winrt::check_hresult(acc->GetBuffer(&raw, &cap));
                    const double inferenceMs = PerfNowMs() - inferenceStart;
                    stage = "Decode WinML output";
                    const double decodeStart = PerfNowMs();
                    mDecodeOutput.clear();
                    if (os.Size() == 3)
                    {
                        const int64_t d1 = os.GetAt(1), d2 = os.GetAt(2);
                        if (!raw || d1 <= 0 || d2 <= 0 ||
                            d1 > INT_MAX || d2 > INT_MAX ||
                            uint64_t(d1) * uint64_t(d2) > uint64_t(cap) / sizeof(float))
                            throw std::runtime_error("Invalid WinML output shape or buffer");
                        Decode(reinterpret_cast<const float*>(raw), int(d1), int(d2), lb);
                    }
                    PublishDetections(lb, inferenceMs, PerfNowMs() - decodeStart);
                }
                catch (winrt::hresult_error const& e)
                {
                    RecordWinMlError(stage, e);
                    // Do not repeat the same Bind exception on every frame or
                    // hide it behind a ready=true UI state.
                    mReady.store(false, std::memory_order_release);
                    mHasJob.store(false, std::memory_order_release);
                    NotifyActivity(); return;
                }
                catch (std::exception const& e)
                {
                    SetError(std::string(stage) + ": " + e.what());
                    mReady.store(false, std::memory_order_release);
                    mHasJob.store(false, std::memory_order_release);
                    NotifyActivity(); return;
                }
                mHasJob.store(false, std::memory_order_release);
                NotifyActivity();
            }
        }
        catch (winrt::hresult_error const& e) { RecordWinMlError(stage, e); }
        catch (std::exception const& e) { SetError(e.what()); }
        mReady.store(false, std::memory_order_release);
        NotifyActivity();
    }
