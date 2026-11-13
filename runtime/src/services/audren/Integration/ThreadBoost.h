// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Scheduling for the audio renderer's two threads (the DSP thread and the thread that paces it). Not in the Ryujinx port. The renderer has
// to deliver a 5 ms frame every 5 ms while the game's render and loading threads keep every core busy; a frame that comes late is a click
// on the sound card. The threads join the Multimedia Class Scheduler Service task "Pro Audio" (the Windows way for an audio thread to get
// the CPU on time) and the system timer runs at 1 ms while the renderer is up, so the pacing sleeps are not rounded up to 15 ms.
//
// macOS: "Pro Audio" is the user-interactive QoS class (nxwin). With [audio] realtime_band = true (off by default) the threads also join
// the real-time band, as CoreAudio's own I/O threads do (THREAD_TIME_CONSTRAINT_POLICY: a 5 ms period, one renderer frame, up to 2 ms of
// work in it, done within the period; a thread that works longer than it declared goes back to the normal band, so it cannot take the
// machine). Measured on 2026-11-10 (docs/FINDINGS.md, "Audio under load"): Ocarina of Time's title music under a load average of 63 to
// 130 starved in 2 of 5 runs either way, so it stays off until a measurement shows a gain.
#pragma once
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>

#include <atomic>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include "nxrt/config.h"
#include "nxrt/log.h"
#endif

#if defined(_WIN32)   // Windows link directives (an ELF linker such as lld would look for these libraries)
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace nxrt::audren {

#if defined(__APPLE__)
// The calling thread into the real-time band; true when the kernel took the policy.
inline bool JoinRealtimeBand() {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    const auto ms_to_abs = [&tb](double ms) {
        return static_cast<uint32_t>(ms * 1e6 * static_cast<double>(tb.denom) / static_cast<double>(tb.numer));
    };
    thread_time_constraint_policy_data_t pol{};
    pol.period = ms_to_abs(5.0);
    pol.computation = ms_to_abs(2.0);
    pol.constraint = ms_to_abs(5.0);
    pol.preemptible = TRUE;
    const kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                               reinterpret_cast<thread_policy_t>(&pol), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        NXLOG_I("audio", "[audio] realtime_band: audio threads in the real-time band (5 ms period, 2 ms of work)%s",
                kr == KERN_SUCCESS ? "" : " refused by the kernel: they keep their QoS class only");
    return kr == KERN_SUCCESS;
}
#endif

// Call at the top of an audio thread. The handle is released when the thread ends (the OS drops the registration then).
inline void BoostAudioThread() {
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mm != nullptr) {
        AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
#if defined(__APPLE__)
    if (config_get_bool("audio", "realtime_band", false)) JoinRealtimeBand();
#endif
}

// Raises the calling thread's priority for the lifetime of the object and restores it afterwards. Used by the thread that updates the
// renderer while it holds the renderer's lock (a stand-in for priority inheritance).
class ScopedAudioLockBoost {
  public:
    ScopedAudioLockBoost() : _thread(GetCurrentThread()), _old(GetThreadPriority(_thread)) {
        if (_old != THREAD_PRIORITY_ERROR_RETURN && _old < THREAD_PRIORITY_HIGHEST) {
            _changed = SetThreadPriority(_thread, THREAD_PRIORITY_HIGHEST) != 0;
        }
    }
    ~ScopedAudioLockBoost() {
        if (_changed) {
            SetThreadPriority(_thread, _old);
        }
    }
    ScopedAudioLockBoost(const ScopedAudioLockBoost&) = delete;
    ScopedAudioLockBoost& operator=(const ScopedAudioLockBoost&) = delete;

  private:
    HANDLE _thread;
    int _old;
    bool _changed = false;
};

// Holds the 1 ms timer resolution for as long as any renderer runs (reference counted).
class TimerResolutionScope {
  public:
    static void Acquire() {
        if (_count.fetch_add(1) == 0) {
            timeBeginPeriod(1);
        }
    }
    static void Release() {
        if (_count.fetch_sub(1) == 1) {
            timeEndPeriod(1);
        }
    }

  private:
    static inline std::atomic<int> _count{0};
};

}  // namespace nxrt::audren
