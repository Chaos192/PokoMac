// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Guest-address-keyed synchronisation objects (the smo "side table" model, used where nn::os is answered by the host
// instead of by recompiled SDK code): the guest's MutexType / ConditionVariableType / EventType / SemaphoreType are opaque
// here. Each object is identified by its guest address and backed by a host object that lives in a hash table; the
// guest layout is never read or written. Objects are created lazily by their first use with defaults.
// All functions are thread safe and callable from any host thread. Timeouts are in nanoseconds: < 0 = forever, 0 = poll.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::sync {

// ---- mutex (non-recursive unless made recursive) -----------------------------------------------------------------
void mutex_init(uint64_t addr, bool recursive);
void mutex_lock(uint64_t addr);
bool mutex_try_lock(uint64_t addr);
void mutex_unlock(uint64_t addr);                 // not the owner: logs an error and does nothing
bool mutex_is_locked_by_current(uint64_t addr);
bool mutex_is_locked(uint64_t addr);

// ---- condition variable (Mesa semantics; spurious wakeups allowed, lost wakeups are not) -------------------------------
void cond_init(uint64_t addr);
void cond_wait(uint64_t cond, uint64_t mutex);    // mutex must be held by the caller; it is released while waiting and re-held on return
bool cond_timed_wait(uint64_t cond, uint64_t mutex, int64_t timeout_ns);   // false on timeout (mutex is held either way)
void cond_signal(uint64_t cond);                  // wakes at most one waiter that is already waiting
void cond_broadcast(uint64_t cond);               // wakes every waiter that is already waiting

// ---- event ---------------------------------------------------------------------------------------------------------
void event_init(uint64_t addr, bool signaled, bool auto_clear);
void event_signal(uint64_t addr);
void event_clear(uint64_t addr);
void event_wait(uint64_t addr);                   // auto-clear: exactly one waiter consumes each signal
bool event_try_wait(uint64_t addr);
bool event_timed_wait(uint64_t addr, int64_t timeout_ns);   // false on timeout

// ---- counting semaphore --------------------------------------------------------------------------------------------
void sem_init(uint64_t addr, int32_t count, int32_t max_count);
void sem_acquire(uint64_t addr);
bool sem_try_acquire(uint64_t addr);
bool sem_timed_acquire(uint64_t addr, int64_t timeout_ns);
void sem_release(uint64_t addr, int32_t n = 1);   // counts above max_count are clamped
int32_t sem_count(uint64_t addr);

// ---- housekeeping --------------------------------------------------------------------------------------------------
void destroy(uint64_t addr);                      // forget the object at addr so a new one created there starts clean
size_t object_count();
void reset_all();                                 // drop every object (tests only; no thread may be waiting)

}  // namespace nxrt::sync
