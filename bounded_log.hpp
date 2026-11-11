#pragma once
// FIX-V11-16: Cap each log at 8 MiB plus two rotated files. File access is
// non-waiting across processes; diagnostics can drop a message, never hang UI.
#include <windows.h>
#include <filesystem>
#include <string>
#include "atomic_file.hpp"
namespace albion {
inline bool AppendBoundedLog(const std::wstring& path, const std::string& text) noexcept {
    try {
        if (text.size() > 1024u * 1024u) return false;
        Handle lock(OpenFileLock(path));
        if (!lock.Valid()) return false;
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        // Permission/metadata errors are not an empty log: fail closed.
        if (ec && ec != std::errc::no_such_file_or_directory) return false;
        if (!ec && size > 8u * 1024u * 1024u - text.size()) {
            std::filesystem::remove(path + L".2", ec); ec.clear();
            if (std::filesystem::exists(path + L".1", ec)) {
                ec.clear(); std::filesystem::rename(path + L".1", path + L".2", ec); if (ec) return false;
            }
            ec.clear(); std::filesystem::rename(path, path + L".1", ec); if (ec) return false;
        }
        Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.Valid()) return false;
        DWORD n = 0; return WriteFile(file.Get(), text.data(), DWORD(text.size()), &n, nullptr) && n == text.size();
    } catch (...) { return false; }
}
} // namespace albion
