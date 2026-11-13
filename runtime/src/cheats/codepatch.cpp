// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Code patches, injection and breakpoints (codepatch.h).
#include "codepatch.h"

#include <windows.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "control.h"
#include "disasm.h"
#include "gmem.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/missed.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "reach.h"

namespace nxrt::cheats::cp {
namespace {
constexpr const char* kTag = "cheats";

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// One function the patcher changed: the build's compiled code, the unit that replaces it, and the jump written over the former's entry.
struct Fn {
    uint64_t start = 0, end = 0;
    std::string module;
    GuestFn orig = nullptr;          // the dispatcher's function before the first unit (the build's compiled code)
    GuestFn unit = nullptr;          // the unit now in use
    bool room = false;               // the compiled entry has room for the jump (no other function starts inside it)
    bool redirected = false;
    uint8_t saved[16] = {};
    size_t jump_len = 0;
    uint64_t gen = 0;                // bumped by every rebuild; a worker whose generation is stale throws its result away
    uint64_t done_gen = 0;           // the rebuild whose swap has run (== gen: the code runs what memory holds, see settled())
};

struct G {
    std::mutex mu;
    std::map<uint64_t, Patch> patches;           // by address
    std::map<uint64_t, Breakpoint> bps;          // by address
    std::map<uint64_t, Fn> fns;                  // by function start
    std::map<uint64_t, uint32_t> site_hooks;     // inline hooks registered (address -> hook id)
    std::set<uint64_t> cave_jumps;               // patches that branch into a code cave (live only: the cave is not part of the module file)
    // a held thread
    std::mutex hold_mu;
    std::condition_variable hold_cv;
    bool holding = false;
    Regs held;
    int resume_cmd = -1;                         // -1 none, else Resume
    // stepping: the thread and function a Step / ToReturn follows
    uint64_t step_thread = 0, step_fn = 0;
    int step_mode = -1;
};
G& g() {
    static G s;
    return s;
}

#if defined(_M_X64) || defined(__x86_64__)
constexpr size_t kJumpLen = 14;          // jmp [rip+0]; .quad target
void make_jump(uint8_t* out, uint64_t to) {
    out[0] = 0xff;
    out[1] = 0x25;
    std::memset(out + 2, 0, 4);
    std::memcpy(out + 6, &to, 8);
}
#elif defined(_M_ARM64) || defined(__aarch64__)
constexpr size_t kJumpLen = 16;          // ldr x16, #8; br x16; .quad target
void make_jump(uint8_t* out, uint64_t to) {
    const uint32_t a = 0x58000050u, b = 0xd61f0200u;
    std::memcpy(out, &a, 4);
    std::memcpy(out + 4, &b, 4);
    std::memcpy(out + 8, &to, 8);
}
#else
constexpr size_t kJumpLen = 0;
void make_jump(uint8_t*, uint64_t) {}
#endif

// Writes the jump at `at` (the compiled entry). False with *why when the host does not let its code pages be written.
#if defined(__APPLE__)
// Apple silicon: the program's signed code pages cannot be made writable (mprotect: EACCES, their maximum protection is r-x), and no page
// is ever writable and executable at once (W^X, for every app, hardened runtime or not). What a program without the hardened runtime (pkp
// is signed ad hoc, without it) may do is run code from its own anonymous pages. So the patched pages are made elsewhere: a copy of the
// code pages the jump touches, written, made read+execute, then mapped over the code pages in one step (mach_vm_remap, VM_FLAGS_OVERWRITE),
// so the code there never lacks execute permission, not even for a thread running it at that moment. Measured on macOS 27.2 (M5 Max): the
// patched code runs and the process stays valid (the same rewrite under the hardened runtime is killed), with four threads calling the
// function throughout the swap (docs/CHEATS.md, "Live code patches on Apple silicon").
bool write_code(void* at, const uint8_t* bytes, size_t n, uint8_t* saved, std::string* why) {
    const uint64_t ps = vm_page_size, a = reinterpret_cast<uint64_t>(at);
    const uint64_t lo = a & ~(ps - 1), hi = (a + n + ps - 1) & ~(ps - 1), len = hi - lo;
    void* tmp = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (tmp == MAP_FAILED) {
        *why = "no memory for the patched code page";
        return false;
    }
    std::memcpy(tmp, reinterpret_cast<const void*>(lo), len);
    if (saved) std::memcpy(saved, at, n);
    std::memcpy(static_cast<uint8_t*>(tmp) + (a - lo), bytes, n);
    mach_vm_address_t dst = lo;
    vm_prot_t cur = 0, mx = 0;
    kern_return_t kr = KERN_FAILURE;
    if (mprotect(tmp, len, PROT_READ | PROT_EXEC) == 0) {
        sys_icache_invalidate(tmp, len);
        kr = mach_vm_remap(mach_task_self(), &dst, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                           reinterpret_cast<mach_vm_address_t>(tmp), FALSE, &cur, &mx, VM_INHERIT_COPY);
    }
    munmap(tmp, len);                                    // the code pages keep the new memory
    if (kr != KERN_SUCCESS || dst != lo) {
        *why = "this host did not let the program's code pages be replaced (mach_vm_remap " + std::to_string(kr) + "), so direct calls keep the "
               "old code. Build the game again with this release (its builds check for live patches at every function entry): then every call runs the patch";
        return false;
    }
    sys_icache_invalidate(at, n);
    return true;
}
#else
bool write_code(void* at, const uint8_t* bytes, size_t n, uint8_t* saved, std::string* why) {
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        *why = "this host does not let the program's code be rewritten (code signing), so direct calls keep the old code. Build the "
               "game again with this release (its builds check for live patches at every function entry): then every call runs the patch";
        return false;
    }
    if (saved) std::memcpy(saved, at, n);
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &tmp);
#if defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    __builtin___clear_cache(static_cast<char*>(at), static_cast<char*>(at) + n);
#endif
    return true;
}
#endif

// ---- live patches through NX_OV (modules lifted with --live-patch, ABI 5) -----------------------------------------------------------
// Such a module's every function entry checks Ctx.ov: while no live patch exists it is null (one load and a branch), else it points at
// ov_table, per module index the bitmap of the functions overridden (one bit per 4-byte word of code from the module's base). A set bit
// sends the call to nx_hook_entry, where ov_hook runs the unit. This reaches every caller, direct ones and copies the compiler inlined
// into them included, on every host, with no write to the program's code (which Apple silicon does not allow).
constexpr uint32_t kOvModules = 64;
struct Ov {
    const uint8_t* table[kOvModules] = {};             // what Ctx.ov points at (indexed by NxModule.index)
    std::vector<uint8_t> maps[kOvModules];             // the bitmaps behind table[]
    std::map<uint64_t, uint32_t> hooks;                // overridden function -> its entry hook
    uint32_t frame_cb = 0;                             // re-points new threads' Ctx.ov while any override is live
    std::atomic<bool> live{false};                     // any override (read by the frame callback, which may run on a held thread)
};
Ov& ov() {
    static Ov o;
    return o;
}

// The loop heads: per overridden function, the unit's entry by guest address (missed::code_unit_resume), for nx_hook_resume.
std::mutex g_resume_mu;
std::map<uint64_t, void (*)(::Ctx*, uint64_t)> g_resume;

int ov_hook(::Ctx* c, uint64_t, void* unit) {
    reinterpret_cast<GuestFn>(unit)(c);                // the function recompiled from the patched bytes did the call's work
    return 1;
}

// Every guest thread's Ctx.ov: the table while an override is live, else null. Threads created later get it at the next frame boundary.
void ov_point_threads(const void* to) {
    for (auto& t : kernel::threads())
        if (t) t->ctx.ov = to;
}

void ov_frame(void*) {
    if (ov().live.load(std::memory_order_acquire)) ov_point_threads(ov().table);
}

bool ov_bit(const NxModule* nx, uint64_t fn, uint8_t** byte, uint8_t* mask) {
    if (!nx || nx->index >= kOvModules || fn < nx->base || fn >= nx->text_end) return false;
    Ov& o = ov();
    std::vector<uint8_t>& m = o.maps[nx->index];
    if (m.empty()) {
        m.assign(static_cast<size_t>((nx->text_end - nx->base) >> 5) + 1, 0);
        o.table[nx->index] = m.data();
    }
    const uint64_t k = (fn - nx->base) >> 2;
    *byte = &m[static_cast<size_t>(k >> 3)];
    *mask = static_cast<uint8_t>(1u << (k & 7));
    return true;
}

// Sends every call of fn to unit (the hook first, then the bit, so a thread that sees the bit finds the hook). Called with the others parked.
bool ov_set(uint64_t fn, GuestFn unit) {
    const NxModule* nx = nxmodule_at(fn);
    if (!nx || !nxmodule_live_patch(nx)) return false;
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (!ov_bit(nx, fn, &byte, &mask)) return false;
    Ov& o = ov();
    auto it = o.hooks.find(fn);
    if (it != o.hooks.end()) hook_remove(it->second);
    o.hooks[fn] = hook_add_entry(fn, &ov_hook, reinterpret_cast<void*>(unit));
    {
        std::lock_guard<std::mutex> lk(g_resume_mu);
        g_resume[fn] = missed::code_unit_resume(unit);
    }
    __atomic_or_fetch(byte, mask, __ATOMIC_SEQ_CST);
    o.live.store(true, std::memory_order_release);
    if (!o.frame_cb) o.frame_cb = ctl::add_frame_callback(&ov_frame, nullptr);
    ov_point_threads(o.table);
    return true;
}

// Back to the build's code for fn (the bit first, then the hook). With no override left, Ctx.ov goes back to null.
void ov_clear(uint64_t fn) {
    Ov& o = ov();
    auto it = o.hooks.find(fn);
    if (it == o.hooks.end()) return;
    uint8_t* byte = nullptr;
    uint8_t mask = 0;
    if (ov_bit(nxmodule_at(fn), fn, &byte, &mask)) __atomic_and_fetch(byte, static_cast<uint8_t>(~mask), __ATOMIC_SEQ_CST);
    hook_remove(it->second);
    o.hooks.erase(it);
    {
        std::lock_guard<std::mutex> lk(g_resume_mu);
        g_resume.erase(fn);
    }
    if (o.hooks.empty()) {
        o.live.store(false, std::memory_order_release);
        ov_point_threads(nullptr);
        if (o.frame_cb) { ctl::remove_frame_callback(o.frame_cb); o.frame_cb = 0; }
    }
}

Regs regs_of(::Ctx* c, uint64_t pc) {
    Regs r;
    for (int i = 0; i < 31; ++i) r.x[i] = c->x[i];
    r.sp = c->x[31];
    r.pc = pc;
    r.fn = c->fn;
    r.nzcv = (c->N << 3) | (c->Z << 2) | (c->C << 1) | c->V;
    for (int i = 0; i < 8; ++i) std::memcpy(&r.d[i], &c->v[i], 8);
    if (kernel::KThread* t = kernel::current_thread()) {
        r.thread_id = t->id;
        r.thread = t->name;
    }
    return r;
}

uint64_t cur_thread_id() {
    kernel::KThread* t = kernel::current_thread();
    return t ? t->id : 0;
}

bool is_ret(uint64_t addr) {
    uint32_t w = 0;
    return gmem::read(addr, &w, 4) && (w & 0xfffffc1fu) == 0xd65f0000u;
}

// Holds the calling guest thread until the overlay resumes it. On the presenting thread the pause service keeps the overlay going.
void hold(::Ctx* c, uint64_t pc) {
    G& s = g();
    std::unique_lock<std::mutex> lk(s.hold_mu);
    while (s.holding) s.hold_cv.wait_for(lk, std::chrono::milliseconds(10));     // one held thread at a time
    s.holding = true;
    s.held = regs_of(c, pc);
    s.resume_cmd = -1;
    NXLOG_I(kTag, "breakpoint: thread %s held at %s", s.held.thread.c_str(), where(pc).c_str());
    while (s.resume_cmd < 0) {
        lk.unlock();
        ctl::service_once();
        lk.lock();
        s.hold_cv.wait_for(lk, std::chrono::milliseconds(8));
    }
    // registers edited while held go back into the context
    for (int i = 0; i < 31; ++i) c->x[i] = s.held.x[i];
    const int cmd = s.resume_cmd;
    s.step_mode = cmd == static_cast<int>(Resume::Continue) ? -1 : cmd;
    s.step_thread = s.held.thread_id;
    uint64_t fs = 0, fe = 0;
    std::string m;
    s.step_fn = function_at(pc, &m, &fs, &fe) ? fs : 0;
    s.holding = false;
    s.hold_cv.notify_all();
}

void site_hook(::Ctx* c, uint64_t addr, void*) {
    G& s = g();
    bool stop = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.bps.find(addr);
        if (it != s.bps.end() && it->second.enabled) {
            it->second.hits++;
            it->second.last = regs_of(c, addr);
            stop = it->second.stop;
        }
    }
    if (!stop && s.step_mode >= 0 && s.step_thread == cur_thread_id()) {
        uint64_t fs = 0, fe = 0;
        std::string m;
        if (function_at(addr, &m, &fs, &fe) && fs == s.step_fn)
            stop = s.step_mode == static_cast<int>(Resume::Step) || (s.step_mode == static_cast<int>(Resume::ToReturn) && is_ret(addr));
    }
    if (stop) hold(c, addr);
}

void ensure_site_hooks(const std::vector<uint64_t>& sites) {
    G& s = g();
    for (uint64_t a : sites)
        if (!s.site_hooks.count(a)) s.site_hooks[a] = hook_add_inline(a, &site_hook, nullptr);
}

void set_state_locked(uint64_t fn_start, State st, const std::string& note) {
    G& s = g();
    for (auto& kv : s.patches)
        if (kv.second.fn_start == fn_start && kv.second.state != State::Reverted) { kv.second.state = st; kv.second.note = note; }
    for (auto& kv : s.bps) {
        uint64_t fs = 0, fe = 0;
        std::string m;
        if (function_at(kv.first, &m, &fs, &fe) && fs == fn_start) { kv.second.state = st; kv.second.note = note; }
    }
}

// Everything the patched words reach that the dispatcher may not have yet (a code cave, the instruction after the hook): made ready here,
// on the patch's worker thread before the swap, so no guest thread waits for the build's compiler when it first runs the patch (reach.h;
// a code cheat turned on stalled a frame for 12.7 s on the PC). The game keeps running the old code meanwhile.
void prepare_reach(uint64_t fn_start, uint64_t fn_end, const std::vector<std::pair<uint64_t, uint32_t>>& patched) {
    reach::Source src;
    src.read = [](uint64_t a, uint32_t* w) { return gmem::read(a, w, 4); };
    src.region = [](uint64_t a, uint64_t* b, uint64_t* e) { return missed::code_region_of(a, b, e); };
    const std::vector<uint64_t> targets = reach::to_prepare(patched, fn_start, fn_end, src);
    size_t made = 0;
    for (uint64_t t : targets) {
        std::string e;
        if (missed::prepare(t, &e)) ++made;
        else NXLOG_W(kTag, "code the patch in %s reaches at %s could not be made ready (%s): its first call compiles it", where(fn_start).c_str(),
                     where(t).c_str(), e.c_str());
    }
    if (!targets.empty())
        NXLOG_I(kTag, "code at %s: %zu of the %zu addresses it reaches made ready before the swap", where(fn_start).c_str(), made, targets.size());
}

// Lifts the function again from its bytes in memory (with every patch in it) and the sites its breakpoints need, then swaps it in at the
// next frame boundary. With nothing left in it, the compiled code comes back. Called with s.mu held.
void rebuild_locked(uint64_t fn_start) {
    G& s = g();
    Fn& f = s.fns[fn_start];
    f.gen++;
    const uint64_t gen = f.gen;
    bool any_patch = false, any_bp = false, any_stop = false;
    for (const auto& kv : s.patches)
        if (kv.second.fn_start == fn_start && kv.second.state != State::Reverted) any_patch = true;
    std::vector<uint64_t> sites;
    for (const auto& kv : s.bps) {
        uint64_t fs = 0, fe = 0;
        std::string m;
        if (!function_at(kv.first, &m, &fs, &fe) || fs != fn_start) continue;
        any_bp = true;
        any_stop |= kv.second.stop;
        sites.push_back(kv.first);
    }
    if (!any_patch && !any_bp) {
        // back to the build's code
        if (f.orig) {
            const GuestFn orig = f.orig;
            const bool redirected = f.redirected;
            uint8_t saved[16];
            std::memcpy(saved, f.saved, sizeof saved);
            const size_t len = f.jump_len;
            f.redirected = false;
            f.unit = nullptr;
            ctl::run_held([orig, redirected, saved, len, fn_start, gen] {
                ov_clear(fn_start);
                if (redirected) {
                    std::string why;
                    uint8_t tmp[16];
                    write_code(reinterpret_cast<void*>(orig), saved, len, tmp, &why);
                }
                fn_register(fn_start, orig);
                G& s = g();
                std::lock_guard<std::mutex> lk(s.mu);
                if (auto it = s.fns.find(fn_start); it != s.fns.end() && it->second.gen == gen) it->second.done_gen = gen;
            });
        } else {
            f.done_gen = gen;                                // no compiled code was replaced: nothing to swap back
        }
        return;
    }
    // every instruction gets a site when a stopping breakpoint is in the function, so a held thread can step through it
    if (any_stop)
        for (uint64_t a = f.start; a < f.end; a += 4) sites.push_back(a);
    std::sort(sites.begin(), sites.end());
    sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
    ensure_site_hooks(sites);
    std::vector<uint8_t> bytes(static_cast<size_t>(f.end - f.start));
    if (!gmem::read(f.start, bytes.data(), bytes.size())) {
        set_state_locked(fn_start, State::Failed, "the function's code is not readable");
        return;
    }
    set_state_locked(fn_start, State::Compiling, "lifting the function again from the patched bytes");
    const std::string name = "patch_" + f.module;
    // the patched words, for what they reach (reach.h): made ready on this worker before the swap, never by a guest thread at its first run
    std::vector<std::pair<uint64_t, uint32_t>> patched;
    for (const auto& kv : s.patches) {
        if (kv.second.fn_start != fn_start || kv.second.state == State::Reverted) continue;
        for (size_t k = 0; k + 4 <= kv.second.bytes.size(); k += 4) {
            uint32_t w = 0;
            std::memcpy(&w, kv.second.bytes.data() + k, 4);
            patched.push_back({kv.first + k, w});
        }
    }
    const uint64_t fn_end = f.end;
    std::thread([name, fn_start, fn_end, bytes, sites, gen, patched] {
        const auto t0 = std::chrono::steady_clock::now();
        std::string err;
        const GuestFn unit = missed::make_code_unit(name, fn_start, bytes, sites, &err);
        if (unit) prepare_reach(fn_start, fn_end, patched);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        G& s = g();
        std::lock_guard<std::mutex> lk(s.mu);
        Fn& f = s.fns[fn_start];
        if (f.gen != gen) return;                       // a newer rebuild replaced this one
        if (!unit) {
            // make_code_unit registers nothing on failure: the old code keeps running
            set_state_locked(fn_start, State::Failed, "cannot recompile it now (" + err + "): the IPS patch takes effect with the next build");
            NXLOG_W(kTag, "code patch at %s: %s", where(fn_start).c_str(), err.c_str());
            return;
        }
        f.unit = unit;
        char note[160];
        std::snprintf(note, sizeof note, "recompiled in %.1f s", secs);
        set_state_locked(fn_start, State::Compiling, std::string(note) + "; swapping it in at the next frame");
        const GuestFn orig = f.orig;
        const bool room = f.room;
        ctl::run_held([fn_start, unit, orig, room, gen, secs] {
            G& s = g();
            std::lock_guard<std::mutex> lk(s.mu);
            Fn& f = s.fns[fn_start];
            if (f.gen != gen) return;
            fn_register(fn_start, unit);                     // indirect calls (make_code_unit did this; again after a revert raced it)
            f.done_gen = gen;                                // the entry, its override bit or its jump follow below, in this same hold
            std::string why;
            bool direct = false;
            if (ov_set(fn_start, unit)) {                    // a module lifted with --live-patch: every call, inlined copies included
                char note[200];
                std::snprintf(note, sizeof note, "live for every call (recompiled in %.1f s; the build checks for live patches at each function entry)", secs);
                set_state_locked(fn_start, State::Live, note);
                NXLOG_I(kTag, "code at %s: %s", where(fn_start).c_str(), note);
                return;
            }
            if (kJumpLen && orig && room) {
                uint8_t jump[16];
                make_jump(jump, reinterpret_cast<uint64_t>(unit));
                if (f.redirected) {
                    uint8_t tmp[16];
                    direct = write_code(reinterpret_cast<void*>(orig), jump, kJumpLen, tmp, &why);
                } else {
                    direct = write_code(reinterpret_cast<void*>(orig), jump, kJumpLen, f.saved, &why);
                    if (direct) { f.redirected = true; f.jump_len = kJumpLen; }
                }
            } else if (!room) {
                why = "the compiled function is too short for a jump, so only indirect calls run the new code (build the game again with this "
                      "release: its builds check for live patches at every function entry and reach every call)";
            } else {
                why = "no compiled entry to redirect";
            }
            char note[400];
            if (direct) std::snprintf(note, sizeof note, "live (recompiled in %.1f s; callers the compiler inlined it into keep the old code until the game is built again with this release)", secs);
            else std::snprintf(note, sizeof note, "live for indirect calls (recompiled in %.1f s): %s", secs, why.c_str());
            set_state_locked(fn_start, direct ? State::Live : State::DispatchOnly, note);
            NXLOG_I(kTag, "code at %s: %s", where(fn_start).c_str(), note);
        });
    }).detach();
}

// The function state for a new patch or breakpoint (the build's compiled entry, and whether it has room for the jump).
bool fn_for_locked(uint64_t addr, uint64_t* fn_start, std::string* err) {
    std::string module;
    uint64_t start = 0, end = 0;
    if (!function_at(addr, &module, &start, &end)) {
        *err = hex(addr) + " is not in code the build lifted";
        return false;
    }
    G& s = g();
    auto it = s.fns.find(start);
    if (it == s.fns.end()) {
        Fn f;
        f.start = start;
        f.end = std::min<uint64_t>(end, start + 0x40000);      // a unit of at most 256 KiB
        f.module = module;
        f.orig = fn_lookup(start);
        // room for the jump: no other registered function's host code starts within it (judged before any unit replaces this one)
        if (f.orig && kJumpLen) {
            const uint64_t h = reinterpret_cast<uint64_t>(f.orig);
            f.room = guest_function_of(h) == start && guest_function_of(h + kJumpLen - 1) == start;
        }
        s.fns[start] = f;
    }
    *fn_start = start;
    return true;
}

void put_be(std::vector<uint8_t>& v, uint64_t x, int n) {
    for (int i = n - 1; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::Compiling: return "compiling";
        case State::Live: return "live";
        case State::DispatchOnly: return "live (indirect calls)";
        case State::Failed: return "next build";
        case State::Reverted: return "reverted";
    }
    return "?";
}

bool function_at(uint64_t addr, std::string* module, uint64_t* start, uint64_t* end) {
    const NxModule* nx = nxmodule_at(addr);
    if (!nx || !nx->fns || nx->nfns <= 0 || addr >= nx->text_end) return false;
    int lo = 0, hi = nx->nfns;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (nx->fns[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    // fns may list one address twice (a function and its alias): take the last start at or below addr, the end is the next higher start
    const uint64_t s = nx->fns[lo - 1].addr;
    uint64_t e = nx->text_end;
    for (int i = lo; i < nx->nfns; ++i)
        if (nx->fns[i].addr > s) { e = nx->fns[i].addr; break; }
    if (module) *module = nx->name ? nx->name : "";
    if (start) *start = s;
    if (end) *end = e;
    return true;
}

std::string where(uint64_t addr) {
    std::string out = gmem::describe(addr);
    std::string mod;
    uint64_t s = 0, e = 0;
    if (function_at(addr, &mod, &s, &e)) {
        std::string fn = gmem::describe(s);
        if (Module* m = module_at(s)) {
            const std::string sym = m->describe(s);
            if (!sym.empty()) fn += " " + sym;
        }
        out += " (fn " + fn + ")";
    }
    return out;
}

namespace {
// apply's record and write, without the rebuild (s.mu held): *fn_out is the function to lift again.
bool apply_locked(uint64_t addr, const std::vector<uint8_t>& bytes, bool by_cheat, std::string* err, uint64_t* fn_out) {
    if ((addr & 3) || bytes.empty() || bytes.size() > 64 || (bytes.size() & 3)) {
        *err = "a code patch is 4 to 64 bytes at an instruction address";
        return false;
    }
    G& s = g();
    uint64_t fn_start = 0;
    if (!fn_for_locked(addr, &fn_start, err)) return false;
    const Fn& f = s.fns[fn_start];
    if (addr + bytes.size() > f.end) {
        *err = "the patch runs past the end of its function";
        return false;
    }
    Patch p;
    auto old = s.patches.find(addr);
    p.original.resize(bytes.size());
    if (old != s.patches.end() && old->second.state != State::Reverted && old->second.original.size() == bytes.size()) p.original = old->second.original;
    else if (!gmem::read(addr, p.original.data(), p.original.size())) { *err = "the code is not readable"; return false; }
    if (!gmem::write(addr, bytes.data(), bytes.size())) {
        *err = "the code page could not be written";
        return false;
    }
    p.addr = addr;
    p.bytes = bytes;
    p.module = f.module;
    p.module_base = 0;
    if (const NxModule* nx = nxmodule_at(addr)) p.module_base = nx->base;
    p.fn_start = fn_start;
    p.fn_end = f.end;
    p.by_cheat = by_cheat;
    s.patches[addr] = p;
    NXLOG_I(kTag, "code patch at %s: %zu bytes%s", where(addr).c_str(), bytes.size(), by_cheat ? " (a cheat's)" : "");
    *fn_out = fn_start;
    return true;
}

// revert's write and record, without the rebuild (s.mu held).
bool revert_locked(uint64_t addr, std::string* err, uint64_t* fn_out) {
    G& s = g();
    auto it = s.patches.find(addr);
    if (it == s.patches.end() || it->second.state == State::Reverted) {
        *err = "no patch there";
        return false;
    }
    if (it->second.by_game) {
        *err = "the game wrote this code itself: nothing to put back";
        return false;
    }
    gmem::write(addr, it->second.original.data(), it->second.original.size());
    it->second.state = State::Reverted;
    it->second.note = "original bytes back";
    *fn_out = it->second.fn_start;
    return true;
}
}  // namespace

bool apply(uint64_t addr, const std::vector<uint8_t>& bytes, std::string* err) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    uint64_t fn = 0;
    if (!apply_locked(addr, bytes, false, err, &fn)) return false;
    rebuild_locked(fn);
    return true;
}

bool revert(uint64_t addr, std::string* err) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    uint64_t fn = 0;
    if (!revert_locked(addr, err, &fn)) return false;
    rebuild_locked(fn);
    return true;
}

size_t apply_words(const std::vector<std::pair<uint64_t, uint32_t>>& words, std::vector<std::string>* errs) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    if (errs) errs->assign(words.size(), std::string());
    std::set<uint64_t> fns;
    size_t n = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        std::vector<uint8_t> b(4);
        std::memcpy(b.data(), &words[i].second, 4);
        std::string e;
        uint64_t fn = 0;
        if (apply_locked(words[i].first, b, true, &e, &fn)) { fns.insert(fn); ++n; }
        else if (errs) (*errs)[i] = e;
    }
    for (uint64_t fn : fns) rebuild_locked(fn);
    return n;
}

bool settled(uint64_t addr) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    uint64_t fn = 0;
    if (auto p = s.patches.find(addr); p != s.patches.end()) fn = p->second.fn_start;
    else if (!function_at(addr, nullptr, &fn, nullptr)) return true;
    auto it = s.fns.find(fn);
    return it == s.fns.end() || it->second.done_gen == it->second.gen;
}

size_t revert_words(const std::vector<uint64_t>& addrs) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    std::set<uint64_t> fns;
    size_t n = 0;
    for (uint64_t a : addrs) {
        std::string e;
        uint64_t fn = 0;
        if (revert_locked(a, &e, &fn)) { fns.insert(fn); ++n; }
    }
    for (uint64_t fn : fns) rebuild_locked(fn);
    return n;
}

bool guest_code_written(uint64_t addr, size_t len, std::string* err) {
    if (!len || (addr & 3)) {
        *err = "code is written in whole instructions";
        return false;
    }
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    std::set<uint64_t> fns;
    for (uint64_t a = addr; a < addr + len; a += 4) {
        uint64_t fn_start = 0;
        std::string e;
        if (!fn_for_locked(a, &fn_start, &e)) continue;
        const Fn& f = s.fns[fn_start];
        Patch p;
        p.addr = a;
        p.by_game = true;
        p.bytes.resize(4);
        gmem::read(a, p.bytes.data(), 4);
        p.module = f.module;
        if (const NxModule* nx = nxmodule_at(a)) p.module_base = nx->base;
        p.fn_start = fn_start;
        p.fn_end = f.end;
        p.note = "written by the game";
        s.patches[a] = p;
        fns.insert(fn_start);
    }
    if (fns.empty()) {
        *err = "no lifted code there";
        return false;
    }
    NXLOG_I(kTag, "the game wrote its code at %s (%zu bytes): %zu functions lifted again", where(addr).c_str(), len, fns.size());
    for (uint64_t fn : fns) rebuild_locked(fn);
    return true;
}

void code_unmapped(uint64_t base, uint64_t size) {
    if (!size) return;
    const uint64_t end = base + size;
    auto inside = [&](uint64_t a) { return a >= base && a < end; };
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    size_t fns = 0, pats = 0, bps = 0;
    for (auto it = s.fns.begin(); it != s.fns.end();) {
        if (!inside(it->first)) { ++it; continue; }
        it->second.gen++;                            // a rebuild still compiling for it throws its result away
        ov_clear(it->first);                         // the override bit, its entry hook and the loop-head entry
        if (it->second.unit) fn_unregister(it->first);   // the dispatcher slot the unit took (the module's own go with its tables)
        it = s.fns.erase(it);
        ++fns;
    }
    for (auto it = s.patches.begin(); it != s.patches.end();) {
        if (inside(it->first)) { s.cave_jumps.erase(it->first); it = s.patches.erase(it); ++pats; }
        else ++it;
    }
    for (auto it = s.bps.begin(); it != s.bps.end();) {
        if (inside(it->first)) { it = s.bps.erase(it); ++bps; }
        else ++it;
    }
    for (auto it = s.site_hooks.begin(); it != s.site_hooks.end();) {
        if (inside(it->first)) { hook_remove(it->second); it = s.site_hooks.erase(it); }
        else ++it;
    }
    if (fns || pats || bps)
        NXLOG_I(kTag, "code at 0x%llx..0x%llx unmapped: %zu patched functions, %zu patches and %zu breakpoints dropped",
                static_cast<unsigned long long>(base), static_cast<unsigned long long>(end), fns, pats, bps);
}

std::vector<Patch> patches() {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<Patch> out;
    for (const auto& kv : s.patches) out.push_back(kv.second);
    return out;
}

std::vector<uint8_t> ips_for_module(const std::string& module) {
    struct Rec { uint64_t off, addr; std::vector<uint8_t> bytes; bool operator<(const Rec& o) const { return off < o.off; } };
    std::vector<Rec> recs;
    for (const Patch& p : patches())
        if (p.module == module && p.state != State::Reverted && !p.by_game && !p.by_cheat && !g().cave_jumps.count(p.addr)) recs.push_back({p.addr - p.module_base + 0x100, p.addr, p.bytes});
    if (recs.empty()) return {};
    std::sort(recs.begin(), recs.end());
    bool wide = false;
    for (const auto& r : recs) wide |= r.off + r.bytes.size() > 0xffffff;
    std::vector<uint8_t> out;
    const char* head = wide ? "IPS32" : "PATCH";
    out.insert(out.end(), head, head + 5);
    for (const auto& r : recs) {
        uint64_t off = r.off;
        // 24-bit IPS cannot hold the offset 0x454f46 (it reads as "EOF"): start one byte earlier with the byte that is there now
        std::vector<uint8_t> data = r.bytes;
        if (!wide && off == 0x454f46) {
            uint8_t before = 0;
            gmem::read(r.addr - 1, &before, 1);
            off -= 1;
            data.insert(data.begin(), before);
        }
        put_be(out, off, wide ? 4 : 3);
        put_be(out, data.size(), 2);
        out.insert(out.end(), data.begin(), data.end());
    }
    const char* tail = wide ? "EEOF" : "EOF";
    out.insert(out.end(), tail, tail + std::strlen(tail));
    return out;
}

std::vector<std::string> write_ips(const std::string& dir, std::string* err) {
    std::set<std::string> mods;
    for (const Patch& p : patches())
        if (p.state != State::Reverted) mods.insert(p.module);
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    for (const std::string& m : mods) {
        const std::vector<uint8_t> ips = ips_for_module(m);
        const std::string bid = gmem::module_build_id(m);
        if (ips.empty() || bid.size() < 16) continue;
        const bool wide = ips.size() >= 5 && std::memcmp(ips.data(), "IPS32", 5) == 0;
        const std::string path = dir + "/" + bid.substr(0, 16) + (wide ? ".ips32" : ".ips");
        std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
        if (!f) {
            if (err) *err = "cannot write " + path;
            continue;
        }
        f.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
        out.push_back(path);
        NXLOG_I(kTag, "IPS patch for %s written: %s (%zu bytes)", m.c_str(), path.c_str(), ips.size());
    }
    return out;
}

bool inject(uint64_t at, const std::vector<std::string>& lines, std::string* err) {
    if (at & 3) { *err = "injection goes at an instruction (a multiple of 4)"; return false; }
    const NxModule* nx = nxmodule_at(at);
    if (!nx) { *err = "not in a module's code"; return false; }
    uint32_t displaced = 0;
    if (!gmem::read(at, &displaced, 4)) { *err = "the code is not readable"; return false; }
    const a64::Insn d = a64::decode(displaced, at);
    if (d.branch_imm || d.is_branch) {
        *err = "the instruction there is PC-relative or a branch (" + d.text + "): inject one instruction later or earlier";
        return false;
    }
    // the cave: the zero padding between the end of the module's code and its last code page
    static std::mutex cave_mu;
    static std::map<uint64_t, uint64_t> cave_used;               // module base -> bytes used
    std::lock_guard<std::mutex> lk(cave_mu);
    const uint64_t start = (nx->text_end + 15) & ~15ull;
    const uint64_t page_end = (nx->text_end + 0xfff) & ~0xfffull;
    uint64_t& used = cave_used[nx->base];
    std::vector<uint32_t> words;
    const uint64_t cave = start + used;
    uint64_t pc = cave;
    for (const std::string& l : lines) {
        std::string t = l;
        while (!t.empty() && (t.back() == ' ' || t.back() == '\r')) t.pop_back();
        if (t.empty()) continue;
        uint32_t w = 0;
        std::string e;
        if (!a64::assemble(t, pc, &w, &e)) { *err = "'" + t + "': " + e; return false; }
        words.push_back(w);
        pc += 4;
    }
    words.push_back(displaced);
    pc += 4;
    uint32_t back = 0;
    if (!a64::branch_word(pc, at + 4, false, &back)) { *err = "the cave is out of branch range"; return false; }
    words.push_back(back);
    const uint64_t len = words.size() * 4;
    if (cave + len > page_end) { *err = "no room left in the module's code cave (" + std::to_string(page_end - start) + " bytes after its code)"; return false; }
    std::vector<uint8_t> zero(static_cast<size_t>(len)), have(static_cast<size_t>(len));
    if (!gmem::read(cave, have.data(), have.size()) || have != zero) { *err = "the space after the module's code is not free"; return false; }
    if (!gmem::write(cave, words.data(), len)) { *err = "the cave could not be written"; return false; }
    used += (len + 15) & ~15ull;
    missed::code_region_add(cave, len, "cave");
    uint32_t to_cave = 0;
    if (!a64::branch_word(at, cave, false, &to_cave)) { *err = "the cave is out of branch range"; return false; }
    std::vector<uint8_t> b(4);
    std::memcpy(b.data(), &to_cave, 4);
    NXLOG_I(kTag, "code cave at %s: %zu instructions, entered from %s", gmem::describe(cave).c_str(), words.size(), where(at).c_str());
    {
        std::lock_guard<std::mutex> lk2(g().mu);
        g().cave_jumps.insert(at);
    }
    return apply(at, b, err);
}

bool bp_add(uint64_t addr, bool stop, std::string* err) {
    if (addr & 3) {
        *err = "a breakpoint goes on an instruction (a multiple of 4)";
        return false;
    }
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    uint64_t fn_start = 0;
    if (!fn_for_locked(addr, &fn_start, err)) return false;
    Breakpoint b;
    b.addr = addr;
    b.stop = stop;
    s.bps[addr] = b;
    NXLOG_I(kTag, "%s at %s", stop ? "breakpoint" : "trace point", where(addr).c_str());
    rebuild_locked(fn_start);
    return true;
}

// A thread held at a breakpoint that goes away (removed or switched off) runs on, as after Continue: nothing would stop it there again, and
// with its breakpoint gone the window no longer shows where it is.
void release_held_at(uint64_t addr) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.hold_mu);
    if (!s.holding || s.held.pc != addr) return;
    s.resume_cmd = static_cast<int>(Resume::Continue);
    s.hold_cv.notify_all();
}

bool bp_remove(uint64_t addr, std::string* err) {
    G& s = g();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.bps.erase(addr)) {
            *err = "no breakpoint there";
            return false;
        }
        uint64_t fs = 0, fe = 0;
        std::string m;
        if (function_at(addr, &m, &fs, &fe)) rebuild_locked(fs);
    }
    release_held_at(addr);      // after s.mu: hold() takes hold_mu, then function_at
    return true;
}

void bp_enable(uint64_t addr, bool on) {
    G& s = g();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (auto it = s.bps.find(addr); it != s.bps.end()) it->second.enabled = on;
    }
    if (!on) release_held_at(addr);
}

std::vector<Breakpoint> breakpoints() {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<Breakpoint> out;
    for (const auto& kv : s.bps) out.push_back(kv.second);
    return out;
}

bool held(Regs* r) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.hold_mu);
    if (!s.holding) return false;
    if (r) *r = s.held;
    return true;
}

void resume(Resume how) {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.hold_mu);
    if (!s.holding) return;
    s.resume_cmd = static_cast<int>(how);
    s.hold_cv.notify_all();
}

}  // namespace nxrt::cheats::cp

// A loop head of a function lifted with --live-patch whose bit is set (ctx.h NX_OVL): the running loop continues in the function recompiled
// from the patched bytes, entered at `pc` with the registers as they are; then the old code returns. 0 (no unit entry there): it goes on.
extern "C" int nx_hook_resume(::Ctx* c, uint64_t entry, uint64_t pc) {
    void (*u)(::Ctx*, uint64_t) = nullptr;
    {
        std::lock_guard<std::mutex> lk(nxrt::cheats::cp::g_resume_mu);
        auto it = nxrt::cheats::cp::g_resume.find(entry);
        if (it != nxrt::cheats::cp::g_resume.end()) u = it->second;
    }
    if (!u) return 0;
    u(c, pc);
    return 1;
}
