// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Precise waits ([sched] precise_sleep, [sched] precise_waits; both off by default): a wait that ends on its deadline instead of
// the Windows timer's next tick. Design from pokopia-native (MIT), windows/src/nxrt/host/hle_thread.c wait_until @7c3655d: the
// high-resolution waitable timer wakes 0.4-0.6 ms late (their measurement, PC), so it is asked for the remaining time less its own
// lateness, learned as a moving average over all threads (start 450 us, 1/16 per sample), and the rest is spun through. Theirs
// yields from the timer's wake to the deadline whatever is left; here the spin is bounded: only a remainder of kPreciseSpinMaxNs or
// less is spun, a longer one goes back to the timer (power on the handheld's shared CPU/GPU budget).
#include <windows.h>

#include <atomic>
#include <functional>

#include "kernel_internal.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::kernel {

namespace {
constexpr int64_t kLateStartNs = 450000;    // pokopia-native's start value (their PC: 0.36-0.50 ms measured)
constexpr int64_t kLateMaxNs = 2000000;     // the estimate never exceeds 2 ms (a plain timer under Wine can be far later)
constexpr int64_t kSampleMaxNs = kLateMaxNs; // a wake later than that is preemption, not the timer: not learned (with 5 ms, as theirs, the
                                             // estimate rose to 1-2 ms on a fully loaded PC and every wait then woke early and slept twice)

std::atomic<int64_t> g_late{kLateStartNs};
std::atomic<int> g_sleep_on{-1}, g_waits_on{-1};   // -1: not read yet

bool read_flag(std::atomic<int>& f, const char* key) {
    int v = f.load(std::memory_order_relaxed);
    if (v < 0) {
        v = config_get_bool("sched", key, false) ? 1 : 0;
        int expect = -1;
        if (f.compare_exchange_strong(expect, v) && v) NXLOG_I("kernel", "[sched] %s: on (timer lateness corrected, the last 200 us or less spun)", key);
        v = f.load(std::memory_order_relaxed);
    }
    return v != 0;
}

int64_t qpc_ns() {
    static const int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return static_cast<int64_t>(q.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / f * 1000000000 + c.QuadPart % f * 1000000000 / f;
}

// The calling thread's waitable timer: high resolution when the OS has it, else a plain one (older Wine/Proton builds may refuse
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION; logged once per process). Null only when both fail.
HANDLE thread_timer() {
    struct Timer {
        HANDLE h = nullptr;
        Timer() {
            h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (h) return;
            const DWORD err = GetLastError();
            h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                NXLOG_W("kernel", "precise waits: no high-resolution waitable timer (error %lu), using a %s; the lateness estimate is capped at %lld us",
                        err, h ? "plain waitable timer" : "millisecond wait", static_cast<long long>(kLateMaxNs / 1000));
        }
        ~Timer() { if (h) CloseHandle(h); }
    };
    static thread_local Timer tl_timer;   // closed when the thread ends
    return tl_timer.h;
}

struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_late("kernel.timer_late_us", [] { return g_late.load(std::memory_order_relaxed) / 1000; });
}  // namespace

int64_t precise_timer_request(int64_t rem_ns, int64_t late_ns) {
    if (rem_ns <= kPreciseSpinMaxNs) return 0;
    const int64_t ask = rem_ns - (late_ns < 0 ? 0 : late_ns);
    return ask < 100 ? 100 : ask;      // the timer's unit; it then wakes about late_ns from now, as soon as it can
}

int64_t precise_late_update(int64_t late_ns, int64_t asked_ns, int64_t took_ns) {
    const int64_t got = took_ns - asked_ns;
    if (got < 0 || got >= kSampleMaxNs) return late_ns;            // came back early, or the thread was preempted: no sample
    int64_t next = late_ns + (got - late_ns) / 16;
    return next < 0 ? 0 : next > kLateMaxNs ? kLateMaxNs : next;
}

int64_t precise_late_ns() { return g_late.load(std::memory_order_relaxed); }
bool precise_sleep_enabled() { return read_flag(g_sleep_on, "precise_sleep"); }
bool precise_waits_enabled() { return read_flag(g_waits_on, "precise_waits"); }
void set_precise_timing(int sleep, int waits) {
    if (sleep >= 0) g_sleep_on.store(sleep ? 1 : 0);
    if (waits >= 0) g_waits_on.store(waits ? 1 : 0);
    if (sleep < -1) g_sleep_on.store(-1);   // -2: re-read the config at the next use
    if (waits < -1) g_waits_on.store(-1);
}

bool precise_wait_ns(int64_t ns, void* wake_event) {
    const HANDLE ev = static_cast<HANDLE>(wake_event);
    const int64_t deadline = qpc_ns() + ns;
    const HANDLE timer = thread_timer();
    for (;;) {
        const int64_t now = qpc_ns(), rem = deadline - now;
        if (rem <= 0) return false;
        const int64_t late = g_late.load(std::memory_order_relaxed);
        const int64_t ask = precise_timer_request(rem, late);
        if (ask == 0) {
            // The last kPreciseSpinMaxNs at most: spin to the deadline (a timer would overshoot it by its whole lateness). A pause spin, not
            // SwitchToThread: with the processors busy (measured here with every core building), a hand-over gave the rest of the time slice
            // to another program and a 1 ms sleep took 2.4-7 ms on average instead of 1.4-1.7 ms.
            while (qpc_ns() < deadline) {
                if (ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) return true;
                YieldProcessor();
            }
            return false;
        }
        if (!timer) {   // no timer at all: whole milliseconds, rounded up (never early)
            const DWORD ms = static_cast<DWORD>((rem + 999999) / 1000000);
            if (ev ? WaitForSingleObject(ev, ms) == WAIT_OBJECT_0 : (Sleep(ms), false)) return true;
            continue;
        }
        LARGE_INTEGER due;
        due.QuadPart = -((ask + 99) / 100);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            const DWORD ms = static_cast<DWORD>((rem + 999999) / 1000000);
            if (ev ? WaitForSingleObject(ev, ms) == WAIT_OBJECT_0 : (Sleep(ms), false)) return true;
            continue;
        }
        if (ev) {
            const HANDLE hs[2] = {ev, timer};
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) {
                CancelWaitableTimer(timer);
                return true;
            }
        } else {
            WaitForSingleObject(timer, INFINITE);
        }
        const int64_t next = precise_late_update(late, ask, qpc_ns() - now);
        if (next != late) g_late.store(next, std::memory_order_relaxed);   // racing threads may drop a sample: harmless for an average
    }
}

}  // namespace nxrt::kernel
