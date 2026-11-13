// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.cpp): the pointer over the game's UI panes, hover moves the game's own cursor
// there, left click confirms, right click and the wheel are menu bindings (pkp_data.cpp). Wired into play mode's GamePlay table by pkp_game.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

#include "play_game.h"

namespace nxrt::games::pkp {

void menu_install(uint64_t base);
void menu_remove();
play::Act menu_frame(bool menu_up, const play::mm::Geometry& g);   // main thread, once per frame (play_game.h GamePlay::menu_frame)
bool menu_over_value();                                             // the pointer is over a slider: the wheel steps it
bool menu_active();                                                 // the menu hooks saw an active pointer this frame
bool menu_open();                                                   // a menu with the game's cursor is on screen (for other parts of pkp)
bool menu_in_game(bool in_game);                                    // main thread, once per frame before the context: a menu is up over play
void menu_status_text(char* buf, size_t n);
void menu_set_quit_guide(uint64_t layout);                          // the title's Quit Game key guide (pkp_title.cpp): a click on it is (+)
void menu_set_sort_guide(uint64_t layout);                          // the bag screen's Sort key guide (pkp_sort.cpp): a click on it sorts
void menu_set_craft_guide(uint64_t layout);                         // the crafting screen's Search key guide (pkp_craft.cpp): a click on it asks

}  // namespace nxrt::games::pkp
