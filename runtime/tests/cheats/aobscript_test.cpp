// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test of the CE-style AOB scripts (runtime/src/cheats/aobscript.*) over a fake guest memory. No runtime dependency.
#define _CRT_SECURE_NO_WARNINGS
#include "aobscript.h"
#include "gmem.h"

#include <cstdio>
#include <cstring>

namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"
#include "disasm.cpp"
#include "aobscript.cpp"

using namespace nxrt::cheats;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Mem : MemSource {
    uint64_t base;
    std::vector<uint8_t> m;
    Mem(uint64_t b, size_t n) : base(b), m(n, 0) {}
    std::vector<ScanRegion> regions(const RegionFilter&) override { return {{base, m.size(), 5, "code"}}; }
    bool read(uint64_t a, void* d, size_t n) override {
        if (a < base || a + n > base + m.size()) return false;
        std::memcpy(d, m.data() + (a - base), n);
        return true;
    }
};

struct Host : aa::ScriptHost {
    Mem mem_;
    std::map<std::string, uint64_t> syms;
    int writes = 0;
    Host() : mem_(0x7100000000ull, 0x4000) {}
    MemSource& mem() override { return mem_; }
    bool module_range(const std::string& n, uint64_t* lo, uint64_t* hi) override {
        if (n != "main") return false;
        *lo = mem_.base;
        *hi = mem_.base + mem_.m.size();
        return true;
    }
    bool write(const aa::Write& w, std::string*) override {
        std::memcpy(mem_.m.data() + (w.addr - mem_.base), w.bytes.data(), w.bytes.size());
        ++writes;
        return true;
    }
    std::map<std::string, uint64_t>& symbols() override { return syms; }
    uint32_t word(uint64_t off) { uint32_t v; std::memcpy(&v, mem_.m.data() + off, 4); return v; }
};

int main() {
    Host h;
    // the "coin spend" code: sub w8, w8, #1 ; str w8, [x19, #0x40] ; ret   at main+0x1230
    const uint32_t code[] = {0x51000508u, 0xb9004268u, 0xd65f03c0u};
    std::memcpy(h.mem_.m.data() + 0x1230, code, sizeof code);

    const char* script =
        "// no more coins spent\n"
        "[ENABLE]\n"
        "aobscanmodule(coindec, main, 08 05 00 51 68 ?? 00 B9)  { the sub + str }\n"
        "coindec:\n"
        "  nop\n"
        "registersymbol(coindec)\n"
        "[DISABLE]\n"
        "coindec:\n"
        "  db 08 05 00 51\n"
        "unregistersymbol(coindec)\n";
    aa::Script s;
    std::string err;
    CHECK(aa::parse(script, &s, &err));
    CHECK(s.has_disable && s.enable.lines.size() == 4 && s.disable.lines.size() == 3);
    aa::RunResult on = aa::enable(s, h);
    if (!on.ok) std::printf("enable: %s\n", on.error.c_str());
    CHECK(on.ok);
    CHECK(h.word(0x1230) == 0xd503201fu);                  // nop
    CHECK(h.word(0x1234) == 0xb9004268u);                  // untouched
    CHECK(h.syms.count("coindec") && h.syms["coindec"] == 0x7100001230ull);
    CHECK(on.writes.size() == 1 && on.writes[0].original.size() == 4 && on.writes[0].original[0] == 0x08);
    aa::RunResult off = aa::disable(s, h, on);
    CHECK(off.ok);
    CHECK(h.word(0x1230) == 0x51000508u);
    CHECK(!h.syms.count("coindec"));

    // a script made for another build: the pattern is gone -> a clear error, nothing written
    const int before = h.writes;
    aa::Script s2;
    CHECK(aa::parse("[ENABLE]\naobscanmodule(x, main, DE AD BE EF)\nx:\n nop\n[DISABLE]\n", &s2, &err));
    aa::RunResult bad = aa::enable(s2, h);
    CHECK(!bad.ok && bad.error.find("not in main") != std::string::npos && h.writes == before);

    // no [DISABLE] lines: disabling restores what [ENABLE] wrote; expressions, dd, b to a symbol, mov
    aa::Script s3;
    CHECK(aa::parse("[ENABLE]\naobscan(f, 08 05 00 51)\ndefine(tail, f+8)\nf+4:\n  mov w8, #99\n  b tail\nmain+2000:\n  dd 12345678 #10\n[DISABLE]\n", &s3, &err));
    aa::RunResult on3 = aa::enable(s3, h);
    if (!on3.ok) std::printf("enable3: %s\n", on3.error.c_str());
    CHECK(on3.ok);
    CHECK(h.word(0x1234) == 0x52800c68u);                  // movz w8, #99
    CHECK(h.word(0x1238) == 0x14000000u);                  // b to itself + 0? f+8 = 0x1238: branch to self
    CHECK(h.word(0x2000) == 0x12345678u && h.word(0x2004) == 10u);
    aa::disable(s3, h, on3);
    CHECK(h.word(0x1234) == 0xb9004268u && h.word(0x1238) == 0xd65f03c0u && h.word(0x2000) == 0);

    // a bad line fails with its line number; writes before it are undone
    aa::Script s4;
    CHECK(aa::parse("[ENABLE]\nmain+1230:\n  nop\n  frobnicate x1\n", &s4, &err));
    aa::RunResult on4 = aa::enable(s4, h);
    CHECK(!on4.ok && on4.error.rfind("line 4:", 0) == 0);
    CHECK(h.word(0x1230) == 0x51000508u);

    // assert guards
    aa::Script s5;
    CHECK(aa::parse("[ENABLE]\nassert(main+1230, 08 05 00 51)\nmain+1230:\n nop\n", &s5, &err));
    CHECK(aa::enable(s5, h).ok);
    CHECK(!aa::enable(s5, h).ok);                           // the bytes are a nop now
    std::memcpy(h.mem_.m.data() + 0x1230, code, 4);

    // the script the disassembler's "NOP + make it an AOB script" writes parses and runs
    const std::string gen = aa::make_patch_script("Coins never drop", "main", "08 05 00 51 68 ?? 00 B9", 0, {0x1f, 0x20, 0x03, 0xd5}, {0x08, 0x05, 0x00, 0x51});
    aa::Script s6;
    CHECK(aa::parse(gen, &s6, &err));
    aa::RunResult on6 = aa::enable(s6, h);
    CHECK(on6.ok && h.word(0x1230) == 0xd503201fu);
    CHECK(aa::disable(s6, h, on6).ok && h.word(0x1230) == 0x51000508u);

    // a [DISABLE] that writes nothing (only unregistersymbol) puts back what [ENABLE] wrote, so the next [ENABLE] finds its bytes again
    aa::Script s7;
    CHECK(aa::parse("[ENABLE]\naobscanmodule(st, main, 08 05 00 51)\nst:\n  nop\nregistersymbol(st)\n[DISABLE]\nunregistersymbol(st)\n", &s7, &err));
    aa::RunResult on7 = aa::enable(s7, h);
    CHECK(on7.ok && h.word(0x1230) == 0xd503201fu);
    CHECK(aa::disable(s7, h, on7).ok && h.word(0x1230) == 0x51000508u);
    aa::RunResult again7 = aa::enable(s7, h);
    CHECK(again7.ok && h.word(0x1230) == 0xd503201fu);
    aa::disable(s7, h, again7);

    if (g_fail == 0) std::printf("ALL AOB SCRIPT TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
