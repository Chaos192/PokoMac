// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay UI (phase 6): a Cheat Engine-class memory tool drawn with ImGui over the game, separate from the F1 dev menu and shown
// with its own key ([cheats] key, default F3) and controller combo ([cheats] pad). It draws into the existing overlay's ImGui context
// (overlay.cpp), so it shares input routing, the renderer hook and the DPI scale. Scans, freezes, hotkeys, scripts and watches keep
// running while it is hidden (the engine thread and the frame callbacks, not the windows, do the work).
//
// One panel with four pages, simple first: Cheats (the game's and mods' cheat codes, import / export), Find a value (a guided scan),
// My table (freeze, edit, hotkeys, groups, scripts) and Advanced (Pointer scanner, Snapshots, Memory viewer, Structure, Memory map,
// Labels, Disassembler, Find what accesses, Breakpoints, Code patches, Threads, Game control, Value graphs, Lua, Settings, Help; each can
// also open in a window of its own). Everything works with a controller alone (D-pad, A, B, X actions, Y the on-screen keyboard, L / R
// pages, ZL / ZR page, - undo, + close, with a button-hint bar), a mouse alone and a keyboard alone.
#pragma once
#include <cstdint>

#include <imgui.h>

namespace nxrt::gfx { class Device; }
struct ImFontAtlas;

namespace nxrt::cheats::ui {

// Applies the cheats theme to the current ImGui style (cheats_theme.h: the theme of [cheats] theme, the player's accent, style, opacity,
// blur, tint and size, all from the overlay's own settings). `blur_ok`: the backdrop blur can be drawn this frame (glass needs it).
void apply_theme(bool blur_ok);
// Glass: the blurred game frame the panel shows through (overlay.cpp's backdrop blur, extras/backdrop_blur.h), sampled at UV = frame
// pixel / frame size; a null reference: none this frame (glass draws solid). After draw(): the rectangle (pixels: x, y, w, h) the blur must
// cover and its strength (0 .. 1); false when the panel is not glass this frame.
void set_backdrop(ImTextureRef tex);
bool wants_glass();                    // the look asks for glass (the theme's style or [cheats] style): the blur is made only then
bool backdrop_rect(int out[4], float* strength);
// The look alone ([cheats] theme, accent, style, opacity, blur, tint, scale, animations): read once, also when the cheats are off, because
// the F1 settings menu and its pins wear the same look (one theme for both menus). init() calls it too. apply_theme() applies it.
void init_look();
bool animations();                     // [cheats] animations: the menus' short fades
// The cheats HUD's own toggle ([cheats] hud_key, a key; [cheats] hud_pad, an opt-in controller chord): shows or hides the HUD's pins.
void toggle_hud();
bool hud_shown();
// Adds the overlay's fonts (Inter) to the shared atlas; the atlas's first font stays the dev menu's. Once, after the default font.
void load_fonts(ImFontAtlas* atlas);
// The panel's close button was pressed: the overlay hides it (true once per press).
bool take_close_request();

// Draws the whole cheats overlay (menu bar, status bar and the open tool windows) into the current ImGui frame. Call between NewFrame and
// Render, only when visible. `has_game` is false in a headless test without a window (some actions are then disabled).
void draw(bool visible, bool has_game);

// The HUD over the game while the panel is closed: the values and cheat codes pinned in the overlay (and, with [cheats] mini, every
// active cheat and hotkey), at the place and opacity the player chose. Takes no input. Call only when hud_wanted().
void draw_mini();
bool hud_wanted();                     // something is pinned, or [cheats] mini is on
// Peek: the overlay key held (overlay.cpp: a press closes the panel, holding it fades the panel almost out until it is released).
void set_key_peek(bool on);

// One-time setup: reads [cheats] settings, loads the saved cheat table and window layout for the running title, starts the frame
// callback (hotkeys, Lua, value graphs). Safe to call again; the overlay calls it when [cheats] enable is set, before the first show.
void init();
// The game's icon for the overlay's and the menu's header is decoded on a thread of its own from the overlay's first frame on (cheats_icon.cpp)
void game_icon_prefetch();
// Saves the cheat table and the window layout for the running title (on hide, and every few seconds when something changed).
void save();

// Input the overlay sees, also while the cheats overlay is hidden: hotkeys of the table and of Lua scripts, and which device is in use.
void key_event(int sdl_scancode, bool down);
void pad_input(uint64_t hid_buttons);
bool hotkey_capture_active();          // the next key or pad press binds a hotkey: the overlay gives it to the UI only
bool keyboard_open();                  // the on-screen keyboard is up (+ confirms it there instead of closing the overlay)

// The auto accent: the colour of the game frame under the overlay (the overlay samples one frame when it opens).
bool wants_frame_colour();
void set_frame_colour(const unsigned char rgba[4]);
// The overlay's own "pause the game while it is open" (Game control), on top of [cheats] pause_game.
bool pause_while_open();
// The overlay was just shown (its fade-in starts).
void on_shown();

}  // namespace nxrt::cheats::ui
