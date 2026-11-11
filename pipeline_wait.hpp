#pragma once
// windows.h must be included first. No global timeBeginPeriod or busy-spin.
// Windows 10 1803+ supports high-resolution waitable timers; older systems
// retain the previous event-timeout behavior.
#include <algorithm>
#include <cmath>

inline double PipelineWaitMs(int fpsLimit, double nowMs, double nextCaptureMs) noexcept
{
    // CPU staging and activation polling still need service at least every ms.
    return fpsLimit > 0 ? std::clamp(nextCaptureMs - nowMs, 0.05, 1.0) : 1.0;
}

class PipelineActivityWaiter
{
public:
    explicit PipelineActivityWaiter(bool highResolution = true) noexcept
    {
        // Literal keeps compilation compatible with SDKs predating the flag.
        constexpr DWORD kHighResolution = 0x00000002;
        if (highResolution)
            mTimer = CreateWaitableTimerExW(nullptr, nullptr, kHighResolution,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    ~PipelineActivityWaiter() { if (mTimer) CloseHandle(mTimer); }
    PipelineActivityWaiter(const PipelineActivityWaiter&) = delete;
    PipelineActivityWaiter& operator=(const PipelineActivityWaiter&) = delete;
    bool HighResolution() const noexcept { return mTimer != nullptr; }

    void Wait(HANDLE activity, double milliseconds) noexcept
    {
        if (!std::isfinite(milliseconds) || milliseconds <= 0.0) return;
        milliseconds = std::min(milliseconds, 1000.0);
        if (mTimer)
        {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(std::ceil(milliseconds * 10000.0));
            if (SetWaitableTimer(mTimer, &due, 0, nullptr, nullptr, FALSE))
            {
                // Put activity first: a completed inference wins if both fire.
                HANDLE handles[2]{ activity, mTimer };
                const DWORD count = activity ? 2u : 1u;
                const DWORD result = WaitForMultipleObjects(count,
                    activity ? handles : handles + 1, FALSE, INFINITE);
                if (activity && result == WAIT_OBJECT_0) CancelWaitableTimer(mTimer);
                if (result != WAIT_FAILED) return;
            }
            CancelWaitableTimer(mTimer);
        }
        const DWORD timeout = static_cast<DWORD>(std::ceil(milliseconds));
        if (activity) WaitForSingleObject(activity, timeout);
        else Sleep(timeout);
    }
private:
    HANDLE mTimer = nullptr;
};