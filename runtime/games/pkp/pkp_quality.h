// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, PC picture quality above the console's (pkp_quality.cpp): a quality preset that sets the renderer's texture sharpness
// ([display] lod_bias, anisotropy). The view distance is fixed at the console's (#249). Wired into play mode by pkp_game.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "play_types.h"

namespace nxrt::games::pkp {

void quality_install(uint64_t base, const play::Settings& s);
void quality_remove();
void quality_settings_changed(const play::Settings& s);
// F1 Graphics page: the values of a preset written into d. "switch2", "high", "ultra": texture LOD bias, anisotropy;
// "steamdeck", "legion_go_s", "laptop", "desktop": those and the render scale and frame limit. False for an unknown name (and "custom").
bool quality_preset(const char* name, play::DisplaySettings& d);
void quality_status_text(char* buf, size_t n);

}  // namespace nxrt::games::pkp
