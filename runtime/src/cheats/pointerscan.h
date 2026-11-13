// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Pointer scanner: heap and alias addresses move between runs, so a cheat needs a pointer path rooted in a stable place (a module image at
// its fixed load address). Given a target address, it finds multi-level paths module+offset -> *() + off -> ... -> target, up to a maximum
// depth and offset. A path can be resolved in the current run, rescanned against a second run to drop the ones that did not survive, and
// exported as an Atmosphère pointer cheat (export_pointer_body).
//
// A path matches export_pointer_body(mem_type, base_offset, offsets, value): slot0 = base(mem_type) + base_offset is a static pointer;
// ptr = *slot0; for each offset but the last ptr = *(ptr + off); the target is ptr + offsets.back().
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "scanner.h"

namespace nxrt::cheats {

// A stable root for a path: a module image at its fixed base. Paths start inside one of these.
struct StaticRange {
    uint64_t lo = 0, hi = 0;
    uint64_t base = 0;             // the module's load address (base_off = slot - base)
    std::string module;            // "main", "sdk", ...
    unsigned mem_type = 0;         // the Atmosphère nybble for base() (0 main; 5..F also main)
};

// A pointer map: every aligned 8-byte slot in the scanned regions whose value points into guest memory, sorted by the pointed value for
// range queries. Built once, reused for many target searches.
struct PointerMap {
    std::vector<std::pair<uint64_t, uint64_t>> by_value;   // (value, slot address), sorted by value
    size_t slots() const { return by_value.size(); }
};

struct PointerScanOptions {
    RegionFilter scan;             // where pointers may live (heap, alias, data; default those)
    uint64_t max_offset = 0x1000;  // the largest + offset at each level
    unsigned max_depth = 4;        // levels from the static root
    size_t max_paths = 4096;       // stop after this many paths
    unsigned align = 8;            // pointer alignment (8)
};

struct PointerPath {
    std::string module;            // the static root module
    unsigned mem_type = 0;
    uint64_t base_offset = 0;      // slot0 = base + base_offset
    std::vector<uint64_t> offsets; // the chain (the last is added, not dereferenced)
    int depth() const { return static_cast<int>(offsets.size()); }
    std::string text() const;      // "main+0x1234 -> +0x10 -> +0x8"
};

// Build the pointer map over the regions the options select. Values that point into one of `in_ranges` (any mapped guest region) are kept.
PointerMap pointer_map_build(MemSource& src, const PointerScanOptions& opt, const std::vector<ScanRegion>& in_ranges, ScanProgress* prog = nullptr);

// Find paths to `target` from the static roots, using the map. Returns up to opt.max_paths paths, shallowest first.
std::vector<PointerPath> pointer_find_paths(const PointerMap& map, const std::vector<StaticRange>& roots, uint64_t target, const PointerScanOptions& opt);

// Resolve a path against the current memory. Returns the target address, or 0 if a dereference failed. `module_base(name)` gives a module's
// current load address.
uint64_t pointer_resolve(MemSource& src, const PointerPath& p, uint64_t (*module_base)(const std::string&));
// Keep the paths that still resolve to `new_target` in the current run (a restart rescan).
std::vector<PointerPath> pointer_rescan(MemSource& src, const std::vector<PointerPath>& paths, uint64_t new_target, uint64_t (*module_base)(const std::string&));

}  // namespace nxrt::cheats
