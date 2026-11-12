// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Code patches (codepatch.h) in a build without live code patches. AnySwitch's codepatch.cpp lifts a patched function again into an
// on-demand unit and swaps it in while the game runs; that needs the lifter's live-patch check (NX_OV) in the game code and the on-demand
// compiler, which this build does not have yet. Here the overlay still finds the lifted function at an address (function_at, where), and
// every patch, injection or breakpoint is refused with the reason, so the overlay says so instead of writing bytes the compiled game never
// runs. Memory cheats (values, freezes, Atmosphere cheat codes that write data) are not affected.
#include "codepatch.h"

#include "gmem.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

namespace nxrt::cheats::cp {

namespace {
const char* const kNoLive = "code patches need the live-patch build of the game, which this PokoMac build does not have yet";
}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::Compiling: return "compiling";
        case State::Live: return "live";
        case State::DispatchOnly: return "dispatch only";
        case State::Failed: return "failed";
        case State::Reverted: return "reverted";
    }
    return "?";
}

bool function_at(uint64_t addr, std::string* module, uint64_t* start, uint64_t* end) {
    const NxModule* nx = nxmodule_at(addr);
    if (!nx || !nx->fns || nx->nfns <= 0 || addr >= nx->text_end) return false;
    int lo = 0, hi = nx->nfns;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (nx->fns[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    // fns may list one address twice (a function and its alias): take the last start at or below addr, the end is the next higher start
    const uint64_t s = nx->fns[lo - 1].addr;
    uint64_t e = nx->text_end;
    for (int i = lo; i < nx->nfns; ++i)
        if (nx->fns[i].addr > s) { e = nx->fns[i].addr; break; }
    if (module) *module = nx->name ? nx->name : "";
    if (start) *start = s;
    if (end) *end = e;
    return true;
}

std::string where(uint64_t addr) {
    std::string out = gmem::describe(addr);
    std::string mod;
    uint64_t s = 0, e = 0;
    if (function_at(addr, &mod, &s, &e)) {
        std::string fn = gmem::describe(s);
        if (Module* m = module_at(s)) {
            const std::string sym = m->describe(s);
            if (!sym.empty()) fn += " " + sym;
        }
        out += " (fn " + fn + ")";
    }
    return out;
}

bool apply(uint64_t, const std::vector<uint8_t>&, std::string* err) {
    if (err) *err = kNoLive;
    return false;
}
bool revert(uint64_t, std::string* err) {
    if (err) *err = kNoLive;
    return false;
}
std::vector<Patch> patches() { return {}; }
bool guest_code_written(uint64_t, size_t, std::string* err) {
    if (err) *err = kNoLive;
    return false;
}
void code_unmapped(uint64_t, uint64_t) {}
std::vector<uint8_t> ips_for_module(const std::string&) { return {}; }
std::vector<std::string> write_ips(const std::string&, std::string* err) {
    if (err) err->clear();
    return {};
}
bool inject(uint64_t, const std::vector<std::string>&, std::string* err) {
    if (err) *err = kNoLive;
    return false;
}
bool bp_add(uint64_t, bool, std::string* err) {
    if (err) *err = kNoLive;
    return false;
}
bool bp_remove(uint64_t, std::string* err) {
    if (err) *err = kNoLive;
    return false;
}
void bp_enable(uint64_t, bool) {}
std::vector<Breakpoint> breakpoints() { return {}; }
bool held(Regs*) { return false; }
void resume(Resume) {}

}  // namespace nxrt::cheats::cp
