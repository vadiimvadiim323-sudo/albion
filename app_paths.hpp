#pragma once
// FIX-V11-16: Separate writable user data from immutable program/assets.
// Keys ALWAYS use LocalAppData, even when portable.flag enables portable configs.
#include <windows.h>
#include <algorithm>
#include <memory>
#include <shlobj.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

inline std::wstring AlbionProgramDirectory() {
    std::vector<wchar_t> b(512);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, b.data(), DWORD(b.size()));
        if (!n) throw std::runtime_error("Cannot locate executable");
        if (n < b.size() - 1) {
            std::wstring p(b.data(), n); const auto slash = p.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
        }
        if (b.size() >= 32768) throw std::runtime_error("Executable path too long");
        b.resize((std::min<size_t>)(b.size() * 2, 32768));
    }
}
inline std::wstring AlbionUserDirectory() {
    static const std::wstring p = [] {
        PWSTR raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)))
            throw std::runtime_error("Cannot locate LocalAppData");
        // Free the shell allocation even if filesystem/string allocation throws.
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> lease(raw, &CoTaskMemFree);
        const std::filesystem::path folder = std::filesystem::path(raw) / L"AlbionAI";
        std::filesystem::create_directories(folder); return folder.wstring();
    }();
    return p;
}
inline std::wstring AlbionDataDirectory() {
    static const std::wstring p = [] {
        const auto portable = AlbionProgramDirectory() + L"portable.flag";
        const DWORD a = GetFileAttributesW(portable.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return AlbionProgramDirectory();
        return AlbionUserDirectory() + L"\\";
    }();
    return p;
}
inline std::wstring AlbionDataPath(const wchar_t* file) { return AlbionDataDirectory() + file; }

// FEATURE-V11-ACCESS: honor the OS animation preference, with an explicit
// opt-out for remote/low-power use. This does not change capture/input math.
inline bool AlbionReducedMotion() noexcept {
    wchar_t value[8]{};
    const DWORD n = GetEnvironmentVariableW(L"ALBION_REDUCED_MOTION", value, 8);
    // FIX-V11.1-MOTION: the documented opt-out ("0") previously fell through
    // to the OS setting. 1 forces reduced motion, 0 forces animations.
    if (n == 1 && value[0] == L'1') return true;
    if (n == 1 && value[0] == L'0') return false;
    BOOL animate = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate;
}

// Capture/overlay/splash code that needs physical monitor pixels.
// (FIX-V11.1-DPI: the 1.0.0 UNAWARE_GDISCALED UI guard was removed - it made
// the UI blurry and misaligned under Windows scaling.)
class AlbionPhysicalDpi {
    DPI_AWARENESS_CONTEXT previous_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
public:
    AlbionPhysicalDpi() = default;
    AlbionPhysicalDpi(const AlbionPhysicalDpi&) = delete;
    AlbionPhysicalDpi& operator=(const AlbionPhysicalDpi&) = delete;
    ~AlbionPhysicalDpi() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
};
