// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Keyboard tab, logic part: the rows of the list and the edits that keep every action at two keys. No Windows code here, so the
// tests can run it alone. Only the factory layer ([bindings.all]) is edited. The menu, map, gameplay, morph and scan sections are
// left alone.
#pragma once
#include <string>
#include <vector>

#include "play/play_types.h"

namespace mprl {

// One row of the keyboard list: the action, its name on the list and the group heading it sits under.
struct KeyRow {
    nxrt::play::Act act;
    const wchar_t* label;   // "Jump"
    const wchar_t* group;   // "Combat"
};

// The rows shown, in list order.
const std::vector<KeyRow>& key_rows();

// The inputs bound to `a` in the factory layer, in file order.
std::vector<nxrt::play::InputId> inputs_for(const nxrt::play::Settings& s, nxrt::play::Act a);

// Puts `in` into slot 0 or 1 of `a` (the first or second factory binding of that action). Slot 0 keeps the trigger and time of the
// binding it replaces. Slot 1 with no first key yet fills slot 0. If another action uses `in` in the factory layer, that binding is
// removed and its action is stored in *displaced (Act::None when there was none; `displaced` may be null). A second binding of `a`
// for the same `in` is removed without a report.
void bind_slot(nxrt::play::Settings& s, nxrt::play::Act a, int slot, const nxrt::play::InputId& in, nxrt::play::Act* displaced);

// Removes the factory binding in slot 0 or 1 of `a`. Nothing happens when the slot is empty.
void clear_slot(nxrt::play::Settings& s, nxrt::play::Act a, int slot);

// Puts the factory bindings back for the contexts this tab shows (Ctx::All only). Every All binding is replaced by the factory All
// bindings, placed where the first All binding was. The bindings of the other contexts (menu, map, gameplay, morph, scan) stay
// exactly as they were, in the same order.
void reset_shown_bindings(nxrt::play::Settings& s);

// The name of the action on the list, or an empty string for an action without a row.
std::wstring act_label(nxrt::play::Act a);

}  // namespace mprl
