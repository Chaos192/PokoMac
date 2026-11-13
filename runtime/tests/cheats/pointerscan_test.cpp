// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Test the pointer scanner (runtime/src/cheats/pointerscan.*): build a map, find a path to a heap target rooted in a module, resolve it,
// and rescan after a simulated restart that moves the dynamic objects.
#define _CRT_SECURE_NO_WARNINGS
#include "scanner.h"
#include "gmem.h"
#include "pointerscan.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"          // the scanner's array-of-bytes scans
#include "pointerscan.cpp"

using namespace nxrt::cheats;

// A fake source with two regions: main (static) and heap (dynamic).
struct FS : MemSource {
    uint64_t main_b = 0x7100000000ull, heap_b = 0x7800000000ull;
    std::map<uint64_t, uint8_t> mem;
    std::vector<ScanRegion> regions(const RegionFilter& f) override {
        std::vector<ScanRegion> out;
        out.push_back({main_b, 0x10000, 3, "data"});      // main data (static root)
        out.push_back({heap_b, 0x100000, 3, "heap"});     // heap (dynamic)
        (void)f;
        return out;
    }
    bool read(uint64_t a, void* d, size_t n) override {
        // committed memory: within a declared region every byte reads (0 where unset); outside, the read fails.
        const bool in_main = a >= main_b && a + n <= main_b + 0x10000;
        const bool in_heap = a >= heap_b && a + n <= heap_b + 0x100000;
        if (!in_main && !in_heap) return false;
        uint8_t* p = (uint8_t*)d;
        for (size_t i = 0; i < n; ++i) { auto it = mem.find(a + i); p[i] = it == mem.end() ? 0 : it->second; }
        return true;
    }
    void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) mem[a + i] = (v >> (8 * i)) & 0xFF; }
    void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) mem[a + i] = (v >> (8 * i)) & 0xFF; }
};

static FS* g_src = nullptr;
static uint64_t mbase(const std::string& m) { return m == "main" ? g_src->main_b : 0; }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    FS f;
    g_src = &f;
    const uint64_t M = f.main_b, H = f.heap_b;

    // Build a 2-level chain:
    //   static slot   main+0x200  -> objA (heap)
    //   objA+0x18     -> objB
    //   objB+0x40     = the value (target)
    const uint64_t objA = H + 0x1000;
    const uint64_t objB = H + 0x8000;
    const uint64_t target = objB + 0x40;
    f.put64(M + 0x200, objA);
    f.put64(objA + 0x18, objB);
    f.put32(target, 777);

    PointerScanOptions opt;
    opt.scan.main = true; opt.scan.heap = true; opt.scan.alias = false; opt.scan.other = true;
    opt.max_offset = 0x100;
    opt.max_depth = 3;

    std::vector<ScanRegion> inr = f.regions(opt.scan);
    PointerMap map = pointer_map_build(f, opt, inr, nullptr);
    CHECK(map.slots() >= 2);

    std::vector<StaticRange> roots = {{M, M + 0x10000, M, "main", 0}};
    auto paths = pointer_find_paths(map, roots, target, opt);
    CHECK(!paths.empty());

    // At least one path resolves to the target now.
    bool any = false;
    PointerPath good;
    for (auto& p : paths) {
        if (pointer_resolve(f, p, mbase) == target) { any = true; good = p; break; }
    }
    CHECK(any);
    // the expected shortest path is main+0x200 -> +0x18 -> +0x40
    CHECK(good.module == "main");
    CHECK(good.base_offset == 0x200);
    CHECK(good.offsets.size() == 2 && good.offsets[0] == 0x18 && good.offsets[1] == 0x40);

    // --- simulate a restart: the heap objects move, but the structure holds. Rescan keeps the path. ---
    FS f2;
    g_src = &f2;
    const uint64_t objA2 = H + 0x3000;    // moved
    const uint64_t objB2 = H + 0x9000;    // moved
    const uint64_t target2 = objB2 + 0x40;
    f2.put64(M + 0x200, objA2);
    f2.put64(objA2 + 0x18, objB2);
    f2.put32(target2, 777);

    auto kept = pointer_rescan(f2, paths, target2, mbase);
    CHECK(!kept.empty());
    CHECK(pointer_resolve(f2, kept[0], mbase) == target2);   // the path still lands on the (new) target

    if (g_fail == 0) std::printf("ALL POINTERSCAN TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
