// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// In-game Options (pkp_options.cpp): the PC settings as tabs and rows of the game's own Settings screen. [pkp] options_menu, on by default.
#pragma once
#include <cstdint>

#include "play_game.h"

namespace nxrt::games::pkp {

void options_install(uint64_t base, play::GameHost* host);
void options_frame();   // frame boundary, main thread: the Mouse Mode Key row's key capture

}  // namespace nxrt::games::pkp
