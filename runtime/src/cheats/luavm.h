// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Lua scripts for the cheats overlay (phase 6): an embedded Lua 5.4 (MIT, PUC-Rio; cmake/lua_deps.cmake) with a small, sandboxed API.
// A script reads and writes guest memory, scans for AOB patterns, follows pointer paths, freezes values, binds hotkeys and runs a function
// every frame. It has no file, process or network access: io, os, package and debug are not built, dofile / loadfile / load-from-file are
// removed, and nx.readfile / nx.writefile reach only plain file names inside the title's cheats folder. A runaway script is stopped (an
// instruction budget per call) and its memory is capped.
//
//   nx.read8/16/32/64(a)  nx.readf(a)  nx.readd(a)  nx.readbytes(a, n)  nx.readstring(a [, max])      -> value / string, nil if unreadable
//   nx.write8/16/32/64(a, v)  nx.writef(a, v)  nx.writed(a, v)  nx.writebytes(a, s)                    -> true / false
//   nx.module(name) -> base, size          nx.ptr(base, off1, off2, ...) -> address (nil when a pointer is unreadable)
//   nx.aobscan(pattern [, module]) -> address or nil       nx.aobscanall(pattern [, module]) -> { addresses }
//   nx.freeze(a, type, value) -> id        nx.unfreeze(id)          type: "u8" "u16" "u32" "u64" "f32" "f64"
//   nx.onframe(fn)  nx.hotkey(name, fn)  nx.held(name) -> bool      name: a key ("F5", "Left Ctrl") or pad buttons ("L+R+A")
//   nx.pause(on)  nx.advance()  nx.speed([x]) -> x  nx.frame() -> n   nx.tick() -> console clock   nx.time() -> real seconds
//   nx.readfile(name) -> string  nx.writefile(name, s) -> true
//   print(...) and nx.log(...) write to the overlay's console.
// Everything runs on the presenting thread at the frame boundary (cheats/control.h), so a script sees one consistent frame.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scanner.h"

namespace nxrt::cheats::lua {

// What the scripts reach. The live host is guest memory, the engine's freezes and the overlay's input; tests use their own.
class Host {
public:
    virtual ~Host() = default;
    virtual MemSource& mem() = 0;
    virtual bool write(uint64_t addr, const void* src, size_t n) = 0;
    virtual bool module_range(const std::string& name, uint64_t* lo, uint64_t* hi) = 0;
    virtual uint64_t freeze(uint64_t addr, ScanType t, uint64_t bits) = 0;     // id, 0 on failure
    virtual void unfreeze(uint64_t id) = 0;
    virtual bool held(const std::string& name) = 0;                          // a key or pad combination is down now
    virtual void pause(bool on) = 0;
    virtual void advance() = 0;
    virtual double speed(double set) = 0;                                     // set <= 0: only read
    virtual uint64_t frame() = 0;
    virtual uint64_t tick() { return 0; }                                     // the console clock (svcGetSystemTick), speed hack included
    virtual double host_seconds() { return 0; }                              // real time (a monotonic clock), for timing
    virtual std::string folder() = 0;                                        // the title's cheats folder ("" = no file access)
    virtual void console(const std::string& line) = 0;
};

struct ScriptInfo {
    uint32_t id = 0;
    std::string name;
    bool running = false;          // loaded and not stopped (an error stops it)
    std::string error;
    size_t frame_fns = 0, hotkeys = 0;
    uint64_t calls = 0;
};

class Vm {
public:
    explicit Vm(Host* host);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // Compiles and runs a script's top level (where it registers its callbacks). Returns its id (0 with *err on a compile or run error).
    uint32_t load(const std::string& name, const std::string& source, std::string* err);
    // Runs a line in a scratch environment shared by the console (the overlay's Lua prompt). Returns what it printed or the error.
    std::string eval(const std::string& line);
    void stop(uint32_t id);
    void stop_all();
    // The frame boundary: hotkeys pressed since the last call, then every onframe function (frame = false: called while the game is
    // paused, hotkeys only).
    void on_frame(bool frame = true);
    std::vector<ScriptInfo> scripts() const;

    static constexpr uint64_t kInstructionBudget = 20000000;   // per callback; a script past it stops ("took too long")
    static constexpr size_t kMemoryLimit = 64u << 20;           // bytes for all scripts together

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nxrt::cheats::lua
