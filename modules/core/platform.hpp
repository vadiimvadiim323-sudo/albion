// modules/core/platform.hpp
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include "auth_gate.hpp"
#include "ui_fonts.hpp"
#include "startup_splash.hpp"
#include <windowsx.h>
#include <unknwn.h>
#include <objidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <d3d11_1.h>
#include <d3d10.h>          // ID3D10Multithread
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "desktop_srv_cache.hpp"
#include "pipeline_wait.hpp"
#include "config_document.hpp"
#include "config_repository.hpp"
#include "win32_overlapped_io.hpp"
#include "worker_guard.hpp"
#include "app_paths.hpp"
#include "variant.hpp"
#include "async_tasks.hpp"
#include "license_cache.hpp"
#include "bounded_log.hpp"
#include "diagnostics_panel.hpp"
#include "performance_panel.hpp"
#include "onnx_converter.hpp"
#include "conversion_dialog.hpp"
// FIX-V11.1-BUILD: DevLog is used by Discord/NVML code below; 1.0.0/1.0.1 got
// it only through gpu_infer.hpp (line ~1011) -> MSVC C3861 "DevLog not found".
#include "dev_diagnostics.hpp"
#include <shellapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.AI.MachineLearning.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using Microsoft::WRL::ComPtr;
namespace WinML = winrt::Windows::AI::MachineLearning;

struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable)
    IMemoryBufferByteAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

