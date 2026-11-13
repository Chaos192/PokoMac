// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test of the Atmosphère cheat parser and VM (runtime/src/cheats/cheat_vm.*): no runtime dependencies, so it builds and runs
// on any platform with a C++20 compiler. CMake target nxrt_cheat_vm_test (runtime/tests/cheats/CMakeLists.txt). Each cheat below is hand
// assembled from the opcode encodings in nxnative docs/reference/cheats.md.
#define _CRT_SECURE_NO_WARNINGS
#include "cheat_vm.h"
#include "cheat_vm.cpp"

#include <cstdio>
#include <map>

using namespace nxrt::cheats;

struct ArrHost : VmHost {
    std::map<uint64_t, uint8_t> mem;           // sparse guest memory
    uint64_t main_b = 0x7100000000ull, heap_b = 0x7800000000ull, alias_b = 0x6000000000ull;
    uint64_t keys = 0;
    bool fail_reads = false;
    bool read(uint64_t a, void* dst, size_t n) override {
        if (fail_reads) return false;
        uint8_t* d = (uint8_t*)dst;
        for (size_t i = 0; i < n; ++i) { auto it = mem.find(a + i); if (it == mem.end()) return false; d[i] = it->second; }
        return true;
    }
    bool write(uint64_t a, const void* src, size_t n) override {
        const uint8_t* s = (const uint8_t*)src;
        for (size_t i = 0; i < n; ++i) mem[a + i] = s[i];
        return true;
    }
    uint64_t base(unsigned t) override {
        switch (t & 0xF) { case 1: return heap_b; case 2: return alias_b; case 4: return 0; default: return main_b; }
    }
    uint64_t keys_held() override { return keys; }
    void put(uint64_t a, uint64_t v, int n) { for (int i = 0; i < n; ++i) mem[a + i] = (v >> (8 * i)) & 0xFF; }
    uint64_t get(uint64_t a, int n) { uint64_t v = 0; for (int i = 0; i < n; ++i) v |= (uint64_t)mem[a + i] << (8 * i); return v; }
};

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static CheatFile parse_ok(const char* t) { CheatFile f = ParseFile(t); if (!f.ok) std::printf("parse error: %s\n", f.error.c_str()); return f; }

int main() {
    // --- parse: a simple 4-byte write cheat (opcode 0, T=4, M=0 main, R=0) ---
    {
        CheatFile f = parse_ok("[Infinite]\n04000000 00123456 000003E7\n");
        CHECK(f.ok);
        CHECK(f.count() == 1);
        CHECK(f.entries[1].used && f.entries[1].name == "Infinite");
        CHECK(f.entries[1].words.size() == 3);
        Vm vm; CHECK(vm.load(f));
        ArrHost h;
        PassReport r = vm.run(h);
        CHECK(r.ran && !r.stopped);
        CHECK(h.get(0x7100000000ull + 0x123456, 4) == 999);
        CHECK(r.writes == 1);
    }
    // --- labels vanish, master always on ---
    {
        CheatFile f = parse_ok("{Master}\n04000000 00000010 00000005\n[Label]\n[Real]\n04000000 00000020 00000009\n");
        CHECK(f.ok);
        CHECK(f.entries[0].used && f.entries[0].enabled);
        CHECK(f.count() == 2);   // master + Real; Label vanished (slot reused by Real)
        CHECK(f.entries[1].name == "Real");
    }
    // --- glued words split; a bad char fails the file ---
    {
        CheatFile f = ParseFile("[X]\n0400000000123456 000003E7\n");
        CHECK(f.ok);   // 16 hex digits = two words
        CHECK(f.entries[1].words.size() == 3);
        CheatFile g = ParseFile("[X]\n0x04 00000000\n");
        CHECK(!g.ok);  // '0x' has an 'x'
        CheatFile pr = ParseFile("not a cheat\n");
        CHECK(!pr.ok); // prose, a word before any header
    }
    // --- opcode 1 conditional + end: write only when value matches ---
    {
        // if main+0x100 (u32) == 5 { main+0x200 = 0xABCD (u32) } end   (1 T=4 M=0 C=5 X=0 r=0)
        CheatFile f = parse_ok("[C]\n14050000 00000100 00000005\n04000000 00000200 0000ABCD\n20000000\n");
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h; h.put(0x7100000000ull + 0x100, 5, 4);
        vm.run(h);
        CHECK(h.get(0x7100000000ull + 0x200, 4) == 0xABCD);
        // now make it not match
        ArrHost h2; h2.put(0x7100000000ull + 0x100, 4, 4);
        vm.run(h2);
        CHECK(h2.mem.find(0x7100000000ull + 0x200) == h2.mem.end());   // nothing written
    }
    // --- if/else: unmet if runs the else ---
    {
        // if main+0 (u32) == 1 { main+0x10 = 0xAA } else { main+0x10 = 0xBB } end
        CheatFile f = parse_ok(
            "[E]\n"
            "14050000 00000000 00000001\n"   // if u32 main+0 == 1
            "04000000 00000010 000000AA\n"
            "21000000\n"                       // else
            "04000000 00000010 000000BB\n"
            "20000000\n");
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h; h.put(0x7100000000ull, 0, 4);   // not 1 -> else
        vm.run(h);
        CHECK(h.get(0x7100000000ull + 0x10, 4) == 0xBB);
        ArrHost h2; h2.put(0x7100000000ull, 1, 4);  // ==1 -> if
        vm.run(h2);
        CHECK(h2.get(0x7100000000ull + 0x10, 4) == 0xAA);
    }
    // --- loop: write 0x11 to five consecutive bytes via register store + increment ---
    {
        // reg0 = main+0x300 ; load loop reg1=5 { store u8 0x11 to [reg0]+ ; }
        // 400 R=0 -> set reg0 to address; use opcode A to store reg2, with post-increment
        // Simpler: reg0=addr (op4), reg2=0x11 (op4), loop reg1=5 { A: store u8 reg2 to reg0, inc } end-loop
        CheatFile f = parse_ok(
            "[L]\n"
            "40000000 00000071 00000300\n"   // reg0 = 0x7100000300
            "40020000 00000000 00000011\n"   // reg2 = 0x11
            "30010000 00000005\n"            // loop start reg1 = 5
            "A1201000\n"                     // A: T=1,S=2,R=0,I=1,O=0 -> store u8 reg2 to [reg0], reg0 += 1
            "31010000\n");                   // loop end reg1
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h;
        PassReport r = vm.run(h);
        CHECK(!r.stopped);
        for (int i = 0; i < 5; ++i) CHECK(h.get(0x7100000300ull + i, 1) == 0x11);
        CHECK(h.get(0x7100000300ull + 5, 1) == 0);  // not the sixth
    }
    // --- arithmetic opcode 9: reg0 = reg1 + reg2 ---
    {
        CheatFile f = parse_ok(
            "[A]\n"
            "40010000 00000000 00000064\n"   // reg1 = 100
            "40020000 00000000 00000007\n"   // reg2 = 7
            "94001020\n"                     // 9 T=4 C=0(add) R=0 S=1 imm=0 s=2 -> reg0 = reg1 + reg2
            "A4000400 00000400\n");          // A T=4 S=0 R=0 I=0 O=4 x=0, a=0x400 -> store u32 reg0 to main+0x400
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h; vm.run(h);
        CHECK(h.get(0x7100000000ull + 0x400, 4) == 107);
    }
    // --- unknown opcode stops the pass; earlier writes ran ---
    {
        CheatFile f = parse_ok("[U]\n04000000 00000010 00000001\nB0000000\n04000000 00000020 00000002\n");
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h;
        PassReport r = vm.run(h);
        CHECK(h.get(0x7100000000ull + 0x10, 4) == 1);   // ran before the bad opcode
        CHECK(h.mem.find(0x7100000000ull + 0x20) == h.mem.end());  // after it did not
        CHECK(r.stopped);
    }
    // --- failed read: compare sees 0, cheat stays, no crash ---
    {
        CheatFile f = parse_ok("[R]\n14050000 00000100 00000000\n04000000 00000200 00000063\n20000000\n");  // if u32 main+0x100 == 0
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h; h.fail_reads = true;
        PassReport r = vm.run(h);
        CHECK(r.failed_reads >= 1);
        CHECK(!r.stopped);   // the pass goes on
    }
    // --- keypress opcode 8: only writes when the mask is held ---
    {
        CheatFile f = parse_ok("[K]\n80000001\n04000000 00000010 00000042\n20000000\n");  // if button bit0 held
        CHECK(f.ok);
        Vm vm; vm.load(f);
        ArrHost h; h.keys = 0;   // not held
        vm.run(h);
        CHECK(h.mem.find(0x7100000000ull + 0x10) == h.mem.end());
        h.keys = 1;              // held
        vm.run(h);
        CHECK(h.get(0x7100000000ull + 0x10, 4) == 0x42);
    }
    // --- toggles.txt turns a cheat off ---
    {
        CheatFile f = parse_ok("[One]\n04000000 00000010 00000001\n[Two]\n04000000 00000020 00000002\n");
        std::string err;
        CHECK(ParseToggles("[Two]\noff\n", f, &err));
        CHECK(f.entries[1].enabled);    // One stays on
        CHECK(!f.entries[2].enabled);   // Two off
        Vm vm; vm.load(f);
        ArrHost h; vm.run(h);
        CHECK(h.get(0x7100000000ull + 0x10, 4) == 1);
        CHECK(h.mem.find(0x7100000000ull + 0x20) == h.mem.end());
    }
    // --- toggles.txt for cheats that share a name (a cheat site's folder markers): each keeps its own state through WriteToggles ---
    {
        CheatFile f = parse_ok("[Group]\n20000000\n[A]\n04000000 00000010 00000001\n[Group]\n20000001\n[B]\n04000000 00000020 00000002\n");
        CHECK(f.count() == 4);
        for (size_t k = 1; k <= 4; ++k) f.entries[k].enabled = false;
        f.entries[3].enabled = true;            // the second Group only
        const std::string tog = WriteToggles(f);
        CheatFile g = parse_ok("[Group]\n20000000\n[A]\n04000000 00000010 00000001\n[Group]\n20000001\n[B]\n04000000 00000020 00000002\n");
        std::string err;
        CHECK(ParseToggles(tog, g, &err));
        for (size_t k = 1; k <= 4; ++k) CHECK(g.entries[k].enabled == f.entries[k].enabled);
        // more lines than cheats of that name: the extra one sets the first, as before
        CheatFile h = parse_ok("[Group]\n20000000\n[A]\n04000000 00000010 00000001\n");
        CHECK(ParseToggles("[Group]\nfalse\n[Group]\ntrue\n", h, &err));
        CHECK(h.entries[1].enabled);
    }
    // --- round-trip WriteFile -> ParseFile ---
    {
        CheatFile f = parse_ok("{M}\n04000000 00000010 00000001\n[A]\n04000000 00000020 00000002\n");
        std::string t = WriteFile(f);
        CheatFile g = ParseFile(t);
        CHECK(g.ok);
        CHECK(g.entries[0].used && g.entries[0].name == "M");
        CHECK(g.count() == 2);
    }

    if (g_fail == 0) std::printf("ALL VM TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
