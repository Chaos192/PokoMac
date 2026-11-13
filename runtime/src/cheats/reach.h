// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
#include <string>
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

// Makes every address to_prepare names ready with `prepare` (missed::prepare on the patcher's worker), in that order, and stops at the first
// that cannot be. ok only when all of them are: a patch whose cave, or the code after its hook, could not be compiled must never go live,
// since its first run would meet code nothing can run (PokoMac on the PC, 11-13: the on-demand helper could not compile the Water Gun
// cheat's cave, the patch went live anyway, and its first use raised a guest trap that ended the game).
struct Ready {
    bool ok = true;
    size_t made = 0, total = 0;
    uint64_t failed_at = 0;            // the address that could not be made ready
    std::string why;                   // and why (the compiler's answer)
};
Ready make_ready(const std::vector<std::pair<uint64_t, uint32_t>>& patched, uint64_t fn_lo, uint64_t fn_hi, const Source& src,
                 const std::function<bool(uint64_t addr, std::string* why)>& prepare);

}  // namespace nxrt::cheats::reach
