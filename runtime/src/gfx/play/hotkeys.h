// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The overlay's toggles: the keys and controller chords that open the F1 settings menu ([menu] key / pad), the cheats menu ([cheats] key /
// pad), hide or show every pin ([hud] key / pad) and show or hide one pin group (its own key / pad, play/pin_board.h). Pure: no SDL state,
// no window, so the decisions are unit tested (tests/menu_ui).
//
// A key toggle is a key with modifiers ("F1", "Ctrl+F1", "Shift+Alt+P"). Its press with exactly those modifiers fires it, and that press's
// repeats and release belong to it too: none of them reach the game. The same key with other modifiers is not the toggle's and reaches the
// game (a menu on Ctrl+F1 leaves a plain F1 to the game).
//
// A pin's controller chord is opt-in (none by default) and never reaches the game (PadToggle): it must hold a button that presses nothing
// in the game (a rear grip or a trackpad click that [padmap] leaves unmapped) and that button must go down first; while it is held, the
// chord's other buttons are held back from the game, so the game sees neither a press nor a release of the chord. Holding game buttons
// first and then the free one does nothing (nothing fires by accident during play, and nothing the game already sees is taken away).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::play {

enum : uint8_t { kModCtrl = 1, kModShift = 2, kModAlt = 4, kModGui = 8 };

struct KeyChord {
    int32_t sc = 0;        // SDL scancode; 0 = none
    uint8_t mods = 0;      // kMod* (left and right count the same)
    bool empty() const { return sc == 0; }
    bool operator==(const KeyChord& o) const { return sc == o.sc && mods == o.mods; }
    bool operator!=(const KeyChord& o) const { return !(*this == o); }
};
// "F1", "ctrl+f1", "Shift + Alt + P", SDL's key names; "" or "none": empty. False for an unknown key, a modifier on its own or junk.
bool parse_key_chord(const std::string& text, KeyChord* out);
std::string key_chord_text(const KeyChord& k);      // "Ctrl+F1"; "none" when empty
uint8_t key_mods_of(uint16_t sdl_mods);             // SDL_Keymod bits -> kMod*
bool is_modifier_key(int32_t sc);                   // Ctrl, Shift, Alt, Gui (left or right)

// One key toggle's state (see the top of the file).
class KeyToggle {
public:
    enum Hit : uint8_t { NotOurs = 0, Fired, Swallowed };
    KeyChord chord;
    // A key event (down / up, repeat flag, the modifiers held). Fired: the press that toggles; Swallowed: a repeat or the release of it.
    Hit event(int32_t sc, uint16_t sdl_mods, bool down, bool repeat);
    void reset() { down_ = false; }
    bool held() const { return down_; }

private:
    bool down_ = false;
};

// The menu's key from the configuration: [menu] key (the app's setting), else the older [gfx] overlay_key, else F1. A value that is not
// a key gives F1 and a reason in *warn.
KeyChord menu_key_from_config(std::string* warn = nullptr);

// ---- controller chords (bits of PadSrc, play_types.h) ------------------------------------------------------------------------------
// The buttons that press nothing in the game: the rear grips and the trackpad clicks, unless the player's [padmap] gives them a button.
// `remap` is PadSettings::remap.
uint32_t free_pad_buttons(const uint64_t* remap);
// Why `chord` cannot toggle a pin (empty: it can): it needs two buttons or more, one of them free.
std::string pin_pad_problem(uint32_t chord, uint32_t free_bits);

// A pin's controller chord (see the top of the file). step() takes the physical buttons held now, once per pad poll.
class PadToggle {
public:
    uint32_t chord = 0;     // the buttons (0: none)
    uint32_t free = 0;      // which buttons press nothing in the game (free_pad_buttons)
    // True once each time the chord is completed with a free button down first. *mask gets the buttons held back from the game now.
    bool step(uint32_t held, uint32_t* mask);
    void reset() { armed_ = fired_ = false; prev_ = 0; }

private:
    uint32_t prev_ = 0;
    bool armed_ = false, fired_ = false;
};

// ---- conflicts ------------------------------------------------------------------------------------------------------------------------
// The toggles in force, by name, and the checks between them and the game's input. A key used twice, or one the game's keyboard
// bindings use, a chord that is the quit chord or contains another toggle's (pressing it would also fire the other).
struct ToggleSpec {
    std::string name;        // "Settings menu", "Cheats menu", "Hide all pins", "Pin: Performance"
    KeyChord key;
    uint32_t pad = 0;
};
struct Conflict {
    std::string what;        // a sentence for the menu's footer and the log
    int a = -1, b = -1;      // the toggles (indices), b = -1 against the game or the quit chord
};
// `game_keys`: the scancodes the game's keyboard bindings use (any context). `quit_pad`: the quit chord ([quit] pad).
std::vector<Conflict> toggle_conflicts(const std::vector<ToggleSpec>& t, const std::vector<int32_t>& game_keys, uint32_t quit_pad);

}  // namespace nxrt::play
