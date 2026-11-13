// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Standalone test of the cheat VM's write router (runtime/src/cheats/vmcode.*): the real VM (cheat_vm.cpp) runs Atmosphère cheats against a
// fake game (a module with three lifted functions, the padding after its code, data after it) whose code patcher only records what it is
// asked. Checks: data writes go to memory at once and touch nothing else (no place lookup, no patch: the "nothing changes" proof); code
// writes become one live patch per changed word at the end of the pass, cave bytes are written before the patch that branches there; a
// cheat turned off puts its code back, and its cave once that code has settled; shared words, words written back to their original bytes,
// refused patches, a cheat turned off during a pass, a reload, reads of code written earlier in the pass, byte writes into code, text the
// build did not lift, and a cave turned on again before it went back. The Z-A shapes (a "Restore Code" cheat writing every original word,
// register-offset writes, 8-byte cave writes at the end of the text page) are covered by the synthetic Z-A-style file below.
//
// Optional: nxrt_vmcode_test --file <cheats.txt> --nso <uncompressed main NSO> runs every cheat of a real file over that main's code (one
// fake function per 0x40 bytes): each one on for two passes, then off; every code and cave byte must be back to the NSO's afterwards.
#define _CRT_SECURE_NO_WARNINGS
#include "cheat_vm.h"
#include "vmcode.h"

#include "cheat_vm.cpp"
#include "vmcode.cpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace nxrt::cheats;
using namespace nxrt::cheats::vmcode;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const uint64_t kMain = 0x0800000000ull, kHeap = 0x2000000000ull;

struct FakeSink : Sink {
    std::map<uint64_t, uint8_t> mem;                 // mapped bytes; anything else fails
    uint64_t base = kMain, text_end = kMain + 0x2480, cave_end = kMain + 0x3000;
    std::vector<uint64_t> fns{kMain + 0x0, kMain + 0x1000, kMain + 0x2000};   // lifted function starts (sorted)
    std::set<uint64_t> unlifted;                     // words in the text that no lifted function holds
    std::set<uint64_t> refuse;                       // words the patcher refuses
    std::map<uint64_t, uint32_t> cp_orig;            // the patcher's own record of each patched word's original
    std::vector<std::string> calls;                  // what the router asked, in order
    bool settled_all = true;
    size_t place_calls = 0, maybe_calls = 0, writes = 0;
    const std::string* image = nullptr;              // --nso: main's text read from the file where nothing was written
    uint64_t image_text = 0;                         // its text's file offset

    bool byte_at(uint64_t a, uint8_t* v) const {
        if (auto it = mem.find(a); it != mem.end()) { *v = it->second; return true; }
        if (image && a >= base && a < cave_end) { *v = a < text_end ? uint8_t((*image)[image_text + (a - base)]) : 0; return true; }
        return false;
    }
    uint32_t get32(uint64_t a) const {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) { uint8_t b = 0; byte_at(a + i, &b); v |= uint32_t(b) << (8 * i); }
        return v;
    }
    void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) mem[a + i] = uint8_t(v >> (8 * i)); }

    bool maybe_code(uint64_t a, size_t n) override { ++maybe_calls; return a + n > base && a < cave_end; }
    Place place(uint64_t a) override {
        ++place_calls;
        if (a < base || a >= cave_end) return {};
        if (a >= text_end) return {Kind::Cave, text_end, cave_end};
        if (unlifted.count(a & ~3ull)) return {};
        const size_t k = size_t(std::upper_bound(fns.begin(), fns.end(), a) - fns.begin()) - 1;
        return {Kind::Code, fns[k], k + 1 < fns.size() ? fns[k + 1] : text_end};
    }
    bool read(uint64_t a, void* d, size_t n) override {
        uint8_t* p = static_cast<uint8_t*>(d);
        for (size_t i = 0; i < n; ++i) if (!byte_at(a + i, &p[i])) return false;
        return true;
    }
    bool write(uint64_t a, const void* s, size_t n) override {
        uint8_t dummy = 0;
        for (size_t i = 0; i < n; ++i) if (!byte_at(a + i, &dummy)) return false;
        const uint8_t* p = static_cast<const uint8_t*>(s);
        for (size_t i = 0; i < n; ++i) mem[a + i] = p[i];
        ++writes;
        return true;
    }
    void patch(const std::vector<std::pair<uint64_t, uint32_t>>& words, std::vector<std::string>* errs) override {
        errs->assign(words.size(), std::string());
        for (size_t i = 0; i < words.size(); ++i) {
            const uint64_t w = words[i].first;
            if (refuse.count(w)) { (*errs)[i] = "refused"; continue; }
            if (!cp_orig.count(w)) cp_orig[w] = get32(w);
            put32(w, words[i].second);
            char b[64];
            std::snprintf(b, sizeof b, "patch %llx=%08x", (unsigned long long)(w - base), words[i].second);
            calls.push_back(b);
        }
    }
    void unpatch(const std::vector<uint64_t>& words) override {
        for (uint64_t w : words) {
            char b[64];
            std::snprintf(b, sizeof b, "unpatch %llx", (unsigned long long)(w - base));
            calls.push_back(b);
            if (auto it = cp_orig.find(w); it != cp_orig.end()) { put32(w, it->second); cp_orig.erase(it); }
        }
    }
    bool settled(uint64_t) override { return settled_all; }
    std::set<uint64_t> making;                       // words whose patch is not swapped in yet
    bool applying(uint64_t w) override { return making.count(w) != 0; }
    std::atomic<bool> cave_blocks{false}, in_cave{false}, release{false};   // a cave write waits (the live sink: an on-demand compile)
    void cave_written(uint64_t clo, uint64_t chi, uint64_t lo, uint64_t hi) override {
        if (cave_blocks) {
            in_cave = true;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            while (!release && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            in_cave = false;
        }
        char b[96];
        std::snprintf(b, sizeof b, "cave %llx..%llx in %llx..%llx", (unsigned long long)(lo - base), (unsigned long long)(hi - base),
                      (unsigned long long)(clo - base), (unsigned long long)(chi - base));
        calls.push_back(b);
    }
};

// The engine's host (engine.cpp LiveHost): code-range writes through the router, the rest straight to memory.
struct Host : VmHost {
    Router* r = nullptr;
    FakeSink* s = nullptr;
    const Vm* vm = nullptr;
    uint64_t keys = 0;
    bool read(uint64_t a, void* d, size_t n) override { return r && r->pending() ? r->read(a, d, n) : s->read(a, d, n); }
    bool write(uint64_t a, const void* src, size_t n) override {
        if (r && r->maybe_code(a, n)) return r->write(a, src, n, vm->writing_entry());
        return s->write(a, src, n);
    }
    uint64_t base(unsigned t) override { return (t & 0xF) == 1 ? kHeap : (t & 0xF) == 4 ? 0 : kMain; }
    uint64_t keys_held() override { return keys; }
};

// One game: memory, the router, the VM and the file whose entries are toggled as the engine does (load + entry_off / entry_on).
struct Game {
    FakeSink s;
    Router r{s};
    Vm vm;
    Host h;
    CheatFile f;
    explicit Game(const std::string& text) {
        h.r = &r; h.s = &s; h.vm = &vm;
        for (uint64_t a = s.base; a < s.text_end; a += 4) s.put32(a, 0xAA000000u | uint32_t((a - s.base) >> 2));   // distinct code words
        for (uint64_t a = s.text_end; a < s.cave_end; a += 4) s.put32(a, 0);                                        // the zero padding
        for (uint64_t a = kMain + 0x10000; a < kMain + 0x11000; a += 4) s.put32(a, 0);                              // data
        for (uint64_t a = kHeap; a < kHeap + 0x1000; a += 4) s.put32(a, 0);
        f = ParseFile(text);
        CHECK(f.ok);
        for (size_t k = 1; k < kMaxEntries; ++k) f.entries[k].enabled = false;   // the parser's default is on (a console's): all off, as toggles.txt
    }
    void set(int entry, bool on) {
        f.entries[entry].enabled = on;
        if (on) r.entry_on(entry); else r.entry_off(entry);
    }
    void pass() {
        vm.load(f);
        if (vm.loaded() && vm.program_words()) r.begin_pass();
        vm.run(h);
        r.end_pass();
    }
    uint32_t orig(uint64_t off) const { return 0xAA000000u | uint32_t(off >> 2); }
    uint32_t at(uint64_t off) const { return s.get32(kMain + off); }
    std::string calls() const { std::string out; for (auto& c : s.calls) out += c + "; "; return out; }
};

static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t((int64_t(to) - int64_t(from)) >> 2) & 0x3FFFFFFu); }
static std::string hex8(uint32_t v) { char b[12]; std::snprintf(b, sizeof b, "%08X", v); return b; }

static void test_data_only() {
    // data cheats: a static write into main's data, a pointer chain into the heap; the same memory with and without the router
    const std::string text = "[Coins]\n04000000 00010000 0000270F\n"
                             "[Ptr]\n580F0000 00010100\n780F0000 00000010\n640F0000 00000000 00000063\n";
    Game g(text);
    g.s.put32(kMain + 0x10100, uint32_t(kHeap & 0xFFFFFFFF));
    g.s.put32(kMain + 0x10104, uint32_t(kHeap >> 32));
    g.set(1, true);
    g.set(2, true);
    for (int i = 0; i < 3; ++i) g.pass();
    CHECK(g.s.get32(kMain + 0x10000) == 9999);
    CHECK(g.s.get32(kHeap + 0x10) == 0x63);
    CHECK(g.s.calls.empty());
    CHECK(g.s.place_calls == 0);                      // no write was even looked up: each took the range test and went to memory
    const Counters c = g.r.counters();
    CHECK(c.code_writes == 0 && c.cave_writes == 0 && c.passes_with_code == 0 && c.words_patched == 0);
    CHECK(g.r.held().empty());
    // the same file through a plain host (no router): identical memory
    Game p(text);
    p.h.r = nullptr;
    p.s.put32(kMain + 0x10100, uint32_t(kHeap & 0xFFFFFFFF));
    p.s.put32(kMain + 0x10104, uint32_t(kHeap >> 32));
    p.f.entries[1].enabled = p.f.entries[2].enabled = true;
    for (int i = 0; i < 3; ++i) { p.vm.load(p.f); p.vm.run(p.h); }
    CHECK(p.s.mem == g.s.mem);
    CHECK(p.s.writes == g.s.writes);
}

// An ASM cheat as tomvita writes them (Pokopia 2.0.1's): the original word, then the branch over it into a cave in the padding after the
// code, then the cave (the displaced instruction, the new code, the branch back) in 8-byte writes.
static std::string hook_cheat(const char* name, uint64_t hook, uint64_t cave) {
    std::string t = std::string("[") + name + "]\n";
    t += "04000000 " + hex8(uint32_t(hook)) + " " + hex8(0xAA000000u | uint32_t(hook >> 2)) + "\n";
    t += "04000000 " + hex8(uint32_t(hook)) + " " + hex8(branch(hook, cave)) + "\n";
    t += "40000000 00000000 00000000\n";
    t += "08000000 " + hex8(uint32_t(cave)) + " 52800148 " + hex8(0xAA000000u | uint32_t(hook >> 2)) + "\n";   // displaced; mov w8, #10
    t += "08000000 " + hex8(uint32_t(cave + 8)) + " D503201F " + hex8(branch(cave + 4, hook + 4)) + "\n";       // b back; nop
    return t;
}

static void test_asm_on_off() {
    Game g(hook_cheat("Hook", 0x1010, 0x2500));
    g.set(1, true);
    g.pass();
    // one patch: the branch (not the original the cheat wrote first), after the cave it enters
    CHECK(g.s.calls.size() == 2);
    CHECK(g.s.calls.size() == 2 && g.s.calls[0] == "cave 2500..2510 in 2480..3000");
    CHECK(g.s.calls.size() == 2 && g.s.calls[1] == "patch 1010=" + std::string([&] { char b[12]; std::snprintf(b, sizeof b, "%08x", branch(0x1010, 0x2500)); return std::string(b); }()));
    CHECK(g.at(0x1010) == branch(0x1010, 0x2500));
    CHECK(g.at(0x2500) == g.orig(0x1010) && g.at(0x2504) == 0x52800148 && g.at(0x2508) == branch(0x2504, 0x1014) && g.at(0x250c) == 0xD503201F);
    Counters c = g.r.counters();
    CHECK(c.code_writes == 2 && c.cave_writes == 2 && c.words_patched == 1 && c.cave_words_written == 4);
    CHECK(g.r.held().size() == 5);
    for (const Held& h : g.r.held()) CHECK(h.owners == std::set<int>{1});
    // "applying...": while the patch of its word is being made, and only for the cheat that holds it
    CHECK(!g.r.applying(1));
    g.s.making.insert(kMain + 0x1010);
    CHECK(g.r.applying(1) && !g.r.applying(2));
    g.s.making.clear();
    // the next passes change nothing
    g.s.calls.clear();
    g.pass();
    g.pass();
    CHECK(g.s.calls.empty());
    // off while the code's swap is still on the way: the code goes back now, the cave waits
    g.s.settled_all = false;
    g.set(1, false);
    CHECK(g.calls() == "unpatch 1010; ");
    CHECK(g.at(0x1010) == g.orig(0x1010));
    CHECK(g.r.caves_waiting() == 4);
    CHECK(g.at(0x2504) == 0x52800148);                 // still there: a thread may still branch into it
    g.pass();
    CHECK(g.r.caves_waiting() == 4);
    g.s.settled_all = true;
    g.s.calls.clear();
    g.pass();
    CHECK(g.r.caves_waiting() == 0);
    CHECK(g.calls() == "cave 2500..2510 in 2480..3000; ");
    for (uint64_t a = 0x2500; a < 0x2510; a += 4) CHECK(g.at(a) == 0);
    CHECK(g.r.held().empty());
    c = g.r.counters();
    CHECK(c.words_reverted == 1 && c.cave_words_restored == 4);
}

static void test_cave_back_on_again() {
    Game g(hook_cheat("Hook", 0x1010, 0x2500));
    g.set(1, true);
    g.pass();
    g.s.settled_all = false;
    g.set(1, false);
    CHECK(g.r.caves_waiting() == 4);
    g.set(1, true);                                  // on again before the cave went back
    g.s.calls.clear();
    g.pass();
    CHECK(g.r.caves_waiting() == 0);
    CHECK(g.calls().find("patch 1010=") != std::string::npos);
    CHECK(g.calls().find("cave") == std::string::npos);   // the cave still held the cheat's bytes: nothing to write
    CHECK(g.r.held().size() == 5);
    g.s.settled_all = true;
    g.set(1, false);
    g.pass();
    for (uint64_t a = 0x2500; a < 0x2510; a += 4) CHECK(g.at(a) == 0);
    CHECK(g.at(0x1010) == g.orig(0x1010));
}

static void test_shared_and_written_back() {
    // two cheats patch the same word the same way: one off keeps it, both off put it back
    const std::string t = "[A]\n04000000 00001100 D503201F\n[B]\n04000000 00001100 D503201F\n"
                          // a keyed cheat: L patches, R writes the original back
                          "[Keyed]\n80000040\n04000000 00002100 D503201F\n20000000\n80000080\n04000000 00002100 AA000840\n20000000\n";
    Game g(t);
    g.set(1, true);
    g.set(2, true);
    g.pass();
    CHECK(g.at(0x1100) == 0xD503201F);
    CHECK(g.r.held().size() == 1 && g.r.held()[0].owners == (std::set<int>{1, 2}));
    g.set(2, false);
    CHECK(g.at(0x1100) == 0xD503201F);               // A still holds it
    g.pass();
    g.set(1, false);
    CHECK(g.at(0x1100) == g.orig(0x1100));
    g.set(3, true);
    g.h.keys = 0x40;
    g.pass();
    CHECK(g.at(0x2100) == 0xD503201F);
    g.h.keys = 0;
    g.pass();
    CHECK(g.at(0x2100) == 0xD503201F);               // no key: the code stays as the cheat left it (as on a console)
    g.h.keys = 0x80;
    g.s.calls.clear();
    g.pass();
    CHECK(g.calls() == "unpatch 2100; ");             // the cheat wrote the original back: the patch goes
    CHECK(g.at(0x2100) == g.orig(0x2100));
    CHECK(g.r.held().empty());
}

static void test_refused_and_unlifted() {
    Game g("[R]\n04000000 00001200 D503201F\n[U]\n04000000 00001300 D503201F\n");
    g.s.refuse.insert(kMain + 0x1200);
    g.s.unlifted.insert(kMain + 0x1300);
    g.set(1, true);
    g.set(2, true);
    g.pass();
    CHECK(g.at(0x1200) == 0xD503201F);               // refused: written as plain bytes, as before the router
    CHECK(g.at(0x1300) == 0xD503201F);               // not lifted code: a plain write at once
    CHECK(g.r.counters().patch_failures == 1);
    CHECK(g.r.held().size() == 1 && g.r.held()[0].plain);
    g.pass();
    CHECK(g.r.counters().patch_failures == 1);       // not asked again
    g.set(1, false);
    CHECK(g.at(0x1200) == g.orig(0x1200));           // plain bytes put back
    CHECK(g.calls().find("unpatch") == std::string::npos);
}

static void test_revoked_in_pass_and_reset() {
    Game g("[A]\n04000000 00001400 D503201F\n");
    g.set(1, true);
    g.vm.load(g.f);
    g.r.begin_pass();
    g.vm.run(g.h);
    g.r.entry_off(1);                                // turned off while the pass ran
    g.r.end_pass();
    CHECK(g.s.calls.empty());
    CHECK(g.at(0x1400) == g.orig(0x1400));
    g.r.entry_on(1);
    g.pass();
    CHECK(g.at(0x1400) == 0xD503201F);
    // a reload: every word back; a pass begun before it settles nothing
    g.vm.load(g.f);
    g.r.begin_pass();
    g.vm.run(g.h);
    g.r.reset();
    g.r.end_pass();
    CHECK(g.at(0x1400) == g.orig(0x1400));
    CHECK(g.r.held().empty());
    g.pass();
    CHECK(g.at(0x1400) == 0xD503201F);
}

// The cheats overlay reads the router every frame (applying, held, counters) and toggles cheats from the render thread. A pass's sink work
// can wait (the live sink's cave_written waits for the on-demand compiler's make lock, held through a compile of a second or more):
// Pokopia had frames of 217-293 ms when a cheat's cave was written while another cheat's code was compiling. With the cave write
// held for up to 800 ms, every reader and a toggle must answer at once, and a cheat turned off meanwhile must still put everything back.
static double ms_of(const std::function<void()>& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void test_readers_never_wait_for_the_sink() {
    Game g(hook_cheat("Hook", 0x1010, 0x2500));
    g.set(1, true);
    g.s.cave_blocks = true;
    std::thread pass([&] { g.pass(); });
    for (int i = 0; i < 2000 && !g.s.in_cave; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g.s.in_cave);
    double worst = 0;
    worst = std::max(worst, ms_of([&] { (void)g.r.applying(1); }));
    worst = std::max(worst, ms_of([&] { (void)g.r.held(); }));
    worst = std::max(worst, ms_of([&] { (void)g.r.counters(); }));
    worst = std::max(worst, ms_of([&] { (void)g.r.caves_waiting(); }));
    worst = std::max(worst, ms_of([&] { g.set(1, false); }));     // turned off while its cave write waits: the pass does the work
    std::printf("readers and a toggle during a waiting cave write: worst %.1f ms\n", worst);
    CHECK(worst < 100.0);
    g.s.release = true;
    pass.join();
    g.s.cave_blocks = false;
    g.pass();                                        // the toggle's unpatch, then the cave once its code has settled
    g.pass();
    CHECK(g.at(0x1010) == g.orig(0x1010));
    for (uint64_t a = 0x2500; a < 0x2510; a += 4) CHECK(g.at(a) == 0);
    CHECK(g.r.held().empty() && g.r.caves_waiting() == 0);
}

static void test_read_overlay_and_bytes() {
    // the cheat writes a code word, then reads it back and marks data when it sees its own write
    // (5: register 1 = main+0x1500 as u32; C0: register 1 == D503201F)
    const std::string t = "[Self]\n04000000 00001500 D503201F\n54010000 00001500\nC0451400 D503201F\n04000000 00010010 00000001\n20000000\n"
                          "[Byte]\n01000000 00001600 000000EE\n";
    Game g(t);
    g.set(1, true);
    g.set(2, true);
    g.pass();
    CHECK(g.s.get32(kMain + 0x10010) == 1);
    CHECK(g.at(0x1600) == ((g.orig(0x1600) & ~0xFFu) | 0xEE));   // one byte into code: the rest of the word as it was
    g.set(2, false);
    CHECK(g.at(0x1600) == g.orig(0x1600));
}

// The Z-A shapes (Pokemon Legends: Z-A 2.0.2's cheat file): a "Restore Code" cheat writing every original word through a register offset
// (040A0000: main + offset + register A, which is 0), single-word code changes, and a cave at the very end of the text page in 8-byte writes
// (080A0000) entered by a branch. Here at this fake main's addresses.
static void test_za_shapes() {
    Game g("");
    const uint64_t hook = 0x2010, cave = 0x2FF0;   // cave at the end of the last code page, as Z-A's 0x3163FF0
    std::string t = "[Restore Code (Use after unchecking any cheats below)]\n";
    for (uint64_t off : {0x0100ull, 0x0104ull, 0x1100ull, hook}) t += "040A0000 " + hex8(uint32_t(off)) + " " + hex8(g.orig(off)) + "\n";
    t += "[Item/Misc]\n20000000\n";
    t += "[Money]\n040A0000 00000100 B9004009\n040A0000 00001100 B9004009\n";
    t += "[Max Item Quantity]\n080A0000 " + hex8(uint32_t(cave + 8)) + " " + hex8(branch(cave + 12, hook + 4)) + " F9000102\n";
    t += "080A0000 " + hex8(uint32_t(cave)) + " B3607D42 52807CEA\n";
    t += "040A0000 " + hex8(uint32_t(hook)) + " " + hex8(branch(hook, cave)) + "\n";
    Game z(t);
    CHECK(z.f.count() == 4);
    // Restore Code alone: every word it writes is already the original: nothing at all
    z.set(1, true);
    z.pass();
    CHECK(z.s.calls.empty());
    CHECK(z.r.held().empty());
    // with Money: one patch per Money word (Restore Code wrote them first, Money last: Money owns them)
    z.set(3, true);
    z.pass();
    CHECK(z.at(0x0100) == 0xB9004009 && z.at(0x1100) == 0xB9004009);
    for (const Held& h : z.r.held()) CHECK(h.owners == std::set<int>{3});
    z.set(3, false);
    CHECK(z.at(0x0100) == z.orig(0x0100) && z.at(0x1100) == z.orig(0x1100));
    // the cave cheat: cave written, then the branch; off: all back
    z.s.calls.clear();
    z.set(4, true);
    z.pass();
    CHECK(z.at(hook) == branch(hook, cave));
    CHECK(z.at(cave) == 0x52807CEA && z.at(cave + 4) == 0xB3607D42 && z.at(cave + 8) == 0xF9000102 && z.at(cave + 12) == branch(cave + 12, hook + 4));
    CHECK(z.s.calls.size() == 2 && z.s.calls[0].rfind("cave", 0) == 0 && z.s.calls[1].rfind("patch", 0) == 0);
    z.set(4, false);
    z.pass();
    CHECK(z.at(hook) == z.orig(hook));
    for (uint64_t a = cave; a < cave + 16; a += 4) CHECK(z.at(a) == 0);
    CHECK(z.r.held().empty() && z.r.caves_waiting() == 0);
    z.set(1, false);
}

// --file / --nso: a real cheat file over a real main's code
static int run_file(const char* file, const char* nso) {
    std::ifstream ff(file, std::ios::binary), nf(nso, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(ff)), std::istreambuf_iterator<char>());
    const std::string img((std::istreambuf_iterator<char>(nf)), std::istreambuf_iterator<char>());
    if (img.size() < 0x100 || img.compare(0, 4, "NSO0") != 0) { std::printf("not an uncompressed NSO: %s\n", nso); return 2; }
    uint32_t tfo = 0, tsz = 0;
    std::memcpy(&tfo, img.data() + 0x10, 4);
    std::memcpy(&tsz, img.data() + 0x18, 4);
    Game g("");
    g.s.mem.clear();
    g.s.text_end = kMain + tsz;
    g.s.cave_end = kMain + ((uint64_t(tsz) + 0xfff) & ~0xfffull);
    g.s.fns.clear();
    for (uint64_t a = 0; a < tsz; a += 0x40) g.s.fns.push_back(kMain + a);
    g.s.image = &img;                                // main's code and its zero cave, read from the file
    g.s.image_text = tfo;
    // every byte the cheats wrote is the file's again (text) or zero (cave)
    auto pristine = [&] {
        for (const auto& [a, v] : g.s.mem) {
            if (a < kMain || a >= g.s.cave_end) continue;
            const uint8_t want = a < g.s.text_end ? uint8_t(img[tfo + (a - kMain)]) : 0;
            if (v != want) return false;
        }
        return true;
    };
    g.f = ParseFile(text);
    if (!g.f.ok) { std::printf("cheat file refused: %s\n", g.f.error.c_str()); return 2; }
    for (size_t k = 1; k < kMaxEntries; ++k) g.f.entries[k].enabled = false;
    std::printf("main text 0x%x bytes, cave to 0x%llx; %zu cheats\n", tsz, (unsigned long long)(g.s.cave_end - kMain), g.f.count());
    int bad = 0;
    for (size_t k = 1; k < kMaxEntries; ++k) {
        if (!g.f.entries[k].used) continue;
        g.s.calls.clear();
        const Counters c0 = g.r.counters();
        g.set(int(k), true);
        g.pass();
        g.pass();
        const Counters c1 = g.r.counters();
        const size_t held = g.r.held().size();
        g.set(int(k), false);
        g.pass();
        const bool back = pristine() && g.r.held().empty() && g.r.caves_waiting() == 0;
        if (!back) ++bad;
        std::printf("%3zu %-48s code writes %3llu cave writes %3llu  patched %2llu cave words %3llu  held %3zu  off: %s\n", k,
                    g.f.entries[k].name.c_str(), (unsigned long long)(c1.code_writes - c0.code_writes),
                    (unsigned long long)(c1.cave_writes - c0.cave_writes), (unsigned long long)(c1.words_patched - c0.words_patched),
                    (unsigned long long)(c1.cave_words_written - c0.cave_words_written), held, back ? "all back" : "NOT BACK");
    }
    std::printf("%s\n", bad ? "SOME CHEATS DID NOT PUT THE CODE BACK" : "every cheat's code and cave went back when it was turned off");
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--file" && std::string(argv[3]) == "--nso") return run_file(argv[2], argv[4]);
    test_data_only();
    test_asm_on_off();
    test_cave_back_on_again();
    test_shared_and_written_back();
    test_refused_and_unlifted();
    test_revoked_in_pass_and_reset();
    test_read_overlay_and_bytes();
    test_za_shapes();
    test_readers_never_wait_for_the_sink();
    if (g_fail == 0) std::printf("ALL VMCODE TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
