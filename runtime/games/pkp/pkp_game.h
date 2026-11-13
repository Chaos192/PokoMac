// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Pokémon Pokopia game module (docs/GAME_BOUNDARY.md): what the game-independent base needs to run this title, as one
// nxrt::game::Info. Libraries (games/pkp/CMakeLists.txt), each a superset of the one before:
//   nxrt_game_pkp_data   identity, branding, factory controls.ini (pure data)   the launcher           nxrt_register_game_pkp_data()
//   nxrt_game_pkp_core   + title facts (needs nxrt)              the service tests      nxrt_register_game_pkp_core()
//   nxrt_game_pkp        + play-mode hooks (needs nxrt_gfx)      pkp.exe               nxrt_register_game_pkp()
// The game exe (game-build/src/pkp_main.cpp) calls nxrt_register_game_pkp() before nxrt::init().
#pragma once

#include "nxrt/game.h"

void nxrt_register_game_pkp();
void nxrt_register_game_pkp_core();
void nxrt_register_game_pkp_data();

namespace nxrt::games::pkp {
const game::Info& data_info();     // pkp_data.cpp
const game::Info& core_info();     // pkp_core.cpp
const game::Info& full_info();     // pkp_game.cpp
}  // namespace nxrt::games::pkp
