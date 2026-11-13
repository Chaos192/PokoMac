// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Standalone test of the cheat engine host (runtime/src/cheats/engine.*): loads an Atmosphère file from a temp folder, runs a pass, and
// checks the write reached guest memory; then checks the freeze modes (normal, never-decrease, never-increase). The few runtime symbols
// the engine calls (gmem, hid, config, log) are stubbed here, so no runtime is needed.
#define _CRT_SECURE_NO_WARNINGS
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include "cheat_vm.h"
#include "gmem.h"
#include "engine.h"
#include "nxrt/game.h"
#include "nxrt/input_source.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

// ---- stubs for the runtime symbols the engine references ----
static std::map<uint64_t, uint8_t> g_mem;
static uint64_t g_keys = 0;
namespace nxrt::cheats::gmem {
bool read(uint64_t a, void* d, size_t n) {
    uint8_t* p = (uint8_t*)d;
    for (size_t i = 0; i < n; ++i) { auto it = g_mem.find(a + i); if (it == g_mem.end()) return false; p[i] = it->second; }
    return true;
}
bool write(uint64_t a, const void* s, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (size_t i = 0; i < n; ++i) g_mem[a + i] = p[i];
    return true;
}
uint64_t region_base(unsigned t) { return (t & 0xF) == 1 ? 0x7800000000ull : (t & 0xF) == 2 ? 0x6000000000ull : (t & 0xF) == 4 ? 0 : 0x7100000000ull; }
uint64_t main_base() { return 0x7100000000ull; }
uint64_t main_size() { return 0x1000000; }
std::string module_build_id(const std::string&) { return "0123456789abcdef0000000000000000000000000000000000000000000000ff"; }
}  // namespace nxrt::cheats::gmem
namespace nxrt::hid { uint64_t sampled_buttons() { return g_keys; } }
namespace nxrt::cheats::aw { bool watching() { return false; } void rearm() {} }   // find what accesses: no watch here
static std::map<std::string, std::string> g_cfg;   // "section.key" -> value ([cheats] mod_files below)
namespace nxrt { std::string config_get_str(const char* s, const char* k, const char* d) { auto it = g_cfg.find(std::string(s) + "." + k); return it != g_cfg.end() ? it->second : d ? d : ""; }
bool config_get_bool(const char*, const char*, bool d) { return d; }
// no game folder here (78c8c53a's title_path): paths stay as given, as in the development runs
const std::string& game_dir() { static const std::string none; return none; }
std::string title_path(const std::string& path) { return path; }
std::string config_get_path(const char* s, const char* k, const char* d) { return title_path(config_get_str(s, k, d)); }
void logf(LogLevel, const char*, const char*, ...) {}
bool log_enabled(LogLevel) { return false; } }

// The router's sink (vmcode_live.cpp in the runtime): main's code is [main, main+0x800) in functions of 0x100 bytes, its cave up to
// main+0x1000; memory is g_mem; the patcher records what it is asked and writes the word.
static std::vector<std::string> g_cp_calls;
static std::map<uint64_t, uint32_t> g_cp_orig;
namespace nxrt::cheats::vmcode {
struct TestSink : Sink {
    static constexpr uint64_t B = 0x7100000000ull, TEXT_END = B + 0x800, CAVE_END = B + 0x1000;
    bool maybe_code(uint64_t a, size_t n) override { return a + n > B && a < CAVE_END; }
    Place place(uint64_t a) override {
        if (a < B || a >= CAVE_END) return {};
        if (a >= TEXT_END) return {Kind::Cave, TEXT_END, CAVE_END};
        return {Kind::Code, a & ~0xFFull, (a & ~0xFFull) + 0x100};
    }
    bool read(uint64_t a, void* d, size_t n) override { return gmem::read(a, d, n); }
    bool write(uint64_t a, const void* s, size_t n) override { return gmem::write(a, s, n); }
    void patch(const std::vector<std::pair<uint64_t, uint32_t>>& words, std::vector<std::string>* errs) override {
        errs->assign(words.size(), std::string());
        for (auto& [w, v] : words) {
            uint32_t cur = 0;
            gmem::read(w, &cur, 4);
            if (!g_cp_orig.count(w)) g_cp_orig[w] = cur;
            gmem::write(w, &v, 4);
            g_cp_calls.push_back("patch");
        }
    }
    void unpatch(const std::vector<uint64_t>& words) override {
        for (uint64_t w : words) {
            if (auto it = g_cp_orig.find(w); it != g_cp_orig.end()) { gmem::write(w, &it->second, 4); g_cp_orig.erase(it); }
            g_cp_calls.push_back("unpatch");
        }
    }
    void cave_written(uint64_t, uint64_t, uint64_t, uint64_t) override { g_cp_calls.push_back("cave"); }
};
Sink& live_sink() { static TestSink s; return s; }
}  // namespace nxrt::cheats::vmcode

#include "cheat_vm.cpp"
#include "vmcode.cpp"
#include "engine.cpp"

using namespace nxrt::cheats;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static uint32_t get32(uint64_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) { auto it = g_mem.find(a + i); v |= (uint64_t)(it == g_mem.end() ? 0 : it->second) << (8 * i); } return v; }
static void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) g_mem[a + i] = (v >> (8 * i)) & 0xFF; }

int main() {
    namespace fs = std::filesystem;
    // register a title so program_id_hex() is non-empty
    static nxrt::game::Info info;
    info.program_id = 0x0100abcdef000000ull;
    info.platform = nxrt::game::Platform::Switch2;
    nxrt::game::set_current(&info);

    // a temp cheats folder with <pid>/cheats/<bid>.txt
    const fs::path tmp = fs::temp_directory_path() / "nxrt_cheat_engine_test";
    fs::remove_all(tmp);
    const std::string pid = "0100abcdef000000";
    const std::string bid = "0123456789abcdef";
    const fs::path dir = tmp / pid / "cheats";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / (bid + ".txt"), std::ios::binary);
        // [Coins 999]  04000000 00001000 000003E7   -> write u32 999 to main+0x1000
        f << "[Coins 999]\n04000000 00001000 000003E7\n";
        // [Off cheat]  04000000 00002000 00000001   (will be toggled off)
        f << "[Off]\n04000000 00002000 00000001\n";
    }

    Engine& e = Engine::instance();
    e.set_folder(tmp.string());
    auto res = e.reload();
    CHECK(res.found);
    CHECK(res.ok);
    CHECK(res.cheats == 2);
    CHECK(e.program_id_hex() == pid);
    CHECK(e.build_id_hex() == bid);

    // run one pass: both cheats write
    e.step_once();
    CHECK(get32(0x7100000000ull + 0x1000) == 999);
    CHECK(get32(0x7100000000ull + 0x2000) == 1);

    // toggle [Off] off, reset memory, run again: only Coins writes
    e.set_cheat_enabled(2, false);
    g_mem.clear();
    e.step_once();
    CHECK(get32(0x7100000000ull + 0x1000) == 999);
    CHECK(g_mem.find(0x7100000000ull + 0x2000) == g_mem.end());

    // toggles.txt was written
    CHECK(fs::exists(dir / "toggles.txt"));
    // data cheats only: the router did nothing (vmcode.h's counters stay 0)
    {
        const auto c = e.stats().code;
        CHECK(c.code_writes == 0 && c.cave_writes == 0 && c.passes_with_code == 0 && c.words_patched == 0);
        CHECK(g_cp_calls.empty());
    }

    // --- [cheats] mod_files (docs/MODS.md): the enabled mods' cheat files, after the title's own, toggles by name still apply ---
    {
        const fs::path mod = tmp / "mod" / "cheats" / (bid + ".txt");
        fs::create_directories(mod.parent_path());
        std::ofstream(mod, std::ios::binary) << "{Mod master}\n04000000 00004000 00000009\n[Mod gems]\n04000000 00003000 00000005\n";
        const fs::path list = tmp / "mod_cheats.txt";
        std::ofstream(list, std::ios::binary) << "# the mod manager's list\r\n" << mod.string() << "\r\n" << (tmp / "absent.txt").string() << "\n";
        g_cfg["cheats.mod_files"] = list.string();
        auto r2 = e.reload();
        CHECK(r2.found && r2.ok);
        CHECK(r2.cheats == 4);              // Coins, Off, the mod's master and Mod gems; an unreadable entry is left out
        g_mem.clear();
        e.step_once();
        CHECK(get32(0x7100000000ull + 0x1000) == 999);
        CHECK(get32(0x7100000000ull + 0x3000) == 5);
        CHECK(get32(0x7100000000ull + 0x4000) == 9);
        CHECK(g_mem.find(0x7100000000ull + 0x2000) == g_mem.end());   // [Off] stays off (toggles.txt)
        g_cfg.erase("cheats.mod_files");
        auto r3 = e.reload();
        CHECK(r3.cheats == 2);
    }

    // --- freeze: normal mode holds the value even as the game changes it ---
    const uint64_t A = 0x7800000000ull + 0x40;   // a heap address
    put32(A, 50);
    uint32_t vbits = 50;
    uint64_t fid = e.freeze_add(A, 4, vbits, FreezeMode::Normal, false);
    put32(A, 7);            // the game lowers it
    e.step_once();          // the freeze writes 50 back
    CHECK(get32(A) == 50);
    e.freeze_remove(fid);

    // --- never-decrease: the game may raise it, not lower it ---
    put32(A, 100);
    uint64_t fid2 = e.freeze_add(A, 4, 100, FreezeMode::AllowIncrease, false);
    put32(A, 150);          // game raised it
    e.step_once();          // kept at 150 (frozen value follows up)
    CHECK(get32(A) == 150);
    put32(A, 120);          // game lowers it
    e.step_once();          // restored to 150 (the high-water mark)
    CHECK(get32(A) == 150);
    e.freeze_remove(fid2);

    // --- never-increase ---
    put32(A, 100);
    uint64_t fid3 = e.freeze_add(A, 4, 100, FreezeMode::AllowDecrease, false);
    put32(A, 80);
    e.step_once();
    CHECK(get32(A) == 80);  // lower kept
    put32(A, 200);
    e.step_once();
    CHECK(get32(A) == 80);  // raise refused
    e.freeze_remove(fid3);

    // --- an ASM cheat (a branch at main+0x104 into a cave at main+0x900): a live patch while on, the code and cave back when off ---
    {
        const uint64_t B = 0x7100000000ull;
        put32(B + 0x104, 0xAA000041);
        for (uint64_t a = B + 0x800; a < B + 0x1000; a += 4) put32(a, 0);
        std::ofstream(dir / (bid + ".txt"), std::ios::binary) << "[Coins 999]\n04000000 00001000 000003E7\n"
                                                                  "[Hook]\n04000000 00000104 AA000041\n04000000 00000104 140001FF\n"
                                                                  "08000000 00000900 17FFFE01 52800148\n";
        fs::remove(dir / "toggles.txt");
        auto r = e.reload();
        CHECK(r.ok && r.cheats == 2);
        g_cp_calls.clear();
        e.set_cheat_enabled(2, true);
        e.step_once();
        CHECK(get32(B + 0x104) == 0x140001FF);
        CHECK(get32(B + 0x900) == 0x52800148 && get32(B + 0x904) == 0x17FFFE01);
        CHECK(g_cp_calls.size() == 2 && g_cp_calls[0] == "cave" && g_cp_calls[1] == "patch");
        e.step_once();
        CHECK(g_cp_calls.size() == 2);                 // nothing new while it stays on
        const auto c = e.stats().code;
        CHECK(c.code_writes == 4 && c.cave_writes == 2 && c.words_patched == 1 && c.cave_words_written == 2);
        CHECK(e.code_held().size() == 3);
        e.set_cheat_enabled(2, false);
        CHECK(get32(B + 0x104) == 0xAA000041);
        CHECK(get32(B + 0x900) == 0 && get32(B + 0x904) == 0);
        CHECK(e.code_held().empty());
        // a reload puts held code back too
        e.set_cheat_enabled(2, true);
        e.step_once();
        CHECK(get32(B + 0x104) == 0x140001FF);
        e.reload();
        CHECK(get32(B + 0x104) == 0xAA000041);
        CHECK(e.code_held().empty());
    }

    fs::remove_all(tmp);
    if (g_fail == 0) std::printf("ALL ENGINE TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
