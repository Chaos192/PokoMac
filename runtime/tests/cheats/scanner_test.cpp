// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test of the memory scanner (runtime/src/cheats/scanner.*) against a fake memory source. No runtime dependency: it provides
// its own MemSource, and gmem is stubbed out (scanner.cpp's GmemSource / live_source are not exercised here). The large-region case spills
// an unknown value's snapshot to a file in argv[1] (default: the current folder); the file is gone once the search is cleared.
#define _CRT_SECURE_NO_WARNINGS
#include "scanner.h"
#include "gmem.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

// Stub the two gmem functions scanner.cpp's live source references, so the test links without the runtime (its GmemSource is not used here).
namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"

using namespace nxrt::cheats;

#if defined(_WIN32)
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

// The process's memory as the memory guard counts it (Windows: private commit; macOS: the footprint), now and at its peak; 0 elsewhere.
static uint64_t process_memory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pm{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) return pm.PrivateUsage;
#elif defined(__APPLE__)
    task_vm_info_data_t vi{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &n) == KERN_SUCCESS) return vi.phys_footprint;
#endif
    return 0;
}
static uint64_t process_memory_peak() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pm{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) return pm.PeakPagefileUsage;
#elif defined(__APPLE__)
    task_vm_info_data_t vi{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &n) == KERN_SUCCESS) return vi.ledger_phys_footprint_peak;
#endif
    return 0;
}

static std::string g_spill_dir = ".";

// A large sparse source: `size` bytes of address space in which one page in `every` holds data (a nonzero pattern of the address); the rest
// were never touched (zeros, and untouched() says so, as the live source's mincore does on macOS). set() changes a u32; in a page never
// touched it touches that page (zeros around the value).
constexpr uint64_t kBigPage = 16384;
struct BigSource : MemSource {
    uint64_t base, size, every;
    std::vector<uint8_t> mode;            // per page: 0 never touched, 1 the pattern, 2 touched later (zeros)
    std::map<uint64_t, uint32_t> words;   // set() values
    std::atomic<uint64_t> untouched_reads{0};
    BigSource(uint64_t b, uint64_t n, uint64_t e) : base(b), size(n), every(e), mode(static_cast<size_t>(n / kBigPage), 0) {
        for (size_t i = 0; i < mode.size(); i += static_cast<size_t>(e)) mode[i] = 1;
    }
    uint64_t data_bytes() const { return (mode.size() + every - 1) / every * kBigPage; }
    static uint32_t pattern(uint64_t a) { return static_cast<uint32_t>(((a * 0x9E3779B97F4A7C15ull) >> 40) & 0x0FFFFFFF) | 1; }
    size_t page_of(uint64_t a) const { return static_cast<size_t>((a - base) / kBigPage); }
    bool is_touched(uint64_t a) const { return mode[page_of(a)] != 0; }
    uint32_t word(uint64_t a) const {
        auto it = words.find(a);
        if (it != words.end()) return it->second;
        return mode[page_of(a)] == 1 ? pattern(a) : 0;
    }
    void set(uint64_t a, uint32_t v) {
        uint8_t& m = mode[page_of(a)];
        if (!m) m = 2;
        words[a] = v;
    }
    std::vector<ScanRegion> regions(const RegionFilter&) override { return {{base, size, 3, "heap"}}; }
    bool read(uint64_t a, void* d, size_t n) override {
        if (a < base || a + n > base + size) return false;
        uint8_t* out = static_cast<uint8_t*>(d);
        for (uint64_t q = a; q < a + n;) {
            const size_t pi = page_of(q);
            const uint64_t pe = std::min<uint64_t>(a + n, base + (pi + 1) * kBigPage);
            if (mode[pi] == 0) untouched_reads += pe - q;
            if (mode[pi] == 1) {
                for (uint64_t x = q; x < pe; ++x) out[x - a] = static_cast<uint8_t>(pattern(x & ~uint64_t{3}) >> (8 * (x & 3)));
            } else {
                std::memset(out + (q - a), 0, static_cast<size_t>(pe - q));
            }
            q = pe;
        }
        for (auto it = words.lower_bound(a >= 3 ? a - 3 : 0); it != words.end() && it->first < a + n; ++it)
            for (uint64_t k = 0; k < 4; ++k) {
                const uint64_t x = it->first + k;
                if (x >= a && x < a + n) out[x - a] = static_cast<uint8_t>(it->second >> (8 * k));
            }
        return true;
    }
    bool untouched(uint64_t a, size_t n, size_t* page, std::vector<uint8_t>* flags) override {
        const uint64_t lo = a & ~(kBigPage - 1), hi = (a + n + kBigPage - 1) & ~(kBigPage - 1);
        flags->assign(static_cast<size_t>((hi - lo) / kBigPage), 0);
        for (size_t i = 0; i < flags->size(); ++i) {
            const uint64_t pa = lo + i * kBigPage;
            (*flags)[i] = pa >= base && pa < base + size && mode[page_of(pa)] == 0;
        }
        *page = static_cast<size_t>(kBigPage);
        return true;
    }
};

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

int main(int argc, char** argv) {
    if (argc > 1) g_spill_dir = argv[1];     // where the large region's snapshot spills (the build folder by default)
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
    CHECK(sc2.result_count() == 0x10000 / 4);     // every u32 slot is a candidate (a bit over the snapshot, not a list entry)
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

    // --- unknown value with few slots allowed in a list (result_cap 100): the candidates stay bits over the snapshot until a next scan
    //     leaves 100 or fewer; undo returns to the snapshot; "same as first" reads the first scan's bytes ---
    {
        auto ks = std::make_shared<FakeSource>(B, 0x10000);
        ks->put<uint32_t>(B + 0x100, 7);
        Scanner sk(ks);
        ScanOptions o; o.type = ScanType::U32; o.result_cap = 100;
        ScanProgress p;
        Value z;
        sk.first_scan(o, FirstScan::Unknown, z, z, p);
        CHECK(sk.result_count() == 0x10000 / 4);
        sk.next_scan(NextScan::Unknown, z, z, p);               // everything stays: still the snapshot, its last values now separate
        CHECK(sk.result_count() == 0x10000 / 4);
        {
            auto r = sk.results(0x40, 2);                       // the 0x40th slot on: B+0x100 holds 7 (first and now)
            CHECK(r.size() == 2 && r[0].addr == B + 0x100 && r[0].first.as_int() == 7 && r[0].last.as_int() == 7 && r[1].addr == B + 0x104);
        }
        ks->put<uint32_t>(B + 0x100, 9);
        ks->put<uint32_t>(B + 0x200, 3);
        sk.next_scan(NextScan::Changed, z, z, p);               // two left: a list
        CHECK(sk.result_count() == 2);
        {
            auto r = sk.results(0, 10);
            CHECK(r.size() == 2 && r[0].addr == B + 0x100 && r[0].first.as_int() == 7 && r[0].last.as_int() == 9);
            CHECK(r.size() == 2 && r[1].addr == B + 0x200 && r[1].first.as_int() == 0 && r[1].last.as_int() == 3);
        }
        sk.undo();                                              // back to the snapshot
        CHECK(sk.result_count() == 0x10000 / 4);
        ks->put<uint32_t>(B + 0x100, 7);                        // back to its first value; B+0x200 stays 3
        sk.next_scan(NextScan::SameAsFirst, z, z, p);
        CHECK(sk.result_count() == 0x10000 / 4 - 1);
        {
            auto r = sk.results(0, sk.result_count());
            bool a = false, b = false, ordered = true;
            for (size_t k = 0; k < r.size(); ++k) {
                a |= r[k].addr == B + 0x100;
                b |= r[k].addr == B + 0x200;
                if (k && r[k].addr <= r[k - 1].addr) ordered = false;
            }
            CHECK(r.size() == 0x10000 / 4 - 1 && a && !b && ordered);
        }
        ks->put<uint32_t>(B + 0x3000, 1);
        sk.next_scan(NextScan::Increased, z, z, p);
        CHECK(sk.result_count() == 1 && sk.results(0, 1)[0].addr == B + 0x3000 && sk.results(0, 1)[0].first.as_int() == 0);
        sk.clear();
        CHECK(sk.result_count() == 0 && !sk.can_undo() && sk.snapshot_stats().bytes == 0);
    }

    // --- all numeric types, unknown value: a u32 that drops from 100 to 95 is found in every type that covers it, in address order ---
    {
        auto as = std::make_shared<FakeSource>(B, 0x1000);
        as->put<uint32_t>(B + 0x304, 100);
        Scanner sa(as);
        ScanOptions o; o.kind = ScanKind::AllNumeric; o.type = ScanType::U32;
        ScanProgress p;
        Value z;
        sa.first_scan(o, FirstScan::Unknown, z, z, p);
        CHECK(sa.result_count() == 0x1000 + 0x800 + 0x400 + 0x200 + 0x400 + 0x200);   // u8, u16, u32, u64, f32, f64 slots
        as->put<uint32_t>(B + 0x304, 95);
        sa.next_scan(NextScan::Decreased, z, z, p);
        auto r = sa.results(0, 10);
        CHECK(r.size() == 6);
        bool ordered = true, u32 = false;
        for (size_t k = 0; k < r.size(); ++k) {
            if (k && (r[k].addr < r[k - 1].addr || (r[k].addr == r[k - 1].addr && r[k].type <= r[k - 1].type))) ordered = false;
            if (r[k].addr == B + 0x304 && r[k].type == ScanType::U32 && r[k].first.as_int() == 100 && r[k].last.as_int() == 95) u32 = true;
        }
        CHECK(ordered && u32 && r.size() == 6 && r[0].addr == B + 0x300 && r[0].type == ScanType::U64);
    }

    // --- a large region: an unknown u32 over 8 GiB of address space, as Pokopia's 11 GB heap (11-12: "scan u32 unknown" copied all of it
    //     and took the process from 8.2 GB to 19 GB). Untouched pages are never read; the snapshot keeps 16 MiB in RAM and spills the rest
    //     to its file; the process grows by about the budget, not by the region; the values that come back from the file are right ---
    {
        const uint64_t size = uint64_t{8} << 30;
        auto bs = std::make_shared<BigSource>(B, size, 256);     // one page in 256 holds data: 32 MiB
        Scanner sb(bs);
        ScanOptions o; o.type = ScanType::U32; o.snapshot_ram = uint64_t{16} << 20; o.spill_dir = g_spill_dir;
        ScanProgress p;
        Value z;
        const uint64_t mem0 = process_memory();
        sb.first_scan(o, FirstScan::Unknown, z, z, p);
        const SnapshotStats s1 = sb.snapshot_stats();
        std::printf("large region: first scan: %llu candidates; snapshot of %llu MiB: %llu MiB zero, %llu MiB RAM (budget %llu), %llu MiB in %s; "
                    "untouched bytes read %llu\n", (unsigned long long)sb.result_count(), (unsigned long long)(s1.bytes >> 20),
                    (unsigned long long)(s1.zero >> 20), (unsigned long long)(s1.ram >> 20), (unsigned long long)(s1.ram_budget >> 20),
                    (unsigned long long)(s1.file >> 20), s1.file_path.c_str(), (unsigned long long)bs->untouched_reads.load());
        CHECK(sb.error().empty());
        CHECK(sb.result_count() == size / 4);
        CHECK(bs->untouched_reads.load() == 0);
        CHECK(s1.bytes == size && s1.zero == size - bs->data_bytes());
        CHECK(s1.ram <= o.snapshot_ram && s1.ram_peak <= o.snapshot_ram);
        CHECK(s1.file == bs->data_bytes() - s1.ram);             // the rest is in the file
        // values change: in data pages (most of them past the RAM budget, so in the file) and one in a page never touched before
        const uint64_t at[] = {B + 0x40, B + (uint64_t{1} << 30) + 0x80, B + (uint64_t{5} << 30) + 0x1234, B + size - kBigPage * 256 + 0x10};
        uint32_t was[4];
        for (int k = 0; k < 4; ++k) { was[k] = bs->word(at[k]); bs->set(at[k], was[k] + 5); }
        const uint64_t fresh = B + (uint64_t{3} << 30) + kBigPage + 8;   // the page after 3 GiB: untouched
        CHECK(!bs->is_touched(fresh));
        bs->set(fresh, 77);
        sb.next_scan(NextScan::Unchanged, z, z, p);               // nearly every slot: still the snapshot
        CHECK(sb.result_count() == size / 4 - 5);
        CHECK(sb.error().empty());
        sb.undo();
        sb.next_scan(NextScan::Increased, z, z, p);               // five: a list
        CHECK(sb.result_count() == 5);
        {
            auto r = sb.results(0, 10);
            bool ok = r.size() == 5;
            for (int k = 0; k < 4 && ok; ++k) {
                bool hit = false;
                for (auto& e : r) hit |= e.addr == at[k] && (uint32_t)e.first.bits == was[k] && (uint32_t)e.last.bits == was[k] + 5;
                ok = hit;
            }
            bool hit = false;
            for (auto& e : r) hit |= e.addr == fresh && e.first.bits == 0 && e.last.bits == 77;
            CHECK(ok && hit);
        }
        sb.undo();                                               // the unchanged step and the snapshot again
        CHECK(sb.result_count() == size / 4);
        const uint64_t mem1 = process_memory_peak();
        const SnapshotStats s2 = sb.snapshot_stats();
        std::printf("large region: process memory %llu MiB before, peak %llu MiB; snapshot RAM peak %llu MiB, file peak %llu MiB\n",
                    (unsigned long long)(mem0 >> 20), (unsigned long long)(mem1 >> 20), (unsigned long long)(s2.ram_peak >> 20),
                    (unsigned long long)(s2.file_peak >> 20));
        CHECK(s2.ram_peak <= o.snapshot_ram);
        if (mem0 && mem1) CHECK(mem1 < mem0 + (uint64_t{160} << 20));   // the budget and the workers' chunks, not 8 GiB
        const std::string spill = s2.file_path;
        sb.clear();
        const SnapshotStats s3 = sb.snapshot_stats();
        CHECK(s3.bytes == 0 && s3.ram == 0 && s3.file == 0);
        std::error_code ec;
        CHECK(!spill.empty() && !std::filesystem::exists(std::filesystem::path(reinterpret_cast<const char8_t*>(spill.c_str())), ec));   // the spill file is gone
    }

    // --- a spill folder that is not there (a game's cheats folder before its first cheat): the snapshot spills to the temp folder; the
    //     values come back from it ---
    {
        auto ks = std::make_shared<FakeSource>(B, 0x400000);
        for (uint64_t a = 0; a < 0x400000; a += 4) ks->put<uint32_t>(B + a, static_cast<uint32_t>(a | 1));
        Scanner sk(ks);
        ScanOptions o; o.type = ScanType::U32; o.snapshot_ram = 1 << 20; o.spill_dir = g_spill_dir + "/no such folder/x";
        ScanProgress p;
        Value z;
        sk.first_scan(o, FirstScan::Unknown, z, z, p);
        const SnapshotStats ss = sk.snapshot_stats();
        std::printf("spill folder not there: %s%s\n", ss.file_path.c_str(), sk.error().c_str());
        CHECK(sk.error().empty() && sk.result_count() == 0x400000 / 4 && ss.file > 0 && ss.file_path.find("no such folder") == std::string::npos);
        ks->put<uint32_t>(B + 0x3FFFF0, 1);                    // was 0x3FFFF1 (three of the four chunks are in the file)
        sk.next_scan(NextScan::Decreased, z, z, p);
        CHECK(sk.result_count() == 1 && sk.results(0, 1)[0].addr == B + 0x3FFFF0 && sk.results(0, 1)[0].first.bits == 0x3FFFF1);
    }

    if (g_fail == 0) std::printf("ALL SCANNER TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
