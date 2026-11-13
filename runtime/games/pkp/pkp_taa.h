// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the engine's own temporal anti-aliasing (pkp_taa.cpp): a probe of the game's TAA switch and [pkp] game_taa, which turns it on.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::games::pkp {

void taa_install(uint64_t base);
void taa_remove();
void taa_status_text(char* buf, size_t n);

}  // namespace nxrt::games::pkp
