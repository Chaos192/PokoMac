// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test of AOB parsing, scanning and signature making (runtime/src/cheats/aob.*).
#define _CRT_SECURE_NO_WARNINGS
#include "scanner.h"
#include "gmem.h"
#include "aob.h"

#include <cstdio>
#include <cstring>

namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"

using namespace nxrt::cheats;

struct FakeSource : MemSource {
    uint64_t base;
    std::vector<uint8_t> mem;
    FakeSource(uint64_t b, size_t n) : base(b), mem(n, 0) {}
    std::vector<ScanRegion> regions(const RegionFilter&) override { return {{base, mem.size(), 5, "code"}}; }
    bool read(uint64_t a, void* d, size_t n) override {
        if (a < base || a + n > base + mem.size()) return false;
        std::memcpy(d, mem.data() + (a - base), n);
        return true;
    }
};

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // --- parse ---
    {
        AobPattern p; std::string err;
        CHECK(aob_parse("48 8B ?? 4?", &p, &err));
        CHECK(p.size() == 4);
        CHECK(p.val[0] == 0x48 && p.mask[0] == 0xFF);
        CHECK(p.mask[2] == 0x00);              // ??
        CHECK(p.val[3] == 0x40 && p.mask[3] == 0xF0);   // 4?
        CHECK(aob_text(p) == "48 8B ?? 4?");
        AobPattern q;
        CHECK(aob_parse("488B??", &q, &err));  // no spaces
        CHECK(q.size() == 3);
        AobPattern bad;
        CHECK(!aob_parse("4G 8B", &bad, &err));  // bad hex
    }

    const uint64_t B = 0x7100000000ull;
    auto src = std::make_shared<FakeSource>(B, 0x4000);
    FakeSource& f = *src;
    // place a unique-ish sequence
    const uint8_t seq[] = {0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3};
    std::memcpy(f.mem.data() + 0x1000, seq, sizeof seq);
    std::memcpy(f.mem.data() + 0x2500, seq, sizeof seq);   // a second copy (so a short pattern is not unique)

    // --- scan: wildcard the 4 immediate bytes, expect 2 hits ---
    {
        AobPattern p; std::string err;
        CHECK(aob_parse("48 8B 05 ?? ?? ?? ?? C3", &p, &err));
        RegionFilter filt; filt.main = filt.heap = filt.alias = true; filt.code = true;
        auto hits = aob_scan(f, p, filt);
        CHECK(hits.size() == 2);
        CHECK(hits[0].addr == B + 0x1000);
        CHECK(hits[1].addr == B + 0x2500);
    }

    // --- make a signature for 0x1000: with the immediates wildcarded it is not unique (2 copies); without, it is unique ---
    {
        std::vector<size_t> wild = {3, 4, 5, 6};   // the four immediate bytes
        Signature s = aob_make_signature(f, B + 0x1000, B, B + 0x4000, wild, 8, 32);
        // the two copies share bytes, so uniqueness requires growing beyond the shared run; make the tail differ
        // here both copies are identical for 8 bytes, so 8-byte sig matches 2. Grow won't help (identical). So matches==2.
        CHECK(s.matches == 2);
        // now make the second copy differ after the sequence so the signature can become unique by growing
    }
    {
        // differentiate: put distinct bytes right after each copy, then a longer sig is unique
        f.mem[0x1000 + 8] = 0xAA;
        f.mem[0x2500 + 8] = 0xBB;
        std::vector<size_t> wild;   // no wildcards
        Signature s = aob_make_signature(f, B + 0x1000, B, B + 0x4000, wild, 8, 32);
        CHECK(s.unique());
        CHECK(s.pattern.size() >= 9);   // needed the 9th byte to disambiguate
        // the produced pattern really matches only at 0x1000
        RegionFilter filt; filt.main = true; filt.code = true;
        auto hits = aob_scan(f, s.pattern, filt);
        CHECK(hits.size() == 1 && hits[0].addr == B + 0x1000);
    }

    if (g_fail == 0) std::printf("ALL AOB TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
