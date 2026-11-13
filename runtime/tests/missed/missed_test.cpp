// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch); PokoMac has no jit service, so its step 4 is left out.
// End-to-end test of code the lift did not see (nxrt/missed.h, docs/ARCHITECTURE.md) on the hand-made module of
// recomp/tests/missed_fixture.py, lifted at the default entry level:
//   1. call_mid makes an indirect call into the middle of F (F+0x10, an address taken in code): entered through the module's entry
//      table, in compiled code, with no unit and no compiler run
//   2. call_hidden calls H+0xc, an address nothing in the module says is code: recompiled on demand by the real helper (pkp_build.py
//      --ondemand: the lifter with the build's lift map, Zig with the build's flags), loaded, entered; the entered code jumps back above
//      the entry (the unit holds all of H). The second call is a plain hash hit; another address of H is entered through the same unit;
//      a second run of the test compiles nothing (the unit comes from the cache)
//   2b. a big function met at two addresses gets a unit for each (the second one is made although the first covers the function)
//   3. code made at run time (a code region with `movz x0, #99; ret`) is recompiled from its bytes
//   4. the jit service with a hand-made plugin (recomp/tests/jit_fixture.py): the plugin NRO is lifted whole on demand (or comes from
//      the cache) and prepared; the code its GenerateCode writes into the code memory is compiled from its bytes when the game calls it;
//      rewritten code drops the old unit and runs the new bytes; rewriting back finds the earlier unit in the cache; Control
//   5. missed.txt holds H+0xc once; with [missed] mode = record (what iPhone / iPad do) nothing is compiled and the message says that
//      the next build includes the code
//
//   nxrt_missed_test --work DIR --python PY --zig ZIG --expect fresh|cached [--drop-index]
//   --drop-index removes the cache's index first, as when every run that made a unit ended before it was recorded: a cached run must
//   still find each unit (by its file) and compile nothing
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/missed.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/nxmodule.h"

extern "C" const NxModule nxmod_fixture;
extern "C" const uint32_t nxmod_fixture_abi;

namespace {
constexpr uint64_t kBase = 0x5100000000ull;          // missed_fixture.py BASE
constexpr uint64_t kCallMid = kBase + 0x000, kF = kBase + 0x040, kCallHidden = kBase + 0x080, kH = kBase + 0x0c0;
constexpr uint64_t kCodeAt = 0x5200000000ull;        // where the test puts its run-time code (VirtualAlloc at a fixed address)

int g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        nxrt::log_flush();
        std::exit(1);
    }
}

std::string arg(int argc, char** argv, const char* name, const char* def = nullptr) {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    if (!def) {
        std::fprintf(stderr, "missing %s\n", name);
        std::exit(2);
    }
    return def;
}

int count_lines(const std::string& path, const std::string& prefix) {
    std::ifstream f(path);
    std::string line;
    int n = 0;
    while (std::getline(f, line)) n += line.compare(0, prefix.size(), prefix) == 0;
    return n;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string work = arg(argc, argv, "--work"), python = arg(argc, argv, "--python"), zig = arg(argc, argv, "--zig");
    const bool cached = arg(argc, argv, "--expect", "fresh") == "cached";
    CreateDirectoryA(work.c_str(), nullptr);
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--drop-index") == 0) DeleteFileA((work + "/cache/index.tsv").c_str());
    const std::string log_file = work + "/missed_test.log";
    nxrt::LogOptions lo;
    lo.path = log_file.c_str();
    lo.stderr_all = true;                      // the runtime's lines ("recompiled 0x... on demand in N ms") next to the checks
    nxrt::log_init(lo);
    // the manifest a build would write (pkp_build.py write_manifest), pointing the helper at this checkout's lifter and the given Zig
    const std::string manifest = work + "/manifest.tsv";
    {
        std::ofstream m(manifest);
        m << "format\t1\nlifter_hash\tfixture-test\ncache\tcache\nlog\tondemand.log\nmissed\tmissed.txt\n"
          << "command\t" << python << "\t-I\t" << NXRT_PKP_BUILD << "\t--ondemand\t--recomp\t" << NXRT_RECOMP_DIR << "\t--zig\t" << zig << "\n"
          << "module\tfixture\t5100000000\tfixture-build\t" << NXRT_FIXTURE_MAP << "\t" << NXRT_FIXTURE_ELF << "\n";
    }
    nxrt::config_set("missed", "manifest", manifest.c_str());
    const std::string missed = work + "/missed.txt";

    const nxrt::RegResult r = nxrt::nxmodule_register(nullptr, &nxmod_fixture, nxmod_fixture_abi);
    check(r.ok(), "the fixture module (ABI 4, one function with mid-function entries) registers");
    int n = 0;
    check(nxrt::nxmodule_entry_table(&nxmod_fixture, &n) && n == 1, "its entry table has the one function F");

    static Ctx c;
    std::vector<uint8_t> stack(1 << 20);
    auto call = [&](uint64_t a) {
        c.x[31] = (reinterpret_cast<uint64_t>(stack.data()) + stack.size()) & ~15ull;
        c.x[0] = 0;
        c.x[30] = 0;
        nxrt::dispatch_call(&c, a);
        return c.x[0];
    };

    // 1. mid-function entry through the build's table
    check(call(kCallMid) == 43, "call_mid: an indirect call into the middle of F returns 42 (+1)");
    nxrt::missed::Stats s = nxrt::missed::stats();
    check(s.static_entries == 1 && s.units_compiled == 0 && s.units_from_cache == 0, "... through F's entry table, no unit made or loaded");

    // 2. recompiled on demand (or from the cache)
    check(call(kCallHidden) == 51, "call_hidden: H+0xc, covered by no table, runs (50, then the jump back above it: 51)");
    s = nxrt::missed::stats();
    if (cached)
        check(s.units_compiled == 0 && s.units_from_cache == 1, "... from the title's cache: nothing compiled");
    else
        check(s.units_compiled == 1 && s.units_from_cache == 0, "... recompiled on demand once");
    check(call(kCallHidden) == 51 && nxrt::missed::stats().units_compiled == s.units_compiled &&
              nxrt::missed::stats().units_from_cache == s.units_from_cache,
          "a second call is a hash hit (nothing made or loaded again)");
    check(nxrt::fn_lookup(kH + 0xc) != nullptr, "H+0xc is now a function of the dispatcher's table");
    check(call(kH + 0x4) == 1, "another address of H (its loop head) enters the same unit");
    check(nxrt::missed::stats().units_compiled == s.units_compiled && nxrt::missed::stats().unit_entries >= 1, "... without another unit");
    check(count_lines(missed, "fixture\tcc\t") == 1, "missed.txt holds fixture+0xcc once");

    // 2b. a big function met at two addresses: a unit of a big function answers for its own address only (ondemand.py), so the
    //     second address needs a second unit although the first unit's row covers the whole function (that row's switch says no).
    //     F stands in for a big function: the helper gets NXRECOMP_UNIT_ALL_MAX=4 (F has 6 instructions).
#ifdef _WIN32
    _putenv_s("NXRECOMP_UNIT_ALL_MAX", "4");
#else
    setenv("NXRECOMP_UNIT_ALL_MAX", "4", 1);
#endif
    const nxrt::missed::Stats b0 = nxrt::missed::stats();
    check(call(kF + 0x4) == 3, "F+4 (movz x0, #2; movz x0, #3; ret), in no table: its own unit runs it");
    check(call(kF + 0xc) == 0, "F+0xc (ret), another address of F: entered through a second unit");
    const nxrt::missed::Stats b1 = nxrt::missed::stats();
    check(b1.units_compiled + b1.units_from_cache == b0.units_compiled + b0.units_from_cache + 2 && b1.compile_failures == b0.compile_failures,
          "... one unit for each address, made or loaded from the cache");
#ifdef _WIN32
    _putenv_s("NXRECOMP_UNIT_ALL_MAX", "");
#else
    unsetenv("NXRECOMP_UNIT_ALL_MAX");
#endif

    // 3. code made at run time
    void* mem = VirtualAlloc(reinterpret_cast<void*>(kCodeAt), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const bool fixed = mem == reinterpret_cast<void*>(kCodeAt);
    if (!mem) mem = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // any address (then no cache hit across runs)
    check(mem != nullptr, "memory for code made at run time");
    const uint32_t code[] = {0xd2800c60u, 0xd65f03c0u};   // movz x0, #99; ret
    std::memcpy(mem, code, sizeof code);
    const uint64_t at = reinterpret_cast<uint64_t>(mem);
    nxrt::missed::code_region_add(at, sizeof code, "code");
    const nxrt::missed::Stats before = nxrt::missed::stats();
    check(call(at) == 99, "code made at run time (movz x0, #99; ret) is recompiled from its bytes and runs");
    const nxrt::missed::Stats after = nxrt::missed::stats();
    if (cached && fixed)
        check(after.units_from_cache == before.units_from_cache + 1 && after.units_compiled == before.units_compiled, "... from the cache");
    else
        check(after.units_compiled == before.units_compiled + 1, "... compiled on demand");

    // 4. the jit service: not in PokoMac (no jit service), left out of this port

    // 5. what iPhone and iPad do: record only
    nxrt::missed::reset_for_tests();
    nxrt::config_set("missed", "mode", "record");
    check(!nxrt::missed::enter(&c, kF + 0x8), "record mode: F+8 (no entry of F) is not run");
    const std::string why = nxrt::missed::explain(kF + 0x8);
    std::printf("      message: %s\n", why.c_str());
    check(why.find("next build") != std::string::npos && why.find("fixture+0x48") != std::string::npos, "... and the message names it and the next build");
    check(nxrt::missed::stats().units_compiled == 0 && count_lines(missed, "fixture\t48\t") == 1, "... nothing compiled, recorded in missed.txt");

    std::printf("missed-code test: all %d checks passed (%s run)\n", g_checks, cached ? "cached" : "fresh");
    nxrt::log_flush();
    return 0;
}
