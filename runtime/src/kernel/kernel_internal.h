// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Internal to src/kernel: state shared by thread.cpp, kernel_sync.cpp and svc_thread.cpp.
#pragma once
#include <windows.h>

#include "nxrt/kernel.h"

namespace nxrt::kernel::detail {

static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void*), "KThread::cv_storage holds one CONDITION_VARIABLE");
inline CONDITION_VARIABLE* thread_cv(KThread& t) { return reinterpret_cast<CONDITION_VARIABLE*>(&t.cv_storage[0]); }

void sync_reset();                       // forget every wait queue (kernel_sync.cpp)
void thread_mark_exited(KThread& t);     // marks exited under the kernel lock and wakes WaitSynchronization waiters (kernel_sync.cpp)
extern SRWLOCK g_k;                      // the "kernel lock": wait queues and wait state of every thread
// Blocks the calling thread on its own condition variable with g_k held until t.wake or the deadline (ns, < 0 = forever).
bool block_on_wake(KThread& t, int64_t timeout_ns);
void wake_thread(KThread& t, uint32_t result = 0);   // g_k held
// The set_blocked_hook function when the calling thread is the one it names, else null (kernel_sync.cpp).
void (*blocked_hook_here())();
// The sleep of the yield back-off: always the high-resolution timer, never the yield loop sleep_ns uses for spans up to
// 200 us (time.cpp). Windows wakes it ~0.5 ms late at the shortest; Wine/Proton sleeps about the span asked for.
void sleep_timer_ns(int64_t ns);

}  // namespace nxrt::kernel::detail
