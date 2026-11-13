// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode: where a mouse event goes when it came from a finger. SDL turns a touch into mouse events (motion and the left button) and the
// platform layer marks those with input::kFlagTouch. A finger should work the menus (the game's own touch screen, the pointer over its menus,
// the F1 menu), but it is not a mouse in play: a tap must not fire the left-click action (use the tool), and touching must not switch the
// button prompts to the keyboard's keys on a handheld or a touch laptop. Pure, no state.
#pragma once
#include <cstdint>
#include "nxrt/input.h"

namespace nxrt::play {

struct MouseRoute {
    bool pointer;           // feeds the menu pointer (which also gives the game its touch screen)
    bool game_click;        // is a click for the game's bindings (Mouse 1 = primary and so on)
    bool sets_last_device;  // counts as the player using the keyboard and mouse (the prompts follow it)
};

constexpr bool is_touch_event(uint8_t flags) { return (flags & input::kFlagTouch) != 0; }

constexpr MouseRoute route_mouse(uint8_t flags) {
    return is_touch_event(flags) ? MouseRoute{true, false, false} : MouseRoute{true, true, true};
}

}  // namespace nxrt::play
