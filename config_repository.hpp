#pragma once
// FEATURE-V11-HISTORY: Keep the previous file before replacement. The same
// destination lock covers history+replace; a failed history write aborts save.
// History is bounded to 20 prior snapshots per profile, not an unlimited log.
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
#include "atomic_file.hpp"
namespace albion {
inline bool ReadSmallFile(const std::filesystem::path& path, std::string& text) {
    text.clear(); std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 4u * 1024u * 1024u) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    text.resize(size_t(size));
    if (size && !file.read(text.data(), std::streamsize(size))) { text.clear(); return false; }
    // A concurrent direct writer must not be mistaken for the checked snapshot.
    if (file.peek() != std::char_traits<char>::eof()) { text.clear(); return false; }
    return true;
}
// FIX-V11.1-REPARSE: MSVC std::filesystem::is_symlink() does not report NTFS
// junctions/mount points. Treat ANY reparse point as unsafe for history paths.
inline bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    try {
        const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    } catch (...) { return true; }
}
inline bool SamePathIgnoringCase(const std::wstring& a, const std::wstring& b) noexcept {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}
inline std::filesystem::path HistoryDirectory(const std::filesystem::path& path) {
    return path.parent_path() / L".history" / path.filename();
}
inline bool SafeHistoryDirectory(const std::filesystem::path& path, bool create) {
    const auto folder = HistoryDirectory(path);
    // Do not follow a replaced .history/profile directory out of the data root.
    std::error_code ec;
    for (const auto& item : {folder.parent_path(), folder}) {
        if (std::filesystem::is_symlink(item, ec) || IsReparsePoint(item)) return false;
        ec.clear();
    }
    if (create) std::filesystem::create_directories(folder);
    return true;
}
inline bool IsHistorySnapshot(const std::filesystem::path& snapshot, const std::filesystem::path& profile) noexcept {
    try {
        std::error_code ec;
        return SafeHistoryDirectory(profile, false) && snapshot.extension() == L".cfg" &&
            !std::filesystem::is_symlink(snapshot, ec) && !ec && !IsReparsePoint(snapshot) &&
            std::filesystem::is_regular_file(snapshot, ec) && !ec &&
            std::filesystem::equivalent(snapshot.parent_path(), HistoryDirectory(profile), ec) && !ec;
    } catch (...) { return false; }
}
inline std::vector<std::filesystem::path> ConfigHistory(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    if (!SafeHistoryDirectory(path, false)) return found;
    const auto folder = HistoryDirectory(path);
    if (!std::filesystem::exists(folder, ec)) return found;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (!entry.is_symlink() && !IsReparsePoint(entry.path()) && entry.is_regular_file() &&
            entry.path().extension() == L".cfg")
            found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end(), std::greater<std::filesystem::path>());
    return found;
}
inline bool PruneConfigHistory(const std::filesystem::path& path) {
    auto history = ConfigHistory(path);
    for (size_t i = 20; i < history.size(); ++i) {
        std::error_code ec;
        if (!std::filesystem::remove(history[i], ec) || ec) return false;
    }
    return true;
}
inline bool SnapshotConfigUnlocked(const std::filesystem::path& path, const std::string& previous) {
    if (!SafeHistoryDirectory(path, true)) return false;
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    const unsigned long long tick = (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    // Fixed-width time/order fields keep lexical newest-first ordering stable
    // even for more than nine saves within the same system-clock timestamp.
    static std::atomic<unsigned long long> sequence{0};
    std::wostringstream name; name.imbue(std::locale::classic());
    name << std::setfill(L'0') << std::setw(20) << tick << L"-"
         << std::setw(20) << sequence.fetch_add(1) << L"-" << GetCurrentProcessId() << L".cfg";
    const auto snapshot = HistoryDirectory(path) / name.str();
    // The destination lock serializes writers; unique PID+sequence names avoid
    // replacing a snapshot produced by another process at the same clock tick.
    if (!WriteAtomicUnlocked(snapshot.wstring(), previous, false)) return false;
    // Prune even when the following destination replacement later fails.
    return PruneConfigHistory(path);
}
inline bool SaveConfigWithHistory(const std::wstring& path, const std::string& text) noexcept {
    try {
        if (text.empty() || text.size() > 4u * 1024u * 1024u) return false;
        Handle lock(OpenFileLock(path));
        if (!lock.Valid()) return false;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::string previous;
            if (!ReadSmallFile(path, previous)) return false;
            if (previous == text) return true;
            if (!SnapshotConfigUnlocked(path, previous)) return false;
        } else if (ec) return false;
        return WriteAtomicUnlocked(path, text);
    } catch (...) { return false; }
}
inline bool DeleteConfigWithHistory(const std::wstring& path) noexcept {
    try {
        Handle lock(OpenFileLock(path));
        std::string previous;
        // Deletion is reversible through F8 -> Restore; never erase history.
        return lock.Valid() && ReadSmallFile(path, previous) &&
            SnapshotConfigUnlocked(path, previous) && DeleteFileW(path.c_str());
    } catch (...) { return false; }
}
inline bool RenameConfigWithHistory(const std::wstring& from, const std::wstring& to,
    bool& historyMoved) noexcept {
    historyMoved = false;
    bool fileMoved = false;
    try {
        // Allocate both history paths BEFORE committing the file rename.
        const auto oldFolder = HistoryDirectory(from), newFolder = HistoryDirectory(to);
        // FIX-V11.1-CASE-RENAME: "main" -> "Main" names the SAME NTFS file. Two
        // exclusive locks on one case-insensitive name always failed before.
        const bool caseOnly = SamePathIgnoringCase(from, to);
        Handle first(OpenFileLock(from));
        Handle second(caseOnly ? INVALID_HANDLE_VALUE : OpenFileLock(to));
        if (!first.Valid() || (!caseOnly && !second.Valid()) || !MoveFileW(from.c_str(), to.c_str())) return false;
        fileMoved = true;
        std::error_code ec;
        if (!std::filesystem::exists(oldFolder, ec)) historyMoved = !ec;
        else if (SafeHistoryDirectory(from, false) && SafeHistoryDirectory(to, false) &&
            (caseOnly || !std::filesystem::exists(newFolder, ec))) {
            // MoveFileW also performs a case-only directory rename in place.
            historyMoved = MoveFileW(oldFolder.wstring().c_str(), newFolder.wstring().c_str()) != FALSE;
        }
        // Two filesystem names cannot be renamed as one atomic transaction.
        // A history collision/failure leaves the OLD history intact and produces
        // a visible warning; do not risk a destructive multi-step rollback.
        return true;
    } catch (...) {
        // If history metadata allocation failed AFTER MoveFileW, report the
        // successful rename plus historyMoved=false, not a phantom failure.
        return fileMoved;
    }
}
inline void ImportLegacyConfigs(const std::filesystem::path& oldFolder, const std::filesystem::path& folder) noexcept try {
    if (oldFolder == folder) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(oldFolder, ec)) return;
    // FIX-V11.1-MIGRATION: range-for ++ throws on a directory error; migration
    // is best effort and must never abort startup or the configs screen.
    std::filesystem::directory_iterator it(oldFolder, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        auto extension = entry.path().extension().wstring();
        for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        std::error_code entryError; // per-entry errors skip one file, not the scan
        if (!entry.is_symlink(entryError) && !entryError && entry.is_regular_file(entryError) && !entryError && extension == L".cfg") {
            std::string text;
            if (ReadSmallFile(entry.path(), text)) {
                const auto target = folder / entry.path().filename();
                // Never overwrite an existing user profile during migration.
                Handle file(CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (file.Valid()) {
                    DWORD n = 0; const bool ok = WriteFile(file.Get(), text.data(), DWORD(text.size()), &n, nullptr) && n == text.size() && FlushFileBuffers(file.Get());
                    file.Reset(); if (!ok) DeleteFileW(target.c_str());
                }
            }
        }
    }
}
catch (...) {}
} // namespace albion
