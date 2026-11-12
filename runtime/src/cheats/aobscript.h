// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Cheat Engine-style auto-assembler scripts for AArch64 guest code (phase 6, CHEATS.md "AOB scripts"): a table entry whose [ENABLE]
// section finds its code by an AOB signature and patches it, and whose [DISABLE] section puts it back. Because the code is found by
// pattern, not by address, a script keeps working after a game update moves the code (when the pattern no longer matches, enabling fails
// with a clear message and the entry stays off).
//
//   [ENABLE]
//   aobscanmodule(coindec, main, 08 05 00 51 ?? ?? 00 B9)   // find the "sub w8, w8, #1; str w8, [..]" that spends a coin
//   coindec:                                               // the write address (an expression: symbol, module, hex, +/-)
//     nop                                                  // any line the A64 assembler takes (a64::assemble), or db / dw / dd / dq
//   registersymbol(coindec)
//   [DISABLE]
//   coindec:
//     db 08 05 00 51                                       // the original bytes (a [DISABLE] that writes nothing restores what [ENABLE] wrote)
//   unregistersymbol(coindec)
//
// Statements (one per line; `//` and `{ ... }` are comments; names are case-insensitive):
//   aobscan(name, pattern)                    the first match anywhere in the guest's code and data
//   aobscanmodule(name, module, pattern)      the first match inside one module's image ("main", "sdk", ...)
//   aobscanregion(name, start, end, pattern)  the first match in [start, end)
//   define(name, expr)                        a named address or number
//   label(name)                               accepted (names need no declaration here)
//   registersymbol(name) / unregistersymbol(name)   publish a name to the cheat table (other entries and scripts can use it)
//   assert(expr, bytes)                       stop with an error unless the bytes at expr are these (`??` allowed)
//   <expr>:                                   set the write address
//   db / dw / dd / dq <values...>             bytes / 16 / 32 / 64-bit little-endian values (hex, or #decimal)
//   readmem(expr, n)                          the n bytes now at expr (to restore code a script overwrote)
//   <instruction>                             one A64 instruction (nop, ret, mov/movz/movn/movk, b/bl <expr>, .word ...)
// Expressions: terms joined by + and -; a term is a hex number (CE's default; 0x allowed), #decimal, a name (define / aobscan /
// a registered symbol) or a module name (its load address).
//
// Pure: no runtime dependency. Memory goes through a ScriptHost (the overlay passes the live guest memory; tests a fake).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "scanner.h"

namespace nxrt::cheats::aa {

struct Write {
    uint64_t addr = 0;
    std::vector<uint8_t> bytes;        // what the script writes
    std::vector<uint8_t> original;     // what was there before (read just before the write)
};

class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual MemSource& mem() = 0;
    // A module's image range (load address .. end); false for an unknown name.
    virtual bool module_range(const std::string& name, uint64_t* lo, uint64_t* hi) = 0;
    // Applies one write (data straight to memory; code through the live code patcher). False with *err when it could not be done.
    virtual bool write(const Write& w, std::string* err) = 0;
    // Symbols shared across scripts and the table (registersymbol); the host keeps them.
    virtual std::map<std::string, uint64_t>& symbols() = 0;
};

struct Section {
    std::vector<std::string> lines;
    std::vector<int> line_no;          // 1-based source line of each
};

struct Script {
    Section enable, disable;
    bool has_disable = false;
};

// Splits a script into its [ENABLE] / [DISABLE] sections (a script with neither header is all [ENABLE]). False with *err on a malformed
// layout (a second [ENABLE], text before the first header when there is one).
bool parse(const std::string& text, Script* out, std::string* err);

struct RunResult {
    bool ok = false;
    std::string error;                 // "line 3: aobscan coindec: the pattern is not in main (made for another build?)"
    std::vector<Write> writes;         // what was written, in order (the overlay keeps them to undo an [ENABLE] without a [DISABLE])
    std::map<std::string, uint64_t> names;   // every name the section defined (aobscan / define), for display
};

// Runs one section. A failure stops before the failing line's write; the writes before it stay done and are listed in the result, so the
// caller can undo them (enable: on error the overlay restores result.writes in reverse). `seed`: names known before the first line.
RunResult run(const Section& s, ScriptHost& host, const std::map<std::string, uint64_t>& seed = {});

// Enables a script: runs [ENABLE]; on failure undoes its writes. Disables: runs [DISABLE] with the names the enable found (so its
// `coindec:` resolves without scanning again), or, when the script has none, restores the writes the enable made. Both return the
// section's result.
RunResult enable(const Script& s, ScriptHost& host);
RunResult disable(const Script& s, ScriptHost& host, const RunResult& enabled);

// Parses an expression with the names known so far (the host's symbols + `local`). False with *err on an unknown name or bad syntax.
bool eval(const std::string& expr, ScriptHost& host, const std::map<std::string, uint64_t>& local, uint64_t* out, std::string* err);

// A ready-made script for "NOP this instruction" / "write these bytes at this signature": what the overlay's disassembler and signature
// maker hand to the table, so the patch survives an update. `module` scopes the scan; `offset` is from the match to the patched bytes.
std::string make_patch_script(const std::string& name, const std::string& module, const std::string& signature, int64_t offset,
                              const std::vector<uint8_t>& new_bytes, const std::vector<uint8_t>& old_bytes);

}  // namespace nxrt::cheats::aa
