// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test of the memory scanner (runtime/src/cheats/scanner.*) against a fake memory source. No runtime dependency: it provides
// its own MemSource, and gmem is stubbed out (scanner.cpp's GmemSource / live_source are not exercised here).
#define _CRT_SECURE_NO_WARNINGS
#include "scanner.h"
#include "gmem.h"

#include <cstdio>
#include <cstring>
#include <map>

// Stub the two gmem functions scanner.cpp's live source references, so the test links without the runtime (its GmemSource is not used here).
namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"

using namespace nxrt::cheats;

// A fake source: one region backed by a byte vector.
struct FakeSource : MemSource {
    uint64_t base;
    std::vector<uint8_t> mem;
    FakeSource(uint64_t b, size_t n) : base(b), mem(n, 0) {}
    std::vector<ScanRegion> regions(const RegionFilter&) override { return {{base, mem.size(), 3, "heap"}}; }
    bool read(uint64_t a, void* d, size_t n) override {
        if (a < base || a + n > base + mem.size()) return false;
        std::memcpy(d, mem.data() + (a - base), n);
        return true;
    }
    template <class T> void put(uint64_t a, T v) { std::memcpy(mem.data() + (a - base), &v, sizeof v); }
    template <class T> T get(uint64_t a) { T v; std::memcpy(&v, mem.data() + (a - base), sizeof v); return v; }
};

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    const uint64_t B = 0x7800000000ull;
    auto src = std::make_shared<FakeSource>(B, 0x10000);
    FakeSource& f = *src;
    // scatter a few 4-byte values of 1000; everything else 0
    f.put<uint32_t>(B + 0x100, 1000);
    f.put<uint32_t>(B + 0x800, 1000);
    f.put<uint32_t>(B + 0x2000, 1000);
    f.put<uint32_t>(B + 0x40, 7);

    Scanner sc(src);
    ScanOptions opt;
    opt.type = ScanType::U32;
    ScanProgress prog;
    Value v1, v2;

    // --- exact first scan for 1000 ---
    parse_value(ScanType::U32, "1000", &v1);
    sc.first_scan(opt, FirstScan::Exact, v1, v2, prog);
    CHECK(sc.result_count() == 3);
    CHECK(prog.bytes_total == 0x10000);

    // --- next scan: change one of them, then "changed" keeps that one ---
    f.put<uint32_t>(B + 0x800, 1234);
    sc.next_scan(NextScan::Changed, v1, v2, prog);
    CHECK(sc.result_count() == 1);
    {
        auto r = sc.results(0, 10);
        CHECK(r.size() == 1);
        CHECK(r[0].addr == B + 0x800);
        CHECK(r[0].last.as_int() == 1234);
    }

    // --- undo returns to the 3 results ---
    sc.undo();
    CHECK(sc.result_count() == 3);

    // --- "increased": raise two values, decrease none ---
    f.put<uint32_t>(B + 0x100, 1001);
    f.put<uint32_t>(B + 0x2000, 2000);
    // (B+0x800 is still 1234 > 1000 as well)
    sc.next_scan(NextScan::Increased, v1, v2, prog);
    CHECK(sc.result_count() == 3);

    // --- between 1500 and 3000 keeps only B+0x2000 (=2000) ---
    parse_value(ScanType::U32, "1500", &v1);
    parse_value(ScanType::U32, "3000", &v2);
    sc.next_scan(NextScan::Between, v1, v2, prog);
    CHECK(sc.result_count() == 1);
    CHECK(sc.results(0, 1)[0].addr == B + 0x2000);

    // --- unknown first scan + decreased ---
    Scanner sc2(src);
    ScanProgress p2;
    sc2.first_scan(opt, FirstScan::Unknown, v1, v2, p2);
    CHECK(sc2.result_count() == 0);     // not materialised yet
    f.put<uint32_t>(B + 0x2000, 1);     // this one drops
    sc2.next_scan(NextScan::Decreased, v1, v2, p2);
    CHECK(sc2.result_count() >= 1);
    {
        bool found = false;
        auto r = sc2.results(0, sc2.result_count());
        for (auto& e : r) if (e.addr == B + 0x2000) found = true;
        CHECK(found);
    }

    // --- float scan ---
    auto fs = std::make_shared<FakeSource>(B, 0x1000);
    fs->put<float>(B + 0x10, 3.5f);
    fs->put<float>(B + 0x20, 99.0f);
    Scanner scf(fs);
    ScanOptions of; of.type = ScanType::F32;
    ScanProgress pf;
    Value fv1, fv2;
    parse_value(ScanType::F32, "3.5", &fv1);
    scf.first_scan(of, FirstScan::Exact, fv1, fv2, pf);
    CHECK(scf.result_count() == 1);
    CHECK(scf.results(0, 1)[0].addr == B + 0x10);

    // --- the live w22 cases (Wonder): a float exact scan keeps only the typed value (1000.21 and 999.586 were kept with a 1e-3 tolerance);
    //     "changed" sees a float go from 1012.5 to 1013.5; a byte holding 113 is "smaller than 200" (200 is above a signed byte's range) ---
    {
        auto ws = std::make_shared<FakeSource>(B, 0x1000);
        ws->put<float>(B + 0x10, 1000.5f);
        ws->put<float>(B + 0x20, 1000.21f);
        ws->put<float>(B + 0x30, 999.586f);
        ws->put<float>(B + 0x40, 6.627e8f);          // a large float that never changes: never "decreased by 10" (w23 kept 2,000,000 such)
        Scanner sw(ws);
        ScanOptions ow; ow.type = ScanType::F32;
        ScanProgress pw;
        Value a, b;
        parse_value(ScanType::F32, "1000.5", &a);
        sw.first_scan(ow, FirstScan::Exact, a, b, pw);
        CHECK(sw.result_count() == 1);
        ws->put<float>(B + 0x10, 1013.5f);
        sw.first_scan(ow, FirstScan::Unknown, a, b, pw);
        ws->put<float>(B + 0x10, 1003.5f);
        parse_value(ScanType::F32, "10", &a);
        sw.next_scan(NextScan::DecreasedBy, a, b, pw);
        CHECK(sw.result_count() == 1);              // only the word that went 1013.5 -> 1003.5
        ws->put<float>(B + 0x10, 1012.5f);
        sw.next_scan(NextScan::Changed, a, b, pw);
        CHECK(sw.result_count() == 1);
        ws->put<float>(B + 0x10, 1013.5f);
        sw.next_scan(NextScan::Changed, a, b, pw);
        CHECK(sw.result_count() == 1);
        sw.next_scan(NextScan::Unchanged, a, b, pw);
        CHECK(sw.result_count() == 1);
        parse_value(ScanType::F32, "20", &a);
        ws->put<float>(B + 0x10, 1033.5f);
        sw.next_scan(NextScan::IncreasedBy, a, b, pw);
        CHECK(sw.result_count() == 1);

        auto bs = std::make_shared<FakeSource>(B, 0x100);
        bs->put<uint8_t>(B + 0x10, 113);
        bs->put<uint8_t>(B + 0x11, 230);
        Scanner sb(bs);
        ScanOptions ob; ob.type = ScanType::U8;
        ScanProgress pb;
        parse_value(ScanType::U8, "112", &a);
        parse_value(ScanType::U8, "114", &b);
        sb.first_scan(ob, FirstScan::Between, a, b, pb);
        CHECK(sb.result_count() == 1);
        parse_value(ScanType::U8, "200", &a);
        sb.next_scan(NextScan::Less, a, b, pb);
        CHECK(sb.result_count() == 1);              // 113 < 200
        parse_value(ScanType::U8, "200", &a);
        sb.first_scan(ob, FirstScan::Greater, a, b, pb);
        CHECK(sb.result_count() == 1);              // only 230 (as an unsigned byte); 0 bytes and 113 are not
        CHECK(sb.results(0, 1)[0].addr == B + 0x11);
        parse_value(ScanType::U8, "-26", &a);
        sb.first_scan(ob, FirstScan::Exact, a, b, pb);
        CHECK(sb.result_count() == 1);              // a negative reference compares signed: 230 is the signed byte -26
    }

    // --- alignment: an unaligned value is found only with byte alignment ---
    auto us = std::make_shared<FakeSource>(B, 0x1000);
    us->put<uint32_t>(B + 0x11, 4242);   // offset 0x11, not 4-aligned
    Scanner scu(us);
    ScanOptions oa; oa.type = ScanType::U32; oa.fast = true;   // 4-aligned
    ScanProgress pa;
    Value av1, av2;
    parse_value(ScanType::U32, "4242", &av1);
    scu.first_scan(oa, FirstScan::Exact, av1, av2, pa);
    CHECK(scu.result_count() == 0);      // aligned scan misses it
    oa.fast = false; oa.alignment = 1;   // every byte
    scu.first_scan(oa, FirstScan::Exact, av1, av2, pa);
    CHECK(scu.result_count() == 1);

    // --- all numeric types: 100 as a u8, u16, u32, float and double in one scan ---
    {
        auto as = std::make_shared<FakeSource>(B, 0x1000);
        as->put<uint8_t>(B + 0x101, 100);      // u8 at an odd address
        as->put<uint16_t>(B + 0x202, 100);
        as->put<uint32_t>(B + 0x304, 100);
        as->put<float>(B + 0x408, 100.0f);
        as->put<double>(B + 0x510, 100.0);
        Scanner sa(as);
        ScanOptions o; o.kind = ScanKind::AllNumeric; o.type = ScanType::U32; o.text = "100";
        ScanProgress p;
        Value a1, a2;
        parse_value(ScanType::U32, "100", &a1);
        sa.first_scan(o, FirstScan::Exact, a1, a2, p);
        auto r = sa.results(0, 100);
        bool u8 = false, u16 = false, u32 = false, f32 = false, f64 = false;
        for (auto& e : r) {
            if (e.addr == B + 0x101 && e.type == ScanType::U8) u8 = true;
            if (e.addr == B + 0x202 && e.type == ScanType::U16) u16 = true;
            if (e.addr == B + 0x304 && e.type == ScanType::U32) u32 = true;
            if (e.addr == B + 0x408 && e.type == ScanType::F32) f32 = true;
            if (e.addr == B + 0x510 && e.type == ScanType::F64) f64 = true;
        }
        CHECK(u8 && u16 && u32 && f32 && f64);
        // next scan: the u32 drops by 5, the others stay; "decreased by 5" keeps only it, in its own type
        as->put<uint32_t>(B + 0x304, 95);
        Value d5, dz; parse_value(ScanType::U32, "5", &d5);
        sa.next_scan(NextScan::DecreasedBy, d5, dz, p);
        // the u32 at 0x304, and the u8 / u16 that start at the same byte (100 -> 95 in each width): only that address is left
        auto left = sa.results(0, 10);
        bool have32 = false, only304 = !left.empty();
        for (auto& e : left) { have32 |= e.type == ScanType::U32; only304 &= e.addr == B + 0x304; }
        CHECK(have32 && only304 && left.size() == 3);
        sa.undo();
        CHECK(sa.result_count() == r.size());
    }

    // --- text: UTF-8 (case-insensitive), UTF-16, straddling a chunk boundary ---
    {
        auto ts = std::make_shared<FakeSource>(B, 0x200000);
        const char* hello = "Hello Mario";
        std::memcpy(ts->mem.data() + 0x40, hello, std::strlen(hello));
        std::memcpy(ts->mem.data() + 0xFFFFC, "MARIO", 5);           // crosses the 1 MiB chunk edge
        const char16_t* w = u"Coins";
        std::memcpy(ts->mem.data() + 0x1000, w, 10);
        Scanner st(ts);
        ScanOptions o; o.kind = ScanKind::Utf8; o.text = "mario"; o.ignore_case = true;
        ScanProgress p;
        Value z;
        st.first_scan(o, FirstScan::Exact, z, z, p);
        CHECK(st.result_count() == 2);
        auto r = st.results(0, 10);
        CHECK(r.size() == 2 && r[0].addr == B + 0x46 && r[1].addr == B + 0xFFFFC && r[0].len == 5);
        o.ignore_case = false;
        st.first_scan(o, FirstScan::Exact, z, z, p);
        CHECK(st.result_count() == 0);
        ScanOptions o16; o16.kind = ScanKind::Utf16; o16.text = "Coins";
        st.first_scan(o16, FirstScan::Exact, z, z, p);
        CHECK(st.result_count() == 1 && st.results(0, 1)[0].addr == B + 0x1000 && st.match_len() == 10);
        // the text changes: "changed" keeps it, "exact" (still the text) drops it
        ts->mem[0x1000] = 'K';
        st.next_scan(NextScan::Changed, z, z, p);
        CHECK(st.result_count() == 1);
        st.next_scan(NextScan::Exact, z, z, p);
        CHECK(st.result_count() == 0);
    }

    // --- array of bytes with wildcards ---
    {
        auto bs = std::make_shared<FakeSource>(B, 0x1000);
        const uint8_t code[] = {0x1F, 0x20, 0x03, 0xD5, 0x08, 0x05, 0x00, 0x51};
        std::memcpy(bs->mem.data() + 0x300, code, sizeof code);
        Scanner sb(bs);
        ScanOptions o; o.kind = ScanKind::Aob; o.text = "1F 20 03 D5 ?8 ?? 00 51";
        ScanProgress p;
        Value z;
        sb.first_scan(o, FirstScan::Exact, z, z, p);
        CHECK(sb.result_count() == 1 && sb.results(0, 1)[0].addr == B + 0x300);
        o.text = "zz";
        sb.first_scan(o, FirstScan::Exact, z, z, p);
        CHECK(sb.result_count() == 0);
    }

    // --- unknown initial value, then decreased (the classic "lives went down") ---
    {
        auto ks = std::make_shared<FakeSource>(B, 0x1000);
        ks->put<uint32_t>(B + 0x80, 5);
        ks->put<uint32_t>(B + 0x84, 9);
        Scanner sk(ks);
        ScanOptions o; o.type = ScanType::U32;
        ScanProgress p;
        Value z;
        sk.first_scan(o, FirstScan::Unknown, z, z, p);
        ks->put<uint32_t>(B + 0x80, 4);
        sk.next_scan(NextScan::Decreased, z, z, p);
        CHECK(sk.result_count() == 1 && sk.results(0, 1)[0].addr == B + 0x80);
        CHECK(sk.results(0, 1)[0].first.as_int() == 5);
    }

    if (g_fail == 0) std::printf("ALL SCANNER TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
