// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// nn::fontll::ScalableFontEngine with the host's stand-in fonts (nxnative services.md "A shared font larger than the game's font engine
// memory"; the rule and its 512 KB are nxnative's, dm/host/hle_sdk16.c FONT_WORK_MIN, MIT). A game sizes each engine's work memory
// for Nintendo's fonts, and a stand-in with more glyphs or bigger tables needs more: Mario Tennis Fever gives its glyph thread's engine
// 50 KB, so FSS_load_font ran out of memory (201), SetFont failed (301) and every glyph lookup missed. Here Initialize with less than
// 512 KB runs the SDK's own Initialize on 512 KB of runtime memory instead; the game's buffer stays unused, GetPointerToWorkBuffer still
// returns it (the game frees it), and Finalize gives the runtime's memory back for the next engine.
//
// The SDK keeps the buffer at this+0xb0 and passes it to FS_init_ex (Super Mario Bros. Wonder sdk+0x308fbc; GetPointerToWorkBuffer is
// `ldr x0, [x0, #0xb0]`), so the three are taken over by name rather than by that layout, and each calls the SDK's own definition.
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

constexpr const char* kNames[3] = {
    "_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj",
    "_ZN2nn6fontll18ScalableFontEngine8FinalizeEv",
    "_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv",
};
enum { kInit = 0, kFinal = 1, kWork = 2 };
constexpr uint32_t kWorkMin = 512 * 1024;

struct Engines {
    std::mutex mu;
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> live;   // engine -> (the game's buffer, the runtime's)
    std::vector<uint64_t> spare;                               // runtime buffers of finalized engines (the kernel region is never freed)
    uint64_t original[3] = {};                                 // tests: the definitions to call
    bool logged = false;
};
Engines& engines() {
    static Engines* e = new Engines();
    return *e;
}

uint64_t original(int which) {
    {
        Engines& e = engines();
        std::lock_guard<std::mutex> lk(e.mu);
        if (e.original[which]) return e.original[which];
    }
    return nxmodule_find_export(kNames[which]);   // the SDK's own definition
}

// The SDK's function with the caller's registers, as if the caller had branched to it; false when no module defines it.
bool call_original(::Ctx* c, int which) {
    const uint64_t fn = original(which);
    if (!fn) {
        NXLOG_W("font", "%s: no module defines it", kNames[which]);
        c->x[0] = 0;
        return false;
    }
    const uint64_t lr = c->x[30], f = c->fn;
    dispatch_call(c, fn);
    c->x[30] = lr;
    c->fn = f;
    return true;
}

void engine_initialize(::Ctx* c) {   // Initialize(void* work, u32 size)
    const uint64_t self = c->x[0], work = c->x[1];
    const uint32_t size = static_cast<uint32_t>(c->x[2]);
    if (size >= kWorkMin) {
        call_original(c, kInit);
        return;
    }
    Engines& e = engines();
    uint64_t mine = 0;
    {
        std::lock_guard<std::mutex> lk(e.mu);
        if (!e.spare.empty()) {
            mine = e.spare.back();
            e.spare.pop_back();
        }
        if (!e.logged) {
            e.logged = true;
            NXLOG_I("font", "ScalableFontEngine::Initialize with %u bytes: given %u bytes of runtime memory (the stand-in fonts need more "
                    "than the game's engine memory was sized for)", size, kWorkMin);
        }
    }
    if (!mine) mine = mem::alloc_kernel(kWorkMin, 0x1000);
    if (!mine) {
        call_original(c, kInit);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(e.mu);
        e.live[self] = {work, mine};
    }
    c->x[1] = mine;
    c->x[2] = kWorkMin;
    call_original(c, kInit);
}

void engine_finalize(::Ctx* c) {
    const uint64_t self = c->x[0];
    call_original(c, kFinal);
    Engines& e = engines();
    std::lock_guard<std::mutex> lk(e.mu);
    auto it = e.live.find(self);
    if (it == e.live.end()) return;
    e.spare.push_back(it->second.second);
    e.live.erase(it);
}

void engine_work_buffer(::Ctx* c) {
    const uint64_t self = c->x[0];
    {
        Engines& e = engines();
        std::lock_guard<std::mutex> lk(e.mu);
        auto it = e.live.find(self);
        if (it != e.live.end()) {
            c->x[0] = it->second.first;   // the game's own buffer, which it frees
            return;
        }
    }
    call_original(c, kWork);
}

}  // namespace

void install_font_engine_hle() {
    const HleFn fns[3] = {engine_initialize, engine_finalize, engine_work_buffer};
    for (int i = 0; i < 3; ++i) {
        hle_register(kNames[i], fns[i]);
        module_override_symbol(kNames[i]);   // the game's imports; the SDK's own definitions stay what these call
    }
}

void font_engine_set_originals_for_tests(uint64_t init, uint64_t fin, uint64_t work) {
    Engines& e = engines();
    std::lock_guard<std::mutex> lk(e.mu);
    e.original[kInit] = init;
    e.original[kFinal] = fin;
    e.original[kWork] = work;
    e.live.clear();
}

}  // namespace nxrt::services
