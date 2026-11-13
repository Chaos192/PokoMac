// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The runtime's _Unwind_* entry points as HLE thunks, entry hooks and lifter function overrides (eh.h).
#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "nxrt/module.h"

namespace nxrt::eh {

void register_unwind_hle() {
    struct Item { const char* name; HleFn fn; bool override_symbol; };
    const Item items[] = {
        {"_Unwind_RaiseException", unwind_raise_exception, true},
        {"_Unwind_Resume", unwind_resume, true},
        {"_Unwind_Resume_or_Rethrow", unwind_resume_or_rethrow, true},
        {"_Unwind_DeleteException", unwind_delete_exception, true},
        {"_Unwind_ForcedUnwind", unwind_forced_unwind, false},
    };
    for (const Item& it : items) {
        hle_register(it.name, it.fn);
        if (it.override_symbol) module_override_symbol(it.name);   // the guest's own libunwind must never run: the runtime is the unwinder
    }
}

int hook_unwind_raise_exception(::Ctx* c, uint64_t, void*) { unwind_raise_exception(c); return 1; }
int hook_unwind_resume(::Ctx* c, uint64_t, void*) { unwind_resume(c); return 1; }
int hook_unwind_resume_or_rethrow(::Ctx* c, uint64_t, void*) { unwind_resume_or_rethrow(c); return 1; }
int hook_unwind_delete_exception(::Ctx* c, uint64_t, void*) { unwind_delete_exception(c); return 1; }

}  // namespace nxrt::eh

extern "C" {
void h_nxrt_unwind_raise_exception(Ctx* c) { nxrt::eh::unwind_raise_exception(c); }
void h_nxrt_unwind_resume(Ctx* c) { nxrt::eh::unwind_resume(c); }
void h_nxrt_unwind_resume_or_rethrow(Ctx* c) { nxrt::eh::unwind_resume_or_rethrow(c); }
void h_nxrt_unwind_delete_exception(Ctx* c) { nxrt::eh::unwind_delete_exception(c); }
}
