// modules/app/app.hpp
#pragma once
#include "modules/core/platform.hpp"
#include "modules/core/runtime.hpp"
#include "modules/core/clock.hpp"
#include "modules/core/timing.hpp"
#include "modules/core/utilities.hpp"
#include "modules/ui/strings.hpp"
#include "modules/ui/theme.hpp"
#include "modules/services/discord.hpp"
#include "modules/services/gpu_metrics.hpp"
#include "modules/inference/detector.hpp"
#include "modules/pipeline/frame_mailbox.hpp"
#include "basic_winapi_aim.hpp"
#include "arduino_hid.hpp"
#ifdef AIM_ENABLE_WGC
#include "wgc_capture.hpp"
#endif
class App
{
    friend struct AlbionUiComponentCheck;
public:
    ~App();

    void Run(HINSTANCE hInst);

private:
    ULONG_PTR mGdiplusToken = 0;
    std::unique_ptr<Gdiplus::Image> mImgLogo;
    std::unique_ptr<Gdiplus::Image> mImgSettings;
    std::unique_ptr<Gdiplus::Image> mImgVisual;
    std::unique_ptr<Gdiplus::Image> mImgAi;
    std::unique_ptr<Gdiplus::Image> mImgConfigs;
    std::unique_ptr<Gdiplus::Image> mImgDelete;
    std::unique_ptr<Gdiplus::Image> mImgRename;
    std::unique_ptr<Gdiplus::Image> mImgTrigger;
    std::unique_ptr<Gdiplus::Image> mImgUpdate;
    std::unique_ptr<Gdiplus::Image> mImgConvert;

    ComPtr<ID3D11Device>            mDev;
    ComPtr<ID3D11DeviceContext>     mCtx;
    ComPtr<IDXGIOutput1>            mOutput1;
    ComPtr<IDXGIOutputDuplication>  mDupl;
    ComPtr<ID3D11Texture2D>         mFrameTex;
    ComPtr<ID3D11ShaderResourceView>mFrameSRV;
    // Pryamoe chtenie kadra duplikacii, bez promezhutochnoy kopii v mFrameTex.
    // Ne vse drayvery otdayut teksturu s flagom SHADER_RESOURCE, poetomu put
    // vklyuchaetsya sam i pri neudache navsegda perekhodit na kopiyu.
    DesktopSrvCache mDesktopSrvs;
    bool                            mDirectSrvSupported = true;
    bool                            mDirectCapture = false;

#ifdef AIM_ENABLE_WGC
    WgcCapture mWgc;
    bool mUseWgc=false,mWgcApartment=false;
#endif
    GpuPreprocessor mPre;
    Detector        mDet;
    SharedFrameMailbox mSharedFrames;

    std::vector<float>      mTensorCpu = std::vector<float>(kTensorFloatsMax);
    LetterboxInfo           mTensorLb{};
    bool                    mHasTensor = false;
    std::uint64_t           mCapturedFrameId = 0;
    double                  mCapturedFrameMs = 0.0;
    double mAcquiredFrameMs=0;
    std::uint32_t mAccumulatedFrames=0;
    DetectionTiming         mLatestTiming{};
    std::atomic<double>     mDecodeMs{ 0.0 };
    std::atomic<double>     mResultAgeMs{ 0.0 };
    double                  mSubmitTime = 0.0;
    double                  mPipelineMs = 0.0;
    std::atomic<double>     mCaptureMs{ 0.0 };
    std::atomic<double>     mPreprocessMs{ -1.0 };
    std::atomic<std::uint64_t> mDxgiTimeouts{ 0 };
    std::uint64_t           mLastLoggedDxgiTimeouts = 0;
    bool                    mF9WasDown = false;
    bool                    mF10WasDown = false;
    struct JitterRow {
        double observedMs=0, capturedMs=0, completedMs=0;
        DetectionTiming timing{};
        std::uint64_t dxgiTimeouts=0;
        int activationKey=0, targetClass=0;
        std::uint64_t frameId=0;
        int detectionCount=0; float centerX=0, centerY=0, scale=0, speed=0;
        bool hardMode=false, smoothTracking=false;
        AimTraceObservation trace{};
    };
    static constexpr size_t kJitterMaxRows = 8192;
    std::atomic<bool> mJitterRecording{false};
    std::mutex mJitterMx;
    std::vector<JitterRow> mJitterRows;
    std::uint64_t mJitterDropped=0;
    double mJitterStartedMs=0;
    double mAutoPerfIntervalMs=0,mLastAutoPerfMs=0;
    std::uint64_t mLastLoggedInferenceCount = 0;
    double mLastInferenceLogMs = PerfNowMs();
    std::string mCaptureDeviceDetails;
    double mCaptureUsedMb = -1, mCaptureBudgetMb = -1, mLastMemorySampleMs = 0;
    NvmlSampler             mGpuSampler;
    DiscordPresence         mDiscord;
    std::vector<Detection>  mDets;
    std::uint64_t           mLastDetVer = 0;
    bool                    mDetectEnabled = true;

    BasicAimConfig           mAimCfg{};
    BasicWinApiAim           mAim{ mAimCfg };
    CaptureRegion            mCaptureRegion{};
    bool                     mAimEnabled = true;
    std::optional<AimTarget> mAimTarget;

    std::atomic<UINT> mDeskW{0}, mDeskH{0};
    std::atomic<LONG> mOutputLeft{0}, mOutputTop{0};
    UINT mFrameW = 0, mFrameH = 0;
    bool mFrameNeedsFullCopy = true;
    bool mRunning = true;

    // --- Potok konveyera ---
    // V Aimmy ves tsikl (zakhvat -> preprocess -> inferens -> navedenie) zhivyot
    // v otdelnoy dolgoigrayushchey zadache s prioritetom vyshe obychnogo, a okno
    // tolko chitaet rezultaty. Delaem tak zhe: okno nastroek bolshe ne tormozit
    // konveyer, i chastota detekciy perestayot zaviset ot otrisovki interfeysa.
    std::thread mPipeThread;
    std::atomic<bool> mPipeRunning{ false };
    std::recursive_mutex mPipeMx;

    // Snimok dlya interfeysa. Okno chitaet tolko ego kopiyu i nikogda ne zhdyot
    // konveyer: imenno ozhidanie obshchey blokirovki podveshivalo nastroyki.
    struct UiSnapshot
    {
        std::vector<Detection>   dets;
        std::optional<AimTarget> target;
        CaptureRegion            region{};
        double capFps = 0.0;
        double inferMs = 0.0;
        double pipeMs = 0.0;
        float  confidence = 0.5f;
        bool   ready = false;
        std::uint64_t revision = 0;
        double captureUsedMb = -1, captureBudgetMb = -1;
        std::string captureDevice;
        std::string captureSource = "DXGI synchronous";
        const char* backend="?";
        const char* layout="?";
        double deliveryMs=0,submitToBackendMs=0,presentToAcquireMs=0;
        BackendStageTiming stageTiming{};
        std::string error;
        int devOutputMethod = 0, devFpsLimit = 0, devRadius = 0;
        float devSpeed = 0, devMouseScale = 0;
        bool devAimEnabled = false, devDetectEnabled = false;
    };
    std::mutex mUiMx;
    UiSnapshot mUiSnap;

    HWND mHwnd = nullptr;
    HWND mDiagnosticsButton = nullptr, mDiagnosticsLabel = nullptr;
    HWND mWinMinimize = nullptr, mWinMaximize = nullptr, mWinClose = nullptr;
    static constexpr int kTitleBarHeight = 74;
    HWND mSpeedLabel = nullptr, mSpeedSlider = nullptr, mSpeedValue = nullptr;
    HWND mCaptureLabel = nullptr, mCaptureSlider = nullptr, mCaptureValue = nullptr;
    HWND mBodyPartLabel = nullptr;
    HWND mHeadYLabel = nullptr, mHeadYSlider = nullptr, mHeadYValue = nullptr;
    HWND mHeadYReset = nullptr, mHeadYHint = nullptr;
    HWND mMouseScaleLabel = nullptr, mMouseScaleSlider = nullptr, mMouseScaleValue = nullptr;
    std::array<HWND, 4> mBodyPartButtons{};
    HWND mConfLabel = nullptr, mConfSlider = nullptr, mConfValue = nullptr;
    HWND mMethodLabel = nullptr, mMethodCombo = nullptr;
    HWND mFpsLabel = nullptr, mFpsCombo = nullptr;
    HWND mCapLabel = nullptr, mCapCombo = nullptr;
    HWND mPathLabel = nullptr, mPathCombo = nullptr;
    HWND mTabSettings = nullptr, mTabVisuals = nullptr, mTabAi = nullptr, mTabConfig = nullptr;
    std::array<HWND, 4> mHeaderTabs{};
    HWND mUiHovered = nullptr;
    std::array<float, 5> mSwitchAnim{ -1.f, -1.f, -1.f, -1.f, -1.f };
    std::array<float, 5> mSwitchFrom{};
    std::array<int, 5> mSwitchGoal{ -1, -1, -1, -1, -1 };
    std::array<ULONGLONG, 5> mSwitchStart{};
    // Tab change cross-fade: snapshot of the old page that fades out on top.
    HWND mTabFadeWnd = nullptr; HDC mTabFadeDc = nullptr; HBITMAP mTabFadeBmp = nullptr;
    HGDIOBJ mTabFadeOld = nullptr; RECT mTabFadeRc{}; ULONGLONG mTabFadeStart = 0;
    HWND mConfigTitle = nullptr, mConfigList = nullptr, mConfigNameLabel = nullptr, mConfigNameEdit = nullptr;
    HWND mSaveConfigBtn = nullptr, mLoadConfigBtn = nullptr, mDeleteConfigBtn = nullptr, mRenameConfigBtn = nullptr;
    HWND mConfigStatusLabel = nullptr;
    HWND mConfigNewBtn = nullptr, mConfigConfirmBtn = nullptr, mConfigCancelBtn = nullptr;
    struct ConfigEntry { std::wstring name, detail; };
    std::vector<ConfigEntry> mConfigEntries;
    std::vector<size_t> mConfigVisible;
    bool mConfigRebuilding = false, mConfigStatusError = false, mConfigRenameMode = false;
    bool mConfigEditing = false;
    HWND mVisRegion = nullptr, mVisBoxes = nullptr, mVisStats = nullptr;
    HWND mVisConf = nullptr;
    HWND mBindLabel = nullptr, mBindButton = nullptr;
    int  mActivationKey = VK_RBUTTON;
    bool mBindCapture = false;
    bool mBindWaitRelease = false;
    static constexpr UINT_PTR kBindTimerId = 1;
    HBRUSH mBrBg = nullptr, mBrPanel = nullptr, mBrInput = nullptr, mBrStatus = nullptr;
    HFONT  mFontUi = nullptr, mFontBold = nullptr, mFontMono = nullptr;
    HFONT  mFontTitle = nullptr, mFontSmall = nullptr, mFontTab = nullptr;
    std::wstring mHudFps = L"FPS: 0.0";
    std::wstring mHudStatus = RU_HUD_STATUS RU_OFF;
    std::wstring mHudInfo;
    bool mHudReady = false;
    HWND mOverlay = nullptr;
    HDC mOverlayMemDc = nullptr;
    HBITMAP mOverlayBitmap = nullptr;
    HGDIOBJ mOverlayOldBitmap = nullptr;
    HDC mSettingsMemDc = nullptr;
    HBITMAP mSettingsBitmap = nullptr;
    HGDIOBJ mSettingsOldBitmap = nullptr;
    int mSettingsBufferW = 0, mSettingsBufferH = 0;
    int  mActiveTab = 0;
    bool mShowRegion = false, mShowBoxes = false, mShowStats = false;
    bool mShowConfidence = false;
    double mOverlayLastMs = 0.0;
    double mOverlayStatsMs = 0.0;
    UiSnapshot mOverlaySnapshot;
    RECT mOverlayPreviousBounds{};
    bool mOverlaySnapshotValid = false, mOverlayStatusOn = false;
    unsigned mOverlayFlags = 0;
    HPEN mOverlayFovPen = nullptr, mOverlayBoxPen = nullptr;
    HPEN mOverlayTargetPen = nullptr, mOverlayBorderPen = nullptr;
    HBRUSH mOverlayPanelBrush = nullptr;
    HFONT mOverlayFont = nullptr;
    double mCapFps = 0.0;
    HWND mModelLabel = nullptr, mModelCombo = nullptr;
    HWND mConvertButton=nullptr,mConvertStatus=nullptr,mConvertProgress=nullptr;
    HWND mConvertLogButton=nullptr,mOnnxFolderButton=nullptr;
    static constexpr int kConvertId=1081,kConvertLogId=1082,kOnnxFolderId=1083;
    albion::converter::Handle mConversionPauseReady{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    albion::converter::Compiler mConverter;
    std::atomic<bool> mConversionRequested{false};
    bool mConversionPausedOwner=false; // pipeline-owner only
    std::wstring mResumeAfterConversion,mLastConversionLog;


    std::vector<std::wstring> mModelFiles;

    static constexpr int kMinHeadYOffset = kAimMinHeadYOffsetPixels;
    static constexpr int kMaxHeadYOffset = kAimMaxHeadYOffsetPixels;
    static constexpr int kMinMouseScale = 5;    // 5% vyhoda
    static constexpr int kMaxMouseScale = 200;  // 200% vyhoda
    static constexpr int kMinAimSpeed = 1;
    static constexpr int kMaxAimSpeed = 100;
    static constexpr int kMinCaptureSize = 60;   // minimalnyy FOV
    static constexpr int kMaxCaptureSize = 1200;
    static constexpr int kCaptureStep = 10;
    // Ogranichenie chastoty zakhvata. 0 = bez limita.
    static constexpr int kFpsOptions[] = { 0, 30, 60, 120, 144, 240 };
    std::atomic<int> mFpsLimit{ 0 };

    // Sposob zakhvata ekrana: nash (dirty-rects) ili kak v Aimmy (vsya oblast celikom).
    enum class CaptureMethod : int { DirtyRects = 0, DirectXFull = 1 };
    CaptureMethod mCaptureMethod = CaptureMethod::DirectXFull;
    unsigned      mCapFailures = 0;
    static constexpr unsigned kMaxCaptureFailures = 5;

    static constexpr int kMinConfidence = 5;    // 0.05
    static constexpr int kMaxConfidence = 95;   // 0.95

    // Sposob dostavki dvizheniya myshi (kak v main.py: WinAPI ili Arduino HID).
    enum class AimMethod : int { WinApi = 0, ArduinoHid = 1 };
    AimMethod        mAimMethod = AimMethod::WinApi;
    ArduinoHidMouse  mArduino;

    inline static std::atomic<ArduinoHidMouse*> sArduinoSink{nullptr};

    static void ArduinoMouseSink(LONG dx, LONG dy);


    // FEATURE-V11-OWNER: UI owns mAimCfg; pipeline owns mAim and capture.
    // Coalesced snapshots prevent a slider backlog and never borrow UI objects.
    struct SettingsCommand { BasicAimConfig config; int method = 0, capture = 1; std::uint64_t revision = 0; };
    struct ModelRequest { std::wstring path; bool force = false; };
    std::mutex mCommandMx;
    std::optional<SettingsCommand> mPendingSettings;
    std::optional<ModelRequest> mPendingModel;
    bool mPendingCaptureRetry = false, mApplyingConfig = false;
    bool mCaptureFailed = false; // pipeline-owner only
    std::uint64_t mSettingsRevision = 0, mAppliedRevision = 0, mLoadedConfigRevision = 0;
    std::wstring mLoadedConfigName;
    bool mConfigDirty = false;
    AimMethod mWorkerAimMethod = AimMethod::WinApi;
    CaptureMethod mWorkerCaptureMethod = CaptureMethod::DirectXFull;
    albion::TaskQueue mServiceTasks;
    std::atomic<bool> mPerfSnapshotQueued{false};
    albion::DiagnosticPanel mDiagnostics;
    albion::PerformancePanel mPerformance;
    bool mF8WasDown = false;
    std::wstring mLastProfileStatus;

    static std::wstring QuoteArgument(const std::wstring& value);

    static std::optional<std::wstring> PickFile(HWND owner, bool save, const wchar_t* filter,
        const std::wstring& initial = {}, const wchar_t* extension = L"cfg");

    static std::wstring FindPythonLauncher(bool& isPyLauncher);

    void LaunchPython(const std::wstring& script, const std::wstring& arguments);

    void ConfigTransfer(bool importing);

    void RestoreConfigHistory();

    void DiagnosticAction(albion::DiagnosticAction action);

    void ShowPerformance();

    void PollDiagnosticsHotkey();

    void UpdateDiagnostics();

    void QueueSettings(bool includeModel = false);

    void ProcessCommands();

    // ---- Vkladki -------------------------------------------------------
    void ApplyTab(int tab);

    // ---- Tab change animation ------------------------------------------
    // The old page (everything right of the sidebar) is captured into a
    // click-through layered window owned by the settings window. The new page
    // is laid out underneath, then the snapshot fades out and slides up a bit.
    static constexpr UINT_PTR kTabFadeTimer = 0x5A18;
    static constexpr double kTabFadeMs = 220.0;
    void EndTabFade();
    void PushTabFade(BYTE alpha, int dy);
    bool BeginTabFade();
    void StartTabFade();
    static LRESULT CALLBACK TabFadeProc(HWND h, UINT m, WPARAM w, LPARAM l);

    // ---- Vizualy (poverkh igry) ----------------------------------------
    void ApplyVisuals();

    static LRESULT CALLBACK OverlayProcStatic(HWND h, UINT m, WPARAM w, LPARAM l);

    void DestroyOverlayBuffer();

    bool EnsureOverlayBuffer(HDC targetDc);

    void PaintOverlayBuffered(HDC targetDc, const RECT& paintRect);

    void DestroyOverlayResources();

    void EnsureOverlayResources();

    RECT OverlayBounds(const UiSnapshot& ui) const;

    void UpdateOverlayInvalidation(double now);

    void EnsureOverlay();

    void RefreshOverlayGeometry();

    void DrawOverlay(HDC dc);


    std::wstring CaptureMethodStatus() const;

    // Vybor sposoba zakhvata iz vypadayushchego spiska.
    void ApplyCaptureMethod();

    std::wstring FpsLimitStatus() const;

    // Vybor limita chastoty zakhvata iz vypadayushchego spiska.
    void ApplyFpsLimit();

    std::wstring AimMethodStatus() const;

    // Perepodklyuchaet Arduino i perevodit pritsel na vybrannyy sposob otpravki.
    void ApplyAimMethod();

    // -------------------------------------------------------------------
    void CreateDevice();

    void EnsureCaptureTexture();

    void StartDuplication();

    std::wstring UiImagePath(const wchar_t* fileName) const;

    std::unique_ptr<Gdiplus::Image> LoadUiImage(const wchar_t* fileName) const;

    void LoadUiImages();

    void ResetUiImages();

    Gdiplus::Image* ButtonImage(HWND button) const;

    void DrawUiImage(HDC dc, Gdiplus::Image* image, RECT bounds, int padding = 0) const;

    void DrawUiImageMonochrome(HDC dc, Gdiplus::Image* image, RECT bounds,
        COLORREF color) const;

    // -------------------------------------------------------------------
    void CreateSettingsWindow(HINSTANCE hInst);

    // Center in the cursor monitor's work area on the initial reveal only.
    void CenterSettingsWindow();

    void RevealSettingsWindow();

    // -------------------------------------------------------------------
    void CreateControls();

    // Presentation only. No values, ranges, IDs or application callbacks change.
    // Grid: sidebar 0..92, content kL..kR, rows follow the reference layout.
    void LayoutControls();

    static constexpr UINT_PTR kSwitchAnimTimer = 0x5A17;
    int SwitchIndex(HWND h) const;
    bool SwitchTarget(int i) const;

    static LRESULT CALLBACK UiButtonSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref);

    void ApplyRoundedWindowRegion();

    // ---- Otrisovka elementov v stile referensa ----------------------------
    static Gdiplus::Color GpColor(COLORREF c, BYTE a = 255);

    static void GpPill(Gdiplus::Graphics& g, Gdiplus::Brush* brush, float x, float y, float w, float h);

    void DrawThemedButton(DRAWITEMSTRUCT* di);

    void DrawThemedButtonContent(DRAWITEMSTRUCT* di);

    // Closed combo is drawn as a flat pill, like the keybind field.
    void PaintComboControl(HWND hc, HDC dc);

    static LRESULT CALLBACK ComboSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref);

    void DrawThemedComboItem(DRAWITEMSTRUCT* di);

    static void DrawConfigActionGlyph(HDC dc, float x, float y, int kind, COLORREF ink);

    void DrawThemedListItem(DRAWITEMSTRUCT* di);

    void PaintConfigWells(HDC dc);

    static LRESULT CALLBACK ConfigInputSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR data);

    // Trackbars are painted completely by hand in a memory bitmap: thick dark
    // track, accent progress and a white knob, as in the reference.
    void PaintTrackbar(HWND slider, HDC target);

    static LRESULT CALLBACK TrackbarSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref);

    void DestroySettingsBuffer();

    bool EnsureSettingsBuffer(HDC target, int width, int height);

    void PaintSettingsBackground(HDC dc);

    void UpdateBindButton();

    void SetActivationKey(int vk);

    void BeginBindCapture();

    void EndBindCapture();

    // Opros klavish vo vremya zakhvata binda (kak v Python-versii).
    void PollBindCapture();

    // Polzunok masshtaba vyhoda myshi (procenty). Staryy config bez polya
    // daet 100%. Mertvoy zony bolshe net - ona zhyostko 0.
    void SyncMouseScaleControl();

    void ApplyMouseScaleControl();

    void ApplyControlValues();

    // -------------------------------------------------------------------
    void StartModelConversion();

    void PollModelConversion();

    bool PauseForModelConversion(PipelineActivityWaiter& waiter);

    void ScanModelFiles();

    static std::wstring FileNameOf(const std::wstring& p);

    void FillModelCombo(const std::wstring& preferred = {});

    std::wstring CurrentModelPath() const;

    void ApplyModel(bool force = false);

    void ApplyMovementPath();

    void ApplyBodyPart(int selection);

    // -------------------------------------------------------------------
    // Sohranenie/zagruzka konfigov. Format - prostoy tekst "klyuch=znachenie",
    // chtoby ego mozhno bylo legko posmotret/otredaktirovat vruchnuyu.
    std::string BuildConfigText() const;

    // Primenyaet zagruzhennyy iz fayla konfig. Nastroyki bez polzovatelskikh
    // kontrolov pishutsya napryamuyu, a UI-svyazannye (slaydery/kombo) - cherez
    // ikh sushchestvuyushchie obrabotchiki Apply*, chtoby ne dublirovat logiku.
    bool ApplyConfigText(const std::string& text, std::wstring* why = nullptr);

    void SetConfigStatus(const std::wstring& text, bool error = false);

    static bool IsValidConfigName(const std::wstring& name);

    std::wstring ConfigDirectory() const;

    std::wstring ConfigPath(const std::wstring& name) const;

    std::wstring ConfigNameFromEdit() const;

    std::wstring SelectedConfigName() const;

    // Windows ordinal comparison handles Cyrillic without changing the process locale.
    static bool ConfigNameEqual(const std::wstring& a, const std::wstring& b);

    void UpdateConfigTitle();

    void UpdateConfigEditorVisibility();

    void RebuildConfigList(const std::wstring& preferred = std::wstring());

    void RefreshConfigList(const std::wstring& selectName = std::wstring());

    void UpdateConfigButtons();

    void OnConfigSelectionChanged();

    void BeginConfigEdit(bool rename);

    void FinishConfigEdit();

    void NewConfigDraft();

    void CommitConfigEdit();

    void CancelConfigEdit();

    void SaveNamedConfig();

    void LoadSelectedConfig();

    void DeleteSelectedConfig();

    void RenameSelectedConfig();

    bool HandleConfigKey(const MSG& message);

    // -------------------------------------------------------------------
    static double NowMs();

    // Kak v Aimmy: posle sboya zhdem kadr dolshe, v obychnom rezhime - 1 ms.
    UINT CaptureTimeoutMs() const;

    // SRV na samu teksturu duplikacii. nullptr - znachit put nedostupen.
    ID3D11ShaderResourceView* DesktopSrv(ID3D11Texture2D* src);

    // Pora li gotovit ocherednoy tenzor. Ranshe eta proverka zhila vnutri
    // PumpDetection; teper ona nuzhna i pryamomu putyu zakhvata.
    std::uint64_t mLastPreprocessFrameId=0;
    double mPresentedFrameMs=0,mDeliveryMs=0;
    BackendStageTiming mLatestStages{};

    bool WantsPreprocess();

    bool HasPreprocessCapacity();

    // Otpravka regiona v preprocessor. V zero-copy rezhime srazu zhe budit
    // detektor - kopirovat na CPU nechego.
    void SubmitPreprocess(ID3D11ShaderResourceView* srv, UINT texW, UINT texH,
        const CaptureRegion& region, LONG outLeft, LONG outTop);

    void DispatchSharedFrame();

#ifdef AIM_ENABLE_WGC
    bool CaptureWgcFrame();
#endif

    bool CaptureFrame(UINT ms);

    void CopyDirty(const DXGI_OUTDUPL_FRAME_INFO& info, ID3D11Texture2D* src);

    // Rezhim Aimmy: odna kopiya vsey oblasti pritsela, bez dirty-rects.
    void CopyRegionFull(ID3D11Texture2D* src);

    bool PumpDetection(bool /*allowPreprocess*/);

    // -------------------------------------------------------------------
    // -------------------------------------------------------------------
    void ReportPipelineFailure(const std::string& error);

    void StartPipeline();

    void StopPipeline();

    double mNextUiPublishMs = 0;
    std::uint64_t mLastUiRevision = ~std::uint64_t(0);
    bool mUiDetsDirty = true, mLastUiReady = false;

    std::optional<UiSnapshot> CapturePipelineSnapshot(unsigned& frames, bool& detectionsChanged, bool captureDue);

    void PublishPipelineSnapshot(UiSnapshot snapshot, bool detectionsChanged);

    void PublishCaptureFps(double fps);

    // Analog AiLoopAsync iz AIManager.cs.
    void PipelineThread();

    bool QueryDxgiMemory(double& usedMb, double& budgetMb);

    void SavePerformanceSnapshot();

    void PollPerformanceSnapshotHotkey();

    void StopJitterTrace();

    void PollJitterTraceHotkey();

    void MessageLoop();

    // -------------------------------------------------------------------
    static LRESULT CALLBACK WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l);

    LRESULT WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
};
