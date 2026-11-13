// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Crafting search (pkp_craft.cpp): a "(Y) Search" key guide in the crafting screen; Y or a click asks for a word on the game's keyboard and the
// recipe list keeps the recipes whose item name holds it. [pkp] craft_search (on).
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void craft_install(uint64_t base);
bool craft_click();   // main thread: a left click on the Search key guide (pkp_menu.cpp); the screen asks at its next frame. False (click not
                      // taken) unless the crafting screen is up and its key guide shows the Search entry

}  // namespace nxrt::games::pkp
