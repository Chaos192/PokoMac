// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, button prompts (pkp_kbm.cpp): the game's inline button glyphs (the text tag ^30~<action>~) show the key bound to that
// button while a keyboard style shows, or the pad's own button icon (from the prompt icon pack) while an xbox/playstation style shows.
// Installed by pkp_play.cpp.
#pragma once
#include <cstdint>
#include <vector>

#include "nxrt/hooks.h"
#include "play_game.h"

namespace nxrt::games::pkp {

void kbm_install(uint64_t base, std::vector<uint32_t>* hooks);
void kbm_frame(::Ctx* c);                             // the game thread, once a frame (pkp_play.cpp frame hook)
void kbm_settings_changed(const play::Settings& s);   // main thread: the key names for every pad button
void kbm_status_text(char* buf, size_t n);

}  // namespace nxrt::games::pkp
