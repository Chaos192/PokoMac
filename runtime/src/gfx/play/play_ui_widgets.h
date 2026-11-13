// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The F1 settings menu's rows and cards, in the overlay's theme (cheats_theme.h): every page of play_ui*.cpp lays out with these, so the
// menu reads as one product with the cheats menu and the app. A row is a label on the left and its control in a column on the right; a
// card groups rows under a title. Implemented in play_ui.cpp.
#pragma once
#include <imgui.h>

namespace nxrt::play::widgets {

// Starts a card titled `title` (ends the card before it, if one is open). end_card() ends the open one; a page ends its last card itself.
void card(const char* title);
void end_card();
// Draws a row's label ("Text##id": the text before ## is shown) and puts the cursor in the control column. Returns the id to give the
// control ("##id"); a label with no text ("##id") is returned as it is and lays out nothing (a table cell).
const char* row(const char* label);
float control_x();                     // the control column of the last row (window coordinates)
// A switch row (the launcher's switch); true when flipped.
bool check(const char* label, bool* v);
// Controls laid out as rows.
bool combo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
bool slider_int(const char* label, int* v, int lo, int hi, const char* format = "%d", ImGuiSliderFlags flags = 0);
bool slider_float(const char* label, float* v, float lo, float hi, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
bool input_int(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0);
// A row of choices as one segmented control; true when another one was picked.
bool segmented(const char* label, const char* const* names, int n, int* cur);
// The description of the item drawn last: shown in the menu's footer while it has the focus or the mouse (and as a tooltip).
void tip(const char* text);
// Marks the control drawn last as one that applies at the next start (a note under it).
void restart_badge();
// Text in the theme's colours.
void note(const char* fmt, ...) IM_FMTARGS(1);           // secondary text, wrapped
void warn(const char* fmt, ...) IM_FMTARGS(1);           // the theme's warning colour, wrapped
ImVec4 col_warn();
ImVec4 col_danger();
ImVec4 col_ok();
ImVec4 col_accent();
ImVec4 col_off();
ImVec4 col_warn_tint();

}  // namespace nxrt::play::widgets
