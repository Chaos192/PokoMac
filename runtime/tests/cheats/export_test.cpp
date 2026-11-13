// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Test that exported cheats (runtime/src/cheats/export.*) parse and run through the VM: a static write and a two-level pointer path.
#define _CRT_SECURE_NO_WARNINGS
#include "scanner.h"
#include "gmem.h"
#include "cheat_vm.h"
#include "export.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"          // the scanner's array-of-bytes scans
#include "cheat_vm.cpp"
#include "export.cpp"

using namespace nxrt::cheats;

struct MapHost : VmHost {
    std::map<uint64_t, uint8_t> m;
    uint64_t mb = 0x7100000000ull, hb = 0x7800000000ull;
    bool read(uint64_t a, void* d, size_t n) override { uint8_t* p = (uint8_t*)d; for (size_t i = 0; i < n; ++i) { auto it = m.find(a + i); if (it == m.end()) return false; p[i] = it->second; } return true; }
    bool write(uint64_t a, const void* s, size_t n) override { const uint8_t* p = (const uint8_t*)s; for (size_t i = 0; i < n; ++i) m[a + i] = p[i]; return true; }
    uint64_t base(unsigned t) override { return (t & 0xF) == 1 ? hb : mb; }
    uint64_t keys_held() override { return 0; }
    template <class T> void put(uint64_t a, T v) { uint8_t* p = (uint8_t*)&v; for (size_t i = 0; i < sizeof(T); ++i) m[a + i] = p[i]; }
    template <class T> T get(uint64_t a) { T v{}; uint8_t* p = (uint8_t*)&v; for (size_t i = 0; i < sizeof(T); ++i) { auto it = m.find(a + i); p[i] = it == m.end() ? 0 : it->second; } return v; }
};

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // --- static write: main+0x1234 = 999 (u32) ---
    {
        Value v = Value::u(ScanType::U32, 999);
        std::string body = export_static_body(0, 0x1234, v);
        std::string text = export_cheat("Coins 999", body);
        CheatFile f = ParseFile(text);
        CHECK(f.ok);
        Vm vm; vm.load(f);
        MapHost h;
        vm.run(h);
        CHECK(h.get<uint32_t>(0x7100000000ull + 0x1234) == 999);
    }

    // --- pointer path: [main+0x100] -> +0x10 -> +0x8 holds the value; write 1234 (u32) there ---
    {
        MapHost h;
        const uint64_t p1 = 0x7800010000ull;        // first pointer target (in heap)
        const uint64_t p2 = 0x7800020000ull;        // second pointer target
        h.put<uint64_t>(0x7100000000ull + 0x100, p1);     // [main+0x100] = p1
        h.put<uint64_t>(p1 + 0x10, p2);                   // [p1+0x10] = p2
        // final field at p2 + 0x8
        Value v = Value::u(ScanType::U32, 1234);
        std::vector<uint64_t> offs = {0x10, 0x8};         // follow +0x10, write at +0x8
        std::string body = export_pointer_body(0, 0x100, offs, v);
        std::string text = export_cheat("Ptr", body);
        CheatFile f = ParseFile(text);
        CHECK(f.ok);
        Vm vm; vm.load(f);
        vm.run(h);
        CHECK(h.get<uint32_t>(p2 + 0x8) == 1234);
    }

    // --- 8-byte static write ---
    {
        Value v = Value::u(ScanType::U64, 0x1122334455667788ull);
        CheatFile f = ParseFile(export_cheat("Big", export_static_body(1, 0x40, v)));   // heap+0x40
        CHECK(f.ok);
        Vm vm; vm.load(f);
        MapHost h;
        vm.run(h);
        CHECK(h.get<uint64_t>(0x7800000000ull + 0x40) == 0x1122334455667788ull);
    }

    if (g_fail == 0) std::printf("ALL EXPORT TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
