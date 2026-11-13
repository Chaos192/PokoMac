// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Which compiled functions inlined a patched function (runtime/src/cheats/inliners.*): guest bl / b sites, a call left in the compiled
// code on arm64 (straight, through a linker branch island) and on x86-64, and the recursive closure over a call graph where some callers
// inlined the function and one of those was inlined in turn. Standalone: includes inliners.cpp.
#include "inliners.h"

#include "inliners.cpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using namespace nxrt::cheats;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static uint32_t a64_b(uint64_t from, uint64_t to, bool link) {
    return (link ? 0x94000000u : 0x14000000u) | (static_cast<uint32_t>((static_cast<int64_t>(to) - static_cast<int64_t>(from)) >> 2) & 0x03FFFFFFu);
}

int main() {
    // guest sites: a bl and a b to 0x7100001000, another bl elsewhere, a branch backwards
    {
        const uint64_t base = 0x7100000000ull;
        uint32_t w[8] = {0xd503201f, a64_b(base + 4, base + 0x1000, true), 0xd503201f, a64_b(base + 12, base + 0x2000, true),
                         a64_b(base + 16, base + 0x1000, false), 0xd65f03c0, 0xd503201f, a64_b(base + 28, base, false)};
        auto s = inl::guest_sites(w, 8, base, base + 0x1000);
        CHECK(s.size() == 2 && s[0] == base + 4 && s[1] == base + 16);
        CHECK(inl::guest_sites(w, 8, base, base).size() == 1);
    }
    // compiled code, arm64: a bl to the target; none; a bl to an island that branches on to the target
    {
        const uint64_t H = 0x100000000ull, T = H + 0x40000;
        uint32_t code[4] = {0xa9bf7bfd, a64_b(H + 4, T, true), 0xa8c17bfd, 0xd65f03c0};
        CHECK(inl::calls(reinterpret_cast<uint8_t*>(code), 16, H, T, inl::Host::Arm64));
        uint32_t none[3] = {0xa9bf7bfd, 0x11000400, 0xd65f03c0};
        CHECK(!inl::calls(reinterpret_cast<uint8_t*>(none), 12, H, T, inl::Host::Arm64));
        const uint64_t island = H + 0x4000000ull;              // 64 MiB away, within a bl's reach
        uint32_t viai[2] = {a64_b(H, island, true), 0xd65f03c0};
        std::map<uint64_t, uint32_t> mem{{island, a64_b(island, T, false)}};
        auto rd = [&](uint64_t a, uint32_t* w) { auto it = mem.find(a); if (it == mem.end()) return false; *w = it->second; return true; };
        CHECK(inl::calls(reinterpret_cast<uint8_t*>(viai), 8, H, T, inl::Host::Arm64, rd));
        CHECK(!inl::calls(reinterpret_cast<uint8_t*>(viai), 8, H, T, inl::Host::Arm64));   // the island not followed: no call seen
    }
    // compiled code, x86-64: call rel32 and jmp rel32 (a tail call) to the target; neither
    {
        const uint64_t H = 0x140001000ull, T = 0x140005000ull;
        uint8_t c[16] = {0x55, 0x48, 0x89, 0xe5, 0xE8, 0, 0, 0, 0, 0x5d, 0xc3};
        const int32_t rel = static_cast<int32_t>(T - (H + 4 + 5));
        std::memcpy(c + 5, &rel, 4);
        CHECK(inl::calls(c, 11, H, T, inl::Host::X64));
        uint8_t j[8] = {0x48, 0x83, 0xc4, 0x08, 0xE9, 0, 0, 0};
        uint8_t jj[9];
        std::memcpy(jj, j, 5);
        const int32_t rj = static_cast<int32_t>(T - (H + 4 + 5));
        std::memcpy(jj + 5, &rj, 4);
        CHECK(inl::calls(jj, 9, H, T, inl::Host::X64));
        uint8_t n[6] = {0x55, 0x8d, 0x41, 0x01, 0x5d, 0xc3};
        CHECK(!inl::calls(n, 6, H, T, inl::Host::X64));
    }
    // the closure: F is called by A (keeps the call), B (inlined F) and C (inlined F); D calls B and inlined it; E calls D and keeps the
    // call; C calls itself. Redirect with F: B, C (inlined it) and D (inlined B, so F's body is in D too); not A, not E.
    {
        std::map<uint64_t, std::vector<uint64_t>> callers{{'F', {'A', 'B', 'C'}}, {'B', {'D'}}, {'C', {'C'}}, {'D', {'E'}}};
        std::set<std::pair<uint64_t, uint64_t>> kept{{'A', 'F'}, {'E', 'D'}};
        inl::Source src;
        src.guest_callers = [&](uint64_t f) { auto it = callers.find(f); return it == callers.end() ? std::vector<uint64_t>{} : it->second; };
        src.host_of = [](uint64_t f, uint64_t* h, uint64_t* e) { *h = f * 0x100; *e = f * 0x100 + 0x80; return true; };
        src.host_calls = [&](uint64_t h, uint64_t, uint64_t ch) { return kept.count({h / 0x100, ch / 0x100}) != 0; };
        auto r = inl::inliners('F', src);
        std::set<uint64_t> got(r.begin(), r.end());
        CHECK(got == (std::set<uint64_t>{'B', 'C', 'D'}));
        bool capped = false;
        auto r1 = inl::inliners('F', src, 1, &capped);
        CHECK(r1.size() == 1 && capped);
        // a function no caller inlined: nothing more to redirect
        CHECK(inl::inliners('A', src).empty());
    }
    if (g_fail == 0) std::printf("ALL INLINERS TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
