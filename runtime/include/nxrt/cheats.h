// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Public entry points for the cheat engine (phase 6). The implementation and the overlay live in src/cheats and src/gfx; this header lets the
// game exe start the engine at title load, as a console's dmnt attaches before the first instruction. Everything is gated by [cheats] enable
// and does nothing unless a title and (optionally) a cheat file are present.
#pragma once

namespace nxrt::cheats {

// Starts or stops the 1/12 s cheat thread (runs the cheat VM and writes frozen addresses). Idempotent. Off by default.
void engine_enable(bool on);
bool engine_enabled();
// (Re)loads the running title's Atmosphère cheat file (<cheats dir>/<program id>/cheats/<build id>.txt). Safe any time.
void engine_reload();
// Sets the cheats folder (default <exe dir>/cheats or [cheats] dir). Call before the first enable.
void engine_set_folder(const char* dir);
// Reads [cheats] enable; if set, loads the title's cheat file and starts the engine. Call once after the modules are linked.
void engine_start_if_configured();

// The play loop's per-frame hook (play.cpp update_hook, on the presenting thread): runs the cheats' frame callbacks (hotkeys, scripts) and
// holds the game while the overlay has it paused (cheats/control.h). Cheap when nothing is set up.
void frame_hook();
// What a paused game's frame hook runs every few milliseconds: play.cpp pumps the window and the pads, runs the overlay and shows the last
// frame again, so the overlay stays usable over the still game.
void set_pause_service(void (*fn)());

}  // namespace nxrt::cheats
