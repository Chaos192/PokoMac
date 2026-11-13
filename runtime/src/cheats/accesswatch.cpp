// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Find what writes/reads an address (accesswatch.h).
#include "accesswatch.h"

#include <windows.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "gmem.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::cheats::aw {
namespace {
constexpr const char* kTag = "cheats";

uint64_t host_page() {
    static const uint64_t ps = mem::host_page_size();
    return ps;
}

struct State {
    std::mutex mu;
    std::atomic<bool> on{false};
    uint64_t addr = 0;
    unsigned width = 4;
    Access mode = Access::Write;
    uint64_t page_lo = 0, page_hi = 0;     // [lo, hi) of the armed pages
    DWORD orig_protect = 0;                 // protection to restore on stop / between faults
    std::atomic<bool> needs_rearm{false};
    std::vector<Accessor> acc;              // guarded by mu
    std::atomic<uint64_t> faults{0};
    std::atomic<uint64_t> page_faults{0};   // every access the armed pages caught, the value's or not
    std::atomic<uint64_t> near_hits[129];   // accesses at value address + (i - 64) bytes, i != 64.. (nearby fields, wider stores)
    uint64_t start_bits = 0;                // the watched bytes when the watch started (changed_unseen)
    bool start_read = false;
};
State g;
PVOID g_veh = nullptr;

// ---- hardware watchpoints ---------------------------------------------------------------------------------------------------------
// The exact way (as Cheat Engine's default debugger): a data watchpoint on the value's bytes in every guest thread, so only an access that
// touches them traps; a neighbouring field written first in the same page (which unprotects the page for the rest of the pass) or a wider
// store that starts below the value cannot hide it. Windows x64: DR0 / DR7 on each thread, a trap after the access (EXCEPTION_SINGLE_STEP,
// DR6 bit 0), so every access is caught and nothing needs re-arming. macOS on Apple silicon: watchpoint 0 of ARM_DEBUG_STATE64 on each
// thread (thread_set_state works on the process's own threads, measured on an M5 Max: 5 of 5 stores caught, the neighbouring word's
// stores not); it traps before the access (SIGTRAP, which nxwin turns into EXCEPTION_SINGLE_STEP), so the handler switches it off on that
// thread to let the access through and the engine's next pass switches it on again. Elsewhere the page protection below is used.
#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) || (defined(__APPLE__) && defined(__aarch64__))
constexpr bool kHw = true;
#else
constexpr bool kHw = false;
#endif
bool g_hw = false;                           // the current watch uses the watchpoints
std::atomic<bool> g_hw_used{false};          // watchpoints were armed at some time: a trap of one is ours even between watches
std::set<uint64_t> g_hw_armed;               // host thread ids with the watchpoint on (changed under g.mu)
constexpr int kHwOffSlots = 256;
std::atomic<uint64_t> g_hw_off[kHwOffSlots];    // macOS: threads whose handler switched it off (host tid), re-armed at the next pass

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
// DR0 / DR7 of another thread, suspended around the change.
bool hw_set_thread(const kernel::KThread& t, bool on) {
    HANDLE h = static_cast<HANDLE>(t.host_handle);
    if (!h || t.host_tid == GetCurrentThreadId()) return false;
    if (SuspendThread(h) == static_cast<DWORD>(-1)) return false;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = GetThreadContext(h, &c) != 0;
    if (ok) {
        c.Dr7 &= ~(uint64_t{3} | (uint64_t{0xF} << 16));         // L0/G0, RW0, LEN0
        if (on) {
            const unsigned len = g.width >= 8 ? 2u : g.width >= 4 ? 3u : g.width >= 2 ? 1u : 0u;   // LEN: 00 1, 01 2, 11 4, 10 8 bytes
            c.Dr0 = g.addr;
            c.Dr7 |= 1u | (uint64_t{g.mode == Access::Read ? 3u : 1u} << 16) | (uint64_t{len} << 18);
        }
        c.Dr6 = 0;
        ok = SetThreadContext(h, &c) != 0;
    }
    ResumeThread(h);
    return ok;
}
#elif defined(__APPLE__) && defined(__aarch64__)
uint64_t self_tid() { return pthread_mach_thread_np(pthread_self()); }
bool hw_set_port(thread_t port, bool on) {
    arm_debug_state64_t st;
    std::memset(&st, 0, sizeof st);
    if (on) {
        const unsigned off = static_cast<unsigned>(g.addr & 7), n = std::min<unsigned>(g.width, 8 - off);
        st.__wvr[0] = g.addr & ~uint64_t{7};
        // BAS (the value's bytes of the 8-byte block), LSC (2 store, 3 load and store), PAC 2 (EL0), enable
        st.__wcr[0] = ((((1u << n) - 1u) << off) << 5) | ((g.mode == Access::Read ? 3u : 2u) << 3) | (2u << 1) | 1u;
    }
    return thread_set_state(port, ARM_DEBUG_STATE64, reinterpret_cast<thread_state_t>(&st), ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS;
}
bool hw_set_thread(const kernel::KThread& t, bool on) {
    const unsigned port = nxwin::thread_port(static_cast<HANDLE>(t.host_handle));
    return port && hw_set_port(port, on);
}
#else
bool hw_set_thread(const kernel::KThread&, bool) { return false; }
#endif

// Arms (or disarms) every guest thread from a helper thread, so the calling guest thread is armed too (Windows cannot change the context of
// the thread that asks). Returns how many threads took it. Under g.mu.
size_t hw_apply_all(bool on) {
    std::vector<std::shared_ptr<kernel::KThread>> todo;
    for (auto& t : kernel::threads())
        if (t && t->host_handle && (!on || !g_hw_armed.count(t->host_tid))) todo.push_back(std::move(t));
    if (todo.empty()) {
        if (!on) g_hw_armed.clear();
        return 0;
    }
    size_t n = 0;
    std::thread([&] {
        for (const auto& t : todo)
            if (hw_set_thread(*t, on)) {
                ++n;
                if (on) g_hw_armed.insert(t->host_tid);
            }
        if (!on) g_hw_armed.clear();
    }).join();
    return n;
}

DWORD arm_protect() { return g.mode == Access::Read ? PAGE_NOACCESS : PAGE_READONLY; }

// Set [page_lo, page_hi) to `prot`. Returns false on failure.
bool protect_pages(DWORD prot) {
    DWORD old = 0;
    return VirtualProtect(reinterpret_cast<void*>(g.page_lo), static_cast<SIZE_T>(g.page_hi - g.page_lo), prot, &old) != 0;
}

void record(uint64_t host_pc, bool is_write) {
    Accessor a;
    a.is_write = is_write;
    a.guest_fn = guest_function_of(host_pc);
    if (a.guest_fn) a.where = gmem::describe(a.guest_fn);
    else a.where = "host";
    if (::Ctx* c = kernel::current_ctx()) {
        for (int i = 0; i < 31; ++i) a.regs[i] = c->x[i];
        a.sp = c->x[31];
        a.last_pc = c->fn;
        if (!a.guest_fn && c->fn) { a.guest_fn = c->fn; a.where = gmem::describe(c->fn); }
    }
    std::lock_guard<std::mutex> lk(g.mu);
    for (auto& e : g.acc) {
        if (e.guest_fn == a.guest_fn && e.is_write == is_write) {
            e.hits++;
            std::memcpy(e.regs, a.regs, sizeof e.regs);
            e.sp = a.sp;
            e.last_pc = a.last_pc;
            return;
        }
    }
    a.hits = 1;
    if (g.acc.size() < 256) g.acc.push_back(a);
}

LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && g_hw_used.load(std::memory_order_acquire)) {
        // a watchpoint's trap: ours whenever one was armed (a thread may trap while the watch is being set up or taken down)
        const bool counting = g.on.load(std::memory_order_acquire) && g_hw;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        if (!ep->ContextRecord || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;   // not DR0's
        ep->ContextRecord->Dr6 = 0;
        // the access is done (an x64 data breakpoint traps after it); a write watch sees only writes
        if (counting) record(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)), g.mode == Access::Write);
#elif defined(__APPLE__) && defined(__aarch64__)
        if (rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
        // before the access: switch the watchpoint off on this thread so it goes through; the engine's next pass switches it on again
        hw_set_port(pthread_mach_thread_np(pthread_self()), false);
        if (counting) {
            const uint64_t me = self_tid();
            for (auto& s : g_hw_off) {
                uint64_t z = 0;
                if (s.compare_exchange_strong(z, me)) break;
            }
            record(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)), rec->ExceptionInformation[0] == 1);
        }
#else
        return EXCEPTION_CONTINUE_SEARCH;
#endif
        if (counting) {
            g.faults.fetch_add(1, std::memory_order_relaxed);
            g.page_faults.fetch_add(1, std::memory_order_relaxed);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!g.on.load(std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR kind = rec->ExceptionInformation[0];     // 0 read, 1 write, 8 execute
    const uint64_t at = static_cast<uint64_t>(rec->ExceptionInformation[1]);
    if (at < g.page_lo || at >= g.page_hi) return EXCEPTION_CONTINUE_SEARCH;   // not our page: let write_watch / the crash handler see it
    // Only count accesses that touch the watched value's bytes (the page can hold many unrelated values).
    const bool in_value = at >= g.addr && at < g.addr + g.width;
    const bool is_write = kind == 1;
    g.page_faults.fetch_add(1, std::memory_order_relaxed);
    if (!in_value && at + 64 >= g.addr && at <= g.addr + 64) g.near_hits[at + 64 - g.addr].fetch_add(1, std::memory_order_relaxed);
    if (in_value) {
        record(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)), is_write);
        g.faults.fetch_add(1, std::memory_order_relaxed);
    }
    // Let the access through: restore the page's real protection, ask the engine pass to re-arm it.
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(g.page_lo), static_cast<SIZE_T>(g.page_hi - g.page_lo), g.orig_protect, &old);
    g.needs_rearm.store(true, std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool watch_start(uint64_t addr, unsigned width, Access mode, std::string* err) {
    watch_stop();
    if (width == 0) width = 1;
    if (!gmem::in_guest_range(addr, width)) { if (err) *err = "address is outside guest memory"; return false; }
    const uint64_t ps = host_page();
    const uint64_t lo = addr & ~(ps - 1), hi = ((addr + width - 1) & ~(ps - 1)) + ps;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(lo), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) {
        if (err) *err = "page is not committed";
        return false;
    }
    const DWORD pr = mbi.Protect & 0xFF;
    if (pr == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) { if (err) *err = "page is already protected (tracked elsewhere)"; return false; }
    std::lock_guard<std::mutex> lk(g.mu);
    g.addr = addr;
    g.width = width;
    g.mode = mode;
    g.page_lo = lo;
    g.page_hi = hi;
    g.orig_protect = pr;
    g.acc.clear();
    g.faults.store(0);
    g.page_faults.store(0);
    for (auto& h : g.near_hits) h.store(0);
    g.start_bits = 0;
    g.start_read = gmem::read(addr, &g.start_bits, std::min<unsigned>(width, 8));
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_fault);   // front, before the crash handler; we pass non-ours through
    if (!g_veh) { if (err) *err = "could not install the fault handler"; return false; }
    g_hw = false;
    g_hw_armed.clear();
    for (auto& s : g_hw_off) s.store(0);
    // the handler must know the watch before the first thread is armed: the game writes the value at once (run hk6: the main thread's
    // store trapped before the watch said it was on, and the trap went to the crash handler)
    g_hw_used.store(true);
    g_hw = kHw;
    g.on.store(kHw, std::memory_order_release);
    if (kHw && hw_apply_all(true) > 0) {
        NXLOG_I(kTag, "watching %s of %s (width %u) with a hardware watchpoint in %zu threads", mode == Access::Read ? "reads" : "writes",
                gmem::describe(addr).c_str(), width, g_hw_armed.size());
        return true;
    }
    g.on.store(false, std::memory_order_release);
    g_hw = false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(lo), static_cast<SIZE_T>(hi - lo), arm_protect(), &old)) {
        if (err) *err = "could not arm the page";
        return false;
    }
    g.on.store(true, std::memory_order_release);
    NXLOG_I(kTag, "watching %s of %s (width %u)", mode == Access::Read ? "reads" : "writes", gmem::describe(addr).c_str(), width);
    return true;
}

void watch_stop() {
    if (!g.on.exchange(false)) return;
    std::lock_guard<std::mutex> lk(g.mu);
    if (g_hw) {
        hw_apply_all(false);
        g_hw = false;
        return;
    }
    protect_pages(g.orig_protect);
    g.needs_rearm.store(false);
    // the VEH stays installed (cheap, passes through) so a later watch does not reinstall it
}

bool watching() { return g.on.load(std::memory_order_acquire); }
uint64_t watch_addr() { return g.addr; }
Access watch_mode() { return g.mode; }

void rearm() {
    if (!g.on.load(std::memory_order_acquire)) return;
    if (g_hw) {                                  // threads started since, and (macOS) threads whose handler switched it off
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.on.load()) return;
        for (auto& s : g_hw_off)
            if (const uint64_t id = s.exchange(0)) g_hw_armed.erase(id);
        hw_apply_all(true);
        return;
    }
    if (!g.needs_rearm.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.on.load()) return;
    protect_pages(arm_protect());
}

std::vector<Accessor> accessors() {
    std::lock_guard<std::mutex> lk(g.mu);
    std::vector<Accessor> out = g.acc;
    std::sort(out.begin(), out.end(), [](const Accessor& a, const Accessor& b) { return a.hits > b.hits; });
    return out;
}

void clear() {
    std::lock_guard<std::mutex> lk(g.mu);
    g.acc.clear();
    g.faults.store(0);
}

uint64_t total_faults() { return g.faults.load(std::memory_order_relaxed); }

std::string page_report() {
    std::string out = std::to_string(g.page_faults.load()) + " accesses on the watched page";
    std::string nearby;
    for (int i = 0; i < 129; ++i)
        if (const uint64_t n = g.near_hits[i].load())
            nearby += (nearby.empty() ? "" : ", ") + std::string(i < 64 ? "-" : "+") + std::to_string(i < 64 ? 64 - i : i - 64) + " (" + std::to_string(n) + ")";
    if (!nearby.empty()) out += "; nearby the value, at offset (count): " + nearby;
    return out;
}

bool changed_unseen() {
    if (!g.on.load(std::memory_order_acquire) || !g.start_read || g.page_faults.load(std::memory_order_relaxed) != 0) return false;
    uint64_t now = 0;
    return gmem::read(g.addr, &now, std::min<unsigned>(g.width, 8)) && now != g.start_bits;
}

}  // namespace nxrt::cheats::aw
