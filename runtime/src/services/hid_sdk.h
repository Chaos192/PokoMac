// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The SDK's nn::hid getters answered on the host (hid_sdk.cpp, [hid] sdk_hooks): the interface between it and the hid service (hid.cpp),
// and the npad layout rules, pure so the tests can check them.
//
// The game's sdk module reads controller state from the hid shared memory, whose layout changes between SDK versions; nxnative
// (seedds/nxnative, MIT: dm/host/hle_hid.c) answers the SDK's nn::hid functions instead, which needs no layout. The rules it found for
// which controller shows on which npad are the ones below. The requests that only configure the service (SetSupportedNpadStyleSet,
// SetSupportedNpadIdType, the style update event, vibration values, the hold type) still go to the hid service over IPC.
#pragma once
#include <cstdint>

#include "nxrt/input_source.h"

namespace nxrt::services {

// ---- hid.cpp's side ----------------------------------------------------------------------------------------------------------------------
void hid_read_input(nxrt::hid::InputSnapshot* out);                  // the input of one sample (the source's, or the default pads)
void hid_read_raw_input(nxrt::hid::InputSnapshot* out);              // the same before the controller applet's bench (hid_bench_apply)
void hid_supported_sets(uint32_t* styles, uint32_t* index_mask);     // what the game set: style set, npad ids by entry index
void hid_signal_style_event(uint32_t index);                         // the style update event of shared-memory entry `index`
bool hid_sampler_live();                                             // the 5 ms sampler runs (an applet resource exists, sampling on)
// The console mode changed (amctl::set_docked): one sample now, so the style update event of every npad whose style the change moved
// (player 1 between No1 and Handheld) is signalled before the OperationModeChanged notification. Never call it with am's lock held.
void hid_mode_changed();

// ---- hid_sdk.cpp's side ------------------------------------------------------------------------------------------------------------------
void hid_sdk_on_sample(const nxrt::hid::InputSnapshot& snap);        // every sample: press latches, the newest input, style changes
void hid_sdk_reset();
// The players the npad layout shows (a split pair of Joy-Cons as two) and player 1's npad id: the controller applet's answer (am.cpp). Takes
// am's lock (the operation mode): never call it with am's lock held.
int hid_sdk_players(uint32_t* selected_id);

// The controller applet's bench (nxnative 47343a42, services.md "The controller applet leaves out the players past the maximum"). On a
// Switch the applet leaves no more controllers connected than the game takes; answering with the count capped and every npad still
// connected made a one-player game see the second npad on its next frame and open the applet again (1,671 times in one Ocarina of Time
// run). So the players past the game's maximum, the highest first, are left out ("benched"): their npads read as not connected (both the
// shared memory and the SDK's getters: hid.cpp read_input applies it to every sample) and their style events fire; with a maximum of one
// (or single mode) their controllers feed player 1, so whichever controller the player holds plays. They count again when the applet
// opens for more players (the lowest first), when the game sets other supported npad ids (SetSupportedNpadIdType), or when their
// controller goes away; a controller that connects later is a player until the applet opens again. max: the applet argument's
// playerCountMax (0 or less: no maximum), 1 in single mode. Neither may be called with am's lock held.
void hid_applet_open(int max);                          // at the applet's start, before it counts the players for the minimum
int hid_applet_close(int max, uint32_t* selected_id);   // at its answer: benches, then returns hid_sdk_players(selected_id)
void hid_bench_apply(nxrt::hid::InputSnapshot& snap);   // hid.cpp read_input: the bench on one sample (and a controller gone leaves it)
void hid_bench_clear(const char* why);                  // everyone benched counts again
uint32_t hid_bench_mask();                              // bit p: player p + 1 is benched (tests)

namespace hidsdk {

// nn::hid::NpadStyleTag bits; the Joy-Con 2 forms (Switch 2 SDKs) as nxnative found them: a pair 0x1000 (with 0x4), a left 0x2000 (with 0x8),
// a right 0x4000 (with 0x10).
constexpr uint32_t kFullKey = 1u << 0, kHandheld = 1u << 1, kJoyDual = 1u << 2, kJoyLeft = 1u << 3, kJoyRight = 1u << 4;
constexpr uint32_t kJoyDual2 = 1u << 12, kJoyLeft2 = 1u << 13, kJoyRight2 = 1u << 14;
// Shared-memory entry indices (hid_shm.h): 0..7 No1..No8, 8 Handheld (id 0x20), 9 Other (id 0x10).
constexpr int kIndexHandheld = 8, kIndexOther = 9, kIndices = 10;
int index_of_id(uint32_t id);       // -1 for an id that is none of them
uint32_t id_of_index(int index);

enum class Kind : uint8_t { None, Pad, Dual, Left, Right };   // a controller of its own (full key / handheld), a pair of Joy-Cons, one Joy-Con

struct Config {
    uint32_t styles = 0xFFFFFFFFu;  // SetSupportedNpadStyleSet (everything until the game sets it)
    uint32_t index_mask = 0x3FFu;   // SetSupportedNpadIdType, bit = entry index
    bool docked = true;             // operation mode: docked (console) or handheld
    uint32_t single_mask = 0;       // bit n: the game asked for single Joy-Con assignment on No n+1
    uint8_t single_keep[8] = {};    // which Joy-Con of a split pair stays on its npad: 1 or 0 the left, 2 the right
    bool joy2 = false;              // the player holds Joy-Con 2s (the Joy-Con 2 mouse is in use on a Switch 2 title)
};

struct NpadView {
    Kind kind = Kind::None;
    int slot = -1;                  // the input slot (input_source.h numbering) that feeds it
    uint32_t style = 0;             // what GetNpadStyleSet answers
};

struct Layout {
    NpadView npad[kIndices];
    int player1 = 0;                // the entry player 1 is on: No1, or Handheld in handheld mode
    uint32_t merged = 0;            // input slots (bit n) whose player has no npad in a one-player game: they feed player 1
};

// Which controller shows on which npad, and as what:
//  - player 1 (input slot 0) is always connected: on Handheld (0x20) in handheld mode when the game supports it, else on No1;
//  - player n (slot n - 1, when connected) is on No n when the game supports that id; in a one-player game (no id past No1 supported) it
//    feeds player 1 instead, otherwise it is not used, as on the console;
//  - a controller of its own is a full-key controller; a game that takes no full-key controller gets it as a pair of Joy-Cons;
//  - a pair of Joy-Cons is one dual npad, unless the game asked for single assignment on that npad and takes single Joy-Cons: then the
//    left one stays (or the right one, as asked) and the other takes the lowest free supported npad;
//  - the Joy-Con 2 bits go beside the plain ones when the game supports them and the player holds Joy-Con 2s, or the game supports only them.
Layout resolve(const nxrt::hid::InputSnapshot& snap, const Config& c);

// The bench (hid_applet_open above), pure. bench: bit p = player p + 1 (input slot p, 1..7). apply_bench: those slots read as not
// connected; with `one`, their buttons and sticks feed player 1 (slot 0, and the handheld slot when it is connected). players_shown: the
// npads a layout shows. bench_release: the benched players count again, the lowest first, while the game sees fewer than max (max <= 0: no
// maximum). bench_take: the shown players past max are benched, the highest first; then one benched before takes the place of a higher
// player shown now (the lowest players stay).
void apply_bench(nxrt::hid::InputSnapshot& snap, uint32_t bench, bool one);
int players_shown(const Layout& L);
uint32_t bench_release(const nxrt::hid::InputSnapshot& raw, const Config& c, uint32_t bench, int max);
uint32_t bench_take(const nxrt::hid::InputSnapshot& raw, const Config& c, uint32_t bench, int max);

}  // namespace hidsdk
}  // namespace nxrt::services
