// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The generated-code ABI as the runtime sees it. The recompiler owns the header (recomp/include/nxrecomp/ctx.h: Ctx, V128, FnEntry,
// DynSym, NxModule, NxEh and the declarations of the functions this runtime provides: call_indirect, host_import(c, import_id),
// guest_trap, guest_unimpl, guest_svc, guest_mrs, guest_msr, stx_diag, nx_catch_call, nx_site_inline, nx_site_midhook,
// nx_hook_entry; their definitions are src/core/abi_glue.cpp, src/core/hooks.cpp and src/eh/catch_call.cpp).
// Everything else in nxrt reaches the guest CPU state through the small accessors below.
#pragma once

#include <nxrecomp/ctx.h>

static_assert(NXRECOMP_ABI_VERSION == 4,
              "nxrt was written against nxrecomp ABI version 4: review src/core/abi_glue.cpp, nxmodule.cpp, missed.cpp, hooks.cpp, eh/ and diag/ before bumping");

namespace nxrt {

// The ABI version this runtime implements. A lifted module built against another version is refused (nxmodule.h), except ABI 3:
// version 4 only added the mid-function entry table at the end of NxModule (and nx_no_entry), so an ABI 3 module is taken without it.
// ABI 5 (NXRECOMP_ABI_LIVE) is ABI 4 lifted with --live-patch: its functions check Ctx.ov at their entry (ctx.h NX_OV), which the
// cheats overlay's live code patches use to reach direct callers (src/cheats/codepatch.cpp).
constexpr uint32_t kAbiVersion = NXRECOMP_ABI_LIVE;
constexpr uint32_t kAbiVersionOldest = 3;

using GuestFn = void (*)(::Ctx*);   // a lifted function (or an HLE handler): guest registers in, guest registers out
using HleFn = GuestFn;

// Register access. x[31] is SP; w-register views are the caller's business.
inline uint64_t& reg(::Ctx* c, int n) { return c->x[n]; }
inline uint64_t& sp(::Ctx* c) { return c->x[31]; }
inline uint64_t& lr(::Ctx* c) { return c->x[30]; }
inline uint64_t& fp(::Ctx* c) { return c->x[29]; }
inline uint64_t& tpidr_el0(::Ctx* c) { return c->tpidr; }
inline uint64_t& tpidrro_el0(::Ctx* c) { return c->tpidrro; }

}  // namespace nxrt
