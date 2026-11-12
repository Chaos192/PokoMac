// SPDX-License-Identifier: GPL-3.0-or-later
// Kernel-level synchronisation with the semantics of the Horizon SVCs: ArbitrateLock/Unlock (guest mutex words),
// WaitProcessWideKeyAtomic/SignalProcessWideKey (condition variables), WaitForAddress/SignalToAddress, WaitSynchronization and
// CancelSynchronization. The recompiled SDK's nn::os mutexes and condition variables sit on these.
// Logic ported from SuperMarioWonderRecomp runtime/src/kernel.cpp (MIT, Copyright (c) 2026 BelmanteGu; see THIRD_PARTY_NOTICES.md),
// reworked: one SRWLOCK kernel lock, a condition variable per thread, waiters woken in priority order, a pending
// CancelSynchronization is remembered until the next wait.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <atomic>
#include <vector>

#include "kernel_internal.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/tt.h"

namespace nxrt::kernel {
namespace detail {

SRWLOCK g_k = SRWLOCK_INIT;

namespace {
std::vector<KThread*> g_sync_waiters;    // in WaitSynchronization: woken by any change of waitable state
std::vector<KThread*> g_mutex_waiters;   // waiting for a guest mutex word to be handed over
std::vector<KThread*> g_cv_waiters;      // waiting on a process-wide key
std::vector<KThread*> g_addr_waiters;    // WaitForAddress

struct Guard {
    Guard() { AcquireSRWLockExclusive(&g_k); }
    ~Guard() { ReleaseSRWLockExclusive(&g_k); }
};

void remove_from(std::vector<KThread*>& v, KThread* t) { v.erase(std::remove(v.begin(), v.end(), t), v.end()); }

uint32_t load32(uint64_t a) { return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST); }
void store32(uint64_t a, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(a), v, __ATOMIC_SEQ_CST); }
bool cas32(uint64_t a, uint32_t expected, uint32_t v) {
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// The calling guest thread, or a per-host-thread stand-in (tests and host threads that call kernel functions directly).
KThread& self() {
    if (KThread* t = current_thread()) return *t;
    static thread_local KThread host;
    return host;
}

// Highest priority (lowest number) first, FIFO among equals: the order in which Horizon hands a mutex over or signals a key.
KThread* pick_best(std::vector<KThread*>& v, uint64_t KThread::*field, uint64_t value) {
    KThread* best = nullptr;
    for (KThread* t : v)
        if (t->*field == value && (!best || t->prio < best->prio)) best = t;
    return best;
}

// Hands the mutex word at addr to its best waiter (or clears it). g_k held.
void unlock_mutex(uint64_t addr) {
    KThread* next = pick_best(g_mutex_waiters, &KThread::wait_mutex, addr);
    if (!next) {
        store32(addr, 0);
        return;
    }
    int more = 0;
    for (KThread* t : g_mutex_waiters)
        if (t != next && t->wait_mutex == addr) more++;
    remove_from(g_mutex_waiters, next);
    store32(addr, next->wait_tag | (more ? kHandleWaitMask : 0));
    wake_thread(*next);
}

// Monotonic nanoseconds from the registered console's tick frequency, split so the multiplication cannot overflow.
int64_t now_ns() {
    const uint64_t t = system_tick();
    const uint64_t hz = tick_hz();
    return static_cast<int64_t>((t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz);
}

// set_blocked_hook: the host thread that runs a hook while it is blocked in a kernel wait, and the hook.
std::atomic<DWORD> g_blocked_tid{0};
std::atomic<void (*)()> g_blocked_fn{nullptr};

}  // namespace

// One sleep on t's condition variable (g_k held, released while sleeping). On the thread set_blocked_hook names it sleeps 10 ms at most and,
// when that runs out, calls the hook with g_k released. Every caller loops and re-checks its condition, so a wake that comes while the hook
// runs is not lost.
void sleep_locked(KThread& t, DWORD ms) {
    void (*fn)() = g_blocked_fn.load(std::memory_order_acquire);
    if (fn && GetCurrentThreadId() == g_blocked_tid.load(std::memory_order_relaxed)) {
        if (!SleepConditionVariableSRW(thread_cv(t), &g_k, std::min<DWORD>(ms, 10), 0)) {
            ReleaseSRWLockExclusive(&g_k);
            fn();
            AcquireSRWLockExclusive(&g_k);
        }
        return;
    }
    SleepConditionVariableSRW(thread_cv(t), &g_k, ms, 0);
}

// Wakes t's current sleep (g_k held): its condition variable, and its event while it is in a precise wait's last stretch.
void kick(KThread& t) {
    WakeConditionVariable(thread_cv(t));
    if (t.precise_event) SetEvent(static_cast<HANDLE>(t.precise_event));
}

// One sleep of a timed wait with rem_ns (> 0) left, or forever (rem_ns < 0); g_k held, released while sleeping. The caller loops and
// re-checks its condition and the deadline. Default: whole ms rounded up, so a 100 us timeout takes 1 ms (2 ms and more at a coarse timer).
// [sched] precise_waits: whole ms rounded down while more than 2.5 ms remain, then the last stretch on the thread's waitable timer with the
// lateness correction (precise_wait.cpp), ended early by kick(). From pokopia-native (MIT), windows/src/nxrt/host/hle_thread.c block @7c3655d
// (their last stretch cannot be woken; here an event is). Not under the deterministic clock (now_ns is then virtual time).
void sleep_rem_locked(KThread& t, int64_t rem_ns) {
    if (rem_ns < 0) return sleep_locked(t, INFINITE);
    if (precise_waits_enabled() && !tt::deterministic()) {
        if (rem_ns > 2500000) return sleep_locked(t, static_cast<DWORD>(std::min<int64_t>((rem_ns - 1500000) / 1000000, 0x7FFFFFFE)));
        struct Event {
            HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ~Event() { if (h) CloseHandle(h); }
        };
        static thread_local Event tl_ev;   // closed when the thread ends
        if (const HANDLE ev = tl_ev.h) {
            t.precise_event = ev;
            ReleaseSRWLockExclusive(&g_k);
            precise_wait_ns(rem_ns, ev);
            AcquireSRWLockExclusive(&g_k);
            t.precise_event = nullptr;
            ResetEvent(ev);      // a kick that came after the wait ended must not end the next one
            return;
        }
    }
    const int64_t m = (rem_ns + 999999) / 1000000;
    sleep_locked(t, static_cast<DWORD>(m > 0x7FFFFFFE ? 0x7FFFFFFE : m));
}

// Waits (g_k held, released while sleeping) until t.wake or the timeout; false on timeout.
bool block_on_wake(KThread& t, int64_t timeout_ns) {
    timeout_ns = scale_wait_ns(timeout_ns);                 // the speed hack (1.0: unchanged)
    const int64_t until = timeout_ns < 0 ? 0 : now_ns() + timeout_ns;
    while (!t.wake) {
        int64_t rem = -1;
        if (timeout_ns >= 0) {
            rem = until - now_ns();
            if (rem <= 0) return false;
        }
        sleep_rem_locked(t, rem);
    }
    return true;
}

void wake_thread(KThread& t, uint32_t result) {
    t.wake = true;
    t.wait_result = result;
    kick(t);
}

void thread_mark_exited(KThread& t) {
    Guard g;
    t.exited = true;
    for (KThread* w : g_sync_waiters) kick(*w);
}

void sync_reset() {
    Guard g;
    g_sync_waiters.clear();
    g_mutex_waiters.clear();
    g_cv_waiters.clear();
    g_addr_waiters.clear();
}

}  // namespace detail

using namespace detail;

std::string sync_wait_report() {
    // Called from the watchdog thread while the process may be frozen: never block on the kernel lock.
    if (!TryAcquireSRWLockExclusive(&g_k)) return "kernel sync report: the kernel lock is held (a thread is inside a sync call)\n";
    struct Release { ~Release() { ReleaseSRWLockExclusive(&g_k); } } release;
    std::string out = "kernel sync waiters:\n";
    char b[256];
    auto list = [&](const char* kind, const std::vector<KThread*>& v, uint64_t KThread::*field) {
        for (KThread* t : v) {
            const uint64_t a = t->*field;
            std::snprintf(b, sizeof b, "  %-18s %s (guest %llu) address 0x%llx value 0x%08x tag 0x%x wake %d\n", kind, t->name.c_str(),
                          static_cast<unsigned long long>(t->id), static_cast<unsigned long long>(a), a ? load32(a) : 0u, t->wait_tag, t->wake ? 1 : 0);
            out += b;
        }
    };
    list("WaitForAddress", g_addr_waiters, &KThread::wait_addr);
    list("process-wide key", g_cv_waiters, &KThread::wait_key);
    list("mutex hand-over", g_mutex_waiters, &KThread::wait_mutex);
    std::snprintf(b, sizeof b, "  WaitSynchronization waiters: %zu\n", g_sync_waiters.size());
    out += b;
    return out;
}

uint32_t arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    if (addr & 3) return kInvalidAddress;
    Guard g;
    KThread& me = self();
    if (load32(addr) != (owner | kHandleWaitMask)) return kOk;      // the owner already released it: the caller retries in guest code
    me.wake = false;
    me.wait_mutex = addr;
    me.wait_tag = tag;
    g_mutex_waiters.push_back(&me);
    block_on_wake(me, -1);
    return me.wait_result;
}

uint32_t arbitrate_unlock(uint64_t addr) {
    if (addr & 3) return kInvalidAddress;
    Guard g;
    unlock_mutex(addr);
    return kOk;
}

uint32_t wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout_ns) {
    if (mutex & 3 || key & 3) return kInvalidAddress;
    Guard g;
    KThread& me = self();
    // Like Horizon: the condition variable's "has waiters" word is written BEFORE the mutex is released. The SDK's Signal/Broadcast
    // read that word in guest code and skip the SVC when it is 0; the other order lost wake-ups (Wonder: a thread took the freed mutex,
    // posted, saw 0, and the waiter then slept forever).
    me.wake = false;
    me.wait_mutex = mutex;
    me.wait_key = key;
    me.wait_tag = tag;
    me.wait_result = 0;
    g_cv_waiters.push_back(&me);
    store32(key, 1);
    unlock_mutex(mutex);
    if (timeout_ns == 0 || !block_on_wake(me, timeout_ns)) {
        // Timed out (not handed the mutex): leave the condition variable or the mutex queue a signal moved us to, return without
        // the mutex (the SDK re-locks it itself: TimedWait -> InternalCriticalSection::Enter).
        remove_from(g_cv_waiters, &me);
        remove_from(g_mutex_waiters, &me);
        bool others = std::any_of(g_cv_waiters.begin(), g_cv_waiters.end(), [&](KThread* t) { return t->wait_key == key; });
        if (!others) store32(key, 0);
        return kTimedOut;
    }
    return me.wait_result;
}

uint32_t signal_process_wide_key(uint64_t key, int32_t count) {
    if (key & 3) return kInvalidAddress;
    Guard g;
    int n = 0;
    while (count <= 0 || n < count) {
        KThread* t = pick_best(g_cv_waiters, &KThread::wait_key, key);
        if (!t) break;
        remove_from(g_cv_waiters, t);
        n++;
        // Try to give it the mutex right away.
        for (;;) {
            uint32_t v = load32(t->wait_mutex);
            if (v == 0) {
                if (!cas32(t->wait_mutex, 0, t->wait_tag)) continue;
                wake_thread(*t);
                break;
            }
            if (!(v & kHandleWaitMask) && !cas32(t->wait_mutex, v, v | kHandleWaitMask)) continue;
            g_mutex_waiters.push_back(t);
            break;
        }
    }
    bool others = std::any_of(g_cv_waiters.begin(), g_cv_waiters.end(), [&](KThread* t) { return t->wait_key == key; });
    if (!others) store32(key, 0);
    return kOk;
}

uint32_t wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout_ns) {
    if (addr & 3) return kInvalidAddress;
    if (type > 2) return kInvalidEnum;
    Guard g;
    KThread& me = self();
    int32_t cur = static_cast<int32_t>(load32(addr));
    switch (type) {
        case 0:  // WaitIfLessThan
            if (!(cur < value)) return kInvalidState;
            break;
        case 1:  // DecrementAndWaitIfLessThan: one atomic step, as the kernel's exclusive load/store loop (guest threads change the word
                 // with their own atomics without the kernel lock; a plain load and store lost their updates)
            for (;;) {
                if (!(cur < value)) return kInvalidState;
                if (cas32(addr, static_cast<uint32_t>(cur), static_cast<uint32_t>(cur) - 1u)) break;
                cur = static_cast<int32_t>(load32(addr));
            }
            break;
        case 2:  // WaitIfEqual
            if (cur != value) return kInvalidState;
            break;
    }
    if (timeout_ns == 0) return kTimedOut;
    me.wake = false;
    me.wait_addr = addr;
    me.wait_result = 0;
    g_addr_waiters.push_back(&me);
    if (!block_on_wake(me, timeout_ns)) {
        remove_from(g_addr_waiters, &me);
        return kTimedOut;
    }
    return me.wait_result;
}

uint32_t signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    if (addr & 3) return kInvalidAddress;
    if (type > 2) return kInvalidEnum;
    Guard g;
    int waiters = static_cast<int>(std::count_if(g_addr_waiters.begin(), g_addr_waiters.end(), [&](KThread* t) { return t->wait_addr == addr; }));
    switch (type) {
        case 0:  // Signal
            break;
        case 1:  // SignalAndIncrementIfEqual
            if (!cas32(addr, static_cast<uint32_t>(value), static_cast<uint32_t>(value + 1))) return kInvalidState;
            break;
        case 2: {  // SignalAndModifyByWaitingCountIfEqual
            int32_t updated;
            if (count <= 0)
                updated = waiters ? value - 2 : value + 1;
            else if (waiters)
                updated = waiters <= count ? value - 1 : value;
            else
                updated = value + 1;
            if (!cas32(addr, static_cast<uint32_t>(value), static_cast<uint32_t>(updated))) return kInvalidState;
            break;
        }
    }
    int n = 0;
    while (count <= 0 || n < count) {
        KThread* t = pick_best(g_addr_waiters, &KThread::wait_addr, addr);
        if (!t) break;
        remove_from(g_addr_waiters, t);
        wake_thread(*t);
        n++;
    }
    return kOk;
}

uint32_t wait_synchronization(const uint32_t* handles, int32_t count, int64_t timeout_ns, int32_t* index_out) {
    if (count < 0 || count > 0x40) return kOutOfRange;
    std::vector<std::shared_ptr<KObject>> objs;
    for (int i = 0; i < count; i++) {
        auto o = handle_get(handles[i]);
        if (!o) {
            if (index_out) *index_out = -1;
            return kInvalidHandle;
        }
        objs.push_back(std::move(o));
    }
    auto ready = [&]() -> int {
        for (int i = 0; i < count; i++)
            if (objs[i]->signaled()) return i;
        return -1;
    };
    timeout_ns = scale_wait_ns(timeout_ns);                 // the speed hack (1.0: unchanged)
    const int64_t until = timeout_ns <= 0 ? 0 : now_ns() + timeout_ns;
    Guard g;
    KThread& me = self();
    g_sync_waiters.push_back(&me);
    struct Leave {
        KThread* t;
        ~Leave() { remove_from(g_sync_waiters, t); }
    } leave{&me};
    for (;;) {
        if (me.cancel_sync) {
            me.cancel_sync = false;
            return kCancelled;
        }
        int i = ready();
        if (i >= 0) {
            if (index_out) *index_out = i;
            objs[i]->consume();
            return kOk;
        }
        if (timeout_ns == 0) return kTimedOut;
        int64_t rem = -1;
        if (timeout_ns > 0) {
            rem = until - now_ns();
            if (rem <= 0) return kTimedOut;
        }
        sleep_rem_locked(me, rem);
    }
}

namespace detail {
void (*blocked_hook_here())() {
    void (*fn)() = g_blocked_fn.load(std::memory_order_acquire);
    return fn && GetCurrentThreadId() == g_blocked_tid.load(std::memory_order_relaxed) ? fn : nullptr;
}
}  // namespace detail

void set_blocked_hook(uint32_t host_tid, void (*fn)()) {
    g_blocked_tid.store(host_tid, std::memory_order_relaxed);
    g_blocked_fn.store(fn, std::memory_order_release);
}

uint32_t cancel_synchronization(uint32_t thread_handle) {
    auto t = handle_as<KThread>(thread_handle);
    if (!t) return kInvalidHandle;
    Guard g;
    t->cancel_sync = true;
    if (std::find(g_sync_waiters.begin(), g_sync_waiters.end(), t.get()) != g_sync_waiters.end()) kick(*t);
    return kOk;
}

void notify_waitable_changed() {
    Guard g;
    for (KThread* t : g_sync_waiters) kick(*t);
}

// ---- gauges (include/nxrt/gauge.h): wait-queue lengths ----
namespace {
// Removes its gauge when the file's statics are torn down (declared after the wait queues, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};

// Length of one wait queue under the kernel lock. The sampler never waits for the kernel lock: -1 while a thread holds it.
int64_t queue_len(const std::vector<KThread*>& v) {
    if (!TryAcquireSRWLockExclusive(&g_k)) return -1;
    const int64_t n = static_cast<int64_t>(v.size());
    ReleaseSRWLockExclusive(&g_k);
    return n;
}

GaugeHook gauge_wait_sync("kernel.waiters_sync", [] { return queue_len(g_sync_waiters); });
GaugeHook gauge_wait_mutex("kernel.waiters_mutex", [] { return queue_len(g_mutex_waiters); });
GaugeHook gauge_wait_cv("kernel.waiters_cv", [] { return queue_len(g_cv_waiters); });
GaugeHook gauge_wait_addr("kernel.waiters_addr", [] { return queue_len(g_addr_waiters); });
}  // namespace

}  // namespace nxrt::kernel
