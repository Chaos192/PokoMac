// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Internal to src/eh: the per-thread stack of catch frames shared by the unwinder (landing) and nx_catch_call (establishing).
#pragma once
#include <setjmp.h>

#include <cstdint>
#include <vector>

#include "nxrt/abi.h"

namespace nxrt::eh::detail {

// One nx_catch_call activation. The unwinder finds the one whose cfa equals the CFA of the frame it lands in, stores the landing pad
// and longjmps to jb; nx_catch_call then calls body(c, pad).
struct CatchRec {
    jmp_buf jb;
    uint64_t cfa = 0;              // guest SP at entry of the function = its CFA
    volatile uint64_t pad = 0;
    volatile bool landed = false;  // set by the unwinder right before the longjmp
};

std::vector<CatchRec*>& catch_stack();   // thread_local; the top is the innermost activation

// The CFA of the frame of the lifted function holding `pc` when its code is about to run the instruction at pc with the registers of c
// (the CFI row covering pc itself, not pc - 1: pc is not a return address). False when no FDE or row covers pc. A mid-function entry into
// a function with a catch clause keys its catch frame by it (nx_catch_call_at).
bool cfa_at(const ::Ctx* c, uint64_t pc, uint64_t* cfa);

}  // namespace nxrt::eh::detail
