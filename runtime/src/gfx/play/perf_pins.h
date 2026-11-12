// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The F1 settings menu's pins (pin_board.h): performance readouts and settings shown as readouts, pinned over the game while the menu is
// closed. The board in force comes from the settings file's [hud] (per game), else from the configuration's [hud] default (the launcher's
// default for every game, or this game's own); the menu edits it like any other setting.
//
// Items: fps, frametime (with a graph), low1 (the 1% low), gpu (GPU ms per frame), cpu (the render thread's CPU ms per frame), framegen
// (generated and presented per second, when on), upscaler (mode and scene size, when on), shaders (pipelines compiled in play: 0 after a
// bake), memory (the process), vram (video memory used / granted), resolution (the game's picture size and the output's); readouts of
// settings: set.vsync, set.fps_limit, set.framegen, set.hdr, set.latency, set.volume.
//
// Cost: the numbers are read a few times a second (refresh), the frame time every frame; drawing is a few small ImGui windows. The
// overlay only runs its ImGui frame while something is shown, so with no pin shown this costs nothing.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hotkeys.h"
#include "pin_board.h"

namespace nxrt::play {
struct Settings;
}

namespace nxrt::play::perf {

// The menu's pins in the settings file ([hud]): the groups, the hide-all toggle.
struct HudSettings {
    std::vector<pins::Group> groups;
    bool all_shown = true;           // [hud] shown
    KeyChord key;                    // [hud] key: hide or show every pin (F7 by default)
    uint32_t pad = 0;                // [hud] pad: the same from a controller (opt-in, hotkeys.h PadToggle rules)
    bool in_file = false;            // the file had a [hud] section (else the configuration's default groups are used)
    bool operator==(const HudSettings& o) const { return groups == o.groups && all_shown == o.all_shown && key == o.key && pad == o.pad; }
    bool operator!=(const HudSettings& o) const { return !(*this == o); }
};
HudSettings default_hud();           // F7, no groups
// The groups a game starts with when its settings file has none: [hud] default of the configuration (pkp.ini, or the launcher's default
// for every game through NXRT_HUD_DEFAULT). Bad text: none, and the reason in *err.
std::vector<pins::Group> configured_default_groups(std::string* err);

struct ItemInfo {
    const char* id;
    const char* name;      // "Frame rate"
    const char* desc;      // a line for the menu
    bool setting;          // a settings readout, not a measurement
};
const std::vector<ItemInfo>& catalogue();
const ItemInfo* item_info(const std::string& id);

// ---- what the readouts show ---------------------------------------------------------------------------------------------------------
struct Sample {
    double fps = 0, frame_ms = 0, low1_fps = 0, max_ms = 0;
    double gpu_ms = -1;              // the GPU's time per frame (-1: not measured)
    double render_cpu_ms = -1;       // the presenting thread's CPU time per frame (-1: not known yet)
    bool fg_on = false;
    double fg_real_fps = 0, fg_shown_fps = 0, fg_generated_ps = 0;
    std::string fg_backend;
    std::string upscaler;            // empty: off
    int scene_pct = 100;
    int64_t shaders_in_play = -1;    // pipelines compiled after the first present (-1: not known)
    uint64_t mem_bytes = 0;
    uint64_t vram_used = 0, vram_budget = 0;
    int src_w = 0, src_h = 0, out_w = 0, out_h = 0;
    std::string set_vsync, set_fps_limit, set_framegen, set_hdr, set_latency, set_volume;   // the settings readouts
    static constexpr int kGraph = 120;
    float frame_times[kGraph] = {};  // the last frame times (ms), a ring; the oldest at ft_head
    int ft_head = 0;
};
// The presenting thread's sample (the overlay fills it; the pins and the menu read it on the same thread).
Sample& sample();
void push_frame_time(float ms);      // every drawn frame while a pin is shown
// The settings readouts: set from the settings in force (the play host, any thread, whenever settings are applied) and copied into the
// sample by refresh_readouts (the presenting thread, at its refresh).
void fill_settings_readouts(const Settings& s);
void refresh_readouts();
// Measurements the overlay takes: the calling thread's CPU time (ns) and what the process holds in memory (bytes; 0: unknown).
uint64_t thread_cpu_ns();
uint64_t process_memory_bytes();
// The lines of an item from the sample (pins::Items for the menu's board).
void item_lines(const std::string& item, std::vector<pins::Line>& out);

// ---- the board in force ----------------------------------------------------------------------------------------------------------------
// The menu's pins as the game shows them while the menu is closed. Set by the play host whenever settings are applied; any thread.
void set_board(const HudSettings& h);
HudSettings board();
bool any_shown();                    // a group is shown and has items (the overlay then draws its frame)
// Draws the board in force over the game (the menu closed: display only). The presenting thread, inside an ImGui frame, theme applied.
void draw_board();
// Toggles. A key event (main thread): hide-all or a group's own key; true = it was one of them (the key never reaches the game).
bool key_event(int32_t sc, uint16_t sdl_mods, bool down, bool repeat);
// One pad (main thread, each poll; `slot` tells the pads apart, 0..7): the physical buttons it holds (PadSrc bits) and which ones press
// nothing in the game. Returns the buttons of it to hold back from the game (a pin's chord being held, hotkeys.h PadToggle).
uint32_t pad_step(int slot, uint32_t held, uint32_t free_bits);
// Where a toggle goes: the play host flips the setting and saves it (as the frame rate counter's hotkey does). `group` empty: hide-all.
void set_toggle_handler(std::function<void(const std::string& group)> fn);
// Flips a toggle in `h` (group empty: hide-all); false when there is no such group.
bool flip(HudSettings& h, const std::string& group);

}  // namespace nxrt::play::perf
