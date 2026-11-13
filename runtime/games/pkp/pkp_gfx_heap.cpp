// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the graphics memory pools for render sizes above the console's (pkp_gfx_heap.h). Addresses are offsets in `main` of the
// 2.0.1 update; the sites are in recomp/games/pkp/main.sites.
//
// The game's graphics init (0x1793db0, x1 = a config) creates its NVN memory pools from a fixed config that 0x112ec80 builds on its stack
// and passes at 0x112f008 (x1 = sp+8): the general pool (flags 0x22) u64 at sp+0x40 = 0x14c00000 (332 MiB), the render-target pool
// (flags 0xa2) at sp+0xa0 = 0xdac00000 (3500 MiB), the shader pool at sp+0xd0. Pool storage comes from a game heap whose size the
// memory-config callback 0x112fae4 sets: u64 0x137c07008 at [x3+0x48] (exactly the five pools, 332 + 3500 + 256 + 500 + 400 MiB, plus
// 0x7008) before it branches on at 0x112fb08 (x3 still the struct). Per-pixel buffers of the render size live in the general pool (at 3840x2160
// twelve 1D buffers of 0x40bf00 x 4 B, among others): with play mode's targets about 3800x2140 and up it was ~1 MB short, an allocation failed, the game backed
// out of its init and its teardown unmounted "save" twice (svcBreak, fs 2002-6905; fork #49).
// High-water marks at boot (render size: general / RT): 1920x1080 223.7 / 937.6 MiB, 2560x1440 262.9 / 1179.3, 3712x2088 330.6 / 1788.8:
// the general pool grows ~17-24 B per pixel, the RT pool ~150. Start-up is all those marks cover: the first world load (after the Ditto
// jumping loading screen) needs more of both pools, by an amount nobody has measured. With the first plan (general +28 B per pixel, the RT
// pool grown only past 937.6 MiB + 160 B per extra pixel + 1 GiB, so left at 3500 MiB at 4K) a 3840x2160 render size started up, then the
// world load never finished (#164: a 1920x1080 window at 200%) or crashed on a null pointer right after its first world frame (#119: a 4K
// window; the general pool already past 384 of its 499 MiB). 2560x1440 runs, and a window on a 4K monitor renders a little below
// 3840x2160 (its client area is smaller), so it runs too. Neither log tells which pool ran out, so both now grow with margin:
//   general  +56 B per pixel above 1920x1080 (twice the start-up rate: +332 MiB at 4K);
//   RT       +160 B per pixel, so it keeps the console's headroom above start-up, up to the 4 GiB cap (+595 MiB at 4K).
// The arena grows by the sum (a pool grown alone fails its storage allocation): +928 MiB at 4K, within the +937 MiB that the clamped 8K
// size used before (larger sizes now need more: untested). At 1920x1080 and below nothing is hooked. Pools stay under 4 GiB (the suballocator may keep 32-bit offsets): a
// render size whose start-up RT use plus 1 GiB would not fit is clamped to the largest that fits.
#include "pkp_gfx_heap.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kPassConfig = 0x112f008;     // mid: 0x112ec80 calling the graphics init 0x1793db0 (x1 = sp+8, the config)
constexpr uint64_t kArenaConfig = 0x112fb08;    // mid: the memory-config callback 0x112fae4 branching on (x3 = the struct)
constexpr uint64_t kGeneralOff = 0x40, kRtOff = 0xa0;   // from sp at kPassConfig
constexpr uint64_t kArenaOff = 0x48;                    // from x3 at kArenaConfig

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGeneralPool = 0x14c00000ull, kRtPool = 0xdac00000ull, kArena = 0x137c07008ull;
constexpr uint64_t kPoolCap = 0xfff00000ull;
constexpr int64_t kConsolePixels = 1920ll * 1080;
constexpr uint64_t kGeneralPerPixel = 56;
constexpr uint64_t kRtPerPixel = 160, kRtAt1080 = 9376ull * kMiB / 10, kRtMargin = 1024 * kMiB;

uint64_t round_up_mib(uint64_t v) { return (v + kMiB - 1) / kMiB * kMiB; }

GfxHeapPlan plan_for(int w, int h) {
    GfxHeapPlan p;
    p.w = w;
    p.h = h;
    const uint64_t dpx = static_cast<uint64_t>(std::max<int64_t>(0, static_cast<int64_t>(w) * h - kConsolePixels));
    p.extra_general = round_up_mib(kGeneralPerPixel * dpx);
    // the console's headroom kept (3500 MiB + the extra targets), up to the cap; never less than start-up use + 1 GiB (fits() checks that)
    const uint64_t rt_want = std::min(kRtPool + round_up_mib(kRtPerPixel * dpx), kPoolCap);
    const uint64_t rt_need = round_up_mib(kRtAt1080 + kRtPerPixel * dpx + kRtMargin);
    const uint64_t rt = std::max(rt_want, rt_need);
    p.extra_rt = rt > kRtPool ? rt - kRtPool : 0;
    p.general = kGeneralPool + p.extra_general;
    p.rt = kRtPool + p.extra_rt;
    p.arena = kArena + p.extra_general + p.extra_rt;
    return p;
}

bool fits(const GfxHeapPlan& p) { return p.general <= kPoolCap && p.rt <= kPoolCap; }

struct Heap {
    GfxHeapPlan plan;                           // fixed before the guest boots; both hooks read it
    std::atomic<bool> warned_cfg{false}, warned_arena{false};
    std::atomic<uint64_t> cfg_calls{0}, arena_calls{0};
};
Heap& heap() {
    static Heap h;
    return h;
}

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

int hook_pass_config(::Ctx* c, uint64_t, void*) {
    Heap& hp = heap();
    const uint64_t sp = c->x[31];
    if (!mem::is_mapped(sp + kGeneralOff, kRtOff - kGeneralOff + 8)) return 0;
    const uint64_t g = rd<uint64_t>(sp + kGeneralOff), rt = rd<uint64_t>(sp + kRtOff);
    if (g != kGeneralPool || rt != kRtPool) {
        if (!hp.warned_cfg.exchange(true))
            NXLOG_W(kTag, "graphics pools: unexpected config (general 0x%llx, render targets 0x%llx), left alone", static_cast<unsigned long long>(g),
                    static_cast<unsigned long long>(rt));
        return 0;
    }
    wr<uint64_t>(sp + kGeneralOff, hp.plan.general);
    wr<uint64_t>(sp + kRtOff, hp.plan.rt);
    if (hp.cfg_calls.fetch_add(1) == 0)
        NXLOG_I(kTag, "graphics pools: general %llu MiB, render targets %llu MiB", static_cast<unsigned long long>(hp.plan.general / kMiB),
                static_cast<unsigned long long>(hp.plan.rt / kMiB));
    return 0;
}

int hook_arena_config(::Ctx* c, uint64_t, void*) {
    Heap& hp = heap();
    const uint64_t a = c->x[3] + kArenaOff;
    if (!c->x[3] || !mem::is_mapped(a, 8)) return 0;
    const uint64_t v = rd<uint64_t>(a);
    if (v != kArena) {
        if (!hp.warned_arena.exchange(true))
            NXLOG_W(kTag, "graphics arena: unexpected size 0x%llx, left alone", static_cast<unsigned long long>(v));
        return 0;
    }
    wr<uint64_t>(a, hp.plan.arena);
    if (hp.arena_calls.fetch_add(1) == 0)
        NXLOG_I(kTag, "graphics arena: 0x%llx bytes", static_cast<unsigned long long>(hp.plan.arena));
    return 0;
}

}  // namespace

GfxHeapPlan gfx_heap_plan(int w, int h) {
    GfxHeapPlan p = plan_for(w, h);
    if (fits(p) || w <= 0 || h <= 0) return p;
    // the largest size of the same shape that fits: a 16:9 size in whole 32x18 steps (as pick_size makes them), any other in even sides
    const bool s169 = static_cast<int64_t>(w) * 9 == static_cast<int64_t>(h) * 16 && h % 18 == 0;
    const int step = s169 ? 18 : 2;
    for (int ch = h / step * step; ch >= step; ch -= step) {
        const int cw = s169 ? ch * 16 / 9 : static_cast<int>(std::lround(static_cast<double>(ch) * w / h)) & ~1;
        GfxHeapPlan q = plan_for(cw, ch);
        if (fits(q)) {
            q.clamped = true;
            return q;
        }
    }
    return plan_for(1920, 1080);   // not reached: any shape at 1080 lines fits
}

void gfx_heap_install(uint64_t base, const GfxHeapPlan& plan, std::vector<uint32_t>* hooks) {
    Heap& hp = heap();
    hp.plan = plan;
    if (plan.extra_general == 0 && plan.extra_rt == 0) {
        NXLOG_I(kTag, "graphics pools: the console's (game targets %dx%d), not hooked", plan.w, plan.h);
        return;
    }
    hooks->push_back(hook_add_mid(base + kPassConfig, &hook_pass_config));
    hooks->push_back(hook_add_mid(base + kArenaConfig, &hook_arena_config));
    NXLOG_I(kTag, "graphics pools for %dx%d game targets: general +%llu MiB (%llu MiB), render targets +%llu MiB (%llu MiB), arena 0x%llx (+%llu MiB)",
            plan.w, plan.h, static_cast<unsigned long long>(plan.extra_general / kMiB), static_cast<unsigned long long>(plan.general / kMiB),
            static_cast<unsigned long long>(plan.extra_rt / kMiB), static_cast<unsigned long long>(plan.rt / kMiB),
            static_cast<unsigned long long>(plan.arena), static_cast<unsigned long long>((plan.extra_general + plan.extra_rt) / kMiB));
}

}  // namespace nxrt::games::pkp
