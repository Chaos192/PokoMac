// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the graphics memory pools for render sizes above the console's (pkp_gfx_heap.cpp): the game sizes its NVN pools for
// 1920x1080 targets; play mode's render resolution (pkp_play.cpp) grows them, and the arena they come from, by what the larger targets need.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::games::pkp {

struct GfxHeapPlan {
    int w = 0, h = 0;                 // the game target size to use (the requested one, or smaller when that does not fit: clamped)
    bool clamped = false;
    uint64_t extra_general = 0;       // bytes added to the general pool (flags 0x22)
    uint64_t extra_rt = 0;            // bytes added to the render-target pool (flags 0xa2)
    uint64_t general = 0, rt = 0;     // the resulting pool sizes
    uint64_t arena = 0;               // the resulting graphics arena size
};

// Pure: the pool growth for game targets of w x h (the size the game itself builds them at), clamped (aspect kept, even sides; a 16:9 size
// in steps of 32x18) to the largest size whose pools stay under 4 GiB.
GfxHeapPlan gfx_heap_plan(int w, int h);

// Registers the two hooks (only when the plan grows a pool) and logs the plan. Call before the guest boots.
void gfx_heap_install(uint64_t base, const GfxHeapPlan& plan, std::vector<uint32_t>* hooks);

}  // namespace nxrt::games::pkp
