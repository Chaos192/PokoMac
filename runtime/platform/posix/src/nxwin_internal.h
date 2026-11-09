// SPDX-License-Identifier: GPL-3.0-or-later
// nxwin internals: the kernel object model shared by the implementation files.
//
// Every HANDLE is an Object* with a reference count. A wait locks each object in turn, asks it whether the wait is satisfied now
// (try_acquire, which also consumes the state of an auto-reset event or takes a mutex) and, if none is, registers a Waiter with all of
// them and sleeps until one signals it or the earliest deadline passes (the caller's timeout, or the due time of a timer object, which
// no thread signals). Registration happens under the object's lock right after its check, so a signal can never fall between the two.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nxwin {

std::string narrow(LPCWSTR w);
std::wstring widen(const char* s);
std::string module_path(HMODULE m);
std::string fix_path(const char* p);           // backslashes to slashes
inline std::string fix_path(const std::string& p) { return fix_path(p.c_str()); }
inline std::string fix_path_w(LPCWSTR p) { return fix_path(narrow(p).c_str()); }
uint64_t mono_ns();
void set_errno_error();                          // SetLastError(errno_to_win32(errno))

struct Waiter {
    std::mutex m;
    std::condition_variable cv;
    bool woke = false;
};

struct Object {
    static constexpr uint32_t kMagic = 0x4E58574Fu;   // "NXWO"
    uint32_t magic = kMagic;
    std::atomic<int> refs{1};
    std::mutex m;                                     // guards the object's state and `waiters`
    std::vector<Waiter*> waiters;

    virtual ~Object();
    virtual bool waitable() const { return false; }
    // With `m` held: true when a wait is satisfied now; consumes the state where Win32 does (auto-reset event, mutex ownership).
    virtual bool try_acquire(uint64_t /*thread key*/) { return false; }
    // With `m` held: the monotonic time (mono_ns) at which this object may become signaled without anybody signaling it
    // (a timer's due time, a poll interval for objects outside the process); UINT64_MAX when only a signal can satisfy it.
    virtual uint64_t deadline_ns() { return UINT64_MAX; }
    // Called by CloseHandle before the reference is dropped.
    virtual void on_close() {}

    void ref() { refs.fetch_add(1, std::memory_order_relaxed); }
    void unref() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
    void signal_locked();   // with `m` held: wake every registered waiter
};

// Resolves a handle; pseudo handles (current process / thread) give nullptr with *pseudo set to -1 / -2.
Object* resolve(HANDLE h, int* pseudo = nullptr);
template <typename T>
T* as(HANDLE h) {
    return dynamic_cast<T*>(resolve(h));
}

struct ThreadObj;
ThreadObj* current_thread_obj();                // creates the object for a thread that nxwin did not start
unsigned thread_port_of(ThreadObj* t);

}  // namespace nxwin
