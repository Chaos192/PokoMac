// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// "Find what writes / reads this address" (phase 6), native: the page holding the watched address is made read-only (for writes) or
// no-access (for reads); the first guest access faults, a vectored handler records the faulting code (the lifted host PC resolved to the
// guest function through the dispatcher's tables), bumps that writer's hit count, snapshots the guest registers, then lets the access
// through. The engine's 1/12 s pass re-arms the page, so over a few passes every recurring writer/reader is found, the game never stalling
// more than the one faulting access. Never an interpreter, never a debugger stub in the game: it is the runtime's own page protection and
// the same host-PC -> guest-function map the crash handler uses.
//
// Cross-platform: VirtualProtect / AddVectoredExceptionHandler come from nxwin on macOS and Linux (the same as src/mem/write_watch.cpp).
// It does not single-step, so it works on an arm64 host too; the cost is that only the first access per page per arm-cycle is caught, which
// a 12 Hz re-arm turns into "every writer is found within a second or two".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::cheats::aw {

enum class Access : uint8_t { Write, Read };

// One piece of code that accessed the watched address, keyed by the guest function it belongs to.
struct Accessor {
    uint64_t guest_fn = 0;        // guest address of the lifted function that faulted (0 = host code / unresolved)
    std::string where;            // "main+0x1234" (module+offset of the function), or "host" / "0x..."
    uint64_t hits = 0;
    uint64_t last_pc = 0;         // guest function most-recently-entered (Ctx.fn) at the access
    uint64_t regs[31] = {};       // x0..x30 at the access (guest Ctx; last synced to memory, see docs)
    uint64_t sp = 0;
    bool is_write = false;        // the access was a write (false: a read)
};

// Start watching [addr, addr+width). mode Write arms read-only; Read arms no-access (also sees writes). Replaces any current watch.
// False when the page could not be armed (not committed read-write, or already tracked by the renderer's write watch).
bool watch_start(uint64_t addr, unsigned width, Access mode, std::string* err = nullptr);
void watch_stop();
bool watching();
uint64_t watch_addr();
Access watch_mode();

// Re-arms the watched page(s) (call every engine pass). Cheap when not watching.
void rearm();

// The accessors found so far, most hits first.
std::vector<Accessor> accessors();
void clear();
uint64_t total_faults();
// The watched value changed since the watch started while nothing accessed it through this address: the system writes it (a service the
// runtime runs, such as audio or input, updates it from the host side) or the game writes the same memory through another address.
// Seen live on Hollow Knight's title screen (hk2: a value counting about 180,000 a second, 0 faults in 6 s while it changed).
bool changed_unseen();
// Diagnostics: how many accesses the armed page(s) caught in all, and those within 64 bytes of the value (by offset), which tell a
// neighbouring field's writes (they unprotect the page first) from a wider store that covers the value from below.
std::string page_report();

}  // namespace nxrt::cheats::aw
