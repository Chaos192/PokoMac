// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Which compiled functions inlined a guest function: a redirect of its entry (entryjump.h) does not reach those copies, so the code patcher
// redirects them too, each compiled again from its unchanged guest code as an on-demand unit (whose calls reach the patched function),
// and recursively the functions that inlined those. A guest caller (a bl / b to the function in the guest code) whose compiled code has
// no call or jump to the function's compiled entry inlined it (clang at -O1 inlines small lifted functions into callers in the same unit).
// No map from the build is needed: the guest code and the compiled code are both in memory. Pure: the runtime passes how to read them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace nxrt::cheats::inl {

enum class Host : uint8_t { Arm64, X64 };

// The guest addresses in [base, base + 4n) holding a bl or b to `target` (arm64 guest code).
std::vector<uint64_t> guest_sites(const uint32_t* words, size_t n, uint64_t base, uint64_t target);

// True when the compiled code [code, code + n) at host address `base` calls or jumps straight to `target`: arm64 bl / b (or through one
// linker branch island: a b at the call's target that goes on to `target`, read with read_word); x86-64 call / jmp rel32.
bool calls(const uint8_t* code, size_t n, uint64_t base, uint64_t target, Host h,
           const std::function<bool(uint64_t addr, uint32_t* word)>& read_word = {});

struct Source {
    std::function<std::vector<uint64_t>(uint64_t fn)> guest_callers;                      // guest functions with a bl / b to fn
    std::function<bool(uint64_t fn, uint64_t* host, uint64_t* host_end)> host_of;         // fn's compiled entry and where its code ends
    std::function<bool(uint64_t host, uint64_t host_end, uint64_t callee_host)> host_calls;   // calls() over that code
};

// The guest functions to redirect along with `fn`: those that inlined it, and those that inlined them, at most `limit` (*capped when more).
std::vector<uint64_t> inliners(uint64_t fn, const Source& src, size_t limit = 64, bool* capped = nullptr);

}  // namespace nxrt::cheats::inl
