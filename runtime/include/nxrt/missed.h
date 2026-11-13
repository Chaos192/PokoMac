// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Code the lift did not see (docs/ARCHITECTURE.md): what the dispatcher does with a guest address that is not the start of a lifted
// function, a thunk or anything else in its tables. Never an interpreter, never a JIT; in order:
//
//   1. Mid-function entries. A lifted module of ABI 4 lists the functions it can enter in the middle (NxModule.entries, recomp
//      nxrecomp/entries.py): the one whose range holds the address is entered there through its u_<entry>(c, addr), still in compiled
//      code. The function's own switch says whether the address is an entry (nx_no_entry is its default case: a probe that failed).
//   2. Units made earlier (this run, or found in the title's cache): small shared libraries the runtime loaded, each one function of
//      the module lifted again with every instruction an entry (recomp include/nxrecomp/ondemand.h).
//   3. Recompile on demand (desktop): the calling guest thread waits while the build's own lifter and toolchain make the unit for the
//      address (the command the build wrote in its manifest, pkp_build.py --ondemand); the unit is loaded, registered and entered, and
//      cached per title by (module build id, lifter hash, address range), so the same code is never compiled twice.
//   4. Every address that needed 2 or 3 is recorded in the title's missed.txt, which the next full build folds into the lift (the
//      addresses become entries or functions of the build itself). Where code cannot be loaded (iPhone, iPad: [missed] mode = record)
//      this is all that happens, and the player is told the next build includes it.
//
// Configuration ([missed] section):
//   mode = compile | record | off     default: compile when a manifest is found (desktop), else record; iOS: record
//   manifest = <file>                 the build's on-demand manifest (default: <exe folder>/build/missed/manifest.tsv)
//   file = <file>                     missed.txt (default: the manifest's, else <exe folder>/missed.txt)
//   timeout_s = 900                   longest wait for one unit
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/abi.h"

namespace nxrt::missed {

// Runs the guest code at `addr` on `c` by the steps above and returns true, or returns false (the caller traps) when nothing could run
// it; the address is recorded in missed.txt either way unless a table of the build covered it.
bool enter(::Ctx* c, uint64_t addr);

// Makes the code at `addr` ready ahead of its first call, on the calling (non-guest) thread: true when a call there will run without a
// compile (a function the dispatcher knows, a loaded unit that answers for it, or a unit found in the cache or made now), false with *err
// when no unit can be made. The cheats overlay's code patcher calls it for every address a live patch can reach (a code cave, the
// instruction after it) before it swaps the patch in, so no guest thread waits for the build's compiler (pokomac-port 2026-11-12: a code
// cheat turned on stalled a frame for 12.7 s on the PC while the game's thread waited for its cave to be compiled).
bool prepare(uint64_t addr, std::string* err);

// Code made at run time (the jit service's code memory; Switch games rarely have any): [base, base + size) holds guest code the game
// wrote. An address in it that no table covers is lifted from those bytes (nxrecomp ondemand.lift_code) and cached by their hash.
void code_region_add(uint64_t base, uint64_t size, const char* name);
// The code region that holds addr (its [base, end)), false when none does.
bool code_region_of(uint64_t addr, uint64_t* base, uint64_t* end);
void code_region_remove(uint64_t base);
// The game (the jit service's plugin) wrote new code into [addr, addr + size): every unit made for code there is dropped (its functions
// leave the dispatcher's table), so the next call lifts the new bytes, or finds them in the cache when they were there before.
void code_region_invalidate(uint64_t addr, uint64_t size);

// A whole module the game loads while it runs (an NRO: the jit service's plugin), its image mapped and relocated at `base`: lifted as the
// build lifts its own modules (pkp_build.py --ondemand --image), compiled into a module unit and registered (nxmodule_register) with
// NxModule.index `index`, or found in the title's cache (keyed by a hash of the image, the base, the index and the lifter). Null with *err
// when it cannot be made (no manifest: a build without the on-demand compiler, or iPhone / iPad).
const NxModule* load_module_unit(const char* name, uint64_t base, uint32_t index, const uint8_t* image, size_t size, std::string* err);

// A unit lifted from `bytes` (guest code at `base`, entered there) instead of the module file: the cheats overlay's code patcher runs a
// function whose instructions it changed this way (docs/CHEATS.md "Code patches"). `inline_sites` get an inline site each (hooks.h): the
// overlay's breakpoints and trace points. Made by the build's helper or found in the cache (keyed by the bytes, the sites and the lifter),
// loaded, and its function at `base` registered with the dispatcher. Returns that function, null with *err when it cannot be made (no
// on-demand compiler: iPhone / iPad, a build without the manifest).
GuestFn make_code_unit(const std::string& name, uint64_t base, const std::vector<uint8_t>& bytes, const std::vector<uint64_t>& inline_sites,
                       std::string* err);

// The unit make_code_unit made whose function is `fn` (what it returned): its entry by guest address, u_<base>(c, pc), which runs the
// function from `pc` (a loop head the live-patch check sends a running loop to: ctx.h NX_OVL). Null when the unit has no such entry table.
void (*code_unit_resume(GuestFn fn))(::Ctx*, uint64_t);

// What a player-facing error says when `addr` could not be run (enter returned false).
std::string explain(uint64_t addr);

struct Stats {
    uint64_t static_entries = 0;      // entries through a build's own tables (step 1)
    uint64_t unit_entries = 0;        // entries through units already loaded (step 2)
    uint64_t units_compiled = 0;      // units made by the helper this run
    uint64_t units_from_cache = 0;    // units loaded from the title's cache
    uint64_t units_dropped = 0;       // units dropped because the code they were made for was rewritten
    uint64_t modules_compiled = 0;    // module units (load_module_unit) made this run
    uint64_t modules_from_cache = 0;
    uint64_t compile_failures = 0;
    uint64_t recorded = 0;            // new lines written to missed.txt
    uint64_t compile_ms = 0;          // total time spent making units
};
Stats stats();
void reset_for_tests();               // forget loaded units, recorded addresses and the manifest (the libraries stay loaded)

}  // namespace nxrt::missed

namespace nxrt {
// The table of mid-function entries of a registered module (null with *n = 0 for an ABI 3 module or one without entries).
const NxEntryFn* nxmodule_entry_table(const NxModule* nx, int* n);
}
