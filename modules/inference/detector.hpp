// modules/inference/detector.hpp
#pragma once
#include "modules/core/platform.hpp"
#include "modules/core/runtime.hpp"
#include "modules/core/detection.hpp"
#include "modules/core/timing.hpp"
#include "gpu_preprocessor.hpp"
#include "gpu_infer.hpp"
#include "model_contract.hpp"
class Detector
{
public:
    ~Detector();
    HANDLE ActivityEvent() const;
    void NotifyActivity();
    void WaitForActivity(DWORD timeoutMs);

    void Start(const std::wstring& modelPath);

    const std::wstring& ModelPath() const; // pipeline-owner thread only
    bool Loading() const;
    void WorkerFailed(const char* error) noexcept;
    void WorkerEntry() noexcept;
    void Stop();
    bool Ready()      const;
    bool WantsFrame() const;
    const char* Backend()  const;
    const char* LayoutName() const;
    double InferenceMs()   const;
    std::uint64_t CompletedCount() const;
    LatencyWindow RecentLatency();
    std::string Error();
    std::string BackendDetails();

    // Bez kopirovaniya: vyzyvayushchiy otdayot svoy bufer i zabiraet nash.
    // Ranshe kazhdyy kadr shyol cherez memcpy na 1.2-4.6 MB.
    void Push(std::vector<float>& tensor, LetterboxInfo lb);

    // Zero-copy vkhod: backend chitaet tenzor pryamo iz buffera D3D11.
    // Vyzyvat do Start(); privyazka proiskhodit uzhe v rabochem potoke.
    void EnableSharedInput(ID3D11Device* dev, ID3D11Buffer* first, ID3D11Buffer* second);
    int SharedSlotCount() const;

    bool SharedInput() const;

    // Dannye ne peredayutsya - oni uzhe lezhat v obshchem bufere na GPU.
    void PushShared(LetterboxInfo lb, size_t slot);

    bool CopyLatestIfChanged(std::uint64_t& version, std::vector<Detection>& out, DetectionTiming& timing, BackendStageTiming& stages);

private:
    void SetError(std::string e);

    static float IoU(const Detection& a, const Detection& b);


    // Raznye eksporty ONNX raskladyvayut vykhod po-raznomu. Ranshe my vsegda
    // chitali ego kak YOLOv8 [1, 4+nc, anchors]; na drugom poryadke v "score"
    // popadali pikselnye koordinaty (89.98, 320.01) i ekran zasypalo ramkami.
    struct OutLayout
    {
        int  ch = 0;
        int  anchors = 0;
        bool transposed = false;   // [1, anchors, ch] vmesto [1, ch, anchors]
        bool objectness = false;   // YOLOv5: kanal obj, klassy nachinayutsya s 5
        bool normalized = false;   // koordinaty 0..1 vmesto pikseley
        bool nms = false;          // gotovyy vykhod [N, 6]: x1,y1,x2,y2,score,cls
    };

    static float OutAt(const float* d, const OutLayout& L, int c, int i);

    static OutLayout DetectLayout(const float* d, int d1, int d2, int net);

    static OutLayout ContractLayout(const float* data,int d1,int d2,int net,
        const ModelOutputContract& contract);

    static void SuppressReference(const std::vector<Detection>& cand,
        std::vector<Detection>& keep, std::vector<char>& dead);

    // Input is already score-sorted and capped at 300, as before.
    // Keep the original IoU division/strict threshold to preserve edge cases.
    static void SuppressSorted(const std::vector<Detection>& cand,
        std::vector<Detection>& keep, std::vector<char>& dead);

    void Decode(const float* d, int d1, int d2, LetterboxInfo lb);

    void PublishDetections(const LetterboxInfo& lb, double inferenceMs, double decodeMs,
        BackendStageTiming stages={},double workerStarted=0);

    bool WaitForJob(std::vector<float>& local, LetterboxInfo& lb, bool& shared, size_t& slot);

    void LoopGeneric(IInferenceBackend& backend);

    // Razmer vkhoda iz metadannykh modeli. 0 ili dinamicheskaya os -> kNetMax.
    static int ReadNetSize(const WinML::LearningModel& model);

    static int ProbeNetSize(const std::wstring& path);

    void Loop();

    void RecordWinMlError(const char* stage, const winrt::hresult_error& e);

    void LoopWinMl();

    std::wstring            mModelPath;
    std::vector<float>      mInput;
    LetterboxInfo           mLb{};
    std::vector<Detection>  mResults;
    std::vector<Detection>  mDecodeOutput;
    DetectionTiming mResultTiming{};
    LatencyWindow mLatency;
    std::atomic<std::uint64_t> mCompletedCount{ 0 };
    HANDLE mActivityEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::uint64_t           mResultVersion = 0;
    std::string             mError;
    BackendStageTiming      mStageTiming{};
    std::string             mBackendDetails;
    std::mutex              mMx, mResMx;
    std::condition_variable mCv;
    std::thread             mWorker;
    std::atomic<bool>       mHasJob{ false }, mQuit{ false }, mReady{ false }, mExited{ true };
    std::atomic<const char*>mBackend{ "none" };
    std::atomic<double>     mInferMs{ 0.0 };
    int                     mNetSize = kNetMax;
    ModelOutputContract mContract{};
    OutLayout               mLayout{};
    int                     mOutD1 = 0, mOutD2 = 0;
    std::vector<Detection>  mCand;   // pereispolzuemye bufery dekodera
    std::vector<char>       mDead;
    ID3D11Device* mSharedDev = nullptr;   // zero-copy vkhod
    std::array<ID3D11Buffer*, 2> mSharedBuf{};
    std::atomic<int> mSharedSlotCount{ 0 };
    size_t mJobSlot = 0;
    std::atomic<bool>       mSharedReady{ false };
    bool                    mSharedJob = false;
    std::atomic<const char*> mLayoutName{ "?" };
};
