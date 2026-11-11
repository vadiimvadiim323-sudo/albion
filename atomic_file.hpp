#pragma once
// FIX-V11-11: Unique CREATE_NEW siblings, serialized destination replacement.
// A second writer can no longer overwrite the first writer's temporary file.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

namespace albion {
class Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : value_(h) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    HANDLE Release() noexcept { return std::exchange(value_, INVALID_HANDLE_VALUE); }
    bool Valid() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (Valid()) CloseHandle(value_);
        value_ = h;
    }
};
// FIX-V11.1-LOCKFILES: one helper for every non-waiting destination lock.
// DELETE_ON_CLOSE removes the sidecar when the owner closes it (or the process
// dies), so exports/configs no longer leave "*.lock" files next to user data.
// A stale lock file from 1.0.0 is opened, flagged and removed on first use.
inline HANDLE OpenFileLock(const std::wstring& path) noexcept {
    try {
        return CreateFileW((path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
}
inline std::wstring UniqueSibling(const std::wstring& path) {
    static std::atomic<unsigned long long> sequence{0};
    LARGE_INTEGER clock{}; QueryPerformanceCounter(&clock);
    return path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(clock.QuadPart) + L"." + std::to_wstring(sequence.fetch_add(1));
}
inline bool WriteAtomicUnlocked(const std::wstring& path, const std::string& text, bool replace = true) {
    std::wstring temp;
    Handle file;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temp = UniqueSibling(path);
        file.Reset(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.Valid()) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (!file.Valid()) return false;
    bool ok = true; DWORD error = ERROR_SUCCESS;
    for (size_t offset = 0; offset < text.size();) {
        const DWORD n = DWORD((std::min<size_t>)(text.size() - offset, 1u << 20)); DWORD done = 0;
        if (!WriteFile(file.Get(), text.data() + offset, n, &done, nullptr) || !done || done > n) {
            error = GetLastError(); if (!error) error = ERROR_WRITE_FAULT; ok = false; break;
        }
        offset += done;
    }
    if (ok && !FlushFileBuffers(file.Get())) { error = GetLastError(); ok = false; }
    file.Reset();
    if (ok && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) {
        error = GetLastError(); ok = false;
    }
    if (!ok) { DeleteFileW(temp.c_str()); SetLastError(error); }
    return ok;
}
inline bool WriteAtomic(const std::wstring& path, const std::string& text) {
    // The lock is held through close+rename; it is released automatically if a
    // process exits, and the sidecar file is deleted on close.
    Handle lock(OpenFileLock(path));
    return lock.Valid() && WriteAtomicUnlocked(path, text);
}
} // namespace albion
