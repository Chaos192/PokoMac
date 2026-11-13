// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Pins: small readouts that stay over the game while the menus are closed. One engine for both menus: the F1 settings menu pins
// performance and settings readouts (play/perf_pins.h), the cheats menu pins its values and cheat codes (cheats_ui.cpp). Each menu owns a
// board of its own and manages only that board; a group never mixes the two.
//
// A board is a list of groups. A group is one pinned panel: a name, its items in order (one item, or several: a "group pin"), its place
// (UI units from the top left), size, opacity, layout (vertical, horizontal or compact) and its own toggle (a key, and optionally a
// controller chord: play/hotkeys.h). The board also has a toggle that hides or shows every group at once.
//
// Pins are display only: drawn with ImGuiWindowFlags_NoInputs (and never focused, never in the nav) whenever their menu is closed, so a
// click, the wheel, the keyboard and the pad all go to the game as if they were not there; they never pause the game or change its input.
// While the owning menu is open they can be dragged to a new place (the menu has the input then anyway).
//
// Text form of a group (the settings file's [hud] group lines and the launcher's default; no spaces needed, ';' never after a space):
//   perf:fps+frametime+low1;name=Performance;at=16,16;size=100;opacity=85;layout=vertical;key=F8;pad=none;shown=1
// Several groups in one value are joined by '|'. Unknown fields are refused; missing ones keep their defaults.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hotkeys.h"

namespace nxrt::play::pins {

enum class Layout : uint8_t { Vertical = 0, Horizontal, Compact };
const char* layout_name(Layout l);                     // "vertical" "horizontal" "compact"
bool parse_layout(const std::string& s, Layout* out);

// One line of a pin, made by the board's owner for an item.
struct Line {
    std::string label, value;
    int tone = 0;                     // 0 text, 1 accent, 2 warn, 3 good, 4 dim
    const float* graph = nullptr;     // a sparkline (a ring of graph_n values, the oldest at graph_head), drawn under or beside the value
    int graph_n = 0, graph_head = 0;
    float graph_max = 0.f;            // the top of the graph (0: the largest value)
    int icon = -1;                    // a mark before the label (vertical layout): -1 none, 0 a dot, else a cheats_theme.h Icon
};

struct Group {
    std::string id;                   // unique in its board: letters, digits, '_' and '-'
    std::string name;                 // its title (empty: none)
    std::vector<std::string> items;   // the item ids, in order
    float x = -1.f, y = -1.f;         // UI units from the top left (-1: stacked under the groups before it)
    int size = 100;                   // percent of the UI size, 50..200
    int opacity = 85;                 // percent, 20..100
    Layout layout = Layout::Vertical;
    bool shown = true;                // its own toggle's state
    KeyChord key;                     // its own toggle (empty: none)
    uint32_t pad = 0;                 // its optional controller chord (PadSrc bits, 0 = none; hotkeys.h PadToggle)
    bool operator==(const Group& o) const;
    bool operator!=(const Group& o) const { return !(*this == o); }
};

struct Board {
    std::vector<Group> groups;
    bool all_shown = true;            // the hide-all toggle's state
    Group* find(const std::string& id);
};

bool parse_group(const std::string& text, Group* out, std::string* err);
std::string format_group(const Group& g);
bool parse_groups(const std::string& text, std::vector<Group>* out, std::string* err);   // '|'-joined; "" = none
std::string format_groups(const std::vector<Group>& gs);
// A new group's id that the board does not use yet ("g2", "g3", ...).
std::string new_group_id(const Board& b);

// The lines of an item (the board's owner knows its items). Unknown items give nothing.
using Items = std::function<void(const std::string& item, std::vector<Line>& out)>;

// Draws the shown groups of the board into the current ImGui frame, in the overlay's theme (cheats_theme.h; the caller has applied it).
// interactive: the owning menu is open, so a group can be dragged (its x / y follow, *moved is set on the drop); otherwise the windows
// take no input at all. `scope` keeps the two boards' windows apart ("menu", "cheats").
void draw(Board& b, const Items& items, bool interactive, const char* scope, bool* moved = nullptr);
// The ImGui window name of a group (tests look its flags up).
std::string window_name(const char* scope, const Group& g);

}  // namespace nxrt::play::pins
