// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Functions that inlined a patched function (inliners.h).
#include "inliners.h"

#include <cstring>
#include <set>

namespace nxrt::cheats::inl {
namespace {
// arm64 b / bl: the branch target, or false
bool a64_branch(uint32_t w, uint64_t pc, uint64_t* to) {
    const uint32_t op = w & 0xFC000000u;
    if (op != 0x14000000u && op != 0x94000000u) return false;
    int64_t imm = static_cast<int64_t>(w & 0x03FFFFFFu);
    if (imm & 0x02000000) imm -= 0x04000000;
    *to = pc + static_cast<uint64_t>(imm * 4);
    return true;
}
}  // namespace

std::vector<uint64_t> guest_sites(const uint32_t* words, size_t n, uint64_t base, uint64_t target) {
    std::vector<uint64_t> out;
    for (size_t i = 0; i < n; ++i) {
        uint64_t to = 0;
        if (a64_branch(words[i], base + 4 * i, &to) && to == target) out.push_back(base + 4 * i);
    }
    return out;
}

bool calls(const uint8_t* code, size_t n, uint64_t base, uint64_t target, Host h, const std::function<bool(uint64_t, uint32_t*)>& read_word) {
    if (h == Host::Arm64) {
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint32_t w = 0;
            std::memcpy(&w, code + i, 4);
            uint64_t to = 0;
            if (!a64_branch(w, base + i, &to)) continue;
            if (to == target) return true;
            // the linker's branch island: a b on to the target (a call farther than 128 MiB)
            uint32_t iw = 0;
            uint64_t to2 = 0;
            if (read_word && (to < base || to >= base + n) && read_word(to, &iw) && (iw & 0xFC000000u) == 0x14000000u && a64_branch(iw, to, &to2) &&
                to2 == target)
                return true;
        }
        return false;
    }
    for (size_t i = 0; i + 5 <= n; ++i) {
        if (code[i] != 0xE8 && code[i] != 0xE9) continue;
        int32_t rel = 0;
        std::memcpy(&rel, code + i + 1, 4);
        if (base + i + 5 + static_cast<int64_t>(rel) == target) return true;
    }
    return false;
}

std::vector<uint64_t> inliners(uint64_t fn, const Source& src, size_t limit, bool* capped) {
    std::vector<uint64_t> out;
    std::set<uint64_t> seen{fn};
    std::vector<uint64_t> work{fn};
    if (capped) *capped = false;
    while (!work.empty()) {
        const uint64_t x = work.back();
        work.pop_back();
        uint64_t xh = 0, xe = 0;
        if (!src.host_of(x, &xh, &xe)) continue;
        for (uint64_t y : src.guest_callers(x)) {
            if (seen.count(y)) continue;
            uint64_t yh = 0, ye = 0;
            if (!src.host_of(y, &yh, &ye)) continue;
            if (src.host_calls(yh, ye, xh)) continue;           // it calls x: x's redirect reaches it
            seen.insert(y);
            if (out.size() >= limit) {
                if (capped) *capped = true;
                return out;
            }
            out.push_back(y);                                   // it inlined x: compiled again, and redirected with it
            work.push_back(y);
        }
    }
    return out;
}

}  // namespace nxrt::cheats::inl
