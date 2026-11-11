// modules/core/utilities.hpp
#pragma once
#include "modules/core/platform.hpp"
#include "modules/core/runtime.hpp"
inline void HR(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[32];
        sprintf_s(buf, "0x%08X", unsigned(hr));
        throw std::runtime_error(std::string(what) + " failed, hr=" + buf);
    }
}

inline std::wstring ExeRelativePath(const wchar_t* name)
{
    // MAX_PATH is not enough for long-path-aware applications. Grow the
    // buffer until the complete executable path is returned.
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (length == 0) return name ? std::wstring(name) : std::wstring();
        if (length < buffer.size() - 1)
        {
            std::wstring path(buffer.data(), length);
            const size_t slash = path.find_last_of(L"\\/");
            path.resize(slash == std::wstring::npos ? 0 : slash + 1);
            return path + (name ? name : L"");
        }
        if (buffer.size() >= 32768) return name ? std::wstring(name) : std::wstring();
        buffer.resize(std::min<size_t>(buffer.size() * 2, 32768));
    }
}

inline std::wstring ModelRelativePath(const wchar_t* name)
{
    std::wstring path = ExeRelativePath(L"models");
    if (name && *name)
    {
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
            path += L'\\';
        path += name;
    }
    return path;
}

inline std::wstring PickModelPath()
{
    // Prefer original ONNX when no saved profile has selected a model.
    for (const wchar_t* mask : {L"*.onnx", L"*.engine", L"*.plan", L"*.trt"}) {
        WIN32_FIND_DATAW fd{}; HANDLE h=FindFirstFileW(ModelRelativePath(mask).c_str(), &fd);
        if(h!=INVALID_HANDLE_VALUE) { FindClose(h); if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return ModelRelativePath(fd.cFileName); }
    }
    return ModelRelativePath(L"model.onnx");
}

// --- Sohranenie/zagruzka konfigov (prostoy tekstovyy format key=value) ---

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    if (s.size() > size_t((std::numeric_limits<int>::max)())) return {};
    const int inputLength = static_cast<int>(s.size());
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        s.data(), inputLength, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(size_t(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        s.data(), inputLength, result.data(), len) != len) return {};
    return result;
}

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    if (w.size() > size_t((std::numeric_limits<int>::max)())) return {};
    const int inputLength = static_cast<int>(w.size());
    const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        w.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(size_t(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        w.data(), inputLength, result.data(), len, nullptr, nullptr) != len) return {};
    return result;
}

inline bool WriteTextFileUtf8(const std::wstring& path, const std::string& text)
{
    // FIX-V11-11: unique temp + destination lock; implementation is independently testable.
    return albion::WriteAtomic(path, text);
}

inline bool ReadTextFileUtf8(const std::wstring& path, std::string& out)
{
    out.clear();
    albion::Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h.Valid()) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.Get(), &size) || size.QuadPart < 0 || size.QuadPart > 4 * 1024 * 1024)
    {
        return false;
    }
    out.resize(size_t(size.QuadPart));
    size_t offset = 0;
    bool ok = true;
    while (offset < out.size())
    {
        const DWORD chunk = DWORD(std::min<size_t>(out.size() - offset, 1u << 20));
        DWORD readBytes = 0;
        if (!ReadFile(h.Get(), out.data() + offset, chunk, &readBytes, nullptr) || readBytes == 0)
        {
            ok = false;
            break;
        }
        offset += readBytes;
    }
    if (!ok) out.clear();
    return ok;
}

inline bool AppendTextFileUtf8(const std::wstring& path, const std::string& text)
{
    return albion::AppendBoundedLog(path, text);
}

