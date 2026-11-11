// TRT SHAPE HOTFIX: resolve actual static shape or profile-0 optimum before allocation.
// PERFORMANCE REVISION: two registered D3D11 input slots, checked CUDA
// map/unmap, bounded latest-frame producer integration and restart-on-failure.
// Pair with the supplied WindowsProject1.cpp. Windows/CUDA runtime test required.
#pragma once
//
// GPU-инференс для детектора YOLOv8.
//
// Бэкенды включаются макросами препроцессора в настройках проекта:
//
//   AIM_ENABLE_TRT  — нативный TensorRT, файлы .engine / .plan / .trt
//   AIM_ENABLE_ORT  — ONNX Runtime для .onnx: TensorRT EP -> CUDA EP -> DirectML EP -> CPU
//   AIM_ENABLE_DML  — разрешить DirectML EP внутри ONNX Runtime
//
//   AIM_ENABLE_D3D_INTEROP — вход берётся напрямую из буфера D3D11, который
//                            заполнил compute shader препроцессора. Требует
//                            AIM_ENABLE_TRT и подключённого d3d11.h.
//                            Убирает readback тензора на CPU и обратную
//                            загрузку на GPU (эффект зависит от оборудования).
//
// Если ни один макрос не задан, фабрика вернёт nullptr и приложение
// автоматически откатится на встроенный WinML — код собирается всегда.
//

#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <vector>
#include "dev_diagnostics.hpp"

// ---------------------------------------------------------------------------
// Общий интерфейс
// ---------------------------------------------------------------------------

// Resolve the supported NCHW square input without probing incompatible shapes.
// Dynamic axes use profile 0's optimum, validated against its min/max bounds.
inline bool AimResolveSquareInput(const std::array<int64_t, 4>& declared,
    const std::array<int64_t, 4>& optimal,
    const std::array<int64_t, 4>& minimum,
    const std::array<int64_t, 4>& maximum,
    int& side, std::string& error)
{
    bool dynamic = false;
    for (auto d : declared) dynamic = dynamic || d < 0;
    int64_t height = declared[2] > 0 ? declared[2] : optimal[2];
    int64_t width = declared[3] > 0 ? declared[3] : optimal[3];
    if (declared[2] > 0 && declared[3] < 0) width = height;
    if (declared[3] > 0 && declared[2] < 0) height = width;
    if (height != width || height < 64 || height > 640)
    {
        error = "Unsupported TensorRT input: requires square NCHW spatial dimensions in 64..640; got " +
            std::to_string(height) + "x" + std::to_string(width);
        return false;
    }
    const std::array<int64_t, 4> wanted{ 1, 3, height, width };
    for (size_t i = 0; i < wanted.size(); ++i)
    {
        if (declared[i] >= 0 && declared[i] != wanted[i])
        {
            error = "TensorRT input must be batch=1, channels=3, square NCHW";
            return false;
        }
        if (dynamic && (minimum[i] <= 0 || maximum[i] < minimum[i] ||
            wanted[i] < minimum[i] || wanted[i] > maximum[i]))
        {
            error = "Requested TensorRT input is outside optimization profile 0";
            return false;
        }
    }
    side = int(height);
    return true;
}

struct BackendStageTiming {
    double hostInputMs=-1, waitMs=-1, hostOutputMs=-1;
    double h2dGpuMs=-1, computeGpuMs=-1, d2hGpuMs=-1;
};
// Borrowed until the next Run*/Unbind/destruction on the same backend.
// Consume synchronously on the detector worker; never publish this pointer.
struct InferenceOutputView {
    const float* data = nullptr;
    size_t count = 0;
};
inline double AimBackendNowMs() noexcept {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool AimPerfEnvFlag(const wchar_t* name) noexcept {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(name,value,8)==1 && value[0]==L'1';
}

class IInferenceBackend
{
public:
    virtual ~IInferenceBackend() = default;

    // Стабильная строковая константа для HUD.
    virtual const char* Name() const noexcept = 0;
    virtual std::string Diagnostics() const { return Name(); }
    virtual BackendStageTiming LastStageTiming() const noexcept { return {}; }
    // Zero means an older backend still uses the caller's requested input size.
    virtual int InputSide() const noexcept { return 0; }

    // input  — 1x3xNxN float32 (N = netSize)
    // out    — плоский выход модели
    // channels/anchors — размеры YOLO-выхода (1, channels, anchors)
    virtual bool Run(
        const float* input,
        std::vector<float>& out,
        int& channels,
        int& anchors,
        std::string& error) = 0;

    // -----------------------------------------------------------------
    // Необязательный путь без CPU.
    //
    // Приложение уже держит готовый тензор в ID3D11Buffer — его туда
    // записал compute shader препроцессора. Если бэкенд умеет читать этот
    // буфер напрямую, то ни readback на CPU, ни повторная загрузка на GPU
    // не нужны.
    //
    // Порядок вызовов: SupportsSharedInput() -> BindSharedInput() ->
    // RunShared() ... -> UnbindSharedInput().
    //
    // База отвечает "не умею", поэтому любой бэкенд остаётся рабочим и
    // приложение молча откатывается на обычный Run().
    // -----------------------------------------------------------------

    virtual bool SupportsSharedInput() const noexcept { return false; }

    // device — ID3D11Device*, buffer — ID3D11Buffer* на 3*N*N float32.
    virtual bool BindSharedInput(
        void* /*device*/,
        void* /*buffer*/,
        std::string& error)
    {
        error = "backend does not support shared input";
        return false;
    }

    // Register all buffers once. Slot ownership is managed by the caller:
    // never write a slot while RunSharedSlot(slot) is using it. Successful
    // return from RunSharedSlot means the input is no longer being accessed.
    virtual bool BindSharedInputs(void* device, void* const* buffers,
        size_t count, std::string& error)
    {
        if (count == 1 && buffers) return BindSharedInput(device, buffers[0], error);
        error = "backend does not support multiple shared input slots";
        return false;
    }

    virtual void UnbindSharedInput() noexcept {}

    // То же, что Run(), но вход берётся из привязанного буфера D3D11.
    virtual bool RunShared(
        std::vector<float>& /*out*/,
        int& /*channels*/,
        int& /*anchors*/,
        std::string& error)
    {
        error = "backend does not support shared input";
        return false;
    }
    virtual bool RunSharedSlot(size_t slot, std::vector<float>& out,
        int& channels, int& anchors, std::string& error)
    {
        if (slot == 0) return RunShared(out, channels, anchors, error);
        error = "invalid shared input slot";
        return false;
    }

    // Backwards-compatible adapters: existing backends keep owned storage.
    virtual bool RunView(const float* input, std::vector<float>& scratch,
        InferenceOutputView& view, int& channels, int& anchors, std::string& error)
    {
        view = {};
        if (!Run(input, scratch, channels, anchors, error)) return false;
        view = { scratch.data(), scratch.size() };
        return true;
    }
    virtual bool RunSharedSlotView(size_t slot, std::vector<float>& scratch,
        InferenceOutputView& view, int& channels, int& anchors, std::string& error)
    {
        view = {};
        if (!RunSharedSlot(slot, scratch, channels, anchors, error)) return false;
        view = { scratch.data(), scratch.size() };
        return true;
    }

};

inline bool AimPathHasExt(const std::wstring& path, const wchar_t* ext) noexcept
{
    const size_t n = std::char_traits<wchar_t>::length(ext);

    if (path.size() < n)
        return false;

    for (size_t i = 0; i < n; ++i)
    {
        wchar_t a = path[path.size() - n + i];
        wchar_t b = ext[i];

        if (a >= L'A' && a <= L'Z')
            a = wchar_t(a - L'A' + L'a');

        if (a != b)
            return false;
    }

    return true;
}

inline bool AimIsEngineFile(const std::wstring& path) noexcept
{
    return AimPathHasExt(path, L".engine") ||
           AimPathHasExt(path, L".plan") ||
           AimPathHasExt(path, L".trt");
}

inline std::string AimNarrow(const std::wstring& w)
{
    std::string s;
    s.reserve(w.size());

    for (wchar_t c : w)
    {
        s.push_back(c < 128 ? char(c) : '?');
    }

    return s;
}

// ---------------------------------------------------------------------------
// TensorRT: готовый .engine
// ---------------------------------------------------------------------------

#ifdef AIM_ENABLE_TRT

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#ifdef AIM_ENABLE_D3D_INTEROP
// d3d11.h должен быть подключён раньше — WindowsProject1.cpp это делает.
#include <cuda_d3d11_interop.h>
#include <wrl/client.h>
#endif

// FIX-V11-14/BUILD: names in this project are nvinfer_10 / cudart64_110.
// Reject mismatched SDK headers instead of accidentally importing another DLL
// eagerly and defeating WinML startup fallback. Updating SDKs is a paired change.
#if NV_TENSORRT_MAJOR != 10
#error "This project targets TensorRT 10 (nvinfer_10); use matching SDKs or build.bat false winml."
#endif
#if CUDART_VERSION < 11080 || CUDART_VERSION >= 12000
#error "This project targets the CUDA 11.8 runtime (cudart64_110); update build/runtime names together for CUDA 12+."
#endif

#if NV_TENSORRT_MAJOR >= 10
#pragma comment(lib, "nvinfer_10.lib")
#else
#pragma comment(lib, "nvinfer.lib")
#endif
#pragma comment(lib, "cudart.lib")

// FIX-V11-14: do not enter a delay import when its DLL is unavailable.
// Handles intentionally remain loaded for the process lifetime: backend objects
// may still contain vtables owned by these modules. Search: app dir/System32,
// CUDA toolkit bin (cudart only), then the standard order incl. PATH as in the
// original build.
inline HMODULE AimLoadCudaFromToolkit(const wchar_t* name) {
    // FIX-V11.1-CUDA-PATH: a standard CUDA 11.8 install keeps cudart in
    // %CUDA_PATH_V11_8%\bin and adds it to PATH. 1.0.0 searched only the app
    // and System32 directories and rejected that working setup. Only the
    // installer-defined toolkit directory is added - never CWD or all of PATH.
    for (const wchar_t* variable : {L"CUDA_PATH_V11_8", L"CUDA_PATH"}) {
        wchar_t root[1024]{};
        const DWORD n = GetEnvironmentVariableW(variable, root, DWORD(std::size(root)));
        if (!n || n >= std::size(root)) continue;
        std::wstring path(root, n);
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
        path += L"bin\\"; path += name;
        if (HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return module;
    }
    return nullptr;
}
inline bool AimTensorRtRuntimeAvailable(std::string& error) {
    static std::mutex mutex; std::lock_guard<std::mutex> lock(mutex);
    for (const wchar_t* name : {L"cudart64_110.dll", L"nvinfer_10.dll"}) {
        if (GetModuleHandleW(name) || LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) continue;
        const DWORD failure = GetLastError();
        if (name[0] == L'c' && AimLoadCudaFromToolkit(name)) continue;
        // FIX-V11.1-TRT-SEARCH: the original build resolved these imports with
        // the standard Windows search order, including PATH (TensorRT\lib in
        // PATH is the usual setup). 1.0.x rejected that or picked a different
        // TensorRT build, so an engine made for the PATH runtime failed to load.
        if (LoadLibraryW(name)) continue;
        // Name the missing module; "error 126" alone was not actionable.
        error = "NVIDIA runtime " + AimNarrow(name) + " is missing/incompatible (Win32 error " + std::to_string(failure) +
            "). Copy matching SDK runtime DLLs next to the EXE or select an ONNX model in a WinML build.";
        return false;
    }
    return true;
}

class TrtLogger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) noexcept override
    {
        if (severity <= Severity::kWARNING && msg)
        {
            OutputDebugStringA("[TensorRT] ");
            OutputDebugStringA(msg);
            OutputDebugStringA("\n");
            DevLog("TensorRT", msg);
        }
    }
};

// The transfer/preprocessor path below supports one dense FP32 device input/output.
// TensorRT internal FP16 remains allowed; this checks external tensor storage only.
inline bool AimValidateTensorRtIo(nvinfer1::ICudaEngine& engine,
    std::string& input, std::string& output, std::string& error)
{
    input.clear(); output.clear();
    int inputs = 0, outputs = 0;
    const int count = engine.getNbIOTensors();
    for (int i = 0; i < count; ++i)
    {
        const char* name = engine.getIOTensorName(i);
        if (!name) { error = "TensorRT returned an invalid I/O tensor name"; return false; }
        const auto mode = engine.getTensorIOMode(name);
        if (mode == nvinfer1::TensorIOMode::kINPUT) { ++inputs; input = name; }
        else if (mode == nvinfer1::TensorIOMode::kOUTPUT) { ++outputs; output = name; }
        else { error = "TensorRT returned an invalid I/O mode"; return false; }
    }
    if (inputs != 1 || outputs != 1)
    {
        error = "This backend requires exactly one input and one output tensor"; return false;
    }
    for (const auto* name : { input.c_str(), output.c_str() })
    {
        if (engine.getTensorDataType(name) != nvinfer1::DataType::kFLOAT ||
            engine.getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE ||
            engine.getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR ||
            engine.getTensorVectorizedDim(name) != -1)
        {
            error = "TensorRT I/O must be dense linear FP32 device tensors; internal FP16 is supported";
            return false;
        }
    }
    return true;
}

class TrtBackend : public IInferenceBackend
{
public:
    ~TrtBackend() override
    {
#ifdef AIM_ENABLE_D3D_INTEROP
        UnbindSharedInput();
#endif

        if (mIoGraphExec) cudaGraphExecDestroy(mIoGraphExec);
        if (mIoGraph) cudaGraphDestroy(mIoGraph);
        for (auto event : mStageEvents) if (event) cudaEventDestroy(event);
        // Graph связан с execution context и stream — уничтожаем его первым.
        if (mGraphExec)
            cudaGraphExecDestroy(mGraphExec);

        if (mGraph)
            cudaGraphDestroy(mGraph);

        if (mStream)
            cudaStreamDestroy(mStream);

        if (mDevIn)
            cudaFree(mDevIn);

        if (mDevOut)
            cudaFree(mDevOut);

        if (mHostIn)
            cudaFreeHost(mHostIn);

        if (mHostOut)
            cudaFreeHost(mHostOut);

        delete mContext;
        delete mEngine;
        delete mRuntime;
    }

    bool Init(const std::wstring& path, int netSize, std::string& error)
    {
        // Capture GPU/runtime facts even when deserialization subsequently fails.
        int device = -1, runtimeVersion = 0, driverVersion = 0;
        cudaDeviceProp deviceInfo{};
        cudaError_t deviceStatus = cudaGetDevice(&device);
        cudaError_t propertiesStatus = deviceStatus == cudaSuccess
            ? cudaGetDeviceProperties(&deviceInfo, device) : deviceStatus;
        cudaError_t runtimeStatus = cudaRuntimeGetVersion(&runtimeVersion);
        cudaError_t driverStatus = cudaDriverGetVersion(&driverVersion);
        char gpuInfo[512]{};
        std::snprintf(gpuInfo, sizeof(gpuInfo),
            "device=%d gpu=%s compute=%d.%d cuda-runtime=%d (status=%d) cuda-driver-api=%d (status=%d) trt-headers=%d.%d.%d",
            device, propertiesStatus == cudaSuccess ? deviceInfo.name : "unavailable",
            propertiesStatus == cudaSuccess ? deviceInfo.major : 0,
            propertiesStatus == cudaSuccess ? deviceInfo.minor : 0,
            runtimeVersion, int(runtimeStatus), driverVersion, int(driverStatus),
            NV_TENSORRT_MAJOR, NV_TENSORRT_MINOR, NV_TENSORRT_PATCH);
        DevLog("GPU", gpuInfo);
        for (const wchar_t* dllName : { L"nvinfer_10.dll", L"cudart64_110.dll" }) {
            HMODULE module = GetModuleHandleW(dllName);
            wchar_t dllPath[32768]{};
            const DWORD n = module ? GetModuleFileNameW(module, dllPath, DWORD(sizeof(dllPath) / sizeof(dllPath[0]))) : 0;
            if (n && n < sizeof(dllPath) / sizeof(dllPath[0])) {
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, dllPath, int(n), nullptr, 0, nullptr, nullptr);
                if (bytes > 0) {
                    std::string utf8(size_t(bytes), '\0');
                    if (WideCharToMultiByte(CP_UTF8, 0, dllPath, int(n), utf8.data(), bytes, nullptr, nullptr) > 0)
                        DevLog("DLL", utf8.c_str());
                }
            }
        }
        std::vector<char> blob;

        if (!ReadWholeFile(path, blob))
        {
            error = "Не удалось прочитать " + AimNarrow(path);
            return false;
        }

        DevLog("TensorRT", ("engine bytes=" + std::to_string(blob.size())).c_str());
        mRuntime = nvinfer1::createInferRuntime(mLogger);
        if (!mRuntime)
        {
            error = "createInferRuntime failed";
            return false;
        }

        mEngine = mRuntime->deserializeCudaEngine(blob.data(), blob.size());
        if (!mEngine)
        {
            DevLog("TensorRT", "Engine deserialization failed; see TensorRT messages above.");
            error =
                "Не удалось загрузить engine. Он собран другой версией TensorRT "
                "или на другой видеокарте. Пересобери его через trtexec.";
            return false;
        }

        mContext = mEngine->createExecutionContext();
        if (!mContext)
        {
            error = "createExecutionContext failed";
            return false;
        }

        if (!AimValidateTensorRtIo(*mEngine, mInName, mOutName, error)) return false;

        // Inspect the serialized engine before calling setInputShape.
        const auto declared = mEngine->getTensorShape(mInName.c_str());
        if (declared.nbDims != 4)
        {
            error = "TensorRT input must be rank 4 NCHW"; return false;
        }
        bool dynamic = false;
        std::array<int64_t, 4> shape{}, optimal{}, minimum{}, maximum{};
        for (size_t i = 0; i < shape.size(); ++i)
        {
            shape[i] = int64_t(declared.d[i]);
            dynamic = dynamic || shape[i] < 0;
        }
        if (dynamic)
        {
            const auto opt = mEngine->getProfileShape(mInName.c_str(), 0, nvinfer1::OptProfileSelector::kOPT);
            const auto lo = mEngine->getProfileShape(mInName.c_str(), 0, nvinfer1::OptProfileSelector::kMIN);
            const auto hi = mEngine->getProfileShape(mInName.c_str(), 0, nvinfer1::OptProfileSelector::kMAX);
            if (opt.nbDims != 4 || lo.nbDims != 4 || hi.nbDims != 4)
            {
                error = "TensorRT profile 0 has invalid input dimensions"; return false;
            }
            for (size_t i = 0; i < shape.size(); ++i)
            {
                optimal[i] = int64_t(opt.d[i]); minimum[i] = int64_t(lo.d[i]); maximum[i] = int64_t(hi.d[i]);
            }
        }
        if (!AimResolveSquareInput(shape, optimal, minimum, maximum, netSize, error)) return false;
        mInputSide = netSize;
        // Static engines already have their final shape: no setInputShape call.
        if (dynamic)
        {
            const nvinfer1::Dims4 inDims{ 1, 3, netSize, netSize };
            if (!mContext->setInputShape(mInName.c_str(), inDims))
            {
                error = "TensorRT rejected the validated profile-0 input shape"; return false;
            }
        }
        const std::string shapeLog = "[TensorRT] Resolved input " + mInName + " = [1,3," +
            std::to_string(netSize) + "," + std::to_string(netSize) + "]\n";
        OutputDebugStringA(shapeLog.c_str());

        const nvinfer1::Dims outDims = mContext->getTensorShape(mOutName.c_str());
        if (outDims.nbDims != 3 || outDims.d[0] != 1 ||
            outDims.d[1] <= 0 || outDims.d[2] <= 0 ||
            outDims.d[1] > (std::numeric_limits<int>::max)() ||
            outDims.d[2] > (std::numeric_limits<int>::max)())
        {
            error = "Ожидался статический выход TensorRT вида (1, d1, d2)";
            return false;
        }

        // Сохраняем физический порядок engine. Для best.engine это [1,300,6].
        // Основной Decode() сам распознаёт транспонированный NMS-выход.
        mChannels = int(outDims.d[1]);
        mAnchors = int(outDims.d[2]);
        mInCount = size_t(3) * size_t(netSize) * size_t(netSize);
        if (size_t(mChannels) > (std::numeric_limits<size_t>::max)() / sizeof(float) / size_t(mAnchors))
        {
            error = "TensorRT output byte size overflows size_t"; return false;
        }
        mOutCount = size_t(mChannels) * size_t(mAnchors);

        if (!CudaOk(cudaStreamCreateWithFlags(&mStream, cudaStreamNonBlocking),
                    "cudaStreamCreateWithFlags", error) ||
            !CudaOk(cudaMalloc(&mDevIn, mInCount * sizeof(float)),
                    "cudaMalloc input", error) ||
            !CudaOk(cudaMalloc(&mDevOut, mOutCount * sizeof(float)),
                    "cudaMalloc output", error) ||
            !CudaOk(cudaMallocHost(reinterpret_cast<void**>(&mHostIn),
                                   mInCount * sizeof(float)),
                    "cudaMallocHost input", error) ||
            !CudaOk(cudaMallocHost(reinterpret_cast<void**>(&mHostOut),
                                   mOutCount * sizeof(float)),
                    "cudaMallocHost output", error))
        {
            return false;
        }

        if (!mContext->setTensorAddress(mInName.c_str(), mDevIn) ||
            !mContext->setTensorAddress(mOutName.c_str(), mDevOut))
        {
            error = "TensorRT setTensorAddress failed";
            return false;
        }

        if (!CudaOk(cudaMemsetAsync(mDevIn, 0,
                                    mInCount * sizeof(float), mStream),
                    "cudaMemsetAsync input", error) ||
            !CudaOk(cudaMemsetAsync(mDevOut, 0,
                                    mOutCount * sizeof(float), mStream),
                    "cudaMemsetAsync output", error) ||
            !CudaOk(cudaStreamSynchronize(mStream),
                    "cudaStreamSynchronize init", error))
        {
            return false;
        }

        // Неудача захвата не ломает backend: Run() использует обычный enqueueV3.
        mSpinCompletion = AimPerfEnvFlag(L"ALBION_CUDA_POLL");
        mGraphReady = CreateCudaGraph();
        mProfile = AimPerfEnvFlag(L"ALBION_GPU_TIMING");
        if (mProfile) {
            for (auto& event : mStageEvents) if (cudaEventCreate(&event)!=cudaSuccess) mProfile=false;
        }
        if (AimPerfEnvFlag(L"ALBION_IO_GRAPH")) mIoGraphReady=CreateIoGraph();
        return true;
    }

    std::string Diagnostics() const override
    {
        int device = -1, runtime = 0, driver = 0;
        cudaDeviceProp prop{};
        std::string text = Name();
        if (cudaGetDevice(&device) == cudaSuccess) {
            text += "; CUDA device=" + std::to_string(device);
            if (cudaGetDeviceProperties(&prop, device) == cudaSuccess)
                text += std::string(" (") + prop.name + ")";
        }
        if (cudaRuntimeGetVersion(&runtime) == cudaSuccess)
            text += "; CUDA runtime=" + std::to_string(runtime);
        if (cudaDriverGetVersion(&driver) == cudaSuccess)
            text += "; CUDA driver API=" + std::to_string(driver);
        text += "; TensorRT headers=" + std::to_string(NV_TENSORRT_MAJOR) + "." +
            std::to_string(NV_TENSORRT_MINOR) + "." + std::to_string(NV_TENSORRT_PATCH);
        text += "; input=[1,3," + std::to_string(mInputSide) + "," +
            std::to_string(mInputSide) + "]; output=[1," + std::to_string(mChannels) +
            "," + std::to_string(mAnchors) + "]; IO=FP32";
        text += "; input tensor bytes=" + std::to_string(mInCount * sizeof(float)) +
            "; D2H output bytes=" + std::to_string(mOutCount * sizeof(float));
        text += std::string("; stage GPU timing=") + (mProfile ? "enabled" : "disabled");
        text += std::string("; IO graph=") + (mIoGraphReady ? "enabled" : "disabled/fallback");
        text += std::string("; completion=") + (mSpinCompletion ? "bounded polling" : "stream synchronize");
        text += "; borrowed pinned output supported";
        return text;
    }

    BackendStageTiming LastStageTiming() const noexcept override { return mStageTiming; }

    int InputSide() const noexcept override { return mInputSide; }

    const char* Name() const noexcept override
    {
#ifdef AIM_ENABLE_D3D_INTEROP
        if (mSharedCount == 2)
            return mGraphReady ? "TensorRT/Graph/GPU-input x2" : "TensorRT/GPU-input x2";
        if (mSharedCount == 1)
            return mGraphReady ? "TensorRT/Graph/GPU-input" : "TensorRT/GPU-input";
#endif
        if(mIoGraphReady) return "TensorRT/CUDA Graph+IO";
        return mGraphReady ? "TensorRT/CUDA Graph" : "TensorRT/engine";
    }

    bool Run(
        const float* input,
        std::vector<float>& out,
        int& channels,
        int& anchors,
        std::string& error) override
    {
        return RunInput(input, &out, channels, anchors, error);
    }

    bool RunView(const float* input, std::vector<float>&,
        InferenceOutputView& view, int& channels, int& anchors, std::string& error) override
    {
        view = {};
        if (!RunInput(input, nullptr, channels, anchors, error)) return false;
        view = { mHostOut, mOutCount };
        return true;
    }

private:
    bool RunInput(const float* input, std::vector<float>* out,
        int& channels, int& anchors, std::string& error)
    {
        if (!input)
        {
            error = "TensorRT input is null";
            return false;
        }

        mStageTiming={}; mProfileRun=mProfile;
        const double hostStart=AimBackendNowMs();
        // Вход приложения — pageable std::vector. Копирование в постоянную
        // pinned memory позволяет cudaMemcpyAsync работать действительно async.
        std::memcpy(mHostIn, input, mInCount * sizeof(float));
        mStageTiming.hostInputMs=AimBackendNowMs()-hostStart;
        if(mIoGraphReady) {
            if(!CudaOk(cudaGraphLaunch(mIoGraphExec,mStream),"IO cudaGraphLaunch",error)) return false;
            return FinishOutput(out,channels,anchors,error);
        }
        RecordStage(0);

        if (!CudaOk(cudaMemcpyAsync(
                        mDevIn,
                        mHostIn,
                        mInCount * sizeof(float),
                        cudaMemcpyHostToDevice,
                        mStream),
                    "cudaMemcpyAsync H2D", error))
        {
            return false;
        }

        RecordStage(1);
        return Launch(out, channels, anchors, error);
    }

public:
#ifdef AIM_ENABLE_D3D_INTEROP

    bool SupportsSharedInput() const noexcept override
    {
        return true;
    }

    bool BindSharedInput(void* device, void* buffer, std::string& error) override
    {
        void* buffers[] = { buffer };
        return BindSharedInputs(device, buffers, 1, error);
    }

    bool BindSharedInputs(void* device, void* const* buffers,
        size_t count, std::string& error) override
    {
        UnbindSharedInput();
        // Never register an iGPU resource against an unrelated CUDA context.
        // This is particularly important on hybrid/Optimus laptops.
        if (!device) { error = "D3D11 input device is null"; return false; }
        int d3dCudaDevice = -1, currentCudaDevice = -1;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> captureAdapter;
        const HRESULT queryResult = static_cast<ID3D11Device*>(device)->QueryInterface(
            __uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
        if (FAILED(queryResult) || FAILED(dxgiDevice->GetAdapter(captureAdapter.GetAddressOf()))) {
            error = "Cannot query the capture device's DXGI adapter";
            return false;
        }
        const cudaError_t adapterStatus = cudaD3D11GetDevice(&d3dCudaDevice,
            captureAdapter.Get());
        if (!CudaOk(adapterStatus, "cudaD3D11GetDevice (capture adapter)", error))
            return false;
        if (!CudaOk(cudaGetDevice(&currentCudaDevice), "cudaGetDevice", error))
            return false;
        if (d3dCudaDevice != currentCudaDevice) {
            error = "Capture adapter maps to CUDA device " + std::to_string(d3dCudaDevice) +
                ", inference uses " + std::to_string(currentCudaDevice) +
                "; retaining CPU-transfer fallback";
            return false;
        }
        if (!buffers || count == 0 || count > mSharedRes.size())
        {
            error = "invalid shared input buffer count"; return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            if (!buffers[i])
            {
                error = "shared input buffer is null";
                UnbindSharedInput(); return false;
            }
            auto* buffer = static_cast<ID3D11Buffer*>(buffers[i]);
            D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
            if (size_t(desc.ByteWidth) < mInCount * sizeof(float))
            {
                error = "shared input buffer is smaller than model input";
                UnbindSharedInput(); return false;
            }
            if (!CudaOk(cudaGraphicsD3D11RegisterResource(&mSharedRes[i],
                    buffer, cudaGraphicsRegisterFlagsNone),
                    "cudaGraphicsD3D11RegisterResource", error))
            {
                UnbindSharedInput(); return false;
            }
            if (!CudaOk(cudaGraphicsResourceSetMapFlags(mSharedRes[i],
                    cudaGraphicsMapFlagsReadOnly), "cudaGraphicsResourceSetMapFlags", error))
            {
                UnbindSharedInput(); return false;
            }
        }
        mSharedCount = count; mInteropHealthy = true;
        return true;
    }

    void UnbindSharedInput() noexcept override
    {
        // Called only after the producer is stopped or excluded by its lock.
        if (mStream) cudaStreamSynchronize(mStream);
        for (size_t i = 0; i < mSharedRes.size(); ++i)
        {
            if (!mSharedRes[i]) continue;
            if (mSharedMapped[i])
            {
                cudaGraphicsUnmapResources(1, &mSharedRes[i], mStream);
                if (mStream) cudaStreamSynchronize(mStream);
            }
            cudaGraphicsUnregisterResource(mSharedRes[i]);
            mSharedRes[i] = nullptr; mSharedMapped[i] = false;
        }
        mSharedCount = 0; mInteropHealthy = false;
    }

    bool RunShared(std::vector<float>& out, int& channels,
        int& anchors, std::string& error) override
    {
        return RunSharedSlot(0, out, channels, anchors, error);
    }

    bool RunSharedSlot(size_t slot, std::vector<float>& out,
        int& channels, int& anchors, std::string& error) override
    {
        return RunSharedInput(slot, &out, channels, anchors, error);
    }

    bool RunSharedSlotView(size_t slot, std::vector<float>&,
        InferenceOutputView& view, int& channels, int& anchors, std::string& error) override
    {
        view = {};
        if (!RunSharedInput(slot, nullptr, channels, anchors, error)) return false;
        view = { mHostOut, mOutCount };
        return true;
    }

private:
    bool RunSharedInput(size_t slot, std::vector<float>* out,
        int& channels, int& anchors, std::string& error)
    {
        mStageTiming={}; mProfileRun=false;
        if (!mInteropHealthy || slot >= mSharedCount || !mSharedRes[slot])
        {
            error = "shared input slot is unbound or interop requires restart";
            return false;
        }
        auto*& resource = mSharedRes[slot];
        if (!CudaOk(cudaGraphicsMapResources(1, &resource, mStream),
                "cudaGraphicsMapResources", error))
        {
            mInteropHealthy = false; return false;
        }
        mSharedMapped[slot] = true;
        void* src = nullptr; size_t bytes = 0;
        bool ok = CudaOk(cudaGraphicsResourceGetMappedPointer(&src, &bytes, resource),
            "cudaGraphicsResourceGetMappedPointer", error);
        if (ok && (!src || bytes < mInCount * sizeof(float)))
        {
            error = "mapped shared input buffer is invalid or too small"; ok = false;
        }
        if (ok)
            ok = CudaOk(cudaMemcpyAsync(mDevIn, src, mInCount * sizeof(float),
                cudaMemcpyDeviceToDevice, mStream), "cudaMemcpyAsync D2D", error);

        // Keep a stable TensorRT input address so the existing CUDA Graph
        // remains reusable for both D3D slots. This is GPU-only, NOT zero-copy.
        const cudaError_t unmap = cudaGraphicsUnmapResources(1, &resource, mStream);
        if (unmap == cudaSuccess) mSharedMapped[slot] = false;
        if (!ok || !CudaOk(unmap, "cudaGraphicsUnmapResources", error))
        {
            cudaStreamSynchronize(mStream); mInteropHealthy = false;
            return false;
        }
        // The other D3D slot may be prepared while this inference executes.
        // Launch synchronizes the stream before returning success.
        if (!Launch(out, channels, anchors, error))
        {
            cudaStreamSynchronize(mStream); mInteropHealthy = false;
            return false;
        }
        return true;
    }

#endif // AIM_ENABLE_D3D_INTEROP

private:
    // Запуск сети. Вход к этому моменту уже лежит в mDevIn.
    bool Launch(
        std::vector<float>* out,
        int& channels,
        int& anchors,
        std::string& error)
    {
        if (mGraphReady)
        {
            if (!CudaOk(cudaGraphLaunch(mGraphExec, mStream),
                        "cudaGraphLaunch", error))
            {
                return false;
            }
        }
        else if (!mContext->enqueueV3(mStream))
        {
            error = "TensorRT enqueueV3 failed";
            return false;
        }

        if(mGraphReady && mGraphIncludesOutput) return FinishOutput(out,channels,anchors,error);
        RecordStage(2);
        if (!CudaOk(cudaMemcpyAsync(
                        mHostOut,
                        mDevOut,
                        mOutCount * sizeof(float),
                        cudaMemcpyDeviceToHost,
                        mStream),
                    "cudaMemcpyAsync D2H", error))
        {
            return false;
        }

        RecordStage(3);
        return FinishOutput(out,channels,anchors,error);
    }

    bool FinishOutput(std::vector<float>* out,int& channels,int& anchors,std::string& error) {
        const double waitStart=AimBackendNowMs();
        cudaError_t completion = cudaErrorNotReady;
        if(mSpinCompletion) {
            // Optional, measured low-latency policy. Never waits forever on the CPU.
            // The slow/error path retains CUDA's original synchronization semantics.
            unsigned polls=0;
            do {
                completion=cudaStreamQuery(mStream);
                if(completion!=cudaErrorNotReady) break;
                YieldProcessor();
                ++polls;
            } while((polls & 63u)!=0 || AimBackendNowMs()-waitStart<2.0);
        }
        if(completion==cudaErrorNotReady) completion=cudaStreamSynchronize(mStream);
        if(!CudaOk(completion,"CUDA inference completion",error)) return false;
        mStageTiming.waitMs=AimBackendNowMs()-waitStart;
        if(mProfileRun) {
            float ms=0;
            if(cudaEventElapsedTime(&ms,mStageEvents[0],mStageEvents[1])==cudaSuccess) mStageTiming.h2dGpuMs=ms;
            if(cudaEventElapsedTime(&ms,mStageEvents[1],mStageEvents[2])==cudaSuccess) mStageTiming.computeGpuMs=ms;
            if(cudaEventElapsedTime(&ms,mStageEvents[2],mStageEvents[3])==cudaSuccess) mStageTiming.d2hGpuMs=ms;
        }
        // Decoder can read the completed pinned D2H buffer directly. Preserve
        // the old owning API for other callers and ALBION_COPY_OUTPUT=1.
        mStageTiming.hostOutputMs = 0.0;
        if (out) {
            const double copyStart=AimBackendNowMs();
            out->resize(mOutCount);
            std::memcpy(out->data(), mHostOut, mOutCount * sizeof(float));
            mStageTiming.hostOutputMs=AimBackendNowMs()-copyStart;
        }

        channels = mChannels;
        anchors = mAnchors;
        return true;
    }

    void RecordStage(size_t i) noexcept {
        if(mProfileRun && cudaEventRecord(mStageEvents[i],mStream)!=cudaSuccess) mProfileRun=false;
    }
    bool CreateIoGraph() noexcept {
        // Keep timing events outside stream capture; profile and IO graph are separate experiments.
        if(mProfile) return false;
        // Buffers/addresses/context are persistent. Never used by RunSharedSlot.
        // Existing compute graph remains the fallback and the shared-input path.
        if(cudaStreamBeginCapture(mStream,cudaStreamCaptureModeThreadLocal)!=cudaSuccess) return false;
        mProfileRun=mProfile;
        RecordStage(0);
        bool ok=cudaMemcpyAsync(mDevIn,mHostIn,mInCount*sizeof(float),cudaMemcpyHostToDevice,mStream)==cudaSuccess;
        RecordStage(1);
        if(ok) ok=mContext->enqueueV3(mStream);
        RecordStage(2);
        if(ok) ok=cudaMemcpyAsync(mHostOut,mDevOut,mOutCount*sizeof(float),cudaMemcpyDeviceToHost,mStream)==cudaSuccess;
        RecordStage(3);
        cudaGraph_t graph=nullptr;
        if(mProfile && !mProfileRun) ok=false;
        const auto ended=cudaStreamEndCapture(mStream,&graph);
        if(!ok || ended!=cudaSuccess || !graph) {if(graph)cudaGraphDestroy(graph);return false;}
        cudaGraphExec_t exec=nullptr;
        if(cudaGraphInstantiate(&exec,graph,nullptr,nullptr,0)!=cudaSuccess || !exec) {
            cudaGraphDestroy(graph);return false;
        }
        mIoGraph=graph;mIoGraphExec=exec;return true;
    }

    static bool CudaOk(cudaError_t status, const char* operation,
                       std::string& error)
    {
        if (status == cudaSuccess)
            return true;

        error = std::string(operation) + " failed: " + cudaGetErrorString(status);
        return false;
    }

    bool CreateCudaGraph() noexcept
    {
        mGraphIncludesOutput=false;
        const bool captureOutput=!mProfile && AimPerfEnvFlag(L"ALBION_OUTPUT_GRAPH");
        // Первый enqueue может выполнять ленивую инициализацию, запрещённую
        // во время stream capture. Поэтому сначала прогреваем тот же context.
        constexpr int kWarmupRuns = 10;
        for (int i = 0; i < kWarmupRuns; ++i)
        {
            if (!mContext->enqueueV3(mStream) ||
                cudaStreamSynchronize(mStream) != cudaSuccess)
            {
                return false;
            }
        }

        if (cudaStreamBeginCapture(
                mStream,
                cudaStreamCaptureModeThreadLocal) != cudaSuccess)
        {
            return false;
        }

        if (!mContext->enqueueV3(mStream))
        {
            cudaGraph_t failedGraph = nullptr;
            cudaStreamEndCapture(mStream, &failedGraph);
            if (failedGraph)
                cudaGraphDestroy(failedGraph);
            return false;
        }

        if(captureOutput && cudaMemcpyAsync(mHostOut,mDevOut,mOutCount*sizeof(float),cudaMemcpyDeviceToHost,mStream)!=cudaSuccess) {
            cudaGraph_t failed=nullptr;cudaStreamEndCapture(mStream,&failed);if(failed)cudaGraphDestroy(failed);return false;
        }
        cudaGraph_t graph = nullptr;
        if (cudaStreamEndCapture(mStream, &graph) != cudaSuccess || !graph)
        {
            if (graph)
                cudaGraphDestroy(graph);
            return false;
        }

        cudaGraphExec_t graphExec = nullptr;
        if (cudaGraphInstantiate(&graphExec, graph, nullptr, nullptr, 0) != cudaSuccess ||
            !graphExec)
        {
            cudaGraphDestroy(graph);
            return false;
        }

        mGraph = graph;
        mGraphExec = graphExec;
        mGraphIncludesOutput=captureOutput;
        return true;
    }

    static bool ReadWholeFile(const std::wstring& path, std::vector<char>& blob)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
            return false;

        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size <= 0)
        {
            fclose(f);
            return false;
        }

        blob.resize(size_t(size));
        const size_t got = fread(blob.data(), 1, blob.size(), f);
        fclose(f);
        return got == blob.size();
    }

    TrtLogger mLogger;
    nvinfer1::IRuntime* mRuntime = nullptr;
    nvinfer1::ICudaEngine* mEngine = nullptr;
    nvinfer1::IExecutionContext* mContext = nullptr;

    cudaStream_t mStream = nullptr;
    void* mDevIn = nullptr;
    void* mDevOut = nullptr;
    float* mHostIn = nullptr;
    float* mHostOut = nullptr;

    cudaGraph_t mGraph = nullptr;
    cudaGraphExec_t mGraphExec = nullptr;
    bool mGraphReady = false;
    bool mGraphIncludesOutput = false;
    cudaGraph_t mIoGraph=nullptr;
    cudaGraphExec_t mIoGraphExec=nullptr;
    bool mIoGraphReady=false,mProfile=false,mProfileRun=false,mSpinCompletion=false;
    std::array<cudaEvent_t,4> mStageEvents{};
    BackendStageTiming mStageTiming{};

#ifdef AIM_ENABLE_D3D_INTEROP
    std::array<cudaGraphicsResource*, 2> mSharedRes{};
    std::array<bool, 2> mSharedMapped{};
    size_t mSharedCount = 0;
    bool mInteropHealthy = false;
#endif

    std::string mInName;
    std::string mOutName;

    int mInputSide = 0;
    size_t mInCount = 0;
    size_t mOutCount = 0;
    int mChannels = 0;
    int mAnchors = 0;
};

#endif // AIM_ENABLE_TRT

// ---------------------------------------------------------------------------
// ONNX Runtime: .onnx на GPU (TensorRT EP -> CUDA EP -> DirectML EP -> CPU)
// ---------------------------------------------------------------------------

#ifdef AIM_ENABLE_ORT

#include <onnxruntime_cxx_api.h>

#ifdef AIM_ENABLE_DML
#include <dml_provider_factory.h>
#endif

#pragma comment(lib, "onnxruntime.lib")

class OrtBackend : public IInferenceBackend
{
public:
    bool Init(const std::wstring& path, int netSize, std::string& error)
    {
        mNetSize = netSize;

        try
        {
            mEnv = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "aim");

            Ort::SessionOptions so;
            so.SetIntraOpNumThreads(1);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            const char* chosen = nullptr;

            // 1) TensorRT EP. Первый запуск долгий (строится engine),
            //    дальше берётся из кэша в папке trt_cache.
            if (TryAppendTensorRT(so))
            {
                chosen = "ORT/TensorRT";
            }
            else
            {
                // 2) CUDA EP.
                try
                {
                    OrtCUDAProviderOptions cuda{};
                    cuda.device_id = 0;
                    cuda.arena_extend_strategy = 0;
                    cuda.do_copy_in_default_stream = 1;

                    so.AppendExecutionProvider_CUDA(cuda);
                    chosen = "ORT/CUDA";
                }
                catch (const Ort::Exception&)
                {
                    chosen = nullptr;
                }
            }

#ifdef AIM_ENABLE_DML
            // 3) DirectML EP — работает и без CUDA, на любой видеокарте.
            if (!chosen)
            {
                so.DisableMemPattern();
                so.SetExecutionMode(ORT_SEQUENTIAL);

                OrtStatus* dmlStatus = OrtSessionOptionsAppendExecutionProvider_DML(so, 0);
                if (!dmlStatus) chosen = "ORT/DirectML";
                else Ort::GetApi().ReleaseStatus(dmlStatus);
            }
#endif

            mName = chosen ? chosen : "ORT/CPU";

            mSession = Ort::Session(mEnv, path.c_str(), so);

            Ort::AllocatorWithDefaultOptions alloc;

            mInName = mSession.GetInputNameAllocated(0, alloc).get();
            mOutName = mSession.GetOutputNameAllocated(0, alloc).get();

            mMemInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

            return true;
        }
        catch (const Ort::Exception& e)
        {
            error = e.what();
            return false;
        }
    }

    const char* Name() const noexcept override
    {
        return mName;
    }

    bool Run(
        const float* input,
        std::vector<float>& out,
        int& channels,
        int& anchors,
        std::string& error) override
    {
        try
        {
            const int64_t shape[4] = { 1, 3, mNetSize, mNetSize };
            const size_t count = size_t(3) * size_t(mNetSize) * size_t(mNetSize);

            Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                mMemInfo,
                const_cast<float*>(input),
                count,
                shape,
                4
            );

            const char* inNames[] = { mInName.c_str() };
            const char* outNames[] = { mOutName.c_str() };

            auto outputs = mSession.Run(
                Ort::RunOptions{ nullptr },
                inNames,
                &inTensor,
                1,
                outNames,
                1
            );

            if (outputs.empty() || !outputs[0].IsTensor())
            {
                error = "Модель не вернула тензор";
                return false;
            }

            auto info = outputs[0].GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();

            if (dims.size() != 3)
            {
                error = "Ожидался выход YOLOv8 вида (1, channels, anchors)";
                return false;
            }

            channels = int(dims[1]);
            anchors = int(dims[2]);

            const size_t total = info.GetElementCount();
            const float* src = outputs[0].GetTensorData<float>();

            out.resize(total);
            std::memcpy(out.data(), src, total * sizeof(float));

            return true;
        }
        catch (const Ort::Exception& e)
        {
            error = e.what();
            return false;
        }
    }

private:
    static bool TryAppendTensorRT(Ort::SessionOptions& so)
    {
        const OrtApi& api = Ort::GetApi();

        OrtTensorRTProviderOptionsV2* trt = nullptr;

        OrtStatus* status = api.CreateTensorRTProviderOptions(&trt);
        if (status != nullptr)
        {
            api.ReleaseStatus(status);
            return false;
        }

        const char* keys[] = {
            "device_id",
            "trt_fp16_enable",
            "trt_engine_cache_enable",
            "trt_engine_cache_path",
            "trt_timing_cache_enable"
        };

        const char* vals[] = {
            "0",
            "1",
            "1",
            "trt_cache",
            "1"
        };

        bool ok = false;

        status = api.UpdateTensorRTProviderOptions(trt, keys, vals, 5);
        if (status == nullptr)
        {
            status = api.SessionOptionsAppendExecutionProvider_TensorRT_V2(
                static_cast<OrtSessionOptions*>(so),
                trt
            );

            if (status == nullptr)
            {
                ok = true;
            }
            else
            {
                api.ReleaseStatus(status);
            }
        }
        else
        {
            api.ReleaseStatus(status);
        }

        api.ReleaseTensorRTProviderOptions(trt);

        return ok;
    }

    Ort::Env mEnv{ nullptr };
    Ort::Session mSession{ nullptr };
    Ort::MemoryInfo mMemInfo{ nullptr };

    std::string mInName;
    std::string mOutName;
    const char* mName = "ORT/CPU";
    int mNetSize = 640;
};

#endif // AIM_ENABLE_ORT

// ---------------------------------------------------------------------------
// Фабрика
// ---------------------------------------------------------------------------

inline std::unique_ptr<IInferenceBackend> CreateInferenceBackend(
    const std::wstring& modelPath,
    int netSize,
    std::string& error)
{
    error.clear();

    if (AimIsEngineFile(modelPath))
    {
#ifdef AIM_ENABLE_TRT
        if (!AimTensorRtRuntimeAvailable(error)) return nullptr;
        auto trt = std::make_unique<TrtBackend>();

        if (trt->Init(modelPath, netSize, error))
            return trt;

        return nullptr;
#else
        error =
            "Файл .engine требует сборки с макросом AIM_ENABLE_TRT и библиотеками TensorRT";
        return nullptr;
#endif
    }

#ifdef AIM_ENABLE_ORT
    auto ort = std::make_unique<OrtBackend>();

    if (ort->Init(modelPath, netSize, error))
        return ort;

    return nullptr;
#else
    (void)netSize;
    return nullptr; // откат на WinML
#endif
}
