// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// A compact AArch64 (A64) disassembler and a small assembler for the cheats memory viewer and code tools (phase 6). The disassembler
// turns one 32-bit little-endian instruction word into text (mnemonic + operands) and reports the immediate/branch byte fields a
// signature should wildcard (so a signature made from code survives a rebuild). The assembler turns a line of text back into a word for
// the common patch forms (nop, ret, mov/movz/movn/movk immediate, b/bl to an address, and raw .word). Neither is a full ARM reference:
// the decoder names the instruction classes a game's code and a cheat author meet (loads/stores, add/sub/logical, moves, compares,
// branches, PC-relative, system NOP/RET), and prints ".inst 0x........" for anything it does not model, which is still a valid, safe
// display (never a wrong mnemonic). Pure and standalone: it has no runtime dependency and is unit-tested on its own (tests/cheats).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::cheats::a64 {

// The decode of one instruction word at guest address `pc` (pc is used only to render PC-relative targets as absolute hex).
struct Insn {
    uint32_t raw = 0;
    uint64_t pc = 0;
    std::string text;                 // "add x0, x1, #0x10", ".inst 0x........" when not modelled
    std::string mnemonic;             // "add", "ldr", "b", ... ("" / ".inst" when not modelled)
    bool is_branch = false;           // b, bl, b.cond, cbz/cbnz, tbz/tbnz, ret, br, blr
    bool branch_imm = false;          // the branch target is a PC-relative immediate in this word (b, bl, b.cond, cbz, tbz, adr/adrp)
    uint64_t target = 0;              // absolute target for an immediate branch / adr / adrp (else 0)
    bool modelled = false;            // false = printed as .inst (unknown to this decoder)
    // Byte offsets within the 4-byte word (0..3) whose bits are an immediate or a relocated field: a signature maker wildcards these so
    // a pointer, a branch distance or an adrp page that moves between builds does not break the match.
    std::vector<int> imm_bytes;
};

// Decode one little-endian word.
Insn decode(uint32_t word, uint64_t pc = 0);
// Decode `n` words starting at `pc` from `code` (n*4 bytes, little-endian). Convenience for the viewer's listing.
std::vector<Insn> decode_range(const uint8_t* code, size_t n, uint64_t pc);

// The byte offsets (0..3) a signature should wildcard for the instruction at these 4 bytes (decode(...).imm_bytes). Empty for a fixed
// instruction (no immediate/relocation), so the signature keeps every byte.
std::vector<int> signature_wildcards(uint32_t word);

// ---- assembler ----------------------------------------------------------------------------------------------------
// Assemble one line to a word. Supports: nop; ret [xN]; movz/movn/movk/mov Wn/Xn, #imm [, lsl #s]; b/bl #±imm or b/bl 0xADDR (needs pc);
// .word 0x........ / .inst 0x......... `pc` is the address of this instruction (for b/bl to an absolute target). False with *err on
// anything else (the caller keeps the existing bytes and shows the message).
bool assemble(const std::string& line, uint64_t pc, uint32_t* out, std::string* err);

// The word of a NOP, and of a branch-with-link / branch from `pc` to `target` (for code caves). Returns false if the distance does not
// fit the ±128 MiB range.
uint32_t nop_word();
bool branch_word(uint64_t pc, uint64_t target, bool link, uint32_t* out);

}  // namespace nxrt::cheats::a64
