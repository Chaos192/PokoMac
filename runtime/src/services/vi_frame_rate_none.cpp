// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What AnySwitch's vi_nv.cpp (by Chaos192, taken as it is) asks of AnySwitch's frame-rate engine and host refresh source
// (nxrt/frame_rate.h, nxrt/vblank.h), answered as PokoMac paces its frames: PokoMac keeps its own frame limiter and does not take
// AnySwitch's engine (frame_rate.cpp, vblank.cpp, engine_fields), so the vsync event ticks at 60 Hz on its own clock (scaled by the
// cheats overlay's speed hack in vi_nv.cpp), as PokoMac's vsync thread did. Pokopia draws through NVN and never opens vi or nvdrv.
#include <chrono>
#include <cstdint>

#include "nxrt/frame_rate.h"
#include "nxrt/vblank.h"

namespace nxrt::frame_rate {

double vsync_hz() { return 60.0; }
void note_present(uint64_t) {}
void set_present_hook(void (*)()) {}

}  // namespace nxrt::frame_rate

namespace nxrt::vblank {

bool screen(Screen*) { return false; }
const char* source_name() { return "none"; }
uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace nxrt::vblank
