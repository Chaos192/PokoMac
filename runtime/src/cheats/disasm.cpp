// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// A64 disassembler and assembler (disasm.h). Decodes by the encoding groups of the ARMv8-A instruction set (ARM ARM, "A64 Instruction
// Set Encoding") and prints LLVM's preferred assembly (its aliases: mov, cmp, lsl, ubfx, cset, mul, ...). The assembler takes the same
// syntax back, so every line the disassembler prints for a modelled instruction assembles to the same word (tests/cheats/disasm_test:
// checked against llvm-objdump on a game's code). Anything not modelled prints as ".inst 0x........" (never a wrong mnemonic).
#include "disasm.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nxrt::cheats::a64 {
namespace {

uint32_t bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((hi - lo + 1) >= 32 ? 0xffffffffu : ((1u << (hi - lo + 1)) - 1)); }
bool bit(uint32_t w, int b) { return (w >> b) & 1; }

int64_t sext(uint64_t v, int nbits) {
    const uint64_t m = 1ull << (nbits - 1);
    v &= (nbits >= 64) ? ~0ull : ((1ull << nbits) - 1);
    return static_cast<int64_t>((v ^ m) - m);
}

std::string hx(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}
// "#imm": decimal below 10, else hex (negative: "#-0x10")
std::string imm(int64_t v) {
    char b[40];
    if (v < 0) {
        if (v > -10) std::snprintf(b, sizeof b, "#%lld", static_cast<long long>(v));
        else std::snprintf(b, sizeof b, "#-0x%llx", static_cast<unsigned long long>(-v));
    } else if (v < 10) {
        std::snprintf(b, sizeof b, "#%lld", static_cast<long long>(v));
    } else {
        std::snprintf(b, sizeof b, "#0x%llx", static_cast<unsigned long long>(v));
    }
    return b;
}
std::string uimm(uint64_t v) { return v < 10 ? "#" + std::to_string(v) : "#" + hx(v); }
std::string dec(uint64_t v) { return "#" + std::to_string(v); }

std::string xreg(unsigned n, bool sp = false) { return n == 31 ? (sp ? "sp" : "xzr") : "x" + std::to_string(n); }
std::string wreg(unsigned n, bool sp = false) { return n == 31 ? (sp ? "wsp" : "wzr") : "w" + std::to_string(n); }
std::string reg(bool is64, unsigned n, bool sp = false) { return is64 ? xreg(n, sp) : wreg(n, sp); }
// FP / SIMD scalar register of a size: 0 b, 1 h, 2 s, 3 d, 4 q
std::string vreg(unsigned sz, unsigned n) {
    static const char p[] = {'b', 'h', 's', 'd', 'q'};
    return std::string(1, p[sz]) + std::to_string(n);
}

const char* kConds[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};

// DecodeBitMasks for the logical immediates. False for a reserved encoding.
bool decode_bit_mask(unsigned N, unsigned imms, unsigned immr, bool is64, uint64_t* wmask, uint64_t* tmask = nullptr) {
    const unsigned v = (N << 6) | (~imms & 0x3f);
    if (v == 0) return false;
    int len = 31 - __builtin_clz(v);
    if (len < 1) return false;
    if (!is64 && N) return false;
    const unsigned esize = 1u << len;
    const unsigned levels = esize - 1;
    const unsigned S = imms & levels, R = immr & levels;
    if (S == levels) return false;
    const unsigned d = (S - R) & levels;
    auto ones = [](unsigned n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); };
    const uint64_t emask = ones(esize);
    const uint64_t welem = ones(S + 1);
    const uint64_t telem = ones(d + 1);
    auto ror = [&](uint64_t x, unsigned r) { r %= esize; return r == 0 ? x : (((x >> r) | (x << (esize - r))) & emask); };
    const uint64_t we = ror(welem, R);
    uint64_t w = 0, t = 0;
    for (unsigned i = 0; i < 64; i += esize) {
        w |= we << i;
        t |= telem << i;
    }
    if (!is64) { w &= 0xffffffffull; t &= 0xffffffffull; }
    *wmask = w;
    if (tmask) *tmask = t;
    return true;
}

// The value of an FMOV immediate (VFPExpandImm).
double fp_imm8(unsigned imm8) {
    const int sign = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, frac = imm8 & 15;
    const int e = b ? cd - 3 : cd + 1;
    const double v = (1.0 + frac / 16.0) * std::ldexp(1.0, e);
    return sign ? -v : v;
}
std::string fp_text(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "#%.8f", v);
    // trim trailing zeros but keep one decimal ("#1.0")
    std::string s = b;
    while (s.size() > 3 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    return s;
}

void set(Insn& in, const std::string& mn, const std::string& ops) {
    in.modelled = true;
    in.mnemonic = mn;
    in.text = ops.empty() ? mn : (mn + "\t" + ops);
}

// bytes of the word that carry bit field [hi:lo] (inclusive)
void mark_field(Insn& in, int hi, int lo) {
    for (int b = 0; b < 4; ++b) {
        const int blo = b * 8, bhi = blo + 7;
        if (hi >= blo && lo <= bhi) {
            bool dup = false;
            for (int e : in.imm_bytes) if (e == b) dup = true;
            if (!dup) in.imm_bytes.push_back(b);
        }
    }
}

std::string shift_suffix(unsigned type, unsigned amount) {
    static const char* sh[] = {"lsl", "lsr", "asr", "ror"};
    if (!amount) return "";
    return std::string(", ") + sh[type] + " " + dec(amount);
}

// a PRFM operation's name ("pldl1keep"), or "#n" when it has none
std::string prf_name(unsigned rt) {
    static const char* type[] = {"pld", "pli", "pst"};
    static const char* tgt[] = {"l1", "l2", "l3"};
    if ((rt >> 3) < 3 && ((rt >> 1) & 3) < 3) return std::string(type[rt >> 3]) + tgt[(rt >> 1) & 3] + ((rt & 1) ? "strm" : "keep");
    return "#" + std::to_string(rt);
}

std::string mem_off(unsigned rn, int64_t off) {
    std::string s = "[" + xreg(rn, true);
    if (off) s += ", " + imm(off);
    return s + "]";
}

// ---- decoders by group ------------------------------------------------------------------------------------------------------------
bool dec_branch(uint32_t w, uint64_t pc, Insn& in) {
    if ((w & 0x7c000000) == 0x14000000) {                                  // b / bl
        const int64_t off = sext(bits(w, 25, 0), 26) * 4;
        in.is_branch = in.branch_imm = true;
        in.target = pc + off;
        mark_field(in, 25, 0);
        set(in, bit(w, 31) ? "bl" : "b", hx(in.target));
        return true;
    }
    if ((w & 0x7e000000) == 0x34000000) {                                  // cbz / cbnz
        const int64_t off = sext(bits(w, 23, 5), 19) * 4;
        in.is_branch = in.branch_imm = true;
        in.target = pc + off;
        mark_field(in, 23, 5);
        set(in, bit(w, 24) ? "cbnz" : "cbz", reg(bit(w, 31), bits(w, 4, 0)) + ", " + hx(in.target));
        return true;
    }
    if ((w & 0x7e000000) == 0x36000000) {                                  // tbz / tbnz
        const unsigned b40 = (bits(w, 31, 31) << 5) | bits(w, 23, 19);
        const int64_t off = sext(bits(w, 18, 5), 14) * 4;
        in.is_branch = in.branch_imm = true;
        in.target = pc + off;
        mark_field(in, 18, 5);
        set(in, bit(w, 24) ? "tbnz" : "tbz", reg(b40 >= 32, bits(w, 4, 0)) + ", " + dec(b40) + ", " + hx(in.target));
        return true;
    }
    if ((w & 0xff000010) == 0x54000000) {                                  // b.cond
        const int64_t off = sext(bits(w, 23, 5), 19) * 4;
        in.is_branch = in.branch_imm = true;
        in.target = pc + off;
        mark_field(in, 23, 5);
        set(in, std::string("b.") + kConds[bits(w, 3, 0)], hx(in.target));
        return true;
    }
    if ((w & 0xff9ffc1f) == 0xd61f0000) {                                  // br / blr / ret
        const unsigned opc = bits(w, 22, 21), rn = bits(w, 9, 5);
        in.is_branch = true;
        if (opc == 0) set(in, "br", xreg(rn));
        else if (opc == 1) set(in, "blr", xreg(rn));
        else if (opc == 2) set(in, "ret", rn == 30 ? "" : xreg(rn));
        else return false;
        return true;
    }
    return false;
}

bool dec_system(uint32_t w, Insn& in) {
    if ((w & 0xfffff01f) == 0xd503201f) {                                  // hints
        const unsigned crm_op2 = bits(w, 11, 5);
        static const char* h[] = {"nop", "yield", "wfe", "wfi", "sev", "sevl"};
        if (crm_op2 < 6) set(in, h[crm_op2], "");
        else set(in, "hint", dec(crm_op2));
        return true;
    }
    if ((w & 0xfffff0ff) == 0xd503309f || (w & 0xfffff0ff) == 0xd50330bf || (w & 0xfffff0ff) == 0xd50330df) {   // dsb / dmb / isb
        static const char* opt[] = {"#0x0", "oshld", "oshst", "osh", "#0x4", "nshld", "nshst", "nsh", "#0x8", "ishld", "ishst", "ish",
                                    "#0xc", "ld", "st", "sy"};
        const unsigned crm = bits(w, 11, 8), op2 = bits(w, 7, 5);
        if (op2 == 4) set(in, "dsb", opt[crm]);
        else if (op2 == 5) set(in, "dmb", opt[crm]);
        else set(in, "isb", crm == 15 ? "" : dec(crm));
        return true;
    }
    if ((w & 0xffe0001f) == 0xd4000001) { mark_field(in, 20, 5); set(in, "svc", uimm(bits(w, 20, 5))); return true; }
    if ((w & 0xffe0001f) == 0xd4200000) { mark_field(in, 20, 5); set(in, "brk", uimm(bits(w, 20, 5))); return true; }
    if ((w & 0xffe0001f) == 0xd4400000) { mark_field(in, 20, 5); set(in, "hlt", uimm(bits(w, 20, 5))); return true; }
    if ((w & 0xffd00000) == 0xd5100000) {                                  // mrs / msr (register)
        const bool load = bit(w, 21);
        const unsigned o0 = bits(w, 19, 19) + 2, op1 = bits(w, 18, 16), crn = bits(w, 15, 12), crm = bits(w, 11, 8), op2 = bits(w, 7, 5), rt = bits(w, 4, 0);
        const uint32_t key = (o0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
        const char* name = nullptr;
        switch (key) {
            case 0xde82: name = "tpidr_el0"; break;           // 3,3,13,0,2
            case 0xde83: name = "tpidrro_el0"; break;         // 3,3,13,0,3
            case 0xdf02: name = "cntvct_el0"; break;          // 3,3,14,0,2
            case 0xdf01: name = "cntpct_el0"; break;
            case 0xdf00: name = "cntfrq_el0"; break;
            case 0xda20: name = "fpcr"; break;                // 3,3,4,4,0
            case 0xda21: name = "fpsr"; break;
            case 0xda10: name = "nzcv"; break;                // 3,3,4,2,0
            case 0xd801: name = "ctr_el0"; break;             // 3,3,0,0,1
            case 0xd807: name = "dczid_el0"; break;
            default: break;
        }
        char sreg[48];
        if (name) std::snprintf(sreg, sizeof sreg, "%s", name);
        else std::snprintf(sreg, sizeof sreg, "s%u_%u_c%u_c%u_%u", o0, op1, crn, crm, op2);
        if (load) set(in, "mrs", xreg(rt) + ", " + sreg);
        else set(in, "msr", std::string(sreg) + ", " + xreg(rt));
        return true;
    }
    return false;
}

bool dec_dp_imm(uint32_t w, uint64_t pc, Insn& in) {
    if ((w & 0x1f000000) == 0x10000000) {                                  // adr / adrp
        const unsigned rd = bits(w, 4, 0);
        const int64_t v = sext((static_cast<uint64_t>(bits(w, 23, 5)) << 2) | bits(w, 30, 29), 21);
        mark_field(in, 30, 29);
        mark_field(in, 23, 5);
        in.branch_imm = true;
        if (bit(w, 31)) {
            in.target = (pc & ~0xfffull) + static_cast<uint64_t>(v * 4096);
            set(in, "adrp", xreg(rd) + ", " + hx(in.target));
        } else {
            in.target = pc + v;
            set(in, "adr", xreg(rd) + ", " + hx(in.target));
        }
        return true;
    }
    if ((w & 0x1f800000) == 0x11000000) {                                  // add/sub immediate
        const bool is64 = bit(w, 31), sub = bit(w, 30), S = bit(w, 29), sh = bit(w, 22);
        const unsigned imm12 = bits(w, 21, 10), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
        mark_field(in, 21, 10);
        std::string iv = uimm(imm12);
        if (sh) iv += ", lsl #12";
        if (!sub && !S && !sh && imm12 == 0 && (rd == 31 || rn == 31)) { set(in, "mov", reg(is64, rd, true) + ", " + reg(is64, rn, true)); return true; }
        if (S && rd == 31) { set(in, sub ? "cmp" : "cmn", reg(is64, rn, true) + ", " + iv); return true; }
        set(in, sub ? (S ? "subs" : "sub") : (S ? "adds" : "add"), reg(is64, rd, !S) + ", " + reg(is64, rn, true) + ", " + iv);
        return true;
    }
    if ((w & 0x1f800000) == 0x12000000) {                                  // logical immediate
        const bool is64 = bit(w, 31);
        const unsigned opc = bits(w, 30, 29), N = bits(w, 22, 22), immr = bits(w, 21, 16), imms = bits(w, 15, 10), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
        uint64_t mask = 0;
        if (!decode_bit_mask(N, imms, immr, is64, &mask)) return false;
        mark_field(in, 22, 10);
        static const char* mn[] = {"and", "orr", "eor", "ands"};
        if (opc == 1 && rn == 31) {
            // LLVM prints ORR-with-zr as "mov" unless a MOVZ / MOVN could make the value (then it keeps "orr")
            bool movable = false;
            const uint64_t v = mask, nv = is64 ? ~mask : (~mask & 0xffffffffull);
            for (int s = 0; s < (is64 ? 64 : 32); s += 16)
                if ((v & ~(0xffffull << s)) == 0 || (nv & ~(0xffffull << s)) == 0) movable = true;
            if (!movable) { set(in, "mov", reg(is64, rd, true) + ", #" + hx(mask)); return true; }
        }
        if (opc == 3 && rd == 31) { set(in, "tst", reg(is64, rn) + ", #" + hx(mask)); return true; }
        set(in, mn[opc], reg(is64, rd, opc != 3) + ", " + reg(is64, rn) + ", #" + hx(mask));
        return true;
    }
    if ((w & 0x1f800000) == 0x12800000) {                                  // move wide
        const bool is64 = bit(w, 31);
        const unsigned opc = bits(w, 30, 29), hw = bits(w, 22, 21), imm16 = bits(w, 20, 5), rd = bits(w, 4, 0);
        if (opc == 1 || (!is64 && hw >= 2)) return false;
        mark_field(in, 20, 5);
        const unsigned shift = hw * 16;
        std::string plain = reg(is64, rd) + ", " + uimm(imm16) + (shift ? ", lsl " + dec(shift) : "");
        if (opc == 3) { set(in, "movk", plain); return true; }
        if (opc == 2) {
            if (!(imm16 == 0 && hw != 0)) {
                const uint64_t v = static_cast<uint64_t>(imm16) << shift;
                // a w register's value prints signed, as LLVM does ("mov w11, #-0x3e000000")
                if (!is64 && (v & 0x80000000ull)) set(in, "mov", reg(is64, rd) + ", #-" + hx(0x100000000ull - v));
                else set(in, "mov", reg(is64, rd) + ", #" + hx(v));
                return true;
            }
            set(in, "movz", plain);
            return true;
        }
        // movn: mov #~value unless it is the 0 / all-ones case LLVM keeps as movn
        uint64_t v = ~(static_cast<uint64_t>(imm16) << shift);
        if (!is64) v &= 0xffffffffull;
        if (!(imm16 == 0 && hw != 0) && !(!is64 && imm16 == 0xffff)) {
            const int64_t sv = is64 ? static_cast<int64_t>(v) : static_cast<int64_t>(static_cast<int32_t>(v));
            set(in, "mov", reg(is64, rd) + ", " + (sv < 0 ? "#-" + hx(static_cast<uint64_t>(-sv)) : "#" + hx(static_cast<uint64_t>(sv))));
            return true;
        }
        set(in, "movn", plain);
        return true;
    }
    if ((w & 0x1f800000) == 0x13000000) {                                  // bitfield
        const bool is64 = bit(w, 31);
        const unsigned opc = bits(w, 30, 29), N = bits(w, 22, 22), immr = bits(w, 21, 16), imms = bits(w, 15, 10), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
        if (opc == 3 || N != static_cast<unsigned>(is64)) return false;
        const unsigned size = is64 ? 64 : 32;
        if (!is64 && (immr >= 32 || imms >= 32)) return false;
        const std::string d = reg(is64, rd), n = reg(is64, rn);
        if (opc == 2) {                                                     // ubfm
            if (imms == size - 1) { set(in, "lsr", d + ", " + n + ", " + dec(immr)); return true; }
            if (imms + 1 == immr) { set(in, "lsl", d + ", " + n + ", " + dec(size - 1 - imms)); return true; }
            if (immr == 0 && !is64 && imms == 7) { set(in, "uxtb", d + ", " + wreg(rn)); return true; }
            if (immr == 0 && !is64 && imms == 15) { set(in, "uxth", d + ", " + wreg(rn)); return true; }
            if (imms < immr) { set(in, "ubfiz", d + ", " + n + ", " + dec((size - immr) % size) + ", " + dec(imms + 1)); return true; }
            set(in, "ubfx", d + ", " + n + ", " + dec(immr) + ", " + dec(imms - immr + 1));
            return true;
        }
        if (opc == 0) {                                                     // sbfm
            if (imms == size - 1) { set(in, "asr", d + ", " + n + ", " + dec(immr)); return true; }
            if (immr == 0 && imms == 7) { set(in, "sxtb", d + ", " + wreg(rn)); return true; }
            if (immr == 0 && imms == 15) { set(in, "sxth", d + ", " + wreg(rn)); return true; }
            if (immr == 0 && imms == 31 && is64) { set(in, "sxtw", d + ", " + wreg(rn)); return true; }
            if (imms < immr) { set(in, "sbfiz", d + ", " + n + ", " + dec((size - immr) % size) + ", " + dec(imms + 1)); return true; }
            set(in, "sbfx", d + ", " + n + ", " + dec(immr) + ", " + dec(imms - immr + 1));
            return true;
        }
        // bfm
        if (imms < immr) {
            if (rn == 31) { set(in, "bfc", d + ", " + dec((size - immr) % size) + ", " + dec(imms + 1)); return true; }
            set(in, "bfi", d + ", " + n + ", " + dec((size - immr) % size) + ", " + dec(imms + 1));
            return true;
        }
        set(in, "bfxil", d + ", " + n + ", " + dec(immr) + ", " + dec(imms - immr + 1));
        return true;
    }
    if ((w & 0x1fa00000) == 0x13800000) {                                  // extr / ror
        const bool is64 = bit(w, 31);
        if (bit(w, 22) != is64) return false;
        const unsigned rm = bits(w, 20, 16), imms = bits(w, 15, 10), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
        if (!is64 && imms >= 32) return false;
        if (rn == rm) set(in, "ror", reg(is64, rd) + ", " + reg(is64, rn) + ", " + dec(imms));
        else set(in, "extr", reg(is64, rd) + ", " + reg(is64, rn) + ", " + reg(is64, rm) + ", " + dec(imms));
        return true;
    }
    return false;
}

bool dec_dp_reg(uint32_t w, Insn& in) {
    const bool is64 = bit(w, 31);
    const unsigned rm = bits(w, 20, 16), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if ((w & 0x1f000000) == 0x0a000000) {                                  // logical shifted register
        const unsigned opc = bits(w, 30, 29), nb = bits(w, 21, 21), shift = bits(w, 23, 22), amt = bits(w, 15, 10);
        if (!is64 && amt >= 32) return false;
        static const char* mn[2][4] = {{"and", "orr", "eor", "ands"}, {"bic", "orn", "eon", "bics"}};
        const std::string rhs = reg(is64, rm) + shift_suffix(shift, amt);
        if (opc == 1 && nb == 0 && rn == 31 && amt == 0) { set(in, "mov", reg(is64, rd) + ", " + reg(is64, rm)); return true; }
        if (opc == 1 && nb == 1 && rn == 31) { set(in, "mvn", reg(is64, rd) + ", " + rhs); return true; }
        if (opc == 3 && nb == 0 && rd == 31) { set(in, "tst", reg(is64, rn) + ", " + rhs); return true; }
        set(in, mn[nb][opc], reg(is64, rd) + ", " + reg(is64, rn) + ", " + rhs);
        return true;
    }
    if ((w & 0x1f200000) == 0x0b000000) {                                  // add/sub shifted register
        const bool sub = bit(w, 30), S = bit(w, 29);
        const unsigned shift = bits(w, 23, 22), amt = bits(w, 15, 10);
        if (shift == 3 || (!is64 && amt >= 32)) return false;
        const std::string rhs = reg(is64, rm) + shift_suffix(shift, amt);
        if (S && rd == 31) { set(in, sub ? "cmp" : "cmn", reg(is64, rn) + ", " + rhs); return true; }
        if (sub && rn == 31) { set(in, S ? "negs" : "neg", reg(is64, rd) + ", " + rhs); return true; }
        set(in, sub ? (S ? "subs" : "sub") : (S ? "adds" : "add"), reg(is64, rd) + ", " + reg(is64, rn) + ", " + rhs);
        return true;
    }
    if ((w & 0x1fe00000) == 0x0b200000) {                                  // add/sub extended register
        const bool sub = bit(w, 30), S = bit(w, 29);
        const unsigned opt = bits(w, 15, 13), amt = bits(w, 12, 10);
        if (amt > 4) return false;
        static const char* ex[] = {"uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};
        const bool m64 = (opt & 3) == 3;
        std::string e = ex[opt];
        // LSL is the preferred name of UXTX (64-bit) / UXTW (32-bit) when Rd or Rn is the stack pointer
        const bool spform = rd == 31 || rn == 31;
        if (spform && ((is64 && opt == 3) || (!is64 && opt == 2))) e = amt ? "lsl" : "";
        std::string rhs = reg(is64 && m64, rm);
        if (!e.empty()) rhs += ", " + e + (amt ? " " + dec(amt) : "");
        if (S && rd == 31) { set(in, sub ? "cmp" : "cmn", reg(is64, rn, true) + ", " + rhs); return true; }
        set(in, sub ? (S ? "subs" : "sub") : (S ? "adds" : "add"), reg(is64, rd, !S) + ", " + reg(is64, rn, true) + ", " + rhs);
        return true;
    }
    if ((w & 0x1fe00000) == 0x1a000000) {                                  // adc / sbc
        const bool sub = bit(w, 30), S = bit(w, 29);
        if (bits(w, 15, 10)) return false;
        if (sub && rn == 31) { set(in, S ? "ngcs" : "ngc", reg(is64, rd) + ", " + reg(is64, rm)); return true; }
        set(in, sub ? (S ? "sbcs" : "sbc") : (S ? "adcs" : "adc"), reg(is64, rd) + ", " + reg(is64, rn) + ", " + reg(is64, rm));
        return true;
    }
    if ((w & 0x1fe00000) == 0x1a400000 && !bit(w, 10) && !bit(w, 4) && bit(w, 29)) {   // ccmp / ccmn
        const bool sub = bit(w, 30);
        const unsigned cond = bits(w, 15, 12), nzcv = bits(w, 3, 0);
        const std::string rhs = bit(w, 11) ? dec(rm) : reg(is64, rm);
        set(in, sub ? "ccmp" : "ccmn", reg(is64, rn) + ", " + rhs + ", " + dec(nzcv) + ", " + kConds[cond]);
        return true;
    }
    if ((w & 0x1fe00000) == 0x1a800000 && !bit(w, 29) && bits(w, 11, 11) == 0) {   // conditional select
        const unsigned op = bits(w, 30, 30), o2 = bits(w, 10, 10), cond = bits(w, 15, 12);
        const std::string d = reg(is64, rd);
        const bool inv_ok = (cond & 0xe) != 0xe;
        if (op == 0 && o2 == 0) { set(in, "csel", d + ", " + reg(is64, rn) + ", " + reg(is64, rm) + ", " + kConds[cond]); return true; }
        if (op == 0 && o2 == 1) {
            if (rn == 31 && rm == 31 && inv_ok) { set(in, "cset", d + ", " + kConds[cond ^ 1]); return true; }
            if (rn == rm && rn != 31 && inv_ok) { set(in, "cinc", d + ", " + reg(is64, rn) + ", " + kConds[cond ^ 1]); return true; }
            set(in, "csinc", d + ", " + reg(is64, rn) + ", " + reg(is64, rm) + ", " + kConds[cond]);
            return true;
        }
        if (op == 1 && o2 == 0) {
            if (rn == 31 && rm == 31 && inv_ok) { set(in, "csetm", d + ", " + kConds[cond ^ 1]); return true; }
            if (rn == rm && rn != 31 && inv_ok) { set(in, "cinv", d + ", " + reg(is64, rn) + ", " + kConds[cond ^ 1]); return true; }
            set(in, "csinv", d + ", " + reg(is64, rn) + ", " + reg(is64, rm) + ", " + kConds[cond]);
            return true;
        }
        if (rn == rm && inv_ok) { set(in, "cneg", d + ", " + reg(is64, rn) + ", " + kConds[cond ^ 1]); return true; }
        set(in, "csneg", d + ", " + reg(is64, rn) + ", " + reg(is64, rm) + ", " + kConds[cond]);
        return true;
    }
    if ((w & 0x5fe00000) == 0x1ac00000) {                                  // data processing (2 source)
        const unsigned op = bits(w, 15, 10);
        const char* mn = nullptr;
        switch (op) {
            case 2: mn = "udiv"; break;
            case 3: mn = "sdiv"; break;
            case 8: mn = "lsl"; break;
            case 9: mn = "lsr"; break;
            case 10: mn = "asr"; break;
            case 11: mn = "ror"; break;
            default: return false;
        }
        set(in, mn, reg(is64, rd) + ", " + reg(is64, rn) + ", " + reg(is64, rm));
        return true;
    }
    if ((w & 0x5fe00000) == 0x5ac00000 && rm == 0) {                       // data processing (1 source)
        const unsigned op = bits(w, 15, 10);
        const char* mn = nullptr;
        switch (op) {
            case 0: mn = "rbit"; break;
            case 1: mn = "rev16"; break;
            case 2: mn = is64 ? "rev32" : "rev"; break;
            case 3: if (!is64) return false; mn = "rev"; break;
            case 4: mn = "clz"; break;
            case 5: mn = "cls"; break;
            default: return false;
        }
        set(in, mn, reg(is64, rd) + ", " + reg(is64, rn));
        return true;
    }
    if ((w & 0x1f000000) == 0x1b000000) {                                  // data processing (3 source)
        const unsigned op31 = bits(w, 23, 21), o0 = bits(w, 15, 15), ra = bits(w, 14, 10);
        if (bits(w, 30, 29)) return false;
        if (op31 == 0) {
            if (ra == 31) { set(in, o0 ? "mneg" : "mul", reg(is64, rd) + ", " + reg(is64, rn) + ", " + reg(is64, rm)); return true; }
            set(in, o0 ? "msub" : "madd", reg(is64, rd) + ", " + reg(is64, rn) + ", " + reg(is64, rm) + ", " + reg(is64, ra));
            return true;
        }
        if (!is64) return false;
        if (op31 == 1 || op31 == 5) {
            const bool u = op31 == 5;
            if (ra == 31) { set(in, std::string(u ? "u" : "s") + (o0 ? "mnegl" : "mull"), xreg(rd) + ", " + wreg(rn) + ", " + wreg(rm)); return true; }
            set(in, std::string(u ? "u" : "s") + (o0 ? "msubl" : "maddl"), xreg(rd) + ", " + wreg(rn) + ", " + wreg(rm) + ", " + xreg(ra));
            return true;
        }
        if ((op31 == 2 || op31 == 6) && o0 == 0) { set(in, op31 == 2 ? "smulh" : "umulh", xreg(rd) + ", " + xreg(rn) + ", " + xreg(rm)); return true; }
        return false;
    }
    return false;
}

// integer and SIMD&FP loads and stores
bool dec_ldst(uint32_t w, uint64_t pc, Insn& in) {
    const unsigned size = bits(w, 31, 30), rn = bits(w, 9, 5), rt = bits(w, 4, 0);
    const bool V = bit(w, 26);
    // the register a size/opc pair names, and the mnemonic stem. Returns false for an unallocated pair.
    auto int_name = [&](unsigned opc, std::string* mn, std::string* r, unsigned* scale) -> bool {
        *scale = size;
        if (opc == 0) { *mn = size == 0 ? "strb" : size == 1 ? "strh" : "str"; *r = reg(size == 3, rt); return true; }
        if (opc == 1) { *mn = size == 0 ? "ldrb" : size == 1 ? "ldrh" : "ldr"; *r = reg(size == 3, rt); return true; }
        if (size == 3) { if (opc == 2) { *mn = "prfm"; *r = prf_name(rt); return true; } return false; }
        if (size == 2) { if (opc == 2) { *mn = "ldrsw"; *r = xreg(rt); return true; } return false; }
        *mn = size == 0 ? "ldrsb" : "ldrsh";
        *r = reg(opc == 2, rt);
        return true;
    };
    auto fp_name = [&](unsigned opc, std::string* mn, std::string* r, unsigned* scale) -> bool {
        const bool load = opc & 1;
        unsigned sz = size;
        if (opc >= 2) { if (size != 0) return false; sz = 4; }
        *mn = load ? "ldr" : "str";
        *r = vreg(sz, rt);
        *scale = sz;
        return true;
    };
    // unsigned immediate offset
    if ((w & 0x3b000000) == 0x39000000) {
        const unsigned opc = bits(w, 23, 22), imm12 = bits(w, 21, 10);
        std::string mn, r;
        unsigned scale;
        if (!(V ? fp_name(opc, &mn, &r, &scale) : int_name(opc, &mn, &r, &scale))) return false;
        mark_field(in, 21, 10);
        set(in, mn, r + ", " + mem_off(rn, static_cast<int64_t>(imm12) << scale));
        return true;
    }
    // unscaled / pre / post index (bit 21 = 0), register offset (bit 21 = 1, bits 11:10 = 10)
    if ((w & 0x3b200000) == 0x38000000) {
        const unsigned opc = bits(w, 23, 22), kind = bits(w, 11, 10);
        const int64_t imm9 = sext(bits(w, 20, 12), 9);
        std::string mn, r;
        unsigned scale;
        if (!(V ? fp_name(opc, &mn, &r, &scale) : int_name(opc, &mn, &r, &scale))) return false;
        mark_field(in, 20, 12);
        if (kind == 0) {
            std::string un = mn == "prfm" ? "prfum" : (mn[0] == 'l' ? "ldur" : "stur") + mn.substr(3);
            set(in, un, r + ", " + mem_off(rn, imm9));
            return true;
        }
        if (kind == 2) return false;          // unprivileged (ldtr): not modelled
        if (mn == "prfm") return false;
        if (kind == 1) set(in, mn, r + ", [" + xreg(rn, true) + "], " + imm(imm9));
        else set(in, mn, r + ", [" + xreg(rn, true) + ", " + imm(imm9) + "]!");
        return true;
    }
    if ((w & 0x3b200c00) == 0x38200800) {
        const unsigned opc = bits(w, 23, 22), rm = bits(w, 20, 16), opt = bits(w, 15, 13), S = bits(w, 12, 12);
        if (!(opt & 2)) return false;
        std::string mn, r;
        unsigned scale;
        if (!(V ? fp_name(opc, &mn, &r, &scale) : int_name(opc, &mn, &r, &scale))) return false;
        std::string idx = ((opt & 1) ? xreg(rm) : wreg(rm));
        static const char* ex[] = {"", "", "uxtw", "lsl", "", "", "sxtw", "sxtx"};
        std::string e = ex[opt];
        if (S) e += " " + dec(scale);
        else if (opt == 3) e.clear();
        set(in, mn, r + ", [" + xreg(rn, true) + ", " + idx + (e.empty() ? "" : ", " + e) + "]");
        return true;
    }
    // load register (literal)
    if ((w & 0x3b000000) == 0x18000000) {
        const unsigned opc = bits(w, 31, 30);
        const int64_t off = sext(bits(w, 23, 5), 19) * 4;
        mark_field(in, 23, 5);
        in.branch_imm = true;
        in.target = pc + off;
        if (V) {
            if (opc == 3) return false;
            set(in, "ldr", vreg(opc + 2, rt) + ", " + hx(in.target));
            return true;
        }
        if (opc == 3) { set(in, "prfm", prf_name(rt) + ", " + hx(in.target)); return true; }
        set(in, opc == 2 ? "ldrsw" : "ldr", reg(opc != 0, rt) + ", " + hx(in.target));
        return true;
    }
    // pairs
    if ((w & 0x3a000000) == 0x28000000) {
        const unsigned opc = bits(w, 31, 30), L = bits(w, 22, 22), idx = bits(w, 24, 23), imm7 = bits(w, 21, 15), rt2 = bits(w, 14, 10);
        if (idx == 0) return false;            // ldnp / stnp: not modelled
        std::string a, b, mn = L ? "ldp" : "stp";
        unsigned scale;
        if (V) {
            if (opc == 3) return false;
            scale = opc + 2;
            a = vreg(scale, rt);
            b = vreg(scale, rt2);
        } else {
            if (opc == 3 || (opc == 1 && !L)) return false;
            if (opc == 1) mn = "ldpsw";
            scale = opc == 2 ? 3 : 2;
            a = reg(opc == 2 || opc == 1, rt);
            b = reg(opc == 2 || opc == 1, rt2);
            if (opc == 1) { a = xreg(rt); b = xreg(rt2); }
        }
        const int64_t off = sext(imm7, 7) * (1ll << scale);
        mark_field(in, 21, 15);
        std::string ops = a + ", " + b + ", ";
        if (idx == 1) ops += "[" + xreg(rn, true) + "], " + imm(off);
        else if (idx == 3) ops += "[" + xreg(rn, true) + ", " + imm(off) + "]!";
        else ops += mem_off(rn, off);
        set(in, mn, ops);
        return true;
    }
    // exclusive / ordered
    if ((w & 0x3f000000) == 0x08000000 && !V) {
        const unsigned o2 = bits(w, 23, 23), L = bits(w, 22, 22), o1 = bits(w, 21, 21), rs = bits(w, 20, 16), o0 = bits(w, 15, 15), rt2 = bits(w, 14, 10);
        const bool is64 = size == 3;
        const char* sfx = size == 0 ? "b" : size == 1 ? "h" : "";
        const std::string t = size >= 2 ? reg(is64, rt) : wreg(rt);
        const std::string mem = "[" + xreg(rn, true) + "]";
        if (o2 == 0 && o1 == 0) {
            if (L) {
                if (rs != 31 || rt2 != 31) return false;
                set(in, std::string(o0 ? "ldaxr" : "ldxr") + sfx, t + ", " + mem);
            } else {
                if (rt2 != 31) return false;
                set(in, std::string(o0 ? "stlxr" : "stxr") + sfx, wreg(rs) + ", " + t + ", " + mem);
            }
            return true;
        }
        if (o2 == 0 && o1 == 1 && size >= 2) {
            const std::string t2 = reg(is64, rt2);
            if (L) {
                if (rs != 31) return false;
                set(in, o0 ? "ldaxp" : "ldxp", t + ", " + t2 + ", " + mem);
            } else {
                set(in, o0 ? "stlxp" : "stxp", wreg(rs) + ", " + t + ", " + t2 + ", " + mem);
            }
            return true;
        }
        if (o2 == 1 && o1 == 0 && o0 == 1 && rs == 31 && rt2 == 31) {
            set(in, std::string(L ? "ldar" : "stlr") + sfx, t + ", " + mem);
            return true;
        }
        if (o2 == 1 && o1 == 1 && rt2 == 31) {               // cas / casa / casl / casal
            const std::string s = size >= 2 ? reg(is64, rs) : wreg(rs);
            set(in, std::string("cas") + (L ? "a" : "") + (o0 ? "l" : "") + sfx, s + ", " + t + ", " + mem);
            return true;
        }
        return false;
    }
    // LSE atomic memory operations (ldadd ... swp)
    if ((w & 0x3f200c00) == 0x38200000 && !V) {
        const unsigned A = bits(w, 23, 23), R = bits(w, 22, 22), rs = bits(w, 20, 16), o3 = bits(w, 15, 15), opc = bits(w, 14, 12);
        const bool is64 = size == 3;
        const char* sfx = size == 0 ? "b" : size == 1 ? "h" : "";
        static const char* ops[] = {"ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin"};
        std::string base;
        if (o3) { if (opc != 0) return false; base = "swp"; }
        else base = ops[opc];
        const std::string ord = std::string(A ? "a" : "") + (R ? "l" : "");
        const std::string s = size >= 2 ? reg(is64, rs) : wreg(rs);
        const std::string t = size >= 2 ? reg(is64, rt) : wreg(rt);
        if (!o3 && rt == 31 && !A) {                         // st<op> alias (result discarded)
            set(in, "st" + base.substr(2) + (R ? "l" : "") + sfx, s + ", [" + xreg(rn, true) + "]");
            return true;
        }
        set(in, base + ord + sfx, s + ", " + t + ", [" + xreg(rn, true) + "]");
        return true;
    }
    return false;
}

// scalar floating point
bool dec_fp(uint32_t w, Insn& in) {
    if ((w & 0x5f200000) != 0x1e200000 && (w & 0xff000000) != 0x1f000000) return false;
    const unsigned type = bits(w, 23, 22), rd = bits(w, 4, 0), rn = bits(w, 9, 5), rm = bits(w, 20, 16);
    const unsigned fsz = type == 0 ? 2 : type == 1 ? 3 : type == 3 ? 1 : 99;      // s, d, h
    if ((w & 0xff000000) == 0x1f000000) {                                  // fmadd / fmsub / fnmadd / fnmsub
        if (fsz == 99) return false;
        const unsigned o1 = bits(w, 21, 21), o0 = bits(w, 15, 15), ra = bits(w, 14, 10);
        static const char* mn[2][2] = {{"fmadd", "fmsub"}, {"fnmadd", "fnmsub"}};
        set(in, mn[o1][o0], vreg(fsz, rd) + ", " + vreg(fsz, rn) + ", " + vreg(fsz, rm) + ", " + vreg(fsz, ra));
        return true;
    }
    if (bit(w, 31) && (bits(w, 15, 10) != 0)) return false;               // only the int<->fp group sets sf
    if (bit(w, 29)) return false;
    if (bits(w, 15, 10) == 0 && (w & 0x5f20fc00) == 0x1e200000) {          // fp <-> integer
        const bool sf = bit(w, 31);
        const unsigned rmode = bits(w, 20, 19), op = bits(w, 18, 16);
        if (fsz == 99) {
            if (type == 2 && rmode == 1 && (op == 6 || op == 7) && sf) {    // fmov x, v.d[1] / v.d[1], x
                if (op == 6) set(in, "fmov", xreg(rd) + ", v" + std::to_string(rn) + ".d[1]");
                else set(in, "fmov", "v" + std::to_string(rd) + ".d[1], " + xreg(rn));
                return true;
            }
            return false;
        }
        const std::string f = vreg(fsz, 0);
        if (op == 6 || op == 7) {                                           // fmov general
            if (rmode != 0) return false;
            if ((fsz == 2 && sf) || (fsz == 3 && !sf)) return false;
            if (op == 6) set(in, "fmov", reg(sf, rd) + ", " + vreg(fsz, rn));
            else set(in, "fmov", vreg(fsz, rd) + ", " + reg(sf, rn));
            return true;
        }
        static const char* cvt[4][2] = {{"fcvtns", "fcvtnu"}, {"fcvtps", "fcvtpu"}, {"fcvtms", "fcvtmu"}, {"fcvtzs", "fcvtzu"}};
        if (op <= 1) { set(in, cvt[rmode][op], reg(sf, rd) + ", " + vreg(fsz, rn)); return true; }
        if (op == 2 || op == 3) {
            if (rmode != 0) return false;
            set(in, op == 2 ? "scvtf" : "ucvtf", vreg(fsz, rd) + ", " + reg(sf, rn));
            return true;
        }
        if (op == 4 || op == 5) {
            if (rmode != 0) return false;
            set(in, op == 4 ? "fcvtas" : "fcvtau", reg(sf, rd) + ", " + vreg(fsz, rn));
            return true;
        }
        (void)f;
        return false;
    }
    if (fsz == 99) return false;
    if (bits(w, 14, 10) == 0x10 && bits(w, 21, 21) == 1) {                 // 1 source (bits 14:10 = 10000)
        const unsigned op = bits(w, 20, 15);
        if (op == 4 || op == 5 || op == 7) {                                // fcvt
            const unsigned to = op == 4 ? 2 : op == 5 ? 3 : 1;
            if (to == fsz) return false;
            set(in, "fcvt", vreg(to, rd) + ", " + vreg(fsz, rn));
            return true;
        }
        static const char* mn[] = {"fmov", "fabs", "fneg", "fsqrt", nullptr, nullptr, nullptr, nullptr,
                                   "frintn", "frintp", "frintm", "frintz", "frinta", nullptr, "frintx", "frinti"};
        if (op >= 16 || !mn[op]) return false;
        set(in, mn[op], vreg(fsz, rd) + ", " + vreg(fsz, rn));
        return true;
    }
    if (bits(w, 13, 10) == 0x8 && bits(w, 21, 21) == 1 && bits(w, 15, 14) == 0) {   // fcmp / fcmpe
        const unsigned opc = bits(w, 4, 0);
        const bool zero = opc & 8, e = opc & 16;
        if ((opc & 7) != 0) return false;
        set(in, e ? "fcmpe" : "fcmp", vreg(fsz, rn) + ", " + (zero ? std::string("#0.0") : vreg(fsz, rm)));
        return true;
    }
    if (bits(w, 12, 10) == 0x4 && bits(w, 21, 21) == 1 && bits(w, 9, 5) == 0) {     // fmov immediate
        const unsigned imm8 = bits(w, 20, 13);
        mark_field(in, 20, 13);
        set(in, "fmov", vreg(fsz, rd) + ", " + fp_text(fp_imm8(imm8)));
        return true;
    }
    if (bits(w, 11, 10) == 1 && bits(w, 21, 21) == 1) {                     // fccmp / fccmpe
        const unsigned cond = bits(w, 15, 12), nzcv = bits(w, 3, 0);
        set(in, bit(w, 4) ? "fccmpe" : "fccmp", vreg(fsz, rn) + ", " + vreg(fsz, rm) + ", " + dec(nzcv) + ", " + kConds[cond]);
        return true;
    }
    if (bits(w, 11, 10) == 2 && bits(w, 21, 21) == 1) {                     // 2 source
        const unsigned op = bits(w, 15, 12);
        static const char* mn[] = {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"};
        if (op > 8) return false;
        set(in, mn[op], vreg(fsz, rd) + ", " + vreg(fsz, rn) + ", " + vreg(fsz, rm));
        return true;
    }
    if (bits(w, 11, 10) == 3 && bits(w, 21, 21) == 1) {                     // fcsel
        set(in, "fcsel", vreg(fsz, rd) + ", " + vreg(fsz, rn) + ", " + vreg(fsz, rm) + ", " + kConds[bits(w, 15, 12)]);
        return true;
    }
    return false;
}

}  // namespace

Insn decode(uint32_t w, uint64_t pc) {
    Insn in;
    in.raw = w;
    in.pc = pc;
    bool ok = false;
    const unsigned op0 = bits(w, 28, 25);
    if ((op0 & 0xe) == 0xa) ok = dec_branch(w, pc, in) || dec_system(w, in);          // 101x: branches, exceptions, system
    else if ((op0 & 0xe) == 0x8) ok = dec_dp_imm(w, pc, in);                           // 100x: data processing, immediate
    else if ((op0 & 0x7) == 0x5) ok = dec_dp_reg(w, in);                               // x101: data processing, register
    else if ((op0 & 0x5) == 0x4) ok = dec_ldst(w, pc, in);                             // x1x0: loads and stores
    else if ((op0 & 0x7) == 0x7) ok = dec_fp(w, in);                                   // x111: SIMD and FP (scalar FP modelled)
    if (!ok) {
        in = Insn();
        in.raw = w;
        in.pc = pc;
        in.mnemonic = ".inst";
        in.text = ".inst\t" + hx(w);
        in.modelled = false;
    }
    return in;
}

std::vector<Insn> decode_range(const uint8_t* code, size_t n, uint64_t pc) {
    std::vector<Insn> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t w;
        std::memcpy(&w, code + i * 4, 4);
        out.push_back(decode(w, pc + i * 4));
    }
    return out;
}

std::vector<int> signature_wildcards(uint32_t word) { return decode(word, 0).imm_bytes; }

// ================================================================== assembler
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parse_int(std::string s, int64_t* v) {
    s = trim(s);
    if (!s.empty() && s[0] == '#') s = trim(s.substr(1));
    if (s.empty()) return false;
    bool neg = false;
    if (s[0] == '-') { neg = true; s = s.substr(1); }
    else if (s[0] == '+') s = s.substr(1);
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long long r = std::strtoull(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0') return false;
    *v = neg ? -static_cast<int64_t>(r) : static_cast<int64_t>(r);
    return true;
}
bool parse_u(const std::string& s, uint64_t* v) {
    int64_t x;
    if (!parse_int(s, &x)) return false;
    *v = static_cast<uint64_t>(x);
    return true;
}

// A general register: kind 'x' / 'w', number 0..31 (31 = zr or sp, `sp` says which)
struct R { bool is64 = false; unsigned n = 0; bool sp = false; bool zr = false; };
bool parse_r(const std::string& t0, R* r) {
    const std::string t = lower(trim(t0));
    if (t == "sp") { *r = {true, 31, true, false}; return true; }
    if (t == "wsp") { *r = {false, 31, true, false}; return true; }
    if (t == "xzr") { *r = {true, 31, false, true}; return true; }
    if (t == "wzr") { *r = {false, 31, false, true}; return true; }
    if (t == "lr") { *r = {true, 30, false, false}; return true; }
    if (t == "fp") { *r = {true, 29, false, false}; return true; }
    if (t.size() < 2 || (t[0] != 'x' && t[0] != 'w')) return false;
    for (size_t i = 1; i < t.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
    const unsigned n = static_cast<unsigned>(std::atoi(t.c_str() + 1));
    if (n > 30) return false;
    *r = {t[0] == 'x', n, false, false};
    return true;
}
// A scalar FP / SIMD register: size 0 b .. 4 q
bool parse_v(const std::string& t0, unsigned* sz, unsigned* n) {
    const std::string t = lower(trim(t0));
    if (t.size() < 2) return false;
    const char* p = "bhsdq";
    const char* f = std::strchr(p, t[0]);
    if (!f || !*f) return false;
    for (size_t i = 1; i < t.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
    const unsigned v = static_cast<unsigned>(std::atoi(t.c_str() + 1));
    if (v > 31) return false;
    *sz = static_cast<unsigned>(f - p);
    *n = v;
    return true;
}
int parse_cond(const std::string& t0) {
    const std::string t = lower(trim(t0));
    for (int i = 0; i < 16; ++i) if (t == kConds[i]) return i;
    if (t == "cs") return 2;
    if (t == "cc") return 3;
    return -1;
}

// Splits operands on commas outside brackets; a trailing "!" stays with its bracket.
std::vector<std::string> split_ops(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '[') ++depth;
        if (c == ']') --depth;
        if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

// "lsl #3" / "uxtw #2" / "sxtw" -> kind, amount
bool parse_shift(const std::string& t0, std::string* kind, unsigned* amt) {
    const std::string t = lower(trim(t0));
    const size_t sp = t.find_first_of(" #");
    *kind = sp == std::string::npos ? t : t.substr(0, sp);
    *amt = 0;
    if (sp != std::string::npos) {
        int64_t v;
        if (!parse_int(t.substr(sp), &v) || v < 0) return false;
        *amt = static_cast<unsigned>(v);
    }
    static const char* ok[] = {"lsl", "lsr", "asr", "ror", "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};
    for (const char* k : ok) if (*kind == k) return true;
    return false;
}

// Encodes a logical immediate (the inverse of decode_bit_mask). False when the value has no such encoding.
bool encode_bit_mask(uint64_t v, bool is64, unsigned* N, unsigned* immr, unsigned* imms) {
    if (!is64) { v &= 0xffffffffull; v |= v << 32; }
    if (v == 0 || v == ~0ull) return false;
    unsigned size = 64;
    for (;;) {
        const unsigned half = size / 2;
        if (half < 2) break;
        const uint64_t m = (1ull << half) - 1;
        if ((v & m) != ((v >> half) & m)) break;
        size = half;
    }
    const uint64_t emask = size == 64 ? ~0ull : ((1ull << size) - 1);
    const uint64_t e = v & emask;
    const unsigned ones = static_cast<unsigned>(__builtin_popcountll(e));
    const uint64_t run = ones >= 64 ? ~0ull : ((1ull << ones) - 1);
    for (unsigned r = 0; r < size; ++r) {
        const uint64_t rot = r == 0 ? e : (((e >> r) | (e << (size - r))) & emask);
        if (rot == run) {
            *immr = (size - r) % size;
            *imms = ((~(size - 1) << 1) & 0x3f) | (ones - 1);
            *N = size == 64 ? 1 : 0;
            if (!is64 && *N) return false;
            return true;
        }
    }
    return false;
}

// The 8-bit FMOV immediate for a value, if it has one.
bool encode_fp8(double v, unsigned* imm8) {
    for (unsigned i = 0; i < 256; ++i)
        if (fp_imm8(i) == v) { *imm8 = i; return true; }
    return false;
}

struct Mem {
    unsigned rn = 0;
    int kind = 0;               // 0 [rn, #imm]  1 [rn, #imm]!  2 [rn], #imm  3 [rn, rm{, ext #s}]
    int64_t off = 0;
    R rm;
    std::string ext;
    unsigned amt = 0;
    bool has_amt = false;
};
// The memory operand(s) starting at ops[i] (a post-index takes the next operand too).
bool parse_mem(const std::vector<std::string>& ops, size_t i, Mem* m, std::string* err) {
    if (i >= ops.size()) { *err = "missing [address]"; return false; }
    std::string t = trim(ops[i]);
    if (t.empty() || t[0] != '[') { *err = "expected [base, ...]"; return false; }
    bool pre = false;
    if (t.back() == '!') { pre = true; t = trim(t.substr(0, t.size() - 1)); }
    if (t.back() != ']') { *err = "missing ]"; return false; }
    const auto in = split_ops(t.substr(1, t.size() - 2));
    if (in.empty()) { *err = "empty []"; return false; }
    R base;
    if (!parse_r(in[0], &base) || !base.is64 || base.zr) { *err = "the base must be an x register or sp"; return false; }
    m->rn = base.n;
    if (in.size() == 1) {
        m->kind = 0;
        if (i + 1 < ops.size()) {
            m->kind = 2;
            if (!parse_int(ops[i + 1], &m->off)) { *err = "bad post-index offset"; return false; }
        }
        if (pre) { *err = "! needs an offset"; return false; }
        return true;
    }
    int64_t v;
    if (parse_int(in[1], &v)) {
        if (in.size() != 2) { *err = "too many address parts"; return false; }
        m->off = v;
        m->kind = pre ? 1 : 0;
        return true;
    }
    if (!parse_r(in[1], &m->rm) || m->rm.sp) { *err = "bad index register"; return false; }
    m->kind = 3;
    m->ext = m->rm.is64 ? "lsl" : "uxtw";
    if (in.size() == 3) {
        if (!parse_shift(in[2], &m->ext, &m->amt)) { *err = "bad index extend"; return false; }
        m->has_amt = lower(in[2]).find('#') != std::string::npos;
    }
    return !pre;
}

}  // namespace

uint32_t nop_word() { return 0xd503201f; }

bool branch_word(uint64_t pc, uint64_t target, bool link, uint32_t* out) {
    const int64_t off = static_cast<int64_t>(target) - static_cast<int64_t>(pc);
    if (off < -(1ll << 27) || off >= (1ll << 27) || (off & 3)) return false;
    *out = (link ? 0x94000000u : 0x14000000u) | static_cast<uint32_t>((off >> 2) & 0x3ffffff);
    return true;
}

bool assemble(const std::string& line_in, uint64_t pc, uint32_t* out, std::string* err) {
    std::string e_;
    std::string& e = err ? *err : e_;
    auto fail = [&](const std::string& m) { e = m; return false; };
    std::string line = trim(line_in);
    if (const size_t c = line.find("//"); c != std::string::npos) line = trim(line.substr(0, c));
    if (const size_t c = line.find(';'); c != std::string::npos) line = trim(line.substr(0, c));
    if (line.empty()) return fail("empty line");
    size_t sp = line.find_first_of(" \t");
    const std::string mn = lower(sp == std::string::npos ? line : line.substr(0, sp));
    const std::vector<std::string> ops = sp == std::string::npos ? std::vector<std::string>() : split_ops(line.substr(sp + 1));
    auto nops = [&](size_t n) { return ops.size() == n; };
    auto target_of = [&](const std::string& t, uint64_t* tg) -> bool {
        const std::string s = trim(t);
        if (!s.empty() && s[0] == '#') { int64_t rel; if (!parse_int(s, &rel)) return false; *tg = pc + rel; return true; }
        if (s.size() > 1 && (s[0] == '.') && (s[1] == '+' || s[1] == '-')) { int64_t rel; if (!parse_int(s.substr(1), &rel)) return false; *tg = pc + rel; return true; }
        return parse_u(s, tg);
    };
    auto rel_field = [&](uint64_t tg, int nbits, uint32_t* f) -> bool {
        const int64_t off = static_cast<int64_t>(tg - pc);
        if (off & 3) return false;
        const int64_t q = off / 4;
        if (q < -(1ll << (nbits - 1)) || q >= (1ll << (nbits - 1))) return false;
        *f = static_cast<uint32_t>(q & ((1ll << nbits) - 1));
        return true;
    };

    // ---- raw words, system ----
    if (mn == ".word" || mn == ".inst" || mn == "dd") {
        uint64_t v;
        if (!nops(1) || !parse_u(ops[0], &v) || v > 0xffffffffull) return fail(".word: need a 32-bit value");
        *out = static_cast<uint32_t>(v);
        return true;
    }
    if (ops.empty()) {
        static const struct { const char* n; uint32_t w; } fixed[] = {
            {"nop", 0xd503201f}, {"yield", 0xd503203f}, {"wfe", 0xd503205f}, {"wfi", 0xd503207f}, {"sev", 0xd503209f},
            {"sevl", 0xd50320bf}, {"ret", 0xd65f03c0}, {"isb", 0xd5033fdf}};
        for (const auto& f : fixed) if (mn == f.n) { *out = f.w; return true; }
    }
    if (mn == "hint") {
        uint64_t v;
        if (!nops(1) || !parse_u(ops[0], &v) || v > 127) return fail("hint #0..127");
        *out = 0xd503201fu | (static_cast<uint32_t>(v) << 5);
        return true;
    }
    if (mn == "dmb" || mn == "dsb") {
        static const char* opt[] = {"", "oshld", "oshst", "osh", "", "nshld", "nshst", "nsh", "", "ishld", "ishst", "ish", "", "ld", "st", "sy"};
        if (!nops(1)) return fail(mn + ": need an option (ish, sy, ...)");
        int crm = -1;
        for (int i = 0; i < 16; ++i) if (opt[i][0] && lower(ops[0]) == opt[i]) crm = i;
        if (crm < 0) return fail(mn + ": unknown option");
        *out = (mn == "dmb" ? 0xd50330bfu : 0xd503309fu) | (static_cast<uint32_t>(crm) << 8);
        return true;
    }
    if (mn == "svc" || mn == "brk" || mn == "hlt") {
        uint64_t v;
        if (!nops(1) || !parse_u(ops[0], &v) || v > 0xffff) return fail(mn + ": need #imm16");
        *out = (mn == "svc" ? 0xd4000001u : mn == "brk" ? 0xd4200000u : 0xd4400000u) | (static_cast<uint32_t>(v) << 5);
        return true;
    }

    // ---- branches ----
    if (mn == "b" || mn == "bl") {
        uint64_t tg;
        if (!nops(1) || !target_of(ops[0], &tg)) return fail(mn + ": need a target address");
        if (!branch_word(pc, tg, mn == "bl", out)) return fail(mn + ": the target is out of the +-128 MiB range");
        return true;
    }
    if (mn.size() > 2 && mn.compare(0, 2, "b.") == 0) {
        const int c = parse_cond(mn.substr(2));
        uint64_t tg;
        uint32_t f;
        if (c < 0) return fail("unknown condition " + mn.substr(2));
        if (!nops(1) || !target_of(ops[0], &tg)) return fail(mn + ": need a target");
        if (!rel_field(tg, 19, &f)) return fail(mn + ": the target is out of the +-1 MiB range");
        *out = 0x54000000u | (f << 5) | static_cast<uint32_t>(c);
        return true;
    }
    if (mn == "cbz" || mn == "cbnz") {
        R r;
        uint64_t tg;
        uint32_t f;
        if (!nops(2) || !parse_r(ops[0], &r) || r.sp || !target_of(ops[1], &tg)) return fail(mn + " Rt, target");
        if (!rel_field(tg, 19, &f)) return fail(mn + ": the target is out of the +-1 MiB range");
        *out = (r.is64 ? 0x80000000u : 0) | 0x34000000u | (mn == "cbnz" ? 1u << 24 : 0) | (f << 5) | r.n;
        return true;
    }
    if (mn == "tbz" || mn == "tbnz") {
        R r;
        uint64_t b, tg;
        uint32_t f;
        if (!nops(3) || !parse_r(ops[0], &r) || r.sp || !parse_u(ops[1], &b) || b > 63 || !target_of(ops[2], &tg)) return fail(mn + " Rt, #bit, target");
        if (!rel_field(tg, 14, &f)) return fail(mn + ": the target is out of the +-32 KiB range");
        *out = ((b >> 5) << 31) | 0x36000000u | (mn == "tbnz" ? 1u << 24 : 0) | (static_cast<uint32_t>(b & 31) << 19) | (f << 5) | r.n;
        return true;
    }
    if (mn == "br" || mn == "blr" || mn == "ret") {
        R r{true, 30};
        if (!ops.empty() && (!nops(1) || !parse_r(ops[0], &r) || !r.is64 || r.sp)) return fail(mn + " Xn");
        if (ops.empty() && mn != "ret") return fail(mn + " Xn");
        *out = (mn == "br" ? 0xd61f0000u : mn == "blr" ? 0xd63f0000u : 0xd65f0000u) | (r.n << 5);
        return true;
    }
    if (mn == "adr" || mn == "adrp") {
        R r;
        uint64_t tg;
        if (!nops(2) || !parse_r(ops[0], &r) || !r.is64 || !target_of(ops[1], &tg)) return fail(mn + " Xd, address");
        int64_t v = mn == "adr" ? static_cast<int64_t>(tg - pc) : (static_cast<int64_t>(tg & ~0xfffull) - static_cast<int64_t>(pc & ~0xfffull)) / 4096;
        if (mn == "adrp" && (tg & 0xfff)) return fail("adrp: the target must be a 4 KiB page");
        if (v < -(1ll << 20) || v >= (1ll << 20)) return fail(mn + ": out of range");
        const uint32_t u = static_cast<uint32_t>(v & 0x1fffff);
        *out = (mn == "adrp" ? 0x90000000u : 0x10000000u) | ((u & 3) << 29) | ((u >> 2) << 5) | r.n;
        return true;
    }

    // ---- moves ----
    if (mn == "movz" || mn == "movn" || mn == "movk") {
        R r;
        uint64_t v;
        unsigned shift = 0;
        if (ops.size() < 2 || !parse_r(ops[0], &r) || r.sp || !parse_u(ops[1], &v) || v > 0xffff) return fail(mn + " Rd, #imm16{, lsl #s}");
        if (ops.size() == 3) {
            std::string k;
            if (!parse_shift(ops[2], &k, &shift) || k != "lsl") return fail(mn + ": bad lsl");
        }
        if (shift % 16 || shift > (r.is64 ? 48u : 16u)) return fail(mn + ": lsl must be 0/16/32/48");
        const unsigned opc = mn == "movn" ? 0 : mn == "movz" ? 2 : 3;
        *out = (r.is64 ? 0x80000000u : 0) | (opc << 29) | 0x12800000u | ((shift / 16) << 21) | (static_cast<uint32_t>(v) << 5) | r.n;
        return true;
    }
    if (mn == "mov") {
        R d, s;
        if (!nops(2) || !parse_r(ops[0], &d)) return fail("mov Rd, Rm / #imm");
        if (parse_r(ops[1], &s)) {
            if (s.is64 != d.is64) return fail("mov: register sizes differ");
            if (d.sp || s.sp) { *out = (d.is64 ? 0x91000000u : 0x11000000u) | (s.n << 5) | d.n; return true; }   // add Rd, Rn, #0
            *out = (d.is64 ? 0xaa0003e0u : 0x2a0003e0u) | (s.n << 16) | d.n;                                      // orr Rd, zr, Rm
            return true;
        }
        int64_t iv;
        if (!parse_int(ops[1], &iv)) return fail("mov: bad operand");
        uint64_t v = static_cast<uint64_t>(iv);
        if (!d.is64) {
            if (iv < -(1ll << 31) || iv > 0xffffffffll) return fail("mov: the value does not fit 32 bits");
            v &= 0xffffffffull;
        }
        const unsigned width = d.is64 ? 64 : 32;
        const uint64_t all = d.is64 ? ~0ull : 0xffffffffull;
        for (unsigned s16 = 0; s16 < width; s16 += 16)                               // movz
            if ((v & ~(0xffffull << s16)) == 0 && !d.sp) {
                *out = (d.is64 ? 0x80000000u : 0) | 0x52800000u | ((s16 / 16) << 21) | (static_cast<uint32_t>((v >> s16) & 0xffff) << 5) | d.n;
                return true;
            }
        const uint64_t nv = ~v & all;
        for (unsigned s16 = 0; s16 < width; s16 += 16)                               // movn
            if ((nv & ~(0xffffull << s16)) == 0 && !d.sp) {
                *out = (d.is64 ? 0x80000000u : 0) | 0x12800000u | ((s16 / 16) << 21) | (static_cast<uint32_t>((nv >> s16) & 0xffff) << 5) | d.n;
                return true;
            }
        unsigned N, immr, imms;
        if (encode_bit_mask(v, d.is64, &N, &immr, &imms)) {                         // orr Rd, zr, #bitmask
            *out = (d.is64 ? 0x80000000u : 0) | 0x32000000u | (N << 22) | (immr << 16) | (imms << 10) | (31u << 5) | d.n;
            return true;
        }
        return fail("mov: the value needs more than one instruction (movz + movk per 16-bit part)");
    }

    // ---- add / sub / cmp / cmn / neg ----
    const bool is_addsub = mn == "add" || mn == "adds" || mn == "sub" || mn == "subs" || mn == "cmp" || mn == "cmn" || mn == "neg" || mn == "negs";
    if (is_addsub) {
        std::vector<std::string> o = ops;
        bool sub = mn[0] == 's' || mn == "cmp" || mn == "neg" || mn == "negs";
        bool S = mn.back() == 's' || mn == "cmp" || mn == "cmn";
        if (mn == "cmp" || mn == "cmn") o.insert(o.begin(), "xzr");
        if (mn == "neg" || mn == "negs") o.insert(o.begin() + 1, "xzr");
        if (o.size() < 3) return fail(mn + ": need Rd, Rn, operand");
        R d, n, m;
        if (!parse_r(o[0], &d) || !parse_r(o[1], &n)) return fail(mn + ": bad register");
        if (mn == "cmp" || mn == "cmn") d = {n.is64, 31, false, true};
        if (mn == "neg" || mn == "negs") n = {d.is64, 31, false, true};
        if (d.is64 != n.is64) return fail(mn + ": register sizes differ");
        const uint32_t sf = d.is64 ? 0x80000000u : 0;
        int64_t iv;
        if (parse_int(o[2], &iv)) {
            unsigned sh = 0;
            if (o.size() == 4) {
                std::string k;
                unsigned a;
                if (!parse_shift(o[3], &k, &a) || k != "lsl" || (a != 0 && a != 12)) return fail(mn + ": the shift is lsl #0 or #12");
                sh = a == 12;
            }
            if (iv < 0) { sub = !sub; iv = -iv; }
            if (!sh && iv > 0xfff && (iv & 0xfff) == 0 && iv <= 0xfff000) { sh = 1; iv >>= 12; }
            if (iv > 0xfff) return fail(mn + ": the immediate is 0..4095 (optionally lsl #12)");
            if ((n.zr && !(mn == "neg")) || (d.zr && !S)) {
                if (n.zr) return fail(mn + ": the first source cannot be zr with an immediate");
            }
            *out = sf | (sub ? 0x40000000u : 0) | (S ? 0x20000000u : 0) | 0x11000000u | (sh << 22) | (static_cast<uint32_t>(iv) << 10) | (n.n << 5) | d.n;
            return true;
        }
        if (!parse_r(o[2], &m)) return fail(mn + ": bad operand");
        std::string k;
        unsigned a = 0;
        if (o.size() == 4 && !parse_shift(o[3], &k, &a)) return fail(mn + ": bad shift / extend");
        const bool ext = (!k.empty() && k != "lsl" && k != "lsr" && k != "asr") || d.sp || n.sp || (m.is64 != d.is64);
        if (ext) {
            static const char* ex[] = {"uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};
            int opt = -1;
            if (k.empty() || k == "lsl") opt = d.is64 ? 3 : 2;
            for (int i = 0; i < 8; ++i) if (k == ex[i]) opt = i;
            if (opt < 0 || a > 4) return fail(mn + ": bad extend");
            *out = sf | (sub ? 0x40000000u : 0) | (S ? 0x20000000u : 0) | 0x0b200000u | (m.n << 16) | (static_cast<uint32_t>(opt) << 13) | (a << 10) | (n.n << 5) | d.n;
            return true;
        }
        unsigned st = 0;
        if (k == "lsr") st = 1;
        else if (k == "asr") st = 2;
        if (a >= (d.is64 ? 64u : 32u)) return fail(mn + ": shift too large");
        *out = sf | (sub ? 0x40000000u : 0) | (S ? 0x20000000u : 0) | 0x0b000000u | (st << 22) | (m.n << 16) | (a << 10) | (n.n << 5) | d.n;
        return true;
    }

    // ---- logical ----
    static const struct { const char* n; unsigned opc, N; } kLog[] = {{"and", 0, 0}, {"orr", 1, 0}, {"eor", 2, 0}, {"ands", 3, 0},
                                                                     {"bic", 0, 1}, {"orn", 1, 1}, {"eon", 2, 1}, {"bics", 3, 1}};
    for (const auto& L : kLog) {
        if (mn != L.n && !(mn == "tst" && L.opc == 3 && L.N == 0) && !(mn == "mvn" && L.opc == 1 && L.N == 1)) continue;
        std::vector<std::string> o = ops;
        if (mn == "tst") o.insert(o.begin(), "xzr");
        if (mn == "mvn") o.insert(o.begin() + 1, "xzr");
        if (o.size() < 3) return fail(mn + ": need Rd, Rn, operand");
        R d, n, m;
        if (!parse_r(o[0], &d) || !parse_r(o[1], &n)) return fail(mn + ": bad register");
        if (mn == "tst") d = {n.is64, 31, false, true};
        if (mn == "mvn") n = {d.is64, 31, false, true};
        const uint32_t sf = d.is64 ? 0x80000000u : 0;
        int64_t iv;
        if (parse_int(o[2], &iv)) {
            if (L.N) return fail(mn + ": no immediate form");
            unsigned N, immr, imms;
            if (!encode_bit_mask(static_cast<uint64_t>(iv), d.is64, &N, &immr, &imms)) return fail(mn + ": the immediate is not a bitmask pattern");
            *out = sf | (L.opc << 29) | 0x12000000u | (N << 22) | (immr << 16) | (imms << 10) | (n.n << 5) | d.n;
            return true;
        }
        if (!parse_r(o[2], &m)) return fail(mn + ": bad operand");
        std::string k;
        unsigned a = 0, st = 0;
        if (o.size() == 4) {
            if (!parse_shift(o[3], &k, &a)) return fail(mn + ": bad shift");
            if (k == "lsl") st = 0;
            else if (k == "lsr") st = 1;
            else if (k == "asr") st = 2;
            else if (k == "ror") st = 3;
            else return fail(mn + ": bad shift");
        }
        *out = sf | (L.opc << 29) | 0x0a000000u | (st << 22) | (L.N << 21) | (m.n << 16) | (a << 10) | (n.n << 5) | d.n;
        return true;
    }

    // ---- shifts and bitfields ----
    if (mn == "lsl" || mn == "lsr" || mn == "asr" || mn == "ror") {
        R d, n, m;
        if (!nops(3) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n)) return fail(mn + " Rd, Rn, #s / Rm");
        const uint32_t sf = d.is64 ? 0x80000000u : 0;
        const unsigned size = d.is64 ? 64 : 32;
        if (parse_r(ops[2], &m)) {
            const unsigned op = mn == "lsl" ? 8 : mn == "lsr" ? 9 : mn == "asr" ? 10 : 11;
            *out = sf | 0x1ac00000u | (m.n << 16) | (op << 10) | (n.n << 5) | d.n;
            return true;
        }
        uint64_t s;
        if (!parse_u(ops[2], &s) || s >= size) return fail(mn + ": shift out of range");
        const uint32_t Nb = d.is64 ? (1u << 22) : 0;
        if (mn == "lsl") { *out = sf | 0x53000000u | Nb | (static_cast<uint32_t>((size - s) % size) << 16) | (static_cast<uint32_t>(size - 1 - s) << 10) | (n.n << 5) | d.n; return true; }
        if (mn == "lsr") { *out = sf | 0x53000000u | Nb | (static_cast<uint32_t>(s) << 16) | ((size - 1) << 10) | (n.n << 5) | d.n; return true; }
        if (mn == "asr") { *out = sf | 0x13000000u | Nb | (static_cast<uint32_t>(s) << 16) | ((size - 1) << 10) | (n.n << 5) | d.n; return true; }
        *out = sf | 0x13800000u | Nb | (n.n << 16) | (static_cast<uint32_t>(s) << 10) | (n.n << 5) | d.n;   // ror = extr Rd, Rn, Rn
        return true;
    }
    if (mn == "ubfx" || mn == "sbfx" || mn == "bfxil" || mn == "ubfiz" || mn == "sbfiz" || mn == "bfi" || mn == "bfc") {
        std::vector<std::string> o = ops;
        if (mn == "bfc") o.insert(o.begin() + 1, "xzr");
        R d, n;
        uint64_t lsb, width;
        if (o.size() != 4 || !parse_r(o[0], &d) || !parse_r(o[1], &n) || !parse_u(o[2], &lsb) || !parse_u(o[3], &width)) return fail(mn + " Rd, Rn, #lsb, #width");
        const unsigned size = d.is64 ? 64 : 32;
        if (width < 1 || lsb + width > size) return fail(mn + ": lsb + width exceeds the register");
        const uint32_t base = (mn[0] == 'u' ? 0x53000000u : mn[0] == 's' ? 0x13000000u : 0x33000000u) | (d.is64 ? 0x80400000u : 0);
        unsigned immr, imms;
        if (mn == "ubfx" || mn == "sbfx" || mn == "bfxil") { immr = static_cast<unsigned>(lsb); imms = static_cast<unsigned>(lsb + width - 1); }
        else { immr = static_cast<unsigned>((size - lsb) % size); imms = static_cast<unsigned>(width - 1); }
        *out = base | (immr << 16) | (imms << 10) | ((mn == "bfc" ? 31u : n.n) << 5) | d.n;
        return true;
    }
    if (mn == "uxtb" || mn == "uxth" || mn == "sxtb" || mn == "sxth" || mn == "sxtw") {
        R d, n;
        if (!nops(2) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n)) return fail(mn + " Rd, Wn");
        const unsigned imms = mn.back() == 'b' ? 7 : mn.back() == 'h' ? 15 : 31;
        const bool u = mn[0] == 'u';
        if (u && d.is64) return fail(mn + ": the destination is a w register");
        *out = (u ? 0x53000000u : 0x13000000u) | (d.is64 ? 0x80400000u : 0) | (imms << 10) | (n.n << 5) | d.n;
        return true;
    }
    if (mn == "extr") {
        R d, n, m;
        uint64_t s;
        if (!nops(4) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n) || !parse_r(ops[2], &m) || !parse_u(ops[3], &s)) return fail("extr Rd, Rn, Rm, #lsb");
        *out = (d.is64 ? 0x93c00000u : 0x13800000u) | (m.n << 16) | (static_cast<uint32_t>(s) << 10) | (n.n << 5) | d.n;
        return true;
    }

    // ---- multiply / divide / misc ----
    if (mn == "mul" || mn == "mneg" || mn == "madd" || mn == "msub" || mn == "udiv" || mn == "sdiv") {
        R d, n, m, a{true, 31, false, true};
        const bool four = mn == "madd" || mn == "msub";
        if (ops.size() != (four ? 4u : 3u) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n) || !parse_r(ops[2], &m) || (four && !parse_r(ops[3], &a)))
            return fail(mn + ": bad operands");
        const uint32_t sf = d.is64 ? 0x80000000u : 0;
        if (mn == "udiv" || mn == "sdiv") { *out = sf | 0x1ac00000u | (m.n << 16) | ((mn == "udiv" ? 2u : 3u) << 10) | (n.n << 5) | d.n; return true; }
        const uint32_t o0 = (mn == "mneg" || mn == "msub") ? 0x8000u : 0;
        *out = sf | 0x1b000000u | (m.n << 16) | o0 | (a.n << 10) | (n.n << 5) | d.n;
        return true;
    }
    if (mn == "smull" || mn == "umull" || mn == "smaddl" || mn == "umaddl" || mn == "smulh" || mn == "umulh" || mn == "smsubl" || mn == "umsubl" ||
        mn == "smnegl" || mn == "umnegl") {
        R d, n, m, a{true, 31, false, true};
        const bool four = mn.find("addl") != std::string::npos || mn.find("subl") != std::string::npos;
        if (ops.size() != (four ? 4u : 3u) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n) || !parse_r(ops[2], &m) || (four && !parse_r(ops[3], &a)))
            return fail(mn + ": bad operands");
        const bool u = mn[0] == 'u';
        if (mn == "smulh" || mn == "umulh") { *out = 0x9b000000u | ((u ? 6u : 2u) << 21) | (m.n << 16) | (31u << 10) | (n.n << 5) | d.n; return true; }
        const uint32_t o0 = (mn.find("sub") != std::string::npos || mn.find("neg") != std::string::npos) ? 0x8000u : 0;
        *out = 0x9b000000u | ((u ? 5u : 1u) << 21) | (m.n << 16) | o0 | (a.n << 10) | (n.n << 5) | d.n;
        return true;
    }
    if (mn == "rbit" || mn == "rev16" || mn == "rev32" || mn == "rev" || mn == "clz" || mn == "cls") {
        R d, n;
        if (!nops(2) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n)) return fail(mn + " Rd, Rn");
        unsigned op = mn == "rbit" ? 0 : mn == "rev16" ? 1 : mn == "rev32" ? 2 : mn == "rev" ? (d.is64 ? 3 : 2) : mn == "clz" ? 4 : 5;
        *out = (d.is64 ? 0x80000000u : 0) | 0x5ac00000u | (op << 10) | (n.n << 5) | d.n;
        return true;
    }

    // ---- conditional select / compare ----
    if (mn == "csel" || mn == "csinc" || mn == "csinv" || mn == "csneg") {
        R d, n, m;
        int c;
        if (!nops(4) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n) || !parse_r(ops[2], &m) || (c = parse_cond(ops[3])) < 0) return fail(mn + " Rd, Rn, Rm, cond");
        const uint32_t op = (mn == "csinv" || mn == "csneg") ? 0x40000000u : 0, o2 = (mn == "csinc" || mn == "csneg") ? 0x400u : 0;
        *out = (d.is64 ? 0x80000000u : 0) | op | 0x1a800000u | (m.n << 16) | (static_cast<uint32_t>(c) << 12) | o2 | (n.n << 5) | d.n;
        return true;
    }
    if (mn == "cset" || mn == "csetm") {
        R d;
        int c;
        if (!nops(2) || !parse_r(ops[0], &d) || (c = parse_cond(ops[1])) < 0 || c >= 14) return fail(mn + " Rd, cond");
        const uint32_t op = mn == "csetm" ? 0x40000000u : 0, o2 = mn == "cset" ? 0x400u : 0;
        *out = (d.is64 ? 0x80000000u : 0) | op | 0x1a800000u | (31u << 16) | (static_cast<uint32_t>(c ^ 1) << 12) | o2 | (31u << 5) | d.n;
        return true;
    }
    if (mn == "cinc" || mn == "cinv" || mn == "cneg") {
        R d, n;
        int c;
        if (!nops(3) || !parse_r(ops[0], &d) || !parse_r(ops[1], &n) || (c = parse_cond(ops[2])) < 0 || c >= 14) return fail(mn + " Rd, Rn, cond");
        const uint32_t op = mn == "cinc" ? 0 : 0x40000000u, o2 = mn == "cinv" ? 0 : 0x400u;
        *out = (d.is64 ? 0x80000000u : 0) | op | 0x1a800000u | (n.n << 16) | (static_cast<uint32_t>(c ^ 1) << 12) | o2 | (n.n << 5) | d.n;
        return true;
    }
    if (mn == "ccmp" || mn == "ccmn") {
        R n, m;
        uint64_t nzcv, iv = 0;
        int c;
        if (!nops(4) || !parse_r(ops[0], &n) || !parse_u(ops[2], &nzcv) || nzcv > 15 || (c = parse_cond(ops[3])) < 0) return fail(mn + " Rn, Rm/#imm, #nzcv, cond");
        const uint32_t base = (n.is64 ? 0x80000000u : 0) | (mn == "ccmp" ? 0x7a400000u : 0x3a400000u) | (static_cast<uint32_t>(c) << 12) | (n.n << 5) | static_cast<uint32_t>(nzcv);
        if (parse_r(ops[1], &m)) { *out = base | (m.n << 16); return true; }
        if (!parse_u(ops[1], &iv) || iv > 31) return fail(mn + ": the immediate is 0..31");
        *out = base | 0x800u | (static_cast<uint32_t>(iv) << 16);
        return true;
    }

    // ---- loads and stores ----
    struct LS { const char* n; unsigned size, opc; int rkind; };   // rkind: 0 any (size from reg), 1 w, 2 x
    static const LS kLs[] = {{"strb", 0, 0, 1}, {"ldrb", 0, 1, 1}, {"strh", 1, 0, 1}, {"ldrh", 1, 1, 1}, {"ldrsw", 2, 2, 2}};
    const bool unscaled = mn.compare(0, 3, "ldu") == 0 || mn.compare(0, 3, "stu") == 0;
    const std::string ls_mn = unscaled ? (mn.substr(0, 2) + "r" + mn.substr(4)) : mn;      // ldur -> ldr, sturb -> strb
    if (ls_mn == "ldr" || ls_mn == "str" || ls_mn == "strb" || ls_mn == "ldrb" || ls_mn == "strh" || ls_mn == "ldrh" || ls_mn == "ldrsb" ||
        ls_mn == "ldrsh" || ls_mn == "ldrsw") {
        if (ops.size() < 2) return fail(mn + " Rt, [address]");
        const bool load = ls_mn[0] == 'l';
        R t;
        unsigned vsz = 0, vn = 0;
        bool V = false;
        unsigned size = 0, opc = 0;
        if (parse_r(ops[0], &t)) {
            if (t.sp) return fail(mn + ": Rt cannot be sp");
            if (ls_mn == "ldr" || ls_mn == "str") { size = t.is64 ? 3 : 2; opc = load ? 1 : 0; }
            else if (ls_mn == "ldrsb" || ls_mn == "ldrsh") { size = ls_mn == "ldrsb" ? 0 : 1; opc = t.is64 ? 2 : 3; }
            else {
                bool found = false;
                for (const auto& l : kLs)
                    if (ls_mn == l.n) {
                        size = l.size; opc = l.opc; found = true;
                        if ((l.rkind == 1 && t.is64) || (l.rkind == 2 && !t.is64)) return fail(mn + ": wrong register size");
                    }
                if (!found) return fail(mn + ": unknown");
            }
        } else if (parse_v(ops[0], &vsz, &vn) && (ls_mn == "ldr" || ls_mn == "str")) {
            V = true;
            t.n = vn;
            if (vsz == 4) { size = 0; opc = load ? 3 : 2; }
            else { size = vsz; opc = load ? 1 : 0; }
        } else {
            // literal: ldr Xt, address
            return fail(mn + ": bad register");
        }
        const unsigned scale = V ? vsz : size;
        // literal form: ldr Rt, <address>
        if (load && ops.size() == 2 && !ops[1].empty() && ops[1][0] != '[' && !unscaled) {
            uint64_t tg;
            uint32_t f;
            if (!target_of(ops[1], &tg) || !rel_field(tg, 19, &f)) return fail(mn + ": bad or out-of-range literal address");
            uint32_t lopc;
            if (V) { if (vsz < 2 || vsz > 4) return fail("ldr literal: s, d or q"); lopc = vsz - 2; }
            else if (ls_mn == "ldrsw") lopc = 2;
            else if (ls_mn == "ldr") lopc = t.is64 ? 1 : 0;
            else return fail(mn + ": no literal form");
            *out = (lopc << 30) | 0x18000000u | (V ? 0x04000000u : 0) | (f << 5) | t.n;
            return true;
        }
        Mem m;
        if (!parse_mem(ops, 1, &m, &e)) return false;
        const uint32_t base = (size << 30) | (V ? 0x04000000u : 0) | (opc << 22) | (m.rn << 5) | t.n;
        if (m.kind == 3) {
            if (unscaled) return fail(mn + ": no register offset form");
            int opt = -1;
            if (m.ext == "lsl") opt = 3;
            else if (m.ext == "uxtw") opt = 2;
            else if (m.ext == "sxtw") opt = 6;
            else if (m.ext == "sxtx") opt = 7;
            if (opt < 0 || ((opt & 1) != (m.rm.is64 ? 1 : 0))) return fail(mn + ": bad index extend for its register");
            unsigned S = 0;
            if (m.has_amt) {
                if (m.amt == scale) S = 1;
                else if (m.amt != 0) return fail(mn + ": the index shift must be #0 or #" + std::to_string(scale));
                else S = (scale == 0) ? 1 : 0;      // byte loads print "#0" for S=1
            }
            *out = base | 0x38200800u | (m.rm.n << 16) | (static_cast<uint32_t>(opt) << 13) | (S << 12);
            return true;
        }
        if (m.kind == 0 && !unscaled && m.off >= 0 && (m.off % (1ll << scale)) == 0 && (m.off >> scale) <= 0xfff) {
            *out = base | 0x39000000u | (static_cast<uint32_t>(m.off >> scale) << 10);
            return true;
        }
        if (m.off < -256 || m.off > 255) return fail(mn + ": the offset does not fit (unscaled -256..255, scaled 0..4095 x size)");
        const uint32_t kind = m.kind == 0 ? 0 : m.kind == 2 ? 1 : 3;
        if (m.kind == 0 && !unscaled && m.off >= 0) return fail(mn + ": the offset must be a multiple of the access size (or use ldur/stur)");
        *out = base | 0x38000000u | ((static_cast<uint32_t>(m.off) & 0x1ff) << 12) | (kind << 10);
        return true;
    }
    // exclusives and ordered accesses: ldxr/ldaxr/stxr/stlxr (b/h), ldar/stlr (b/h), ldxp/stxp
    {
        std::string base = mn;
        unsigned esz = 0xff;
        static const char* kEx[] = {"ldxr", "ldaxr", "stxr", "stlxr", "ldar", "stlr", "ldxp", "ldaxp", "stxp", "stlxp"};
        for (const char* k : kEx) {
            const std::string ks = k;
            if (base == ks) esz = 4;                                   // 4 = from the register (w: 2, x: 3)
            else if (base == ks + "b" && ks.back() == 'r') esz = 0;
            else if (base == ks + "h" && ks.back() == 'r') esz = 1;
            if (esz != 0xff) { base = ks; break; }
        }
        if (esz != 0xff) {
            const bool pair = base.back() == 'p', store = base[0] == 's', ordered = base == "ldar" || base == "stlr";
            const bool acq = base == "ldaxr" || base == "stlxr" || base == "ldaxp" || base == "stlxp" || ordered;
            size_t i = 0;
            R s{false, 31}, t, t2{false, 31};
            if (store && !ordered) { if (ops.empty() || !parse_r(ops[0], &s) || s.is64) return fail(mn + ": the status register is a w register"); i = 1; }
            if (ops.size() < i + 1 || !parse_r(ops[i], &t)) return fail(mn + ": bad register");
            if (pair && (ops.size() < i + 2 || !parse_r(ops[i + 1], &t2))) return fail(mn + ": two registers");
            Mem m;
            if (!parse_mem(ops, i + (pair ? 2 : 1), &m, &e)) return false;
            if (m.kind != 0 || m.off != 0) return fail(mn + ": the address is [Xn] only");
            const unsigned size = esz == 4 ? (t.is64 ? 3u : 2u) : esz;
            const uint32_t o2 = ordered ? 1u : 0u, L = store ? 0u : 1u, o1 = pair ? 1u : 0u, o0 = acq ? 1u : 0u;
            *out = (size << 30) | 0x08000000u | (o2 << 23) | (L << 22) | (o1 << 21) | ((ordered || !store ? 31u : s.n) << 16) | (o0 << 15) |
                   ((pair ? t2.n : 31u) << 10) | (m.rn << 5) | t.n;
            return true;
        }
    }
    if (mn == "adc" || mn == "adcs" || mn == "sbc" || mn == "sbcs" || mn == "ngc" || mn == "ngcs") {
        std::vector<std::string> o = ops;
        if (mn[0] == 'n') o.insert(o.begin() + 1, "xzr");
        R d, n, m;
        if (o.size() != 3 || !parse_r(o[0], &d) || !parse_r(o[1], &n) || !parse_r(o[2], &m)) return fail(mn + " Rd, Rn, Rm");
        const bool sub = mn[0] == 's' || mn[0] == 'n', S = mn.back() == 's';
        *out = (d.is64 ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (S ? 0x20000000u : 0) | 0x1a000000u | (m.n << 16) | ((mn[0] == 'n' ? 31u : n.n) << 5) | d.n;
        return true;
    }
    if (mn == "mrs" || mn == "msr") {
        if (!nops(2)) return fail(mn + ": two operands");
        R r;
        const std::string sr = lower(mn == "mrs" ? ops[1] : ops[0]);
        if (!parse_r(mn == "mrs" ? ops[0] : ops[1], &r) || !r.is64) return fail(mn + ": an x register");
        static const struct { const char* n; uint32_t key; } kSys[] = {{"tpidr_el0", 0xde82}, {"tpidrro_el0", 0xde83}, {"cntvct_el0", 0xdf02},
                                                                     {"cntpct_el0", 0xdf01}, {"cntfrq_el0", 0xdf00}, {"fpcr", 0xda20},
                                                                     {"fpsr", 0xda21}, {"nzcv", 0xda10}, {"ctr_el0", 0xd801}, {"dczid_el0", 0xd807}};
        uint32_t key = 0xffffffffu;
        for (const auto& k : kSys) if (sr == k.n) key = k.key;
        unsigned o0, op1, crn, crm, op2;
        if (key == 0xffffffffu) {
            if (std::sscanf(sr.c_str(), "s%u_%u_c%u_c%u_%u", &o0, &op1, &crn, &crm, &op2) != 5 || o0 < 2 || o0 > 3) return fail(mn + ": unknown system register");
            key = (o0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
        }
        // the key is op0:op1:CRn:CRm:op2; the word holds o0 = op0 - 2 at bit 19
        const uint32_t sys = ((((key >> 14) & 3) - 2) << 19) | (((key >> 11) & 7) << 16) | (((key >> 7) & 15) << 12) | (((key >> 3) & 15) << 8) | ((key & 7) << 5);
        *out = 0xd5100000u | (mn == "mrs" ? 0x200000u : 0) | sys | r.n;
        return true;
    }
    // LSE atomics: ld<op>{a}{l}{b|h}, st<op>{l}{b|h}, swp{a}{l}{b|h}, cas{a}{l}{b|h}
    {
        static const char* kOps[] = {"add", "clr", "eor", "set", "smax", "smin", "umax", "umin"};
        std::string m2 = mn;
        unsigned esz = 4;
        // the size letter is always last ("ldaddalb", "casah"): the second pass takes it off
        auto strip_size = [&](std::string& x) {
            if (x.size() > 3 && (x.back() == 'b' || x.back() == 'h')) {
                esz = x.back() == 'b' ? 0 : 1;
                x.pop_back();
            }
        };
        int kind = -1, opi = 0;           // 0 ld<op>, 1 st<op>, 2 swp, 3 cas
        bool A = false, Rl = false;
        auto ord = [&](std::string rest) -> bool {   // "", "a", "l", "al"
            if (rest == "") return true;
            if (rest == "a") { A = true; return true; }
            if (rest == "l") { Rl = true; return true; }
            if (rest == "al") { A = Rl = true; return true; }
            return false;
        };
        std::string body = m2;
        for (int pass = 0; pass < 2 && kind < 0; ++pass) {
            if (pass == 1) strip_size(body);
            for (int o = 0; o < 8 && kind < 0; ++o) {
                const std::string ld = std::string("ld") + kOps[o], st = std::string("st") + kOps[o];
                if (body.rfind(ld, 0) == 0 && ord(body.substr(ld.size()))) { kind = 0; opi = o; }
                else if (body.rfind(st, 0) == 0 && body.substr(st.size()).find('a') == std::string::npos && ord(body.substr(st.size()))) { kind = 1; opi = o; }
            }
            if (kind < 0 && body.rfind("swp", 0) == 0 && ord(body.substr(3))) kind = 2;
            if (kind < 0 && body.rfind("cas", 0) == 0 && body.size() <= 5 && ord(body.substr(3))) kind = 3;
            if (kind < 0) { A = Rl = false; esz = 4; body = m2; }
        }
        if (kind >= 0) {
            R rs, rt{false, 31};
            const size_t want = kind == 1 ? 2 : 3;
            if (ops.size() != want || !parse_r(ops[0], &rs) || (kind != 1 && !parse_r(ops[1], &rt))) return fail(mn + ": bad registers");
            Mem m;
            if (!parse_mem(ops, want - 1, &m, &e)) return false;
            if (m.kind != 0 || m.off != 0) return fail(mn + ": the address is [Xn] only");
            const unsigned size = esz == 4 ? (rs.is64 ? 3u : 2u) : esz;
            if (kind == 3) {
                *out = (size << 30) | 0x08a07c00u | (A ? 0x400000u : 0) | (rs.n << 16) | (Rl ? 0x8000u : 0) | (m.rn << 5) | rt.n;
                return true;
            }
            const uint32_t o3 = kind == 2 ? 0x8000u : 0, opc = kind == 2 ? 0 : static_cast<uint32_t>(opi);
            *out = (size << 30) | 0x38200000u | (A ? 0x800000u : 0) | (Rl ? 0x400000u : 0) | (rs.n << 16) | o3 | (opc << 12) | (m.rn << 5) | (kind == 1 ? 31u : rt.n);
            return true;
        }
    }
    if (mn == "prfm" || mn == "prfum") {
        if (ops.size() != 2) return fail(mn + " <op>, [address]");
        unsigned op = 32;
        for (unsigned k = 0; k < 32; ++k) if (lower(ops[0]) == prf_name(k)) op = k;
        if (op == 32) { uint64_t v; if (!parse_u(ops[0], &v) || v > 31) return fail(mn + ": unknown operation"); op = static_cast<unsigned>(v); }
        Mem m;
        if (!parse_mem(ops, 1, &m, &e)) return false;
        if (m.kind == 3) {
            int opt = m.ext == "lsl" ? 3 : m.ext == "uxtw" ? 2 : m.ext == "sxtw" ? 6 : m.ext == "sxtx" ? 7 : -1;
            if (opt < 0) return fail(mn + ": bad index extend");
            *out = 0xf8a00800u | (m.rm.n << 16) | (static_cast<uint32_t>(opt) << 13) | ((m.has_amt && m.amt == 3) ? 0x1000u : 0) | (m.rn << 5) | op;
            return true;
        }
        if (mn == "prfm" && m.kind == 0 && m.off >= 0 && (m.off % 8) == 0 && m.off / 8 <= 0xfff) {
            *out = 0xf9800000u | (static_cast<uint32_t>(m.off / 8) << 10) | (m.rn << 5) | op;
            return true;
        }
        if (m.kind != 0 || m.off < -256 || m.off > 255) return fail(mn + ": bad offset");
        *out = 0xf8800000u | ((static_cast<uint32_t>(m.off) & 0x1ff) << 12) | (m.rn << 5) | op;
        return true;
    }
    if (mn == "ldp" || mn == "stp" || mn == "ldpsw") {
        if (ops.size() < 3) return fail(mn + " Rt, Rt2, [address]");
        R a, b;
        unsigned vs1 = 0, vn1 = 0, vs2 = 0, vn2 = 0;
        bool V = false;
        uint32_t opc;
        unsigned scale, t1, t2;
        if (parse_r(ops[0], &a) && parse_r(ops[1], &b)) {
            if (a.is64 != b.is64) return fail(mn + ": register sizes differ");
            if (mn == "ldpsw") { if (!a.is64) return fail("ldpsw: x registers"); opc = 1; scale = 2; }
            else { opc = a.is64 ? 2 : 0; scale = a.is64 ? 3 : 2; }
            t1 = a.n;
            t2 = b.n;
        } else if (parse_v(ops[0], &vs1, &vn1) && parse_v(ops[1], &vs2, &vn2) && vs1 == vs2 && vs1 >= 2 && mn != "ldpsw") {
            V = true;
            opc = vs1 - 2;
            scale = vs1;
            t1 = vn1;
            t2 = vn2;
        } else {
            return fail(mn + ": bad registers");
        }
        Mem m;
        if (!parse_mem(ops, 2, &m, &e)) return false;
        if (m.kind == 3) return fail(mn + ": no register offset form");
        if (m.off % (1ll << scale)) return fail(mn + ": the offset must be a multiple of the register size");
        const int64_t q = m.off / (1ll << scale);
        if (q < -64 || q > 63) return fail(mn + ": the offset is out of range");
        const uint32_t idx = m.kind == 0 ? 2 : m.kind == 2 ? 1 : 3;
        *out = (opc << 30) | 0x28000000u | (V ? 0x04000000u : 0) | (idx << 23) | (mn[0] == 'l' ? 0x400000u : 0) |
               ((static_cast<uint32_t>(q) & 0x7f) << 15) | (t2 << 10) | (m.rn << 5) | t1;
        return true;
    }

    // ---- scalar floating point ----
    auto ftype = [](unsigned sz, uint32_t* type) -> bool {
        if (sz == 2) { *type = 0; return true; }
        if (sz == 3) { *type = 1; return true; }
        if (sz == 1) { *type = 3; return true; }
        return false;
    };
    if (mn == "fmov") {
        unsigned ds, dn, ss, sn;
        R r;
        uint32_t type;
        if (!nops(2)) return fail("fmov Vd, Vn / Rn / #imm");
        if (parse_v(ops[0], &ds, &dn) && ftype(ds, &type)) {
            if (parse_v(ops[1], &ss, &sn)) {
                if (ss != ds) return fail("fmov: register sizes differ");
                *out = 0x1e204000u | (type << 22) | (sn << 5) | dn;
                return true;
            }
            if (parse_r(ops[1], &r)) {
                if ((ds == 3) != r.is64) return fail("fmov: d <-> x, s <-> w");
                *out = (r.is64 ? 0x80000000u : 0) | 0x1e270000u | (type << 22) | (r.n << 5) | dn;
                return true;
            }
            const std::string iv = trim(ops[1][0] == '#' ? ops[1].substr(1) : ops[1]);
            char* end = nullptr;
            const double v = std::strtod(iv.c_str(), &end);
            unsigned imm8;
            if (end == iv.c_str() || *end) return fail("fmov: bad immediate");
            if (!encode_fp8(v, &imm8)) return fail("fmov: the value has no 8-bit form (+-n/16 x 2^e, 0.125..31)");
            *out = 0x1e201000u | (type << 22) | (imm8 << 13) | dn;
            return true;
        }
        if (parse_r(ops[0], &r) && parse_v(ops[1], &ss, &sn) && ftype(ss, &type)) {
            if ((ss == 3) != r.is64) return fail("fmov: x <-> d, w <-> s");
            *out = (r.is64 ? 0x80000000u : 0) | 0x1e260000u | (type << 22) | (sn << 5) | r.n;
            return true;
        }
        return fail("fmov: bad operands");
    }
    static const struct { const char* n; uint32_t op; } kF2[] = {{"fmul", 0}, {"fdiv", 1}, {"fadd", 2}, {"fsub", 3}, {"fmax", 4}, {"fmin", 5},
                                                                 {"fmaxnm", 6}, {"fminnm", 7}, {"fnmul", 8}};
    for (const auto& f : kF2) {
        if (mn != f.n) continue;
        unsigned s0, n0, s1, n1, s2, n2;
        uint32_t type;
        if (!nops(3) || !parse_v(ops[0], &s0, &n0) || !parse_v(ops[1], &s1, &n1) || !parse_v(ops[2], &s2, &n2) || s0 != s1 || s1 != s2 || !ftype(s0, &type))
            return fail(mn + " Vd, Vn, Vm (s, d or h)");
        *out = 0x1e200800u | (type << 22) | (n2 << 16) | (f.op << 12) | (n1 << 5) | n0;
        return true;
    }
    static const struct { const char* n; uint32_t op; } kF1[] = {{"fabs", 1}, {"fneg", 2}, {"fsqrt", 3}, {"frintn", 8}, {"frintp", 9},
                                                                 {"frintm", 10}, {"frintz", 11}, {"frinta", 12}, {"frintx", 14}, {"frinti", 15}};
    for (const auto& f : kF1) {
        if (mn != f.n) continue;
        unsigned s0, n0, s1, n1;
        uint32_t type;
        if (!nops(2) || !parse_v(ops[0], &s0, &n0) || !parse_v(ops[1], &s1, &n1) || s0 != s1 || !ftype(s0, &type)) return fail(mn + " Vd, Vn");
        *out = 0x1e204000u | (type << 22) | (f.op << 15) | (n1 << 5) | n0;
        return true;
    }
    if (mn == "fcvt") {
        unsigned s0, n0, s1, n1;
        uint32_t type;
        if (!nops(2) || !parse_v(ops[0], &s0, &n0) || !parse_v(ops[1], &s1, &n1) || s0 == s1 || !ftype(s1, &type)) return fail("fcvt Vd, Vn (s/d/h, different sizes)");
        const uint32_t op = s0 == 2 ? 4 : s0 == 3 ? 5 : 7;
        *out = 0x1e204000u | (type << 22) | (op << 15) | (n1 << 5) | n0;
        return true;
    }
    if (mn == "fcmp" || mn == "fcmpe") {
        unsigned s0, n0, s1, n1;
        uint32_t type;
        if (!nops(2) || !parse_v(ops[0], &s0, &n0) || !ftype(s0, &type)) return fail(mn + " Vn, Vm / #0.0");
        const uint32_t e16 = mn == "fcmpe" ? 0x10u : 0;
        if (parse_v(ops[1], &s1, &n1)) { *out = 0x1e202000u | (type << 22) | (n1 << 16) | (n0 << 5) | e16; return true; }
        const std::string z = trim(ops[1][0] == '#' ? ops[1].substr(1) : ops[1]);
        if (std::strtod(z.c_str(), nullptr) != 0.0) return fail(mn + ": compare with a register or #0.0");
        *out = 0x1e202008u | (type << 22) | (n0 << 5) | e16;
        return true;
    }
    if (mn == "fcsel") {
        unsigned s0, n0, s1, n1, s2, n2;
        uint32_t type;
        int c;
        if (!nops(4) || !parse_v(ops[0], &s0, &n0) || !parse_v(ops[1], &s1, &n1) || !parse_v(ops[2], &s2, &n2) || !ftype(s0, &type) || (c = parse_cond(ops[3])) < 0)
            return fail("fcsel Vd, Vn, Vm, cond");
        *out = 0x1e200c00u | (type << 22) | (n2 << 16) | (static_cast<uint32_t>(c) << 12) | (n1 << 5) | n0;
        return true;
    }
    if (mn == "fccmp" || mn == "fccmpe") {
        unsigned s0, n0, s1, n1;
        uint32_t type;
        uint64_t nzcv;
        int c;
        if (!nops(4) || !parse_v(ops[0], &s0, &n0) || !parse_v(ops[1], &s1, &n1) || !ftype(s0, &type) || !parse_u(ops[2], &nzcv) || nzcv > 15 ||
            (c = parse_cond(ops[3])) < 0)
            return fail(mn + " Vn, Vm, #nzcv, cond");
        *out = 0x1e200400u | (type << 22) | (n1 << 16) | (static_cast<uint32_t>(c) << 12) | (n0 << 5) | (mn == "fccmpe" ? 0x10u : 0) | static_cast<uint32_t>(nzcv);
        return true;
    }
    if (mn == "fmadd" || mn == "fmsub" || mn == "fnmadd" || mn == "fnmsub") {
        unsigned s0, n0, s1, n1, s2, n2, s3, n3;
        uint32_t type;
        if (!nops(4) || !parse_v(ops[0], &s0, &n0) || !parse_v(ops[1], &s1, &n1) || !parse_v(ops[2], &s2, &n2) || !parse_v(ops[3], &s3, &n3) || !ftype(s0, &type))
            return fail(mn + " Vd, Vn, Vm, Va");
        const uint32_t o1 = mn[1] == 'n' ? 0x200000u : 0, o0 = mn.find("sub") != std::string::npos ? 0x8000u : 0;
        *out = 0x1f000000u | (type << 22) | o1 | (n2 << 16) | o0 | (n3 << 10) | (n1 << 5) | n0;
        return true;
    }
    static const struct { const char* n; uint32_t rmode_op; bool to_int; } kCvt[] = {
        {"scvtf", 0x020000, false}, {"ucvtf", 0x030000, false}, {"fcvtzs", 0x180000, true}, {"fcvtzu", 0x190000, true},
        {"fcvtns", 0x000000, true}, {"fcvtnu", 0x010000, true}, {"fcvtps", 0x080000, true}, {"fcvtpu", 0x090000, true},
        {"fcvtms", 0x100000, true}, {"fcvtmu", 0x110000, true}, {"fcvtas", 0x040000, true}, {"fcvtau", 0x050000, true}};
    for (const auto& c : kCvt) {
        if (mn != c.n) continue;
        R r;
        unsigned vs, vn;
        uint32_t type;
        if (!nops(2)) return fail(mn + ": two operands");
        if (c.to_int ? !(parse_r(ops[0], &r) && parse_v(ops[1], &vs, &vn)) : !(parse_v(ops[0], &vs, &vn) && parse_r(ops[1], &r))) return fail(mn + ": bad operands");
        if (!ftype(vs, &type)) return fail(mn + ": s, d or h");
        *out = (r.is64 ? 0x80000000u : 0) | 0x1e200000u | (type << 22) | c.rmode_op | (c.to_int ? (vn << 5) | r.n : (r.n << 5) | vn);
        return true;
    }
    return fail("'" + mn + "' is not supported by this assembler (use .word 0x... for a raw instruction)");
}

}  // namespace nxrt::cheats::a64
