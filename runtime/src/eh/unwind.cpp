// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The C++ unwinder for lifted code: _Unwind_RaiseException / _Unwind_Resume semantics over the NxModule.eh tables.
//
// Walk: from the registers at the call into the runtime (pc = x30 = return address into guest code, sp = x31), find the FDE of the
// frame (pc - 1), read the CFI row at pc - 1: CFA = x[cfa_reg] + cfa_off and the callee-saved registers saved at CFA + off, which
// gives the caller's registers (pc = restored x30, sp = CFA). A frame's LSDA call site covering pc - 1 names a landing pad and an
// action chain; the chain's records are filter > 0 (catch the type types[fde.type_first + filter - 1], 0 = catch(...)), filter == 0
// (cleanup) or filter < 0 (ABI 3 exception specification: -(filter) - 1 indexes NxEh.specs, a 0-terminated list of type filters; an
// exception that matches none of them violates the specification and the frame lands with that (negative) selector, where the guest
// calls __cxa_call_unexpected / terminates; an empty list, `throw()`, allows nothing).
//
// Two phases like libgcc/libc++abi: phase 1 (search) walks without side effects until a handler matches (can_catch over the guest's
// typeinfo) and returns _URC_END_OF_STACK if none does, leaving the guest untouched so the guest's std::terminate sees the throw
// site; phase 2 walks again and lands in every frame that has a cleanup pad (selector 0) and finally in the handler frame
// (selector = filter). A cleanup pad ends in _Unwind_Resume, which repeats the search from the cleaned frame's caller: the search is
// deterministic, so it finds the same handler without any state stored between the phases.
//
// Landing: callee-saved registers x19..x30, d8..d15 (low halves) and SP of the landing frame are restored, x0 = the exception pointer
// the guest passed in, x1 = selector. A function entered through nx_catch_call is re-entered with body(c, pad) after a longjmp to its
// setjmp frame (catch_call.cpp); a cleanup-only function is resumed by calling its u_<entry>(c, pad) from NxModule.resume.
//
// Reference: the oracle's recomp/oracle/e2e/rt/eh_rt.cpp (S1, GPL-3.0-or-later) for the table walk and landing protocol; this file adds
// two-phase search, typeinfo matching with class hierarchies (typeinfo.cpp), the libc++abi exception header protocol, per-thread
// state, rethrow, nested exceptions and uncaught-exception reporting.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "eh_internal.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

namespace nxrt::eh {
namespace detail {
std::vector<CatchRec*>& catch_stack() {
    thread_local std::vector<CatchRec*> s;
    return s;
}
}  // namespace detail

namespace {

constexpr const char* kTag = "eh";
constexpr int kMaxDepth = 100000;

AbiLayout g_abi;
std::mutex g_abi_lock;                          // guards g_abi and the two flags below (the first exceptions of several threads)
std::atomic<bool> g_abi_detected{false};          // false: the layout is still to be detected from the first exception (unless fixed)
std::atomic<bool> g_abi_fixed{false};            // set_abi_layout() was called
AbiLayout current_layout() {
    std::lock_guard<std::mutex> lk(g_abi_lock);
    return g_abi;
}
std::atomic<TerminateHandler> g_terminate_handler{nullptr};
struct AtomicStats {
    std::atomic<uint64_t> raised{0}, resumed{0}, caught{0}, cleanups{0}, uncaught{0}, frames{0}, catch_landings{0}, resume_landings{0};
} g_stats;

inline uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), 8); return v; }
inline void wr64(uint64_t a, uint64_t v) { std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, 8); }
inline void wr32(uint64_t a, uint32_t v) { std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &v, 4); }

// Register state of one frame at its call site: what the callee-saved registers and SP were there.
std::string describe_thrown(::Ctx* c, const Thrown& t);

struct Frame {
    uint64_t pc = 0, sp = 0;
    uint64_t x[31] = {};
    uint64_t d[8] = {};          // low halves of v8..v15
};

Frame frame_from_ctx(const ::Ctx* c) {
    Frame f;
    f.pc = c->x[30];
    f.sp = c->x[31];
    for (int i = 0; i < 31; i++) f.x[i] = c->x[i];
    for (int i = 0; i < 8; i++) f.d[i] = c->v[8 + i].d[0];
    return f;
}

struct FrameInfo {
    const NxModule* mod = nullptr;
    const NxFde* fde = nullptr;
};

// The FDE covering `pc` (a return address minus one), searching every registered module that has unwind tables.
FrameInfo find_frame(uint64_t pc) {
    FrameInfo r;
    const NxModule* m = nxmodule_at(pc);
    if (!m || !m->eh) return r;
    const NxEh* eh = m->eh;
    int lo = 0, hi = eh->nfdes - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const NxFde* f = &eh->fdes[mid];
        if (pc < f->start) hi = mid - 1;
        else if (pc >= f->end) lo = mid + 1;
        else { r.mod = m; r.fde = f; return r; }
    }
    return r;
}

// Computes the CFA of the frame `fr` is in and turns `fr` into the state of its caller. False when the CFI says nothing about pc - 1.
bool step(const FrameInfo& fi, Frame* fr, uint64_t* cfa_out) {
    const NxEh* eh = fi.mod->eh;
    const NxFde* f = fi.fde;
    const uint64_t pcm1 = fr->pc - 1;
    const NxCfiRow* row = nullptr;
    for (uint32_t i = 0; i < f->row_count; i++) {
        const NxCfiRow* q = &eh->rows[f->row_first + i];
        if (q->pc <= pcm1) row = q;
        else break;
    }
    if (!row || row->cfa_reg > 31) return false;
    const uint64_t base = row->cfa_reg == 31 ? fr->sp : fr->x[row->cfa_reg];
    const uint64_t cfa = base + static_cast<uint64_t>(static_cast<int64_t>(row->cfa_off));
    for (int i = 0; i < row->nsaved; i++) {
        const NxCfiSaved* s = &eh->saved[row->saved_first + i];
        const uint64_t v = rd64(cfa + static_cast<uint64_t>(static_cast<int64_t>(s->off)));
        if (s->reg <= 30) fr->x[s->reg] = v;
        else if (s->reg >= 72 && s->reg < 80) fr->d[s->reg - 72] = v;
    }
    fr->sp = cfa;
    fr->pc = fr->x[30];
    *cfa_out = cfa;
    return true;
}

enum class Phase { Search, Cleanup };

struct Pick {
    bool land = false;
    bool handler = false;        // a catch clause matched (as opposed to a cleanup)
    uint64_t pad = 0;
    int64_t selector = 0;
    uint64_t adjusted = 0;
};

// Which landing pad, if any, does the frame take for an exception at `pc` (a return address)?
Pick pick(const FrameInfo& fi, uint64_t pc, const Thrown& t, Phase phase) {
    Pick p;
    const NxEh* eh = fi.mod->eh;
    const NxFde* f = fi.fde;
    const uint64_t pcm1 = pc - 1;
    for (uint32_t i = 0; i < f->site_count; i++) {
        const NxCallSite* s = &eh->sites[f->site_first + i];
        if (pcm1 < s->start || pcm1 - s->start >= s->len) continue;
        if (!s->pad) return p;                                   // covered, no landing pad: the exception goes on
        uint32_t a = s->action;
        if (!a) {                                                // cleanup only
            if (phase == Phase::Cleanup) { p.land = true; p.pad = s->pad; p.selector = 0; }
            return p;
        }
        for (int guard = 0; a && guard < 1024; guard++) {
            const NxAction* act = &eh->actions[a - 1];
            if (act->filter == 0) {                              // a cleanup record ends the scan of this chain
                if (phase == Phase::Cleanup) { p.land = true; p.pad = s->pad; p.selector = 0; }
                return p;
            }
            if (act->filter > 0) {
                uint64_t type = 0, adj = t.object;
                if (nxmodule_type_address(fi.mod, f, static_cast<uint32_t>(act->filter), &type) && can_catch(type, t.type, &adj)) {
                    p.land = true;
                    p.handler = true;
                    p.pad = s->pad;
                    p.selector = act->filter;
                    p.adjusted = adj;
                    return p;
                }
            } else if (eh->specs) {                                      // exception specification: land when the exception is NOT allowed
                bool allowed = false;
                for (const uint32_t* k = eh->specs + (-static_cast<int64_t>(act->filter) - 1); *k && !allowed; k++) {
                    uint64_t type = 0, adj = t.object;
                    allowed = nxmodule_type_address(fi.mod, f, *k, &type) && can_catch(type, t.type, &adj);
                }
                if (!allowed) {
                    p.land = true;
                    p.handler = true;
                    p.pad = s->pad;
                    p.selector = act->filter;
                    p.adjusted = t.object;
                    return p;
                }
            }
            a = act->next;
        }
        return p;
    }
    return p;                                                    // no call-site entry covers pc: keep unwinding (libc++abi would terminate)
}

struct Handler {
    bool found = false;
    uint64_t cfa = 0, pad = 0, adjusted = 0;
    int64_t selector = 0;
    FrameInfo fi;
};

std::string describe_pc(uint64_t pc) {
    std::string d = module_describe(pc);
    const FrameInfo fi = find_frame(pc);
    if (fi.mod && d.compare(0, 2, "0x") == 0) {          // not an image the loader knows (it supplies the symbols): name the lifted module
        char b[96];
        snprintf(b, sizeof b, "%s+0x%llx", fi.mod->name, static_cast<unsigned long long>(pc - fi.mod->base));
        d = b;
    }
    if (fi.fde && d.find('(') == std::string::npos) {
        char b[64];
        snprintf(b, sizeof b, " [function 0x%llx]", static_cast<unsigned long long>(fi.fde->start));
        d += b;
    }
    return d;
}

// Phase 1. `skip_first`: ignore the LSDA of the first frame (the cleanup function that called _Unwind_Resume).
Handler search_phase(const Frame& start, const Thrown& t, bool skip_first, std::string* why_not = nullptr) {
    Handler h;
    Frame fr = start;
    for (int depth = 0; depth < kMaxDepth; depth++) {
        if (fr.pc == 0) { if (why_not) *why_not = "reached the bottom of the guest stack"; return h; }
        const FrameInfo fi = find_frame(fr.pc - 1);
        if (!fi.fde) {
            if (why_not) { char b[96]; snprintf(b, sizeof b, "no unwind information for pc 0x%llx", static_cast<unsigned long long>(fr.pc)); *why_not = b; }
            return h;
        }
        g_stats.frames.fetch_add(1, std::memory_order_relaxed);
        Frame at = fr;
        uint64_t cfa = 0;
        if (!step(fi, &fr, &cfa)) { if (why_not) *why_not = "no CFI row"; return h; }
        if (depth == 0 && skip_first) continue;
        Pick p = pick(fi, at.pc, t, Phase::Search);
        if (p.land && p.handler) {
            h.found = true;
            h.cfa = cfa;
            h.pad = p.pad;
            h.selector = p.selector;
            h.adjusted = p.adjusted;
            h.fi = fi;
            return h;
        }
    }
    if (why_not) *why_not = "unwind depth limit";
    return h;
}

// Legacy host-service form: the type of an in-flight exception, keyed by object pointer (the resume call does not repeat it).
struct TypeEntry { uint64_t exc, type; };
std::vector<TypeEntry>& legacy_types() {
    thread_local std::vector<TypeEntry> v;
    return v;
}
void legacy_remember(uint64_t exc, uint64_t type) {
    auto& v = legacy_types();
    for (auto& e : v) if (e.exc == exc) { e.type = type; return; }
    v.push_back({exc, type});
}
uint64_t legacy_recall(uint64_t exc) {
    for (auto& e : legacy_types()) if (e.exc == exc) return e.type;
    return 0;
}
void legacy_forget(uint64_t exc) {
    auto& v = legacy_types();
    for (size_t i = 0; i < v.size(); i++) if (v[i].exc == exc) { v.erase(v.begin() + static_cast<ptrdiff_t>(i)); return; }
}

// Control transfer into a landing pad. Holds no objects with destructors: it ends in a longjmp.
[[noreturn]] void land(::Ctx* c, const Frame& st, uint64_t frame_cfa, uint64_t pad, int64_t selector, uint64_t exc, bool legacy) {
    for (int i = 19; i <= 30; i++) c->x[i] = st.x[i];
    c->x[31] = st.sp;
    for (int i = 0; i < 8; i++) c->v[8 + i].d[0] = st.d[i];
    c->x[0] = exc;
    c->x[1] = static_cast<uint64_t>(selector);
    if (selector != 0) {                      // a catch clause, or (negative) an exception specification violation
        g_stats.caught.fetch_add(1, std::memory_order_relaxed);
        if (legacy) legacy_forget(exc);
    } else {
        g_stats.cleanups.fetch_add(1, std::memory_order_relaxed);
    }
    auto& stack = detail::catch_stack();
    for (size_t i = stack.size(); i-- > 0;) {
        if (stack[i]->cfa == frame_cfa) {
            detail::CatchRec* rec = stack[i];
            rec->pad = pad;
            rec->landed = true;
            stack.resize(i + 1);
            g_stats.catch_landings.fetch_add(1, std::memory_order_relaxed);
            NXLOG_D(kTag, "landing in catch frame (cfa 0x%llx) at pad 0x%llx selector %lld", static_cast<unsigned long long>(frame_cfa),
                    static_cast<unsigned long long>(pad), static_cast<long long>(selector));
            longjmp(rec->jb, 1);
        }
    }
    // A function with no catch frame: only cleanup pads (and terminate pads) are resumed this way, through NxModule.resume.
    const NxModule* m = nxmodule_at(pad);
    if (m) {
        int lo = 0, hi = m->nresume - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            const NxResume* r = &m->resume[mid];
            if (r->pad == pad) {
                g_stats.resume_landings.fetch_add(1, std::memory_order_relaxed);
                NXLOG_D(kTag, "landing by resume entry at pad 0x%llx selector %lld", static_cast<unsigned long long>(pad), static_cast<long long>(selector));
                r->fn(c, pad);
                NXLOG_E(kTag, "the landing pad at 0x%llx returned (a cleanup pad must end in _Unwind_Resume)", static_cast<unsigned long long>(pad));
                trap("landing pad returned", pad);
            }
            if (r->pad < pad) lo = mid + 1; else hi = mid - 1;
        }
    }
    NXLOG_E(kTag, "landing pad 0x%llx has neither a catch frame (cfa 0x%llx) nor a resume entry", static_cast<unsigned long long>(pad),
            static_cast<unsigned long long>(frame_cfa));
    trap("landing pad without a landing target", pad);
}

// Phase 2: walk again from `start`, landing in every frame with a cleanup pad and in the handler frame.
[[noreturn]] void cleanup_phase(::Ctx* c, const Frame& start, const Thrown& t, bool skip_first, const Handler& h, bool legacy) {
    Frame fr = start;
    for (int depth = 0; depth < kMaxDepth; depth++) {
        if (fr.pc == 0) break;
        const FrameInfo fi = find_frame(fr.pc - 1);
        if (!fi.fde) break;
        Frame at = fr;
        uint64_t cfa = 0;
        if (!step(fi, &fr, &cfa)) break;
        if (depth == 0 && skip_first) continue;
        if (cfa == h.cfa && fi.fde == h.fi.fde) land(c, at, cfa, h.pad, h.selector, t.exc, legacy);
        Pick p = pick(fi, at.pc, t, Phase::Cleanup);
        if (p.land && !p.handler) land(c, at, cfa, p.pad, 0, t.exc, legacy);
    }
    NXLOG_E(kTag, "phase 2 did not reach the handler frame found in phase 1 (cfa 0x%llx)", static_cast<unsigned long long>(h.cfa));
    trap("unwinder lost its handler frame", h.cfa);
}

uint64_t unwind(::Ctx* c, const Thrown& t, bool skip_first, bool legacy) {
    const Frame start = frame_from_ctx(c);
    std::string why;
    Handler h = search_phase(start, t, skip_first, &why);
    if (!h.found) {
        g_stats.uncaught.fetch_add(1, std::memory_order_relaxed);
        NXLOG_E(kTag, "uncaught C++ exception of type %s (exception 0x%llx): no handler on the guest stack (%s)", describe_thrown(c, t).c_str(),
                static_cast<unsigned long long>(t.exc), why.c_str());
        NXLOG_E(kTag, "guest backtrace of the throw:\n%s", guest_backtrace(c).c_str());
        return kUrcEndOfStack;
    }
    if (t.header) {                                              // what libc++abi's personality leaves in the header for __cxa_begin_catch
        const AbiLayout g_abi = current_layout();
        wr32(t.header + static_cast<uint64_t>(g_abi.handler_switch), static_cast<uint32_t>(h.selector));
        wr64(t.header + static_cast<uint64_t>(g_abi.action_record), 0);
        wr64(t.header + static_cast<uint64_t>(g_abi.lsda), 0);
        wr64(t.header + static_cast<uint64_t>(g_abi.catch_temp), h.pad);
        wr64(t.header + static_cast<uint64_t>(g_abi.adjusted_ptr), h.adjusted);
    }
    cleanup_phase(c, start, t, skip_first, h, legacy);
}

bool readable(uint64_t addr, uint64_t n) { return addr && mem::is_mapped(addr, n); }

// Does `ti` look like a std::type_info of this ABI: readable object, a known kind (when the vtables are configured), or else a vptr that
// points into readable memory and a name that starts like a mangled name.
bool plausible_typeinfo(uint64_t ti) {
    if (!readable(ti, 16)) return false;
    if (ti_kind(ti) != TiKind::Unknown) return true;
    if (typeinfo_vtables().klass) return false;                      // kinds are known and this is none of them
    const uint64_t vp = rd64(ti), name = rd64(ti + 8) & ~(1ull << 63);
    if (!readable(vp, 8) || !readable(name, 2)) return false;
    const char c0 = *reinterpret_cast<const char*>(static_cast<uintptr_t>(name));
    return (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9') || c0 == '*' || c0 == '_';
}

// The first exception decides between the two known layouts of the exceptionType field (see eh.h).
void detect_abi_layout(uint64_t ue) {
    std::lock_guard<std::mutex> lk(g_abi_lock);
    if (g_abi_fixed.load() || g_abi_detected.load()) return;
    const AbiLayout cands[2] = {abi_layout_llvm_new(), abi_layout_llvm_old()};
    for (const AbiLayout& l : cands) {
        const uint64_t slot = ue + static_cast<uint64_t>(l.exception_type);
        if (!readable(slot, 8)) continue;
        if (plausible_typeinfo(rd64(slot))) {
            g_abi = l;
            g_abi_detected.store(true);
            NXLOG_I(kTag, "exception header layout: exceptionType at %lld from the _Unwind_Exception (%s libc++abi layout)", static_cast<long long>(l.exception_type),
                    l.exception_type == abi_layout_llvm_new().exception_type ? "newer" : "older");
            return;
        }
    }
}

// "Type: what()" for the uncaught-exception report: the type name and, when the type derives from std::exception, its what().
std::string describe_thrown(::Ctx* c, const Thrown& t) {
    std::string d = t.type ? ti_demangle_hint(t.type) : "(foreign exception)";
    const TypeinfoVtables& vt = typeinfo_vtables();
    if (!t.type || !vt.klass || !t.object || !config_get_bool("eh", "call_what", true)) return d;
    static const char kName[] = "St9exception";
    uint64_t fake[2] = {vt.klass + 16, reinterpret_cast<uint64_t>(kName)};
    uint64_t adj = t.object;
    if (!can_catch(reinterpret_cast<uint64_t>(fake), t.type, &adj) || !readable(adj, 8)) return d;
    const uint64_t vtable = rd64(adj);
    if (!readable(vtable + 16, 8)) return d;
    const uint64_t what_fn = rd64(vtable + 16);                        // std::exception::what is the third slot (after the two destructors)
    if (!fn_lookup(what_fn) && !hle_is_thunk(what_fn)) return d;
    ::Ctx tmp = *c;
    tmp.x[0] = adj;
    tmp.x[31] = (c->x[31] - 0x4000) & ~15ull;                           // below the live frames
    dispatch_call(&tmp, what_fn);
    if (readable(tmp.x[0], 1)) {
        std::string w(reinterpret_cast<const char*>(static_cast<uintptr_t>(tmp.x[0])), strnlen(reinterpret_cast<const char*>(static_cast<uintptr_t>(tmp.x[0])), 200));
        d += ", what(): \"" + w + "\"";
    }
    return d;
}

// Builds the Thrown of a libc++abi exception from its _Unwind_Exception.
Thrown thrown_from_unwind_exception(uint64_t ue) {
    detect_abi_layout(ue);
    const AbiLayout g_abi = current_layout();
    Thrown t;
    t.exc = ue;
    const uint64_t cls = rd64(ue);
    const bool native = (cls & g_abi.class_mask) == g_abi.class_clang || (cls & g_abi.class_mask) == g_abi.class_gnu;
    t.object = ue + static_cast<uint64_t>(g_abi.object);
    if (native) {
        t.header = ue;
        if ((cls & 0xff) == 1) {                                 // dependent exception: the primary's object and type
            const uint64_t primary = rd64(ue + static_cast<uint64_t>(g_abi.primary));
            t.object = primary;
            t.type = rd64(primary + static_cast<uint64_t>(g_abi.object_type));
        } else {
            t.type = rd64(ue + static_cast<uint64_t>(g_abi.exception_type));
        }
    }
    return t;
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------------------------------------------

AbiLayout abi_layout_llvm_new() { return AbiLayout{}; }

AbiLayout abi_layout_llvm_old() {
    AbiLayout l;
    l.exception_type = -0x58;
    l.handler_switch = -0x2c;
    l.action_record = -0x28;
    l.lsda = -0x20;
    l.catch_temp = -0x18;
    l.adjusted_ptr = -0x10;
    l.primary = -0x60;
    l.object_type = -0x78;
    return l;
}

void set_abi_layout(const AbiLayout& l) {
    std::lock_guard<std::mutex> lk(g_abi_lock);
    g_abi = l;
    g_abi_fixed.store(true);
}

void set_abi_autodetect() {
    std::lock_guard<std::mutex> lk(g_abi_lock);
    g_abi = abi_layout_llvm_new();
    g_abi_fixed.store(false);
    g_abi_detected.store(false);
}

const AbiLayout& abi_layout() { return g_abi; }

uint64_t raise(::Ctx* c, const Thrown& t) {
    g_stats.raised.fetch_add(1, std::memory_order_relaxed);
    return unwind(c, t, /*skip_first=*/false, /*legacy=*/t.header == 0);
}

uint64_t resume(::Ctx* c, const Thrown& t) {
    g_stats.resumed.fetch_add(1, std::memory_order_relaxed);
    return unwind(c, t, /*skip_first=*/true, /*legacy=*/t.header == 0);
}

void unwind_raise_exception(::Ctx* c) { c->x[0] = raise(c, thrown_from_unwind_exception(c->x[0])); }
void unwind_resume(::Ctx* c) { c->x[0] = resume(c, thrown_from_unwind_exception(c->x[0])); }
void unwind_resume_or_rethrow(::Ctx* c) { c->x[0] = raise(c, thrown_from_unwind_exception(c->x[0])); }   // no forced unwinding here: always a raise

void unwind_delete_exception(::Ctx* c) {
    const uint64_t ue = c->x[0];
    const uint64_t cleanup = rd64(ue + 8);                       // _Unwind_Exception::exception_cleanup
    if (cleanup) {
        c->x[0] = 1;                                             // _URC_FOREIGN_EXCEPTION_CAUGHT
        c->x[1] = ue;
        dispatch_call(c, cleanup);
    }
}

void unwind_forced_unwind(::Ctx* c) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) NXLOG_W(kTag, "_Unwind_ForcedUnwind is not supported (thread cancellation): returning _URC_FATAL_PHASE1_ERROR");
    c->x[0] = kUrcFatalPhase1Error;
}

void legacy_raise(::Ctx* c) {
    Thrown t;
    t.exc = c->x[0];
    t.type = c->x[1];
    t.object = c->x[0];
    legacy_remember(t.exc, t.type);
    uint64_t r = raise(c, t);                                    // returns only without a handler
    (void)r;
    terminate_process(c, "uncaught C++ exception (host-service raise)");
    c->x[0] = kUrcEndOfStack;
}

void legacy_resume(::Ctx* c) {
    Thrown t;
    t.exc = c->x[0];
    t.object = c->x[0];
    t.type = legacy_recall(t.exc);
    uint64_t r = resume(c, t);
    (void)r;
    terminate_process(c, "uncaught C++ exception (host-service resume)");
    c->x[0] = kUrcEndOfStack;
}

std::string guest_backtrace(const ::Ctx* c, size_t max_frames) {
    std::string out;
    char line[512];
    // #0 is the function that made the call into the runtime (the one containing the return address in x30)
    Frame fr = frame_from_ctx(c);
    for (size_t n = 0; n < max_frames; n++) {
        if (fr.pc == 0) break;
        const FrameInfo fi = find_frame(fr.pc - 1);
        snprintf(line, sizeof line, "  #%zu 0x%llx %s\n", n, static_cast<unsigned long long>(fr.pc), describe_pc(fr.pc - 1).c_str());
        out += line;
        uint64_t cfa = 0;
        if (!fi.fde || !step(fi, &fr, &cfa)) {
            snprintf(line, sizeof line, "  (end: no unwind information for 0x%llx)\n", static_cast<unsigned long long>(fr.pc));
            out += line;
            break;
        }
    }
    return out;
}

void set_terminate_handler(TerminateHandler h) { g_terminate_handler.store(h); }

void terminate_process(::Ctx* c, const char* why) {
    NXLOG_E(kTag, "std::terminate: %s\nguest backtrace:\n%s", why, guest_backtrace(c).c_str());
    log_flush();
    if (TerminateHandler h = g_terminate_handler.load()) {
        h(c, why);
        return;
    }
    TerminateProcess(GetCurrentProcess(), kTerminateExitCode);
}

Stats stats() {
    Stats s;
    s.raised = g_stats.raised;
    s.resumed = g_stats.resumed;
    s.caught = g_stats.caught;
    s.cleanups = g_stats.cleanups;
    s.uncaught = g_stats.uncaught;
    s.frames_walked = g_stats.frames;
    s.catch_frame_landings = g_stats.catch_landings;
    s.resume_fn_landings = g_stats.resume_landings;
    return s;
}

namespace detail {
bool cfa_at(const ::Ctx* c, uint64_t pc, uint64_t* cfa) {
    const FrameInfo fi = find_frame(pc);
    if (!fi.fde) return false;
    const NxEh* eh = fi.mod->eh;
    const NxCfiRow* row = nullptr;
    for (uint32_t i = 0; i < fi.fde->row_count; i++) {
        const NxCfiRow* q = &eh->rows[fi.fde->row_first + i];
        if (q->pc <= pc) row = q;
        else break;
    }
    if (!row || row->cfa_reg > 31) return false;
    *cfa = c->x[row->cfa_reg] + static_cast<uint64_t>(static_cast<int64_t>(row->cfa_off));   // x[31] is SP
    return true;
}
}  // namespace detail

void stats_reset() {
    g_stats.raised = g_stats.resumed = g_stats.caught = g_stats.cleanups = g_stats.uncaught = g_stats.frames = g_stats.catch_landings = g_stats.resume_landings = 0;
}

size_t catch_frame_depth() { return detail::catch_stack().size(); }

}  // namespace nxrt::eh
