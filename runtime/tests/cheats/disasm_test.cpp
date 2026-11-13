// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test for the A64 disassembler + assembler (src/cheats/disasm.cpp). No runtime dependency.
// a64_golden.inc holds hand-written instructions assembled by clang and printed back by llvm-objdump (LLVM is the reference): each row
// must assemble from LLVM's text to its word, decode to LLVM's mnemonic, and survive decode -> assemble unchanged. (The decoder was also
// compared with llvm-objdump on a game's code: docs/CHEATS.md "Disassembler".)
#include "disasm.h"
#include "disasm.cpp"

#include <cstdio>
#include <string>

using namespace nxrt::cheats::a64;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Golden { uint64_t pc; uint32_t word; const char* text; };
static const Golden kGolden[] = {
#include "a64_golden.inc"
};

static std::string first_token(const std::string& s) {
    const size_t k = s.find_first_of(" \t");
    return k == std::string::npos ? s : s.substr(0, k);
}

int main() {
    int rows = 0;
    for (const Golden& g : kGolden) {
        ++rows;
        uint32_t got = 0;
        std::string err;
        if (!assemble(g.text, g.pc, &got, &err)) { std::printf("FAIL asm '%s': %s\n", g.text, err.c_str()); ++fails; }
        else if (got != g.word) { std::printf("FAIL asm '%s' -> 0x%08x (LLVM: 0x%08x)\n", g.text, got, g.word); ++fails; }
        const Insn in = decode(g.word, g.pc);
        if (!in.modelled || in.mnemonic != first_token(g.text)) {
            std::printf("FAIL decode 0x%08x -> '%s' (LLVM: '%s')\n", g.word, in.text.c_str(), g.text);
            ++fails;
            continue;
        }
        uint32_t back = 0;
        if (!assemble(in.text, g.pc, &back, &err) || back != g.word) {
            std::printf("FAIL round trip 0x%08x -> '%s' -> 0x%08x (%s)\n", g.word, in.text.c_str(), back, err.c_str());
            ++fails;
        }
    }

    // branch targets are absolute; the signature maker's wildcards: a branch and an adrp expose immediate bytes, a nop exposes none
    CHECK(decode(0x14000004, 0x1000).text == "b\t0x1010");
    CHECK(decode(0x54000040, 0x2000).text == "b.eq\t0x2008");
    CHECK(!decode(0x14000004, 0).imm_bytes.empty());
    CHECK(!decode(0x90000000, 0).imm_bytes.empty());
    CHECK(decode(0xd503201f, 0).imm_bytes.empty());
    CHECK(decode(0xd65f03c0, 0).imm_bytes.empty());
    // unmodelled words print as .inst, never as a wrong mnemonic
    CHECK(!decode(0x4e040442, 0).modelled && decode(0x4e040442, 0).text == ".inst\t0x4e040442");   // dup v2.4s, w2 (SIMD: not modelled)
    // the patch forms the overlay uses
    uint32_t w = 0;
    std::string err;
    CHECK(nop_word() == 0xd503201f);
    CHECK(assemble(".word 0x12345678", 0, &w, &err) && w == 0x12345678);
    CHECK(assemble("mov w8, #99", 0, &w, &err) && w == 0x52800c68);
    CHECK(!assemble("mov x0, #0x123456789", 0, &w, &err) && !err.empty());   // needs movz + movk: refused with a message
    CHECK(!assemble("frobnicate x0", 0, &w, &err));
    CHECK(branch_word(0x1000, 0x1000 + (1 << 20), false, &w));
    CHECK(!branch_word(0x1000, 0x1000 + (1ll << 28), false, &w));             // too far
    CHECK(assemble("b .+8", 0x1000, &w, &err) && w == 0x14000002);

    if (fails) { std::printf("disasm_test: %d failure(s) in %d golden rows\n", fails, rows); return 1; }
    std::printf("disasm_test: all passed (%d golden rows)\n", rows);
    return 0;
}
