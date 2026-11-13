// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the PC features of play mode (pkp_play.cpp): native render resolution, frame rates above 60 at the right game speed,
// the raw mouse camera; keyboard and mouse button prompts (pkp_kbm.h). Wired into play mode's GamePlay table by pkp_game.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "play_game.h"

namespace nxrt::games::pkp {

void play_install_render(uint64_t base, const play::DisplaySettings& d, int window_w, int window_h);
void play_install(uint64_t base, play::GameHost* host, const play::Settings& s);
void play_remove();
void play_settings_changed(const play::Settings& s);
void play_frame(bool in_game, double step_seconds, play::GameFrame* out);   // the Joy-Con 2 mouse mode key
play::DisplayLive play_display_live();
uint32_t play_display_calls();
void play_status_text(char* buf, size_t n);
bool play_follow_live();   // the follow camera updated within the last second: the player is in the world (not the title)

}  // namespace nxrt::games::pkp
