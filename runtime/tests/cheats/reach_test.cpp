// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// What a live patch reaches (runtime/src/cheats/reach.*): the code the patcher makes ready on its worker before the swap, so no guest
// thread waits for the build's compiler at the patch's first run. Real ASM cheats of Pokopia 2.0.1 (tomvita's Leafage 5x5 and Water Gun
// 5x5: a branch over the hooked instruction into a cave in the padding after main's code, the cave calling or branching back) and the
// shapes around them. Standalone: includes reach.cpp and disasm.cpp.
#include "reach.h"

#include "disasm.cpp"
#include "reach.cpp"

#include <cstdio>
#include <map>

using namespace nxrt::cheats;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const uint64_t M = 0x7100000000ull, CAVE_LO = M + 0x2749480, CAVE_HI = M + 0x274a000;

struct Mem {
    std::map<uint64_t, uint32_t> words;
    void put(uint64_t a, std::initializer_list<uint32_t> ws) { for (uint32_t w : ws) { words[a] = w; a += 4; } }
    reach::Source src() {
        reach::Source s;
        s.read = [this](uint64_t a, uint32_t* w) { auto it = words.find(a); *w = it == words.end() ? 0 : it->second; return a >= CAVE_LO && a < CAVE_HI; };
        s.region = [](uint64_t a, uint64_t* b, uint64_t* e) { if (a < CAVE_LO || a >= CAVE_HI) return false; *b = CAVE_LO; *e = CAVE_HI; return true; };
        return s;
    }
};

int main() {
    // Leafage 5x5: main+0x59d4c0 (pacibsp, a one-instruction function in this lift) becomes b cave+0x...b00; the cave loops 25 times calling
    // its own tail at 0x2749bac (pacibsp; b 0x59d4c4: the function's body after the hooked instruction)
    {
        Mem m;
        m.put(M + 0x2749b00, {0xa9bb7bfd, 0x910003fd, 0xa90153f3, 0xa9025bf5, 0xa90363f7, 0xf90023f9, 0xaa0003f3, 0xaa0103f4, 0xaa0203f5, 0x7940aa96,
                              0x7940b297, 0x52800019, 0x52800018, 0x1100370f, 0x710065ff, 0x5400004b, 0x510065ef, 0x528000a9, 0x1ac909ea, 0x1b09bd4b,
                              0x5100096b, 0x5100094a, 0x0b0b02cc, 0x0b0a02ed, 0x7900aa8c, 0x7900b28d, 0xaa1303e0, 0xaa1403e1, 0xaa1503e2, 0x9400000e,
                              0x2a000339, 0x11000718, 0x7100671f, 0x54fffd8b, 0x7900aa96, 0x7900b297, 0x2a1903e0, 0xf94023f9, 0xa94363f7, 0xa9425bf5,
                              0xa94153f3, 0xa8c57bfd, 0xd65f03c0, 0xd503237f, 0x17794e45});
        const auto t = reach::to_prepare({{M + 0x59d4c0, 0x1486b190}}, M + 0x59d4c0, M + 0x59d4c4, m.src());
        CHECK(t.size() == 3);
        CHECK(t.size() == 3 && t[0] == M + 0x2749b00);          // the cave first
        CHECK(t.size() == 3 && t[1] == M + 0x2749bac);          // the cave's call of its own tail
        CHECK(t.size() == 3 && t[2] == M + 0x59d4c4);           // and where that tail branches: the body after the hook
    }
    // Water Gun 5x5: main+0x5a1384 inside its function becomes b cave+0xa00; the cave ends with b 0x5a1388 (the instruction after the hook)
    {
        Mem m;
        m.put(M + 0x2749a00, {0x39460be8, 0x340002e8, 0x79c00309, 0x91012bed, 0x79c005aa, 0x79c00b0b, 0x1280002c, 0x52800008, 0x1280002e, 0x0b0c012f,
                              0x0b0e0170, 0x528000d1, 0x9bb13511, 0x7900022f, 0x7900062a, 0x79000a30, 0x11000508, 0x110005ce, 0x710009df, 0x54fffecd,
                              0x1100058c, 0x7100099f, 0x54fffe4d, 0x39060be8, 0x39460be8, 0x17795e49});
        const auto t = reach::to_prepare({{M + 0x5a1384, 0x1486a19f}}, M + 0x5a1280, M + 0x5a1b18, m.src());
        CHECK(t.size() == 2 && t[0] == M + 0x2749a00 && t[1] == M + 0x5a1388);
    }
    // a patch that stays inside its function (a NOP, a branch within it) reaches nothing new; a bl to another function is made ready
    {
        Mem m;
        CHECK(reach::to_prepare({{M + 0x1000, 0xd503201f}}, M + 0x1000, M + 0x1100, m.src()).empty());
        CHECK(reach::to_prepare({{M + 0x1000, 0x14000010}}, M + 0x1000, M + 0x1100, m.src()).empty());                 // b +0x40, inside
        const auto t = reach::to_prepare({{M + 0x1000, 0x94000400}}, M + 0x1000, M + 0x1100, m.src());              // bl +0x1000
        CHECK(t.size() == 1 && t[0] == M + 0x2000);
        CHECK(reach::to_prepare({{M + 0x1000, 0x90000000}}, M + 0x1000, M + 0x1100, m.src()).empty());                 // adrp is no branch
    }
    // a cave is followed only to the free padding (four zero words), and a loop back into the cave adds nothing twice
    {
        Mem m;
        m.put(M + 0x2749c00, {0x52800148, 0x390133e8, 0x978a02ea, 0x17816eca, 0, 0, 0, 0, 0x14000000});   // crop x10's cave, then junk after
        const auto t = reach::to_prepare({{M + 0x7a5730, 0x147e9134}}, M + 0x7a5040, M + 0x7a5b98, m.src());
        CHECK(t.size() == 3 && t[0] == M + 0x2749c00 && t[1] == M + 0x9ca7b0 && t[2] == M + 0x7a5734);   // its bl and its branch back
    }
    if (g_fail == 0) std::printf("ALL REACH TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
