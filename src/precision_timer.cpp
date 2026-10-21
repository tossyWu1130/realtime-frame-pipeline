#include "precision_timer.h"

#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod / timeEndPeriod (winmm)
#endif

namespace {

#if defined(_WIN32)
// One timer handle per waiting thread. Created once, reused at 100 ticks per second.
thread_local HANDLE g_waitTimer = nullptr;

HANDLE waitTimer()
{
    if (g_waitTimer != nullptr) {
        return g_waitTimer;
    }

    // Windows 10 1803+ accepts a high-resolution timer, which is not subject to the ~15.6 ms
    // system tick. Fall back to an ordinary waitable timer on older systems.
    g_waitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (g_waitTimer == nullptr) {
        g_waitTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    return g_waitTimer;
}
#endif

}  // namespace

void PrecisionTimer::raiseTimerResolution()
{
#if defined(_WIN32)
    // Kept as a fallback for platforms where the high-resolution timer is unavailable. It does
    // not always take effect (job objects, power policy), which is why waitUntil does not rely
    // on it.
    timeBeginPeriod(1);
#endif
}

void PrecisionTimer::restoreTimerResolution()
{
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

void PrecisionTimer::configureTimerThread()
{
#if defined(_WIN32)
    // A timing thread that competes at normal priority loses its wake-up to ordinary desktop
    // work. This is the priority a control loop would run at, not a hard real-time class.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

bool PrecisionTimer::waitUntil(Clock::time_point deadline)
{
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return true;
    }

#if defined(_WIN32)
    HANDLE timer = waitTimer();
    if (timer == nullptr) {
        std::this_thread::sleep_for(remaining);
        return true;
    }

    // SetWaitableTimer takes a relative due time in 100 ns units, negative meaning "from now".
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    if (due.QuadPart == 0) {
        due.QuadPart = -1;
    }

    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        std::this_thread::sleep_for(remaining);
        return true;
    }

    WaitForSingleObject(timer, INFINITE);
    return true;
#else
    std::this_thread::sleep_until(deadline);
    return true;
#endif
}
