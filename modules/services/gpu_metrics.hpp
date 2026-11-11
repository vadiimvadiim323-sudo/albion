// modules/services/gpu_metrics.hpp
#pragma once
#include "modules/core/utilities.hpp"
class NvmlSampler
{
    using Device = void*;
    using Return = int;
    struct Utilization { unsigned int gpu, memory; };
    struct Memory { unsigned long long total, free, used; };
    using InitFn = Return(__cdecl*)();
    using ShutdownFn = Return(__cdecl*)();
    using GetDeviceFn = Return(__cdecl*)(const char*, Device*);
    using GetUtilFn = Return(__cdecl*)(Device, Utilization*);
    using GetMemoryFn = Return(__cdecl*)(Device, Memory*);
    using GetTemperatureFn = Return(__cdecl*)(Device, unsigned int, unsigned int*);

public:
    ~NvmlSampler() { Shutdown(); }

    void Init()
    {
        if (mLibrary) return;
        mLibrary = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!mLibrary)
        {
            wchar_t path[MAX_PATH]{};
            const DWORD length = ExpandEnvironmentStringsW(
                L"%ProgramW6432%/NVIDIA Corporation/NVSMI/nvml.dll", path, MAX_PATH);
            if (length > 0 && length < MAX_PATH) mLibrary = LoadLibraryW(path);
        }
        if (!mLibrary) return;
        mInit = reinterpret_cast<InitFn>(GetProcAddress(mLibrary, "nvmlInit_v2"));
        if (!mInit) mInit = reinterpret_cast<InitFn>(GetProcAddress(mLibrary, "nvmlInit"));
        mShutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(mLibrary, "nvmlShutdown"));
        mGetDevice = reinterpret_cast<GetDeviceFn>(GetProcAddress(mLibrary, "nvmlDeviceGetHandleByUUID"));
        mGetUtil = reinterpret_cast<GetUtilFn>(GetProcAddress(mLibrary, "nvmlDeviceGetUtilizationRates"));
        mGetMemory = reinterpret_cast<GetMemoryFn>(GetProcAddress(mLibrary, "nvmlDeviceGetMemoryInfo"));
        mGetTemperature = reinterpret_cast<GetTemperatureFn>(GetProcAddress(mLibrary, "nvmlDeviceGetTemperature"));
        if (!mInit || !mGetDevice || mInit() != 0)
        {
            Shutdown();
            return;
        }
        mInitialized = true;
        char uuid[128]{};
        const DWORD length = GetEnvironmentVariableA("ALBION_NVML_GPU_UUID", uuid, DWORD(sizeof(uuid)));
        if (!length || length >= sizeof(uuid) || std::strncmp(uuid, "GPU-", 4) || mGetDevice(uuid, &mDevice) != 0) {
            // DXGI capture memory is still available. N/A is better than showing
            // believable statistics from an unrelated physical GPU.
            DevLog("NVML", "No explicit valid ALBION_NVML_GPU_UUID; use DXGI capture memory, NVML metrics=N/A"); Shutdown();
        } else DevLog("NVML", uuid);
    }

    bool Sample(double& utilization, double& usedMb, double& totalMb, double& temperature)
    {
        if (!mLibrary || !mDevice) return false;
        bool any = false;
        Utilization util{};
        if (mGetUtil && mGetUtil(mDevice, &util) == 0)
        {
            utilization = double(util.gpu);
            any = true;
        }
        Memory memory{};
        if (mGetMemory && mGetMemory(mDevice, &memory) == 0)
        {
            constexpr double bytesPerMb = 1024.0 * 1024.0;
            usedMb = double(memory.used) / bytesPerMb;
            totalMb = double(memory.total) / bytesPerMb;
            any = true;
        }
        unsigned int temp = 0;
        if (mGetTemperature && mGetTemperature(mDevice, 0, &temp) == 0)
        {
            temperature = double(temp);
            any = true;
        }
        return any;
    }

private:
    void Shutdown()
    {
        if (mLibrary && mShutdown && mInitialized) mShutdown();
        mInitialized = false;
        mDevice = nullptr;
        if (mLibrary) FreeLibrary(mLibrary);
        mLibrary = nullptr;
    }
    HMODULE mLibrary = nullptr;
    Device mDevice = nullptr;
    bool mInitialized = false;
    InitFn mInit = nullptr;
    ShutdownFn mShutdown = nullptr;
    GetDeviceFn mGetDevice = nullptr;
    GetUtilFn mGetUtil = nullptr;
    GetMemoryFn mGetMemory = nullptr;
    GetTemperatureFn mGetTemperature = nullptr;
};
