// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Site hooks: the runtime half of the lifter's site lists (recomp nxrecomp/program.py load_sites). Lifted code calls
//   nx_site_inline(c, addr)          before the instruction at addr                       (kind inline)
//   nx_site_midhook(c, addr)         before the instruction at addr; nonzero = "handled"   (kind midhook)
//   nx_hook_entry(c, addr)           at the start of the function at addr; nonzero = "handled" (kind entryhook)
// "handled" means the hook did the work of the rest of the function (an entry hook: the whole function): the lifted function then
// returns at once, so the hook must leave the registers as a return from that function would (x0.. results, sp, callee-saved).
//
// The PC-feature code attaches to those sites by guest address:
//
//     uint32_t id = nxrt::hook_add_entry(0x7100123450, [](::Ctx* c, uint64_t addr, void* user) -> int { ...; return 1; });
//
// Several hooks may share one address; they run in registration order. For inline hooks all of them run. For entry and midhook hooks
// the chain stops at the first one that returns nonzero. A site with no hook registered does nothing (inline: nothing; entry and
// midhook: continue with the lifted function). Registration and removal are safe while guest threads run: lookups never lock and see
// either the old or the new table. A hook may add or remove hooks, including its own site's: the change is seen by the next call.
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/abi.h"

namespace nxrt {

using InlineHookFn = void (*)(::Ctx* c, uint64_t addr, void* user);
using SiteHookFn = int (*)(::Ctx* c, uint64_t addr, void* user);     // midhook and entry hooks: nonzero = handled

enum class HookKind : uint8_t { Inline = 0, Mid = 1, Entry = 2 };

// Each returns a hook id (nonzero), or 0 for a null function. Ids are never reused within a process.
uint32_t hook_add_inline(uint64_t addr, InlineHookFn fn, void* user = nullptr);
uint32_t hook_add_mid(uint64_t addr, SiteHookFn fn, void* user = nullptr);
uint32_t hook_add_entry(uint64_t addr, SiteHookFn fn, void* user = nullptr);
bool hook_remove(uint32_t id);                       // false for an unknown id
size_t hook_count();                                 // registered hooks
size_t hook_count_at(uint64_t addr, HookKind kind);
uint64_t hook_calls(uint32_t id);                    // times the hook was called (0 for an unknown id)
uint64_t hook_site_calls(HookKind kind);             // calls into nx_site_inline / nx_site_midhook / nx_hook_entry, hooked or not
void hooks_clear();                                  // tests: remove every hook and zero the counters (no guest thread may be inside a hook)

}  // namespace nxrt
