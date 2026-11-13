// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The code a live patch makes reachable, which the dispatcher may not have yet: the code patcher (codepatch.cpp) makes all of it ready
// on its worker thread (missed::prepare) before it swaps the patch in, so turning a code cheat on never makes a guest thread wait for the
// build's compiler (pokomac-port 2026-11-12: a 12.7 s frame on the PC, 26.7 s under load; pokopia-cheats' Mac run: the first Leafage after
// the cheat went live waited 0.75 s for its cave, two units). What a patch reaches: the targets of its branches that leave the patched
// function (into a code cave, another function, the middle of one); and, followed through a code region (a cave), every call target and
// every branch that leaves the run of code there (back into the function after the hook, other functions).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace nxrt::cheats::reach {

// The direct branch targets (b, bl, b.cond, cbz/cbnz, tbz/tbnz) of `n` words at `base` that leave [lo, hi), and every bl target.
std::vector<uint64_t> exits(const uint32_t* words, size_t n, uint64_t base, uint64_t lo, uint64_t hi);

// What the runtime is asked: a word of guest memory, and the code region (a cave) holding an address.
struct Source {
    std::function<bool(uint64_t addr, uint32_t* word)> read;
    std::function<bool(uint64_t addr, uint64_t* base, uint64_t* end)> region;
};

// Every address to make ready for the patched words of the function [fn_lo, fn_hi), in the order to make them (a cave's entry before what
// it reaches). Code in a region is followed from each entry until four zero words in a row or the region's end; at most `limit` addresses.
std::vector<uint64_t> to_prepare(const std::vector<std::pair<uint64_t, uint32_t>>& patched, uint64_t fn_lo, uint64_t fn_hi, const Source& src,
                                 size_t limit = 64);

}  // namespace nxrt::cheats::reach
