// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The overlay's own screens (overlay.cpp), drawn in the overlay's theme (cheats_theme.h): the on-screen keyboard of the software keyboard
// applet, the quit and controller prompts, the frame-rate counter, the "Preparing shaders" and "Save failed" notices and the touch
// screen's menu button. overlay.cpp keeps their state and input; these draw them from plain values, so the screenshot tests
// (tests/menu_ui) draw exactly what a game shows.
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

namespace nxrt::play::screens {

// ---- the software keyboard ----------------------------------------------------------------------------------------------------------
struct KeyboardKey {
    std::string label;
    int w = 1;                 // column units of the 10-unit grid
    bool accent = false;       // the confirming key (OK)
    bool on = false;           // a latched key (Shift)
    bool disabled = false;     // OK while the text is shorter than the minimum
};
struct KeyboardView {
    std::string title;         // the dialog's title ("Keyboard" when the game gives none)
    std::string header, sub;   // the game's question and its explanation
    std::string guide;         // the field's placeholder
    std::string shown;         // the text as shown (password: stars)
    std::string before_caret;  // the shown text up to the caret
    std::string count;         // "3 / 12", or empty
    bool caret_on = true;      // the caret's blink phase
    bool keys = true;          // the on-screen keys are shown (with Steam's keyboard: only after a pad press)
    bool steam = false;        // Steam's keyboard is wanted for this question (the dialog sits at the top)
    bool pad = true;           // a controller was used last: the hints show its buttons
    bool by_label = false;     // the pad's face buttons by printed label (else by position)
    int sel_row = -1, sel_key = -1;   // the pad's key
    std::vector<std::vector<KeyboardKey>> rows;
};
struct KeyboardResult {
    int row = -1, key = -1;    // the key the mouse pressed
    bool field_placed = false; // the text field's place on screen (for Steam's keyboard), in pixels
    float field[4] = {};
};
// Inside an ImGui frame: the dialog with the field, the keys and the hints.
void keyboard(const KeyboardView& v, KeyboardResult* r);

// ---- prompts ------------------------------------------------------------------------------------------------------------------------
enum class QuitChoice { None, Quit, Cancel, Settings };
// "Close the game?": Quit, Cancel, Settings; `closing` once confirmed (the buttons wait).
QuitChoice quit_prompt(bool closing, bool pad, bool by_label);
// "Connect controllers": the game wants lo..hi players, `players` connected. True when Cancel was pressed.
bool ctrl_prompt(int lo, int hi, int players, bool pad, bool by_label);

// ---- notices over the game (the foreground list: no window, no input) -----------------------------------------------------------------
// The frame-rate counter: `lines` (the first is the frame rate) in `corner` (0 top left, 1 top right, 2 bottom left, 3 bottom right).
void fps_counter(ImDrawList* dl, const std::string* lines, int n, int corner, ImVec2 frame);
// "Preparing shaders (n left)" at the bottom centre.
void shader_note(ImDrawList* dl, int pending, ImVec2 frame);
// "Save failed": the reason; at the top centre, in the danger colour.
void save_notice(ImDrawList* dl, const char* reason, ImVec2 frame);
// The touch screen's corner button ("Menu" while the menu is hidden, "Close" while it is open) in `rect` (x, y, w, h).
void touch_button(ImDrawList* dl, ImVec4 rect, bool open);

}  // namespace nxrt::play::screens
