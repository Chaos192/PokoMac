// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Array-of-bytes patterns, first-class: parse and match patterns with `??` and nibble wildcards, scan them over guest memory (a module,
// region or range), and make the shortest unique signature for an address. Used by the scanner's AOB search, by AOB-anchored cheat-table
// entries and code patches (so they survive a game update), and by CE-style aobscan scripts.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scanner.h"

namespace nxrt::cheats {

// A byte pattern with per-byte care masks (so a nibble can be a wildcard). For byte i a candidate b matches when (b & mask[i]) == val[i].
struct AobPattern {
    std::vector<uint8_t> val;
    std::vector<uint8_t> mask;         // 0xFF every bit matters, 0x00 a full wildcard, 0xF0 / 0x0F a nibble
    size_t size() const { return val.size(); }
    bool empty() const { return val.empty(); }
    bool matches(const uint8_t* p) const {
        for (size_t i = 0; i < val.size(); ++i)
            if ((p[i] & mask[i]) != val[i]) return false;
        return true;
    }
    // How many of the pattern's bytes are not wildcards (a measure of how specific it is).
    size_t fixed_bytes() const {
        size_t n = 0;
        for (uint8_t m : mask) if (m == 0xFF) ++n;
        return n;
    }
};

// Parse "48 8B ?? 4?" (tokens of two characters, hex / '?' per nibble, whitespace separated; `*` is also a wildcard char). Also accepts a
// run with no spaces ("488B??"). Returns false with *err on a malformed token.
bool aob_parse(const std::string& text, AobPattern* out, std::string* err = nullptr);
// "48 8B ?? 4?" from a pattern.
std::string aob_text(const AobPattern& p);
// A plain pattern of exact bytes (no wildcards).
AobPattern aob_of_bytes(const uint8_t* bytes, size_t n);

struct AobHit { uint64_t addr; };

// Scan the regions the filter selects for every match of `pat`. `limit` caps the hits collected (0 = the built-in cap). Reads through src.
std::vector<AobHit> aob_scan(MemSource& src, const AobPattern& pat, const RegionFilter& filter, size_t limit = 100000, ScanProgress* prog = nullptr);
// Count matches within one address range (for signature uniqueness), stopping once `stop_at` is reached.
size_t aob_count_in_range(MemSource& src, const AobPattern& pat, uint64_t lo, uint64_t hi, size_t stop_at);

// A signature made for an address: the bytes starting at it, grown until unique in the chosen range, with byte positions in `wildcard`
// masked out (the disassembler marks relocated/immediate bytes so the signature survives a rebuild). `delta` is the signature's start
// relative to `addr` (0 here; a negative lead-in can be added later). `matches` is how many times it still matches (1 = unique).
struct Signature {
    AobPattern pattern;
    int64_t delta = 0;
    size_t matches = 0;
    bool unique() const { return matches == 1; }
};
// Build a signature for `addr`: read up to max_len bytes, wildcard the byte offsets listed in `wildcard` (relative to addr), and grow the
// length from min_len until it is unique within [range_lo, range_hi) or max_len is reached.
Signature aob_make_signature(MemSource& src, uint64_t addr, uint64_t range_lo, uint64_t range_hi,
                             const std::vector<size_t>& wildcard, size_t min_len = 8, size_t max_len = 64);

}  // namespace nxrt::cheats
