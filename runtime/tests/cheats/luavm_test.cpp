// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Standalone test of the cheats overlay's Lua scripts (runtime/src/cheats/luavm.*) over a fake guest memory: the nx API, hotkeys, the
// frame callbacks, the sandbox (no io / os / dofile, files only by plain name), the instruction budget and the memory cap.
#define _CRT_SECURE_NO_WARNINGS
#include "luavm.h"
#include "gmem.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

namespace nxrt::cheats::gmem {
std::vector<Region> regions() { return {}; }
bool read(uint64_t, void*, size_t) { return false; }
}  // namespace nxrt::cheats::gmem

#include "scanner.cpp"
#include "aob.cpp"
#include "luavm.cpp"

using namespace nxrt::cheats;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Mem : MemSource {
    uint64_t base;
    std::vector<uint8_t> m;
    Mem(uint64_t b, size_t n) : base(b), m(n, 0) {}
    std::vector<ScanRegion> regions(const RegionFilter& f) override {
        uint64_t lo = base, hi = base + m.size();
        if (f.lo || f.hi) { lo = std::max(lo, f.lo); hi = std::min(hi, f.hi ? f.hi : hi); }
        if (lo >= hi) return {};
        return {{lo, hi - lo, 3, "heap"}};
    }
    bool read(uint64_t a, void* d, size_t n) override {
        if (a < base || a + n > base + m.size()) return false;
        std::memcpy(d, m.data() + (a - base), n);
        return true;
    }
};

struct H : lua::Host {
    Mem mem_{0x7100000000ull, 0x10000};
    std::set<std::string> down;
    std::vector<std::string> lines;
    std::vector<std::pair<uint64_t, uint64_t>> freezes;
    bool paused = false;
    double spd = 1.0;
    uint64_t frames = 0;
    std::string dir;
    MemSource& mem() override { return mem_; }
    bool write(uint64_t a, const void* s, size_t n) override {
        if (a < mem_.base || a + n > mem_.base + mem_.m.size()) return false;
        std::memcpy(mem_.m.data() + (a - mem_.base), s, n);
        return true;
    }
    bool module_range(const std::string& n, uint64_t* lo, uint64_t* hi) override {
        if (n != "main") return false;
        *lo = mem_.base;
        *hi = mem_.base + 0x8000;
        return true;
    }
    uint64_t freeze(uint64_t a, ScanType, uint64_t bits) override { freezes.push_back({a, bits}); return freezes.size(); }
    void unfreeze(uint64_t id) override { if (id && id <= freezes.size()) freezes[id - 1].first = 0; }
    bool held(const std::string& n) override { return down.count(n) != 0; }
    void pause(bool on) override { paused = on; }
    void advance() override {}
    double speed(double set) override { if (set > 0) spd = set; return spd; }
    uint64_t frame() override { return frames; }
    std::string folder() override { return dir; }
    void console(const std::string& l) override { lines.push_back(l); }
    template <class T> void put(uint64_t off, T v) { std::memcpy(mem_.m.data() + off, &v, sizeof v); }
    template <class T> T get(uint64_t off) { T v; std::memcpy(&v, mem_.m.data() + off, sizeof v); return v; }
};

int main() {
    H h;
    lua::Vm vm(&h);
    std::string err;

    // memory, pointers, AOB, print
    h.put<uint32_t>(0x100, 1234);
    h.put<uint64_t>(0x200, 0x7100000300ull);       // a pointer
    h.put<uint32_t>(0x310, 77);
    const uint8_t sig[] = {0xde, 0xad, 0xbe, 0xef};
    std::memcpy(h.mem_.m.data() + 0x4000, sig, 4);
    uint32_t id = vm.load("basics",
                          "local base = nx.module('main')\n"
                          "print('coins', nx.read32(base + 0x100))\n"
                          "nx.write32(base + 0x104, nx.read32(base + 0x100) + 1)\n"
                          "local p = nx.ptr(base + 0x200, 0x10)\n"
                          "nx.write16(base + 0x108, nx.read32(p))\n"
                          "local a = nx.aobscan('DE AD ?? EF', 'main')\n"
                          "nx.write64(base + 0x110, a)\n"
                          "nx.writef(base + 0x118, 2.5)\n"
                          "assert(nx.read32(0x10) == nil)\n",
                          &err);
    if (!err.empty()) std::printf("load: %s\n", err.c_str());
    CHECK(id && err.empty());
    CHECK(h.get<uint32_t>(0x104) == 1235);
    CHECK(h.get<uint16_t>(0x108) == 77);
    CHECK(h.get<uint64_t>(0x110) == 0x7100004000ull);
    CHECK(h.get<float>(0x118) == 2.5f);
    CHECK(!h.lines.empty() && h.lines[0] == "[basics] coins\t1234");

    // frame callbacks and hotkeys: the counter goes up each frame; F5 adds 100 once per press
    vm.load("frames",
            "local n = 0\n"
            "nx.onframe(function() n = n + 1; nx.write32(0x7100000500, n) end)\n"
            "nx.hotkey('F5', function() nx.write32(0x7100000504, nx.read32(0x7100000504) + 100) end)\n",
            &err);
    CHECK(err.empty());
    vm.on_frame();
    vm.on_frame();
    CHECK(h.get<uint32_t>(0x500) == 2);
    h.down.insert("F5");
    vm.on_frame();
    vm.on_frame();                                   // still held: no second fire
    h.down.clear();
    vm.on_frame();
    h.down.insert("F5");
    vm.on_frame();
    CHECK(h.get<uint32_t>(0x504) == 200);
    CHECK(h.get<uint32_t>(0x500) == 6);

    // freeze, pause, speed
    vm.load("ctl", "local id = nx.freeze(0x7100000600, 'u32', 99)\nnx.pause(true)\nnx.speed(2)\n", &err);
    CHECK(err.empty() && h.freezes.size() == 1 && h.freezes[0].second == 99 && h.paused && h.spd == 2.0);

    // the sandbox: no io, os, dofile, loadfile, require, debug; load() of binary chunks refused
    for (const char* bad : {"io.open('x')", "os.execute('x')", "dofile('x')", "loadfile('x')", "require('x')", "debug.getinfo(1)",
                            "load(string.dump(function() end))"}) {
        std::string e;
        uint32_t sid = vm.load("bad", bad, &e);
        CHECK(!e.empty());
        vm.stop(sid);
    }
    // files only by plain name inside the folder
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "nxrt_luavm_test";
    std::filesystem::create_directories(tmp);
    h.dir = tmp.string();
    vm.load("files", "assert(nx.writefile('notes.txt', 'hi'))\nassert(nx.readfile('notes.txt') == 'hi')\n", &err);
    CHECK(err.empty());
    {
        std::string e;
        vm.load("escape", "nx.writefile('../x.txt', 'no')", &e);
        CHECK(!e.empty());
        vm.load("escape2", "nx.readfile('C:/Windows/win.ini')", &e);
        CHECK(!e.empty());
    }
    std::filesystem::remove_all(tmp);

    // a runaway loop stops with a message; an error in onframe stops only that script
    {
        std::string e;
        vm.load("spin", "while true do end", &e);
        CHECK(e.find("took too long") != std::string::npos);
        vm.load("boom", "nx.onframe(function() error('kaboom') end)", &e);
        CHECK(e.empty());
        vm.on_frame();
        bool boom_stopped = false, frames_running = false;
        for (const auto& s : vm.scripts()) {
            if (s.name == "boom") boom_stopped = !s.running && s.error.find("kaboom") != std::string::npos;
            if (s.name == "frames") frames_running = s.running;
        }
        CHECK(boom_stopped && frames_running);
        // memory cap: a script that grows a string without end gets "not enough memory", the VM lives on
        vm.load("hog", "local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 1024) end", &e);
        CHECK(!e.empty());
        CHECK(vm.eval("1 + 2") == "3");
    }

    // the console: expressions print their value; variables live between lines
    CHECK(vm.eval("x = 41") == "");
    CHECK(vm.eval("x + 1") == "42");
    CHECK(vm.eval("nx.read32(0x7100000100)") == "1234");
    CHECK(vm.eval("print('a', 1)") == "a\t1");

    if (g_fail == 0) std::printf("ALL LUA TESTS PASSED\n");
    else std::printf("%d CHECK(s) FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
