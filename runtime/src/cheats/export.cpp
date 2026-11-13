// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Export cheats in Atmosphère format (export.h).
#include "export.h"

#include <cstdio>

namespace nxrt::cheats {
namespace {

std::string w(uint32_t word) {
    char b[10];
    std::snprintf(b, sizeof b, "%08X", word);
    return b;
}
// width nybble T for the store/load opcodes: 1,2,4,8.
unsigned tnyb(ScanType t) { return static_cast<unsigned>(type_size(t)); }

// The value words: one word for T<=4, two (hi, lo) for T=8.
void value_words(const Value& v, std::string& out) {
    const unsigned ts = static_cast<unsigned>(type_size(v.type));
    if (ts == 8) {
        out += w(static_cast<uint32_t>(v.bits >> 32));
        out += ' ';
        out += w(static_cast<uint32_t>(v.bits & 0xFFFFFFFF));
    } else {
        uint32_t m = ts == 1 ? 0xFF : ts == 2 ? 0xFFFF : 0xFFFFFFFF;
        out += w(static_cast<uint32_t>(v.bits & m));
    }
}

}  // namespace

std::string export_static_body(unsigned mem_type, uint64_t offset, const Value& v) {
    // 0TMR00AA AAAAAAAA V(V)   -- opcode 0, R = 0 (no register added)
    const unsigned T = tnyb(v.type);
    uint32_t word0 = (0x0u << 28) | (T << 24) | ((mem_type & 0xF) << 20) | (0u << 16) |
                     (static_cast<uint32_t>((offset >> 32) & 0xFF));
    std::string s = w(word0) + " " + w(static_cast<uint32_t>(offset & 0xFFFFFFFF)) + " ";
    value_words(v, s);
    s += '\n';
    return s;
}

std::string export_pointer_body(unsigned mem_type, uint64_t base_offset, const std::vector<uint64_t>& offsets, const Value& v) {
    std::string s;
    const unsigned T = tnyb(v.type);
    // reg0 = [base(M) + base_offset]  (opcode 5, T=8, R=0, I=0)
    {
        uint32_t word0 = (0x5u << 28) | (8u << 24) | ((mem_type & 0xF) << 20) | (0u << 16) | (0u << 12) | (0u << 8) |
                         static_cast<uint32_t>((base_offset >> 32) & 0xFF);
        s += w(word0) + " " + w(static_cast<uint32_t>(base_offset & 0xFFFFFFFF)) + "\n";
    }
    // for each offset except the last: reg0 = [reg0 + offset]  (opcode 5, T=8, R=0, I=1)
    const size_t n = offsets.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint64_t off = offsets[i];
        uint32_t word0 = (0x5u << 28) | (8u << 24) | (0u << 20) | (0u << 16) | (1u << 12) | (0u << 8) |
                         static_cast<uint32_t>((off >> 32) & 0xFF);
        s += w(word0) + " " + w(static_cast<uint32_t>(off & 0xFFFFFFFF)) + "\n";
    }
    const uint64_t final_off = n ? offsets[n - 1] : 0;
    // reg1 = value  (opcode 4, R=1, 64-bit immediate)
    {
        uint32_t word0 = (0x4u << 28) | (0u << 24) | (1u << 16);
        s += w(word0) + " " + w(static_cast<uint32_t>(v.bits >> 32)) + " " + w(static_cast<uint32_t>(v.bits & 0xFFFFFFFF)) + "\n";
    }
    // store reg1 (low T bytes) to [reg0 + final_off]  (opcode A, S=1, R=0, I=0, O=2, x=0, a=final_off)
    {
        uint32_t word0 = (0xAu << 28) | (T << 24) | (1u << 20) | (0u << 16) | (0u << 12) | (2u << 8) | (0u << 4) |
                         static_cast<uint32_t>((final_off >> 32) & 0xF);
        s += w(word0) + " " + w(static_cast<uint32_t>(final_off & 0xFFFFFFFF)) + "\n";
    }
    return s;
}

std::string export_cheat(const std::string& name, const std::string& body, bool master) {
    std::string s;
    s += master ? "{" : "[";
    s += name;
    s += master ? "}" : "]";
    s += '\n';
    s += body;
    return s;
}

}  // namespace nxrt::cheats
