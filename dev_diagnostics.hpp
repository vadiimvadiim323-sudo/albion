#pragma once
// FIX-V11-16 / FEATURE-V11-DIAG: bounded asynchronous logging. No frames,
// keystrokes, license values or per-command deltas are intentionally recorded.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include "app_paths.hpp"
#include "async_tasks.hpp"
#include "bounded_log.hpp"
inline std::atomic<unsigned long long> gDevLogDropped{0}, gDevLogFailures{0};
inline void DevInitConsole() noexcept {
    if (!GetConsoleWindow()) AllocConsole();
    SetConsoleOutputCP(CP_UTF8);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE); DWORD mode = 0;
    if (input && input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        mode |= ENABLE_EXTENDED_FLAGS; mode &= ~ENABLE_QUICK_EDIT_MODE; SetConsoleMode(input, mode);
    }
}
inline albion::TaskQueue& DevLogOwner() { static albion::TaskQueue owner; return owner; }
inline void DevShutdownLog() noexcept { try { DevLogOwner().Stop(true); } catch (...) {} }
inline void DevLog(const char* category, const char* message) noexcept {
    static std::atomic<unsigned long long> sequence{0};
    static const double qpcScale=[] {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1000.0/double(f.QuadPart);}();
    try {
        SYSTEMTIME t{}; GetLocalTime(&t); char prefix[128]{};
        std::snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] ",
            unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour),
            unsigned(t.wMinute), unsigned(t.wSecond), unsigned(t.wMilliseconds), category ? category : "?");
        LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
        char monotonic[112]{};std::snprintf(monotonic,sizeof(monotonic),"[pid=%lu seq=%llu qpc_ms=%.6f] ",GetCurrentProcessId(),++sequence,double(qpc.QuadPart)*qpcScale);
        std::string line(prefix);line+=monotonic;
        if (message) line.append(message, (std::min<size_t>)(std::char_traits<char>::length(message), 8192));
        line += "\r\n";
        if (!DevLogOwner().Post([line = std::move(line)] {
            OutputDebugStringA(line.c_str());
            if (!albion::AppendBoundedLog(AlbionDataPath(L"albion-dev-events.log"), line)) ++gDevLogFailures;
        })) ++gDevLogDropped;
    } catch (...) { ++gDevLogFailures; }
}
