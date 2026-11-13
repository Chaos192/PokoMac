// SPDX-License-Identifier: GPL-3.0-or-later
// nx_catch_call: the catch frames of lifted functions that contain a catch clause (recomp ctx.h; ssbu-recomp's nro_catch_call is the
// model, only its name and idea are used). The lifter enters such a function as nx_catch_call(c, u_<entry>). This runs body(c, 0)
// under a setjmp frame keyed by the guest SP at entry (= the function's CFA). When the unwinder lands in that function it stores the
// landing pad in the frame and longjmps here, discarding every host frame above; the loop then calls body(c, pad), which resumes the
// lifted function at the pad. The same function may be landed in again (a try inside a loop), hence the loop.
#include "eh_internal.h"
#include "nxrt/dispatch.h"
#include "nxrt/eh.h"

namespace {
void catch_loop(Ctx* c, void (*body)(Ctx*, uint64_t), uint64_t cfa, uint64_t first) {
    using namespace nxrt::eh::detail;
    CatchRec rec;
    rec.cfa = cfa;
    catch_stack().push_back(&rec);
    // State that dispatch_call / host_import set up and restore on the way out; a longjmp skips that, so put it back by hand.
    const char* const saved_hle = nxrt::hle_current_name();
    const char* const saved_in_import = c->in_import;
    const uint64_t saved_import_from = c->import_from;
    const uint32_t saved_cb_flags = nxrt::host_callback_flags();   // a host bit set inside a guest callback the exception left
    volatile uint64_t pad = first;
    for (;;) {
        if (setjmp(rec.jb) == 0) {
            body(c, pad);
            break;
        }
        pad = rec.pad;                                   // landed here by an exception
        nxrt::hle_restore_current(saved_hle);
        c->in_import = saved_in_import;
        c->import_from = saved_import_from;
        nxrt::host_callback_restore(saved_cb_flags);
    }
    auto& st = catch_stack();
    for (size_t i = st.size(); i-- > 0;)
        if (st[i] == &rec) { st.resize(i); break; }
}
}  // namespace

extern "C" void nx_catch_call(Ctx* c, void (*body)(Ctx*, uint64_t)) { catch_loop(c, body, c->x[31], 0); }

// A mid-function entry into such a function (core/missed.cpp): the frame is the function's CFA as the CFI row at pc gives it (the
// prologue has already moved SP), and the first run of the body enters at pc. Without a row for pc the frame is keyed by SP: a catch
// clause of this function then finds no frame and the exception goes on to the caller's.
void nx_catch_call_at(Ctx* c, void (*body)(Ctx*, uint64_t), uint64_t pc) {
    uint64_t cfa = c->x[31];
    nxrt::eh::detail::cfa_at(c, pc, &cfa);
    catch_loop(c, body, cfa, pc);
}
