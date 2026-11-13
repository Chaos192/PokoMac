// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Play mode: the settings page (the "Controls" window of the F1 overlay). The page only talks to the game through UiHost, so it can be
// tested and developed without a game running. It runs inside ImGui on the presenting thread; UiHost implementations are thread safe.
#pragma once
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "play_engine.h"
#include "play_types.h"

namespace nxrt::play {

struct LiveStatus {
    Ctx ctx = Ctx::Menu;               // the context the engine is in now
    const char* aim = "";              // why the mouse aim is on or off ("aim on", "locked on", "not in game" ...)
    bool mouse_captured = false;       // the pointer is captured (relative mode)
    int pads = 0;                      // open gamepads
    std::string pad_name;              // the first one
    bool pad_gyro = false, pad_rumble = false;
    const char* prompt_now = "";       // the button prompt style shown right now ("xbox" when Auto follows an Xbox pad); the Controller tab shows it under Auto
    double fps = 0;                    // presented frames per second (measured over the last second)
    const char* present_mode = "";     // "fifo" "mailbox" "immediate" ...
    const char* window_mode = "";      // "windowed" "borderless" "exclusive" (what the window is, not what the settings ask)
    int snap_k = 0;                    // the frame limit runs at the refresh rate / snap_k (0: not snapped)
    double snap_cap = 0;               // that rate
    float refresh_hz = 0;              // refresh rate of the display the window is on
    int width = 0, height = 0;         // window size in pixels
    EngineOut out;                     // what the keyboard and mouse press on the Switch pad right now
    double pitch_deg = 0;              // the aim's pitch
    uint64_t input_calls = 0;          // the game's per-tick player-input calls seen (GameHost::input_tick; 0: the game has not reached gameplay)
};

class UiHost {
public:
    virtual ~UiHost() = default;
    virtual Settings settings() const = 0;               // the settings in force (a copy)
    virtual Settings factory() const = 0;                // the factory settings
    // Queues new settings: the game's main thread applies them at its next frame (bindings, sensitivity, window mode, vsync, frame cap)
    // and writes them to the settings file. Cheap to call; the page calls it when the user changes something (not every frame).
    virtual void apply(const Settings& s) = 0;
    virtual LiveStatus status() const = 0;
    virtual std::string settings_path() const = 0;
    // Rebinding: after begin_capture() the next key press, mouse button or wheel notch does not reach the game or the page; it is returned
    // by capture_result() instead. Escape cancels (capturing() turns false, no result).
    virtual void begin_capture() = 0;
    virtual bool capturing() const = 0;
    virtual bool capture_result(InputId* out) = 0;       // true once per captured input
    virtual void cancel_capture() = 0;
    // An input the page must not let the player bind (the key that opens this menu). The page keeps waiting for another one.
    virtual bool reserved_key(const InputId& id) const { (void)id; return false; }
    // Opens the overlay's "Close the game?" prompt (the Quit button); the prompt itself ends the process when the player confirms.
    virtual void request_quit_prompt() {}
};

// What the overlay gives the menu for a frame, and what the menu gives back.
struct MenuFrame {
    ImTextureRef backdrop;            // glass: the game frame blurred (overlay.cpp's backdrop blur); none: the panel is solid
    bool pad = false;                 // a controller was used last: the hints show its buttons
    std::string menu_key, menu_pad;   // the toggles in force, for the footer ("F1", "back+right_stick"; empty = none)
    std::string cheats_key;           // the cheats menu's key (empty: the cheats are off)
    std::string pins_key;             // the hide-all-pins key
    std::vector<std::string> conflicts;   // toggle conflicts (hotkeys.h toggle_conflicts), shown in the footer
    std::function<void()> diagnostics;    // the overlay's diagnostics, drawn on the About page
    // out
    bool close = false;               // the close button
    bool glass = false;               // the panel was drawn as glass: the blur must cover glass_rect (pixels: x, y, w, h)
    int glass_rect[4] = {};
    float glass_blur = 0.f;
};

// The F1 settings menu: one panel in the overlay's theme (cheats_theme.h, shared with the cheats menu: the same themes, glass and
// accent) with the pages down the left: Display & FPS, Frame generation, Upscaler, Graphics, Audio, Controls (mouse, bindings, controller,
// Switch layout, status), Performance HUD (the pins, perf_pins.h), Game (when the registered game has tweaks) and About (with the overlay's
// diagnostics). A footer shows the description of the focused row, the controls of the device in use and the toggles in force. Works with
// a controller alone (D-pad / stick, A, B, L / R pages, ZL / ZR scroll, + close), a mouse alone and a keyboard alone. Draws its own window
// (call inside an ImGui frame with the theme applied, overlay.cpp), plus the pins while it is open (they can be dragged then). Keeps its
// editing state in a static (one menu per process).
void draw_menu(UiHost& host, MenuFrame& f);
// The same with no overlay around it (tests).
void draw_settings_page(UiHost& host);
// Tests: show a page (0 Display & FPS, 1 Frame generation, 2 Upscaler, 3 Graphics, 4 Audio, 5 Controls, 6 Performance HUD, 7 Game,
// 8 About), the Controls page's part (0 mouse .. 4 status) and the dock (0 left, 1 centre, 2 right); -1 keeps it.
void menu_show(int page, int controls_part = -1, int dock = -1);
int menu_page();
void menu_scroll(float units);   // tests: scroll the page down this many UI units (at the next frame)

// The About page (play_ui_about.cpp): version, build and the folders and files in use. Draws into the current window.
void draw_about_page(const std::string& settings_path);

}  // namespace nxrt::play
