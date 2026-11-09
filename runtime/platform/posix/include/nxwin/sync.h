// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin: slim reader/writer locks, condition variables, critical sections, WaitOnAddress and the Interlocked family.
// Like their Win32 counterparts they are plain data that is valid when zero-filled (SRWLOCK_INIT and CONDITION_VARIABLE_INIT are {}),
// so structures that memset or value-initialise them work. Waiting goes through os_sync_wait_on_address (macOS 14.4+).
#pragma once
#include "nxwin/types.h"

// ---- SRWLOCK --------------------------------------------------------------------------------------------------------
// state: bits 0..29 reader count, bit 30 a writer is waiting (new readers queue behind it), bit 31 a writer holds the lock.
typedef struct _RTL_SRWLOCK { uint32_t state; uint32_t waiters; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {}
#define RTL_SRWLOCK_INIT {}
typedef struct _RTL_CONDITION_VARIABLE { uint32_t seq; uint32_t waiters; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1

namespace nxwin {
constexpr uint32_t kSrwW = 0x80000000u, kSrwP = 0x40000000u, kSrwR = 0x3FFFFFFFu;
void srw_lock_exclusive_slow(SRWLOCK* l);
void srw_lock_shared_slow(SRWLOCK* l);
void srw_wake(SRWLOCK* l);
// Waits while *addr == value, at most timeout_ms (INFINITE: no limit). False on timeout. size 4 or 8.
bool wait_on_address(void* addr, uint64_t value, size_t size, DWORD timeout_ms);
void wake_address(void* addr, size_t size, bool all);
uint64_t mono_ns();
}  // namespace nxwin

inline void InitializeSRWLock(PSRWLOCK l) { l->state = 0; l->waiters = 0; }
inline void AcquireSRWLockExclusive(PSRWLOCK l) {
    uint32_t e = 0;
    if (__atomic_compare_exchange_n(&l->state, &e, nxwin::kSrwW, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    nxwin::srw_lock_exclusive_slow(l);
}
inline void AcquireSRWLockShared(PSRWLOCK l) {
    uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    if (!(s & (nxwin::kSrwW | nxwin::kSrwP)) &&
        __atomic_compare_exchange_n(&l->state, &s, s + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    nxwin::srw_lock_shared_slow(l);
}
inline BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l) {
    uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    return (s & ~nxwin::kSrwP) == 0 &&
           __atomic_compare_exchange_n(&l->state, &s, nxwin::kSrwW, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
inline BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l) {
    uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    while (!(s & (nxwin::kSrwW | nxwin::kSrwP)))
        if (__atomic_compare_exchange_n(&l->state, &s, s + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return TRUE;
    return FALSE;
}
inline void ReleaseSRWLockExclusive(PSRWLOCK l) {
    __atomic_fetch_and(&l->state, ~nxwin::kSrwW, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&l->waiters, __ATOMIC_SEQ_CST)) nxwin::srw_wake(l);
}
inline void ReleaseSRWLockShared(PSRWLOCK l) {
    const uint32_t old = __atomic_fetch_sub(&l->state, 1u, __ATOMIC_SEQ_CST);
    if ((old & nxwin::kSrwR) == 1 && __atomic_load_n(&l->waiters, __ATOMIC_SEQ_CST)) nxwin::srw_wake(l);
}

inline void InitializeConditionVariable(PCONDITION_VARIABLE c) { c->seq = 0; c->waiters = 0; }
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags);
inline void WakeConditionVariable(PCONDITION_VARIABLE c) {
    __atomic_fetch_add(&c->seq, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST)) nxwin::wake_address(&c->seq, 4, false);
}
inline void WakeAllConditionVariable(PCONDITION_VARIABLE c) {
    __atomic_fetch_add(&c->seq, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST)) nxwin::wake_address(&c->seq, 4, true);
}

// ---- CRITICAL_SECTION (recursive) -----------------------------------------------------------------------------------
typedef struct _RTL_CRITICAL_SECTION {
    SRWLOCK lock;
    uint64_t owner;      // nxwin::thread_key() of the owner, 0 when free
    LONG recursion;
    LONG spin;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms);

// ---- WaitOnAddress ----------------------------------------------------------------------------------------------------
BOOL WaitOnAddress(volatile void* addr, PVOID compare, SIZE_T size, DWORD ms);
void WakeByAddressSingle(PVOID addr);
void WakeByAddressAll(PVOID addr);

// ---- Interlocked --------------------------------------------------------------------------------------------------------
#define NXWIN_IL(T) T volatile*
inline LONG InterlockedIncrement(NXWIN_IL(LONG) p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedDecrement(NXWIN_IL(LONG) p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchange(NXWIN_IL(LONG) p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchangeAdd(NXWIN_IL(LONG) p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedOr(NXWIN_IL(LONG) p, LONG v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedAnd(NXWIN_IL(LONG) p, LONG v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedXor(NXWIN_IL(LONG) p, LONG v) { return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }
inline LONG InterlockedCompareExchange(NXWIN_IL(LONG) p, LONG x, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
inline LONG64 InterlockedIncrement64(NXWIN_IL(LONG64) p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedDecrement64(NXWIN_IL(LONG64) p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedExchange64(NXWIN_IL(LONG64) p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedExchangeAdd64(NXWIN_IL(LONG64) p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedOr64(NXWIN_IL(LONG64) p, LONG64 v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedAnd64(NXWIN_IL(LONG64) p, LONG64 v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedCompareExchange64(NXWIN_IL(LONG64) p, LONG64 x, LONG64 cmp) {
    __atomic_compare_exchange_n(p, &cmp, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
inline PVOID InterlockedCompareExchangePointer(PVOID volatile* p, PVOID x, PVOID cmp) {
    __atomic_compare_exchange_n(p, &cmp, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
inline PVOID InterlockedExchangePointer(PVOID volatile* p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
#undef NXWIN_IL
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#if defined(__aarch64__)
#define YieldProcessor() __asm__ __volatile__("yield")
#else
#define YieldProcessor() __builtin_ia32_pause()
#endif
