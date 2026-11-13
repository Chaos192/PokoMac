// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// nn::os::LockBusyMutex as host code (S15, debug note 240).
//
// The SDK's busy mutex is a pure spin lock: ldaxr / cbnz / wfe, and the lifter turns wfe into a no-op. On the console the waiter spins on
// its own core and the holder cannot be preempted (the SDK raises the thread's disable count in its TLS while it holds the lock). Under
// the console core model (kernel/sched.cpp) several guest threads share three pinned host processors and the guest priority bands map to
// Windows priorities, so a spinner that outranks the holder on the same processor starves it: the allocator lock of the game's heap stayed
// held for minutes and the main thread waited on the graphics frame behind it (freeze reports in the sweep). This replacement takes the lock
// the same way (a compare-and-swap on the lock word at +0x10, the disable count in TLS+0x100 raised on success), but gives the processor
// away while it waits, to any ready thread, including a lower-priority holder. Unlock stays the guest's own code (a release store of zero and
// the disable count), so the lock word and the TLS field look exactly as they do on the console.
//
// Config: [sched] busy_mutex_hle (env NXRT_SCHED_BUSY_MUTEX_HLE), default on; off leaves the SDK code in charge.
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"

namespace nxrt::kernel {
namespace {

constexpr const char* kLockName = "_ZN2nn2os13LockBusyMutexEPNS0_13BusyMutexTypeE";
constexpr uint64_t kWordOffset = 0x10;       // BusyMutexType: the lock word
constexpr uint64_t kTlsDisableCount = 0x100; // ThreadLocalRegion: u16 disable count

void lock_busy_mutex(::Ctx* c) {
    auto* word = reinterpret_cast<std::atomic<uint32_t>*>(c->x[0] + kWordOffset);
    auto* disable = reinterpret_cast<uint16_t*>(c->tpidrro + kTlsDisableCount);
    const uint16_t held = *disable;
    for (uint32_t spins = 0;; spins++) {
        uint32_t expected = 0;
        if (word->load(std::memory_order_relaxed) == 0 && word->compare_exchange_weak(expected, 1, std::memory_order_acquire)) break;
        if (spins < 64) YieldProcessor();
        else if (spins < 20000) yield_host();
        else Sleep(1);
    }
    *disable = static_cast<uint16_t>(held + 1);
}

// WFE and YIELD of the lifted code (nx_spin_hint in recomp ctx.h). WFE: the thread waits for an event, so it gives the processor away at
// once. YIELD marks a spin loop that expects the other side to run soon: a PAUSE, and every 64th call a real hand-over.
void spin_hint(int kind) {
    thread_safepoint();   // a spinner waiting for a paused thread's lock parks here (the cheats overlay's pause, thread.cpp)
    thread_local uint32_t n = 0;
    if (kind == 1 || (++n & 63) == 0) yield_host();
    else YieldProcessor();
}

}  // namespace

void install_spin_hint() {
    if (!config_get_bool("sched", "spin_yield", true)) {
        NXLOG_I("sched", "[sched] spin_yield = 0: the guest's WFE and YIELD only pause the host core");
        return;
    }
    nx_spin_hook = spin_hint;
    NXLOG_I("sched", "the guest's WFE and YIELD hand the host processor to another ready thread (debug note 420)");
}

void install_busy_mutex_hle() {
    if (!config_get_bool("sched", "busy_mutex_hle", true)) {
        NXLOG_I("sched", "[sched] busy_mutex_hle = 0: nn::os::LockBusyMutex stays the SDK's spin loop");
        return;
    }
    hle_register(kLockName, lock_busy_mutex);
    module_override_symbol(kLockName);
    NXLOG_I("sched", "nn::os::LockBusyMutex runs as host code that yields while waiting (debug note 240)");
}

}  // namespace nxrt::kernel
