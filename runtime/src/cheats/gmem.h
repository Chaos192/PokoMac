// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Guest memory as the cheat tools see it (cheat VM, scanner, pointer scanner, memory viewer). Guest addresses are host addresses
// (include/nxrt/memory.h), so a console cheat's main+X or heap+X lands where it would on the console. Every access is fault safe: a page
// the game unmapped in the middle of a pass makes the access fail, as the console's debug read / write SVCs do, instead of crashing.
//   - read: any committed, readable page of the guest's address ranges (the alias region, the module images, heap, stacks, kernel region).
//     Pages the write tracker (nxrt/write_watch.h) arms or guards are read through its fault handler, so the read sees what the game sees.
//   - write: any such page; a page the guest cannot write (main's code and read-only data) is made writable for the copy and put back,
//     as the console's debugger writes them (Horizon's WriteDebugMemory accepts debuggable read-only pages).
//   - never host memory: an address outside the guest ranges fails.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::cheats::gmem {

bool in_guest_range(uint64_t addr, uint64_t size);
bool read(uint64_t addr, void* dst, size_t n);
bool write(uint64_t addr, const void* src, size_t n);
// Reads in place without a copy: calls fn(ptr, n) under a fault guard, false when a fault cut it short. For the scanners' hot loops.
bool with_mapped(uint64_t addr, size_t n, void (*fn)(const uint8_t* p, size_t n, void* user), void* user);

// The memory-type nybble of Atmosphère's cheat VM: 0 main, 1 heap, 2 alias, 3 aslr, 4 absolute (0), 5..F main. The bases are what the guest's
// svcGetInfo answers (kernel/svc_info.cpp) and the main module's load address.
uint64_t region_base(unsigned mem_type);
uint64_t main_base();
uint64_t main_size();

// One block of the guest's memory map (svcQueryMemory's view, memory.cpp vm map), for the scanners and the memory map window.
struct Region {
    uint64_t addr = 0, size = 0;
    uint32_t state = 0, perm = 0;      // Horizon MemoryState and permission bits (1 R, 2 W, 4 X)
    std::string module;                // the module whose image holds it ("main", "sdk", ...), empty when none
    const char* kind = "";             // "code", "data", "heap", "alias", "stack", "tls", "shared", "other"
};
// Every non-free block in address order; blocks of one module stay separate by permission.
std::vector<Region> regions();

// "main+0x1234" / "heap+0x10" / "alias+0x..." / "0x7123456789"; module names come from the loaded modules.
std::string describe(uint64_t addr);
// The module holding addr (name, base, size); false when none.
bool module_of(uint64_t addr, std::string* name, uint64_t* base, uint64_t* size);
// The module's build id (64 hex digits) or "".
std::string module_build_id(const std::string& name);

}  // namespace nxrt::cheats::gmem
