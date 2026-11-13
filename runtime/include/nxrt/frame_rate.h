// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Frame rate above the console's (docs/FINDINGS.md 2026-11-11 fps-unlock). [display] present_interval = auto | game | display:
//   auto (default; an empty value too): display as soon as the scans of the game's main module (nxrt/engine_fields.h) show it steps by
//     the time that passed (Unreal Engine; a target frame rate the game steps by; Unity whose scripts read Time.deltaTime), game until
//     then and otherwise. The log says which way the title went and why ("present_interval auto -> ...").
//   game (default): the console's behaviour. The present interval a game sets (nvnWindowSetPresentInterval, a Vulkan/EGL swap interval)
//     paces it: a frame presented with interval 2 stays two 60 Hz syncs ([display] present_interval_pacing), so a game that asks for
//     interval 2 runs at 30 as on the console.
//   display: the window presents at the host's frame target ([display] fps_limit: any rate the player chooses, auto = the screen's refresh
//     rate, 0 = unlimited) whatever interval the game set. nvnWindowGetPresentInterval answers the interval really presented (1, or 2 at a
//     30 target), the vi vsync event follows a target above 60 (with none: every present), and a Unity title's own limiter (engine rules)
//     is given the target unless [engine] game_fps is set. The only cap is the one the player chose. Right only for
//     a game that steps its logic by the time that passed; a game that steps a fixed amount per frame runs fast (it needs a logic-rate
//     patch first: mods). Per title in pkp.ini, or for every game from the environment (NXRT_DISPLAY_PRESENT_INTERVAL).
#pragma once
#include <cstdint>

namespace nxrt::frame_rate {

bool display_mode();                 // display mode now: the setting, or auto's decision (force_mode_for_test overrides)
void force_mode_for_test(int mode);  // 2 auto (decided afresh from engine_fields::evidence at every call), 1 display, 0 game, -1 the setting

// The host's frame target in frames per second (the frame limiter's effective rate: the player's cap, any rate; 0 = none, unlimited),
// noted by play mode whenever it applies the frame cap. Safe from any thread. Nothing here sets a rate of its own.
void note_target(double fps);
double target();
// Display mode with no target: unlimited. The vi vsync event then has no rate of its own: it ticks on every present (set_present_hook)
// and on the screen's refresh; a game's own limiter is given kEngineFpsUnlimited (the hold below brings it to what the frames reach in
// one window of 30 presents: it is only where the search starts).
bool unlimited();
constexpr int kEngineFpsUnlimited = 10000;   // unlimited: a rate past any machine's, where the hold starts
constexpr int kEngineFpsMax = 1000000;       // a target is given as it is up to this (the engine fields follow it: engine_fields.h apply_row)
void set_present_hook(void (*fn)());        // called on every present while unlimited() (vi_nv.cpp: the vsync event)

// The interval a window presents at for a game interval: in game mode the game's own; in display mode 1, or the most 60 Hz syncs that
// still reach a target below 59 (2 at 30, 1 at 40: the frame limiter keeps the rest). Interval 0 (no sync) stays 0 in both.
int window_interval(int game_interval);

// How many 60 Hz syncs a present holds the next acquire for (the window's pacing; 0: the frame limiter alone paces): the window interval
// when it is above 1 (a 30 target, a movie at the game's interval), 1 while a movie plays on a target above 60 or none, else 0.
int pace_syncs(int game_interval);

// The rate of the vi vsync event (Hz): 60 in game mode (the console's); in display mode the target when it is above 60, 60 for a target
// at or below 60 (the window interval gives 30, 20 ...) and while a movie plays, 0 when unlimited (no rate: vi ticks it on every present
// and on the screen's refresh). The hold below never lowers it: a game that steps by time is never held under what its frames reach.
double vsync_hz();

// The frame rate given to an engine limiter the runtime knows (Unity's targetFrameRate, the engine fields, a frame-step hook's target
// clock) in display mode: the target rounded (1 .. kEngineFpsUnlimited; kEngineFpsUnlimited with none), the rate the hold gives while it
// is under that, 60 while a movie plays; 0 outside display mode.
int engine_fps();

// The hold (nxnative frame-rate.md "120 fps follows the frame step", our own controller). A game that steps by the rate it is given
// (60 / target a frame) plays in slow motion when its frames do not reach it; so on a target above 60, or none, the host gives such a game
// the rate its frames really come at while they do not: every 30 presents (gaps over 250 ms, loading, are not counted) a window whose
// rate is more than 3 % under the rate given does not fit (the mean decides: a game that steps 1 / rate a frame keeps real time when its
// frames average the rate); the game is then given the window's own rate (frames / time, whole frames a second; never a rate of ours
// such as 60). Under the ceiling it tries a quarter more (at most the ceiling) after 2 s, a failed try doubles the wait up to 30 s, a
// good one lets the next follow at once. While a movie plays the game is given 60 (the console's rate its player takes pictures at).
// Only engine_fps() follows it: nothing paces the window to it (the game's own limiter keeps the rate it is given).
void note_present(uint64_t now_ns);   // once per present (NVN window, Vulkan layer swapchain); does nothing outside display mode
bool holding();                       // display mode, a target above 60 or none, and the game given less than it (or a movie playing)
int held_fps();                       // the rate given while holding() (0 otherwise)
void reset_hold_for_test();

// The host's frame step (frame-step hooks, nxrt/frame_step.h; nxnative frame-rate.md "sead's dynamic present interval, ksys::VFR and the
// host's frame step"). A game that steps a whole number of 60 Hz vsyncs a frame is handed the host's step, and its frames are then held on
// screen exactly that long (note_game_hold), so game time is real time and the picture does not judder. The step comes from the frame's
// work, not from how long it was held: the time between presents less the time the acquire waited for the hold (note_pace_wait), the
// mean of the last 4 frames, in vsyncs. k = 1 while that is at most 1.2 vsyncs; above it the next whole number (a step up at once, a step
// down only once the work fits 0.2 under it); at least 2 while a movie plays (a player that takes one picture a frame); within [min, max].
void note_frame_work(uint64_t now_ns);         // every present, in either mode (api_device.cpp, vkg_wsi.cpp): a frame ended
void note_pace_wait(uint64_t ns);              // the acquire waited that long for a held frame (api_device.cpp, vkg_wsi.cpp)
double frame_cost_syncs();                     // the mean work time of the last frames, in 60 Hz vsyncs (0: not known yet)
int host_step(int min_syncs, int max_syncs);
void note_game_hold(int syncs, uint64_t now_ns);   // hold the window's frames that many vsyncs (display mode; 0 = none); it lapses 300 ms
int game_hold(uint64_t now_ns);                     // the hold in force, or 0 (now_ns on the steady clock: frame_rate::steady_ns)
uint64_t steady_ns();
void note_cost_for_test(double syncs);              // tests: a frame of this work time (4 calls fill the mean)

}  // namespace nxrt::frame_rate
