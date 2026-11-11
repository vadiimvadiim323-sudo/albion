#pragma once
// FIX-V11-02/10: A deadline is meaningful only on an OVERLAPPED handle.
// Cancellation must be DRAINED before releasing OVERLAPPED/event/buffer memory.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include "atomic_file.hpp"
namespace albion {
struct IoResult { bool ok = false; DWORD bytes = 0, error = ERROR_SUCCESS; };
inline IoResult Transfer(HANDLE file, void* data, DWORD size, bool write,
    DWORD timeoutMs, const std::atomic<bool>* cancelled = nullptr) noexcept {
    IoResult r;
    if (cancelled && cancelled->load(std::memory_order_acquire)) { r.error = ERROR_OPERATION_ABORTED; return r; }
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.Valid()) { r.error = GetLastError(); return r; }
    OVERLAPPED operation{}; operation.hEvent = event.Get();
    const BOOL started = write ? WriteFile(file, data, size, nullptr, &operation)
                              : ReadFile(file, data, size, nullptr, &operation);
    if (!started && GetLastError() != ERROR_IO_PENDING) { r.error = GetLastError(); return r; }
    const ULONGLONG begin = GetTickCount64();
    bool abort = false;
    if (!started) for (;;) {
        if (cancelled && cancelled->load(std::memory_order_acquire)) {
            r.error = ERROR_OPERATION_ABORTED; abort = true; break;
        }
        const ULONGLONG elapsed = GetTickCount64() - begin;
        if (elapsed >= timeoutMs) { r.error = ERROR_TIMEOUT; abort = true; break; }
        const DWORD wait = WaitForSingleObject(event.Get(), DWORD((std::min<ULONGLONG>)(20, timeoutMs - elapsed)));
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) { r.error = GetLastError(); abort = true; break; }
    }
    if (abort) {
        CancelIoEx(file, &operation);
        DWORD drained = 0;
        // Even ERROR_NOT_FOUND may mean completion raced cancellation. NEVER
        // return with a driver still referencing a stack OVERLAPPED or buffer.
        // A broken kernel driver can delay cancellation; no unsafe detach/free.
        // FIX-V11.1-IO-RACE: if the drained operation actually COMPLETED, report
        // the real result. Reporting a delivered HID/pipe write as "timeout"
        // made callers disconnect after a successful transfer.
        if (GetOverlappedResult(file, &operation, &drained, TRUE)) {
            r.ok = true; r.bytes = drained; r.error = ERROR_SUCCESS;
        }
        return r;
    }
    r.ok = GetOverlappedResult(file, &operation, &r.bytes, FALSE) != FALSE;
    if (!r.ok) r.error = GetLastError();
    return r;
}
inline IoResult Write(HANDLE f, const void* p, DWORD n, DWORD ms,
    const std::atomic<bool>* stop = nullptr) noexcept {
    return Transfer(f, const_cast<void*>(p), n, true, ms, stop);
}
inline IoResult Read(HANDLE f, void* p, DWORD n, DWORD ms,
    const std::atomic<bool>* stop = nullptr) noexcept { return Transfer(f, p, n, false, ms, stop); }
} // namespace albion
