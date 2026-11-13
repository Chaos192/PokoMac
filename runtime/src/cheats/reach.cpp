// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The code a live patch makes reachable (reach.h).
#include "reach.h"

#include <set>
#include <string>

#include "disasm.h"

namespace nxrt::cheats::reach {
namespace {
bool is_direct_branch(const a64::Insn& in) {
    return in.is_branch && in.branch_imm && in.mnemonic != "adr" && in.mnemonic != "adrp";
}
}  // namespace

std::vector<uint64_t> exits(const uint32_t* words, size_t n, uint64_t base, uint64_t lo, uint64_t hi) {
    std::vector<uint64_t> out;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t pc = base + 4 * i;
        const a64::Insn in = a64::decode(words[i], pc);
        if (!is_direct_branch(in)) continue;
        if (in.mnemonic == "bl" || in.target < lo || in.target >= hi) out.push_back(in.target);
    }
    return out;
}

std::vector<uint64_t> to_prepare(const std::vector<std::pair<uint64_t, uint32_t>>& patched, uint64_t fn_lo, uint64_t fn_hi, const Source& src,
                                 size_t limit) {
    std::vector<uint64_t> out, queue;
    std::set<uint64_t> seen;
    for (const auto& [addr, word] : patched)
        for (uint64_t t : exits(&word, 1, addr, fn_lo, fn_hi)) queue.push_back(t);
    for (size_t qi = 0; qi < queue.size() && out.size() < limit; ++qi) {
        const uint64_t t = queue[qi];
        if (!seen.insert(t).second) continue;
        out.push_back(t);
        uint64_t rb = 0, re = 0;
        if (!src.region || !src.region(t, &rb, &re)) continue;          // module code: its own build's, nothing to follow
        // a cave: its words from t on, to four zero words in a row (the free padding) or the region's end
        std::vector<uint32_t> run;
        int zeros = 0;
        for (uint64_t a = t; a + 4 <= re && run.size() < 4096; a += 4) {
            uint32_t w = 0;
            if (!src.read || !src.read(a, &w)) break;
            run.push_back(w);
            if (w == 0) {
                if (++zeros == 4) break;
            } else {
                zeros = 0;
            }
        }
        for (uint64_t x : exits(run.data(), run.size(), t, t, t + 4 * run.size())) queue.push_back(x);
    }
    return out;
}

}  // namespace nxrt::cheats::reach
