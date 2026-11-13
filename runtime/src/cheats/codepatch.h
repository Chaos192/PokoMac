// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Code patches, code injection and breakpoints for the cheats overlay (phase 6), native: never an interpreter.
//
// Guest code runs as compiled host code, so changing an instruction's bytes in guest memory changes nothing by itself. A patch therefore:
//   1. writes the new bytes into guest memory (the disassembler, scans and the next build see them);
//   2. lifts the function that holds them again from the patched bytes into an on-demand unit (nxrt/missed.h make_code_unit: the build's
//      own lifter and toolchain, cached by the bytes) and registers it with the dispatcher, so every indirect call runs the new code;
//   3. at the next frame boundary, with the game held (cheats/control.h run_held), turns the function's compiled entry into a jump to the
//      unit, so the direct calls run it too. On a host whose code pages cannot be written (macOS on Apple silicon), or for a function too
//      short to hold the jump, step 3 is skipped and the patch says so: callers that call it directly, and callers the compiler inlined
//      it into, keep the old code until the next build.
//   4. for the next build it is an IPS patch (<build id>.ips, the IPS offsets as apply_ips.py / pkp_build.py --patches take them), which
//      the build compiles in everywhere.
// A breakpoint uses the same unit with an inline site before the instruction (and before every other instruction of the function, so a held
// thread can step): the site's hook records the registers, counts the hit and, for a stopping breakpoint, holds the thread until the overlay
// continues or steps. A trace point only counts and records.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::cheats::cp {

// The lifted function holding a guest code address: its module, start and end (the next function's start). False when addr is not code
// of a module the build lifted.
bool function_at(uint64_t addr, std::string* module, uint64_t* start, uint64_t* end);
// "main+0x1234 (fn main+0x1200)" for display.
std::string where(uint64_t addr);

enum class State : uint8_t { Compiling, Live, DispatchOnly, Failed, Reverted };
const char* state_name(State s);

struct Patch {
    uint64_t addr = 0;
    std::vector<uint8_t> bytes, original;
    std::string module;
    uint64_t module_base = 0;
    uint64_t fn_start = 0, fn_end = 0;
    State state = State::Compiling;
    std::string note;              // why not live, or how long the unit took
    bool by_game = false;          // the game wrote this code itself (guest_code_written): live like a patch, never reverted or exported
    bool by_cheat = false;         // an Atmosphère cheat wrote it (apply_words, vmcode.h): reverted when the cheat goes off, never exported
};

// Patches the code at addr (a multiple of 4; up to 64 bytes). Returns at once; the unit is made on a worker thread and the patch goes live
// at a later frame boundary (patches() reports its state). False with *err when addr is not code a patch can change.
bool apply(uint64_t addr, const std::vector<uint8_t>& bytes, std::string* err);
// Puts the original bytes back (the function is lifted again without this patch, or returns to its compiled code when none is left).
bool revert(uint64_t addr, std::string* err);
std::vector<Patch> patches();

// The cheat VM's code writes of one pass (vmcode.h): one-word patches (by_cheat), recorded as apply records them, every function they touch
// lifted again once (not once per word). errs[i]: why word i was refused, empty when it was patched. Returns how many were patched.
size_t apply_words(const std::vector<std::pair<uint64_t, uint32_t>>& words, std::vector<std::string>* errs);
// Puts the original bytes of those one-word patches back, each function lifted again (or back to its compiled code) once. Returns how many.
size_t revert_words(const std::vector<uint64_t>& addrs);
// True when the function holding addr runs what its last rebuild made (no swap still on the way, none failed): code a reverted patch
// branched from no longer runs, so the cave it entered may go back.
bool settled(uint64_t addr);

// The game rewrote its own code in [addr, addr + len) (a skyline-style mod: svcSetProcessMemoryPermission makes a text page writable, a
// branch is written, the page goes back to executable): every lifted function holding those words is lifted again from the bytes now in
// guest memory and swapped in at the next frame boundary, as a code patch is (entry and loop-head checks, the code-page jump, the
// dispatcher). Listed among the patches as written by the game; never reverted nor written to an IPS. False with *err when no lifted
// code is there.
bool guest_code_written(uint64_t addr, size_t len, std::string* err);

// The code at [base, base + size) is going away (a module unmapped: ro's UnmapManualLoadModuleMemory): every patch (the overlay's and the
// game's own), breakpoint and trace point there is dropped with its unit: the override bit and hook, the loop-head entry, the dispatcher
// slot it took. Else a module mapped later at the same address runs the old unit (runtime-gaps' ro_test: the next NRO at that address ran
// the previous one's patched code). Call it before the module's own tables are unregistered.
void code_unmapped(uint64_t base, uint64_t size);

// IPS of every patch in one module (offsets into its decompressed NSO, the 0x100 header included, as apply_ips.py reads them). Empty
// when the module has none.
std::vector<uint8_t> ips_for_module(const std::string& module);
// Writes <dir>/<build id 16 hex>.ips for each patched module (the name pkp_build.py --patches looks for). Returns the files written.
std::vector<std::string> write_ips(const std::string& dir, std::string* err);

// Code injection: the instruction at `at` becomes a branch to a code cave holding `lines` (A64, the assembler's syntax), then the
// instruction that was at `at`, then a branch back to at + 4. The cave is the free space after the module's code (the padding to its last
// page), registered as a code region (missed.h), so its code is lifted from its bytes when it first runs; the branch at `at` is a code
// patch like any other (apply). The displaced instruction must not be PC-relative (a branch or adr / adrp / ldr literal: refused).
bool inject(uint64_t at, const std::vector<std::string>& lines, std::string* err);

// ---- breakpoints -----------------------------------------------------------------------------------------------------------------
struct Regs {
    uint64_t x[31] = {}, sp = 0, pc = 0, fn = 0;
    uint32_t nzcv = 0;
    double d[8] = {};              // d0..d7
    uint64_t thread_id = 0;
    std::string thread;
};
struct Breakpoint {
    uint64_t addr = 0;
    bool stop = true;              // false: a trace point (count and record, never hold)
    bool enabled = true;
    uint64_t hits = 0;
    State state = State::Compiling;
    std::string note;
    Regs last;                     // the registers at the last hit
};
bool bp_add(uint64_t addr, bool stop, std::string* err);
bool bp_remove(uint64_t addr, std::string* err);
void bp_enable(uint64_t addr, bool on);
std::vector<Breakpoint> breakpoints();

// A thread held at a breakpoint (or after a step): its registers. False when none is held.
bool held(Regs* r);
enum class Resume : uint8_t { Continue, Step, ToReturn };
void resume(Resume how);           // Step: the next instruction of the same function on the same thread; ToReturn: the function's next ret

}  // namespace nxrt::cheats::cp
