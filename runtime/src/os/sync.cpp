// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Guest-address keyed sync objects. Contract and design: include/nxrt/sync.h.
//
// Table: 4096 buckets, each a chain guarded by its own SRWLOCK. Lookups take the bucket shared; creating, replacing or
// destroying an entry takes it exclusive. Objects are never freed while the process runs unless reset_all() is called:
// destroy() and kind replacement park the old object on a graveyard list, so a thread that already holds a pointer can
// finish its wait on it safely.
//
// Lock order, outermost first: bucket lock, cond lock, mutex lock. The bucket lock and the graveyard lock are never held
// while an object lock is taken, and nothing takes a cond lock while holding a mutex lock.

#include "nxrt/sync.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>

#include "nxrt/gauge.h"
#include "nxrt/log.h"

namespace nxrt::sync {
namespace {

constexpr const char* kTag = "sync";
constexpr size_t kBuckets = 4096;
constexpr int64_t kNsPerMs = 1000000;
constexpr DWORD kMaxWaitMs = 0x7FFFFFFE;  // one below INFINITE

enum class Kind : uint8_t { Mutex, Cond, Event, Sem };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Mutex: return "mutex";
        case Kind::Cond: return "cond";
        case Kind::Event: return "event";
        case Kind::Sem: return "sem";
    }
    return "?";
}

struct Object {
    Object(uint64_t a, Kind k) : addr(a), kind(k) {}
    virtual ~Object() = default;
    uint64_t addr;
    Kind kind;
    Object* next = nullptr;   // bucket chain while live
    Object* gnext = nullptr;  // graveyard chain once destroyed
};

struct Mutex : Object {
    static constexpr Kind kKind = Kind::Mutex;
    explicit Mutex(uint64_t a) : Object(a, kKind) {
        InitializeSRWLock(&lk);
        InitializeConditionVariable(&cv);
    }
    SRWLOCK lk;
    CONDITION_VARIABLE cv;   // lock waiters; signalled whenever the mutex becomes free
    DWORD owner = 0;         // holder's thread id, 0 = free
    uint32_t rec = 0;        // nesting depth held by owner
    bool recursive = false;
};

// A condition variable keeps a FIFO of waiter nodes. signal marks the first unmarked node, broadcast marks all of them, so
// a waiter that registers after a broadcast can never steal the wakeup of one that registered before it.
struct CondWaiter {
    CondWaiter* next = nullptr;
    bool woken = false;
};

struct Cond : Object {
    static constexpr Kind kKind = Kind::Cond;
    explicit Cond(uint64_t a) : Object(a, kKind) {
        InitializeSRWLock(&lk);
        InitializeConditionVariable(&cv);
    }
    SRWLOCK lk;
    CONDITION_VARIABLE cv;  // shared by all waiters; every wakeup is a WakeAll and each waiter checks its own node
    CondWaiter* head = nullptr;
    CondWaiter* tail = nullptr;
};

struct Event : Object {
    static constexpr Kind kKind = Kind::Event;
    explicit Event(uint64_t a) : Object(a, kKind) {
        InitializeSRWLock(&lk);
        InitializeConditionVariable(&cv);
    }
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    bool signaled = false;
    bool auto_clear = false;
};

struct Sem : Object {
    static constexpr Kind kKind = Kind::Sem;
    explicit Sem(uint64_t a) : Object(a, kKind) {
        InitializeSRWLock(&lk);
        InitializeConditionVariable(&cv);
    }
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    int32_t count = 0;
    int32_t max_count = std::numeric_limits<int32_t>::max();
};

struct Bucket {
    SRWLOCK lock;  // zero-initialised static storage is a valid unlocked SRWLOCK
    Object* head;  // nullptr-initialised
};

Bucket g_buckets[kBuckets];
SRWLOCK g_grave_lock;
Object* g_grave = nullptr;
std::atomic<size_t> g_live{0};
std::atomic<size_t> g_grave_n{0};   // objects on the graveyard list (park adds, reset_all clears)

// ---- gauges (include/nxrt/gauge.h) ----
// Removes its gauge when this file's statics are torn down (declared after the counters above, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_sync_objects("os.sync_objects", [] { return static_cast<int64_t>(g_live.load(std::memory_order_relaxed)); });
GaugeHook gauge_sync_graveyard("os.sync_graveyard", [] { return static_cast<int64_t>(g_grave_n.load(std::memory_order_relaxed)); });

size_t bucket_index(uint64_t addr) {
    return static_cast<size_t>((addr * 0x9E3779B97F4A7C15ull) >> 52);
}

Object* find_in(Object* head, uint64_t addr) {
    for (Object* o = head; o; o = o->next)
        if (o->addr == addr) return o;
    return nullptr;
}

void park(Object* o) {
    AcquireSRWLockExclusive(&g_grave_lock);
    o->gnext = g_grave;
    g_grave = o;
    g_grave_n.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_grave_lock);
}

// Caller holds the bucket exclusive. Removes o from the chain and parks it; it is still valid memory afterwards.
void unlink_locked(Bucket& b, Object* o) {
    Object** pp = &b.head;
    while (*pp && *pp != o) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = o->next;
    o->next = nullptr;
    g_live.fetch_sub(1, std::memory_order_relaxed);
    park(o);
}

// Returns the live object at addr, creating it (or replacing a different kind) with defaults.
template <class T>
T* get(uint64_t addr) {
    Bucket& b = g_buckets[bucket_index(addr)];
    AcquireSRWLockShared(&b.lock);
    Object* o = find_in(b.head, addr);
    if (o && o->kind == T::kKind) {
        ReleaseSRWLockShared(&b.lock);
        return static_cast<T*>(o);
    }
    ReleaseSRWLockShared(&b.lock);

    AcquireSRWLockExclusive(&b.lock);
    o = find_in(b.head, addr);  // another thread may have created it in the gap
    if (!o || o->kind != T::kKind) {
        if (o) {
            NXLOG_D(kTag, "guest address 0x%llx: %s replaced by %s", static_cast<unsigned long long>(addr),
                    kind_name(o->kind), kind_name(T::kKind));
            unlink_locked(b, o);
        }
        T* n = new T(addr);
        n->next = b.head;
        b.head = n;
        g_live.fetch_add(1, std::memory_order_relaxed);
        o = n;
    }
    ReleaseSRWLockExclusive(&b.lock);
    return static_cast<T*>(o);
}

// ---- timing -----------------------------------------------------------------------------------------------------------

// Monotonic nanoseconds from QueryPerformanceCounter, split so the multiplication cannot overflow.
int64_t now_ns() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t secs = c.QuadPart / freq;
    const int64_t rem = c.QuadPart % freq;
    return secs * 1000000000LL + static_cast<int64_t>(static_cast<double>(rem) * 1e9 / static_cast<double>(freq));
}

struct Deadline {
    bool forever;  // true: no deadline (timeout_ns < 0)
    int64_t at;    // now_ns() value at which a timed wait gives up
};

Deadline deadline_for(int64_t timeout_ns) {
    if (timeout_ns < 0) return {true, 0};
    const int64_t now = now_ns();
    const int64_t max = std::numeric_limits<int64_t>::max();
    return {false, timeout_ns > max - now ? max : now + timeout_ns};
}

// One sleep on cv with lk held (the sleep releases lk while blocked). Returns false, without sleeping, when a timed wait
// has run out of time. The sleep is rounded up to whole milliseconds so it never ends before the deadline; the caller
// re-checks its predicate after every return, so spurious and early returns from the OS are harmless.
bool wait_step(CONDITION_VARIABLE* cv, SRWLOCK* lk, const Deadline& d) {
    if (d.forever) {
        SleepConditionVariableSRW(cv, lk, INFINITE, 0);
        return true;
    }
    const int64_t rem = d.at - now_ns();
    if (rem <= 0) return false;
    int64_t ms = rem / kNsPerMs + (rem % kNsPerMs ? 1 : 0);
    if (ms > static_cast<int64_t>(kMaxWaitMs)) ms = kMaxWaitMs;
    SleepConditionVariableSRW(cv, lk, static_cast<DWORD>(ms), 0);
    return true;
}

// ---- mutex helpers ------------------------------------------------------------------------------------------------------

// Gives the mutex up completely (for cond_wait) and returns the nesting depth the caller held.
uint32_t mutex_release_all(Mutex* m) {
    AcquireSRWLockExclusive(&m->lk);
    const uint32_t held = m->rec;
    m->owner = 0;
    m->rec = 0;
    WakeConditionVariable(&m->cv);
    ReleaseSRWLockExclusive(&m->lk);
    return held;
}

// Blocks until the mutex is free, then takes it with nesting depth `rec`.
void mutex_take_blocking(Mutex* m, uint32_t rec) {
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&m->lk);
    while (m->owner != 0) SleepConditionVariableSRW(&m->cv, &m->lk, INFINITE, 0);
    m->owner = me;
    m->rec = rec;
    ReleaseSRWLockExclusive(&m->lk);
}

// ---- condition wait -----------------------------------------------------------------------------------------------------

// Implements cond_wait (d.forever) and cond_timed_wait. Returns true when a signal/broadcast reached this waiter, false on
// timeout. The guest mutex is released (fully, whatever its depth) once the waiter is queued, so a signal issued after that
// point cannot be missed, and it is re-acquired at the same depth before returning.
bool cond_wait_impl(uint64_t cond_addr, uint64_t mutex_addr, const Deadline& d) {
    Cond* c = get<Cond>(cond_addr);
    Mutex* m = get<Mutex>(mutex_addr);
    const DWORD me = GetCurrentThreadId();

    AcquireSRWLockExclusive(&m->lk);
    const bool owned = m->owner == me;
    ReleaseSRWLockExclusive(&m->lk);
    if (!owned) {
        NXLOG_E(kTag, "cond wait on 0x%llx without holding mutex 0x%llx; not waiting",
                static_cast<unsigned long long>(cond_addr), static_cast<unsigned long long>(mutex_addr));
        return false;
    }

    CondWaiter node;
    AcquireSRWLockExclusive(&c->lk);
    if (c->tail) c->tail->next = &node; else c->head = &node;
    c->tail = &node;
    const uint32_t held = mutex_release_all(m);  // order: cond lock, then mutex lock
    while (!node.woken) {
        if (!wait_step(&c->cv, &c->lk, d)) break;
    }
    const bool signalled = node.woken;
    if (!signalled) {  // timed out: leave the queue
        CondWaiter** pp = &c->head;
        CondWaiter* prev = nullptr;
        while (*pp && *pp != &node) { prev = *pp; pp = &(*pp)->next; }
        if (*pp) {
            *pp = node.next;
            if (c->tail == &node) c->tail = prev;
        }
    }
    ReleaseSRWLockExclusive(&c->lk);

    mutex_take_blocking(m, held);
    return signalled;
}

// ---- event / semaphore waits ----------------------------------------------------------------------------------------------

bool event_wait_impl(uint64_t addr, const Deadline& d) {
    Event* e = get<Event>(addr);
    AcquireSRWLockExclusive(&e->lk);
    bool ok = true;
    while (!e->signaled) {
        if (!wait_step(&e->cv, &e->lk, d)) {
            ok = false;
            break;
        }
    }
    if (ok && e->auto_clear) e->signaled = false;
    ReleaseSRWLockExclusive(&e->lk);
    return ok;
}

bool sem_acquire_impl(uint64_t addr, const Deadline& d) {
    Sem* s = get<Sem>(addr);
    AcquireSRWLockExclusive(&s->lk);
    bool ok = true;
    while (s->count == 0) {
        if (!wait_step(&s->cv, &s->lk, d)) {
            ok = false;
            break;
        }
    }
    if (ok) s->count--;
    ReleaseSRWLockExclusive(&s->lk);
    return ok;
}

}  // namespace

// ---- mutex --------------------------------------------------------------------------------------------------------------

// Non-recursive by default. Locking a non-recursive mutex the caller already owns is a guest bug: it is logged and
// ignored (the call returns without taking the mutex again, so one unlock frees it). It never blocks.
void mutex_init(uint64_t addr, bool recursive) {
    Mutex* m = get<Mutex>(addr);
    AcquireSRWLockExclusive(&m->lk);
    m->recursive = recursive;  // only the flag changes; owner and depth of a held mutex are kept
    ReleaseSRWLockExclusive(&m->lk);
}

void mutex_lock(uint64_t addr) {
    Mutex* m = get<Mutex>(addr);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&m->lk);
    if (m->owner == me) {
        const bool recursive = m->recursive;
        if (recursive) m->rec++;
        ReleaseSRWLockExclusive(&m->lk);
        if (!recursive)
            NXLOG_E(kTag, "mutex 0x%llx: lock by its owner (thread %lu) on a non-recursive mutex; ignored",
                    static_cast<unsigned long long>(addr), static_cast<unsigned long>(me));
        return;
    }
    while (m->owner != 0) SleepConditionVariableSRW(&m->cv, &m->lk, INFINITE, 0);
    m->owner = me;
    m->rec = 1;
    ReleaseSRWLockExclusive(&m->lk);
}

bool mutex_try_lock(uint64_t addr) {
    Mutex* m = get<Mutex>(addr);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&m->lk);
    bool ok = false;
    if (m->owner == 0) {
        m->owner = me;
        m->rec = 1;
        ok = true;
    } else if (m->owner == me && m->recursive) {
        m->rec++;
        ok = true;
    }
    ReleaseSRWLockExclusive(&m->lk);
    return ok;
}

void mutex_unlock(uint64_t addr) {
    Mutex* m = get<Mutex>(addr);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&m->lk);
    if (m->owner != me) {
        const DWORD owner = m->owner;
        ReleaseSRWLockExclusive(&m->lk);
        NXLOG_E(kTag, "mutex 0x%llx: unlock by thread %lu, owner is thread %lu; ignored",
                static_cast<unsigned long long>(addr), static_cast<unsigned long>(me), static_cast<unsigned long>(owner));
        return;
    }
    if (--m->rec == 0) {
        m->owner = 0;
        WakeConditionVariable(&m->cv);
    }
    ReleaseSRWLockExclusive(&m->lk);
}

bool mutex_is_locked_by_current(uint64_t addr) {
    Mutex* m = get<Mutex>(addr);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&m->lk);
    const bool ok = m->owner == me;
    ReleaseSRWLockExclusive(&m->lk);
    return ok;
}

bool mutex_is_locked(uint64_t addr) {
    Mutex* m = get<Mutex>(addr);
    AcquireSRWLockExclusive(&m->lk);
    const bool ok = m->owner != 0;
    ReleaseSRWLockExclusive(&m->lk);
    return ok;
}

// ---- condition variable -------------------------------------------------------------------------------------------------

void cond_init(uint64_t addr) {
    (void)get<Cond>(addr);  // existence is all there is to initialise; waiter counts are never reset
}

void cond_wait(uint64_t cond, uint64_t mutex) {
    (void)cond_wait_impl(cond, mutex, Deadline{true, 0});
}

bool cond_timed_wait(uint64_t cond, uint64_t mutex, int64_t timeout_ns) {
    return cond_wait_impl(cond, mutex, deadline_for(timeout_ns));
}

// Woken nodes are unlinked here, by the signaller, so the queue only ever holds waiters that have not been woken yet.
// A node's memory belongs to its waiter (it lives on the waiter's stack): the signaller touches it only under the cond lock,
// and the waiter reads `woken` only under the same lock, so the node is never used after its owner has returned.
void cond_signal(uint64_t addr) {
    Cond* c = get<Cond>(addr);
    AcquireSRWLockExclusive(&c->lk);
    if (CondWaiter* w = c->head) {
        c->head = w->next;
        if (!c->head) c->tail = nullptr;
        w->next = nullptr;
        w->woken = true;
        WakeAllConditionVariable(&c->cv);
    }
    ReleaseSRWLockExclusive(&c->lk);
}

void cond_broadcast(uint64_t addr) {
    Cond* c = get<Cond>(addr);
    AcquireSRWLockExclusive(&c->lk);
    CondWaiter* w = c->head;
    c->head = c->tail = nullptr;
    while (w) {
        CondWaiter* n = w->next;
        w->next = nullptr;
        w->woken = true;
        w = n;
    }
    WakeAllConditionVariable(&c->cv);
    ReleaseSRWLockExclusive(&c->lk);
}

// ---- event --------------------------------------------------------------------------------------------------------------

// Auto-clear: one signal releases one waiter, or stays set for the next waiter if none is waiting. Two signals while
// nobody waits leave one pending release (the flag does not count), like a Win32 auto-reset event.
// Manual-clear: signal releases every waiter and stays set until event_clear.
void event_init(uint64_t addr, bool signaled, bool auto_clear) {
    Event* e = get<Event>(addr);
    AcquireSRWLockExclusive(&e->lk);
    e->signaled = signaled;
    e->auto_clear = auto_clear;
    WakeAllConditionVariable(&e->cv);
    ReleaseSRWLockExclusive(&e->lk);
}

void event_signal(uint64_t addr) {
    Event* e = get<Event>(addr);
    AcquireSRWLockExclusive(&e->lk);
    e->signaled = true;
    if (e->auto_clear)
        WakeConditionVariable(&e->cv);
    else
        WakeAllConditionVariable(&e->cv);
    ReleaseSRWLockExclusive(&e->lk);
}

void event_clear(uint64_t addr) {
    Event* e = get<Event>(addr);
    AcquireSRWLockExclusive(&e->lk);
    e->signaled = false;
    ReleaseSRWLockExclusive(&e->lk);
}

void event_wait(uint64_t addr) {
    (void)event_wait_impl(addr, Deadline{true, 0});
}

bool event_try_wait(uint64_t addr) {
    Event* e = get<Event>(addr);
    AcquireSRWLockExclusive(&e->lk);
    bool ok = e->signaled;
    if (ok && e->auto_clear) e->signaled = false;
    ReleaseSRWLockExclusive(&e->lk);
    return ok;
}

bool event_timed_wait(uint64_t addr, int64_t timeout_ns) {
    return event_wait_impl(addr, deadline_for(timeout_ns));
}

// ---- counting semaphore -------------------------------------------------------------------------------------------------

// Defaults: count 0, max INT32_MAX. Re-initialising clamps the count into [0, max].
void sem_init(uint64_t addr, int32_t count, int32_t max_count) {
    Sem* s = get<Sem>(addr);
    AcquireSRWLockExclusive(&s->lk);
    s->max_count = max_count < 0 ? 0 : max_count;
    s->count = count < 0 ? 0 : (count > s->max_count ? s->max_count : count);
    WakeAllConditionVariable(&s->cv);
    ReleaseSRWLockExclusive(&s->lk);
}

void sem_acquire(uint64_t addr) {
    (void)sem_acquire_impl(addr, Deadline{true, 0});
}

bool sem_try_acquire(uint64_t addr) {
    Sem* s = get<Sem>(addr);
    AcquireSRWLockExclusive(&s->lk);
    const bool ok = s->count > 0;
    if (ok) s->count--;
    ReleaseSRWLockExclusive(&s->lk);
    return ok;
}

bool sem_timed_acquire(uint64_t addr, int64_t timeout_ns) {
    return sem_acquire_impl(addr, deadline_for(timeout_ns));
}

void sem_release(uint64_t addr, int32_t n) {
    if (n <= 0) return;
    Sem* s = get<Sem>(addr);
    AcquireSRWLockExclusive(&s->lk);
    const int64_t room = static_cast<int64_t>(s->max_count) - s->count;
    const int64_t added = n > room ? (room > 0 ? room : 0) : n;
    s->count += static_cast<int32_t>(added);
    if (added == 1)
        WakeConditionVariable(&s->cv);
    else if (added > 1)
        WakeAllConditionVariable(&s->cv);
    ReleaseSRWLockExclusive(&s->lk);
}

int32_t sem_count(uint64_t addr) {
    Sem* s = get<Sem>(addr);
    AcquireSRWLockExclusive(&s->lk);
    const int32_t v = s->count;
    ReleaseSRWLockExclusive(&s->lk);
    return v;
}

// ---- housekeeping -------------------------------------------------------------------------------------------------------

void destroy(uint64_t addr) {
    Bucket& b = g_buckets[bucket_index(addr)];
    AcquireSRWLockExclusive(&b.lock);
    Object* o = find_in(b.head, addr);
    if (o) unlink_locked(b, o);
    ReleaseSRWLockExclusive(&b.lock);
}

size_t object_count() {
    return g_live.load(std::memory_order_relaxed);
}

// Drops every live object and frees the graveyard. Only for tests or process teardown: any thread still holding a
// pointer to one of these objects (for example blocked in a wait) would be left referencing freed memory.
void reset_all() {
    for (auto& b : g_buckets) {
        AcquireSRWLockExclusive(&b.lock);
        while (b.head) unlink_locked(b, b.head);
        ReleaseSRWLockExclusive(&b.lock);
    }
    AcquireSRWLockExclusive(&g_grave_lock);
    Object* g = g_grave;
    g_grave = nullptr;
    g_grave_n.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_grave_lock);
    while (g) {
        Object* n = g->gnext;
        delete g;
        g = n;
    }
}

}  // namespace nxrt::sync
