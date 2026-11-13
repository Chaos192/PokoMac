// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The overlay's toggles: key chords, pin controller chords and their conflicts (hotkeys.h).
#include "hotkeys.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>

#include "nxrt/config.h"
#include "play_types.h"

namespace nxrt::play {

namespace {
std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}
std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
uint8_t modifier_word(const std::string& w) {
    const std::string l = lower_copy(w);
    if (l == "ctrl" || l == "control") return kModCtrl;
    if (l == "shift") return kModShift;
    if (l == "alt" || l == "option") return kModAlt;
    if (l == "gui" || l == "cmd" || l == "command" || l == "win" || l == "super" || l == "meta") return kModGui;
    return 0;
}
}  // namespace

bool is_modifier_key(int32_t sc) {
    switch (sc) {
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return true;
        default: return false;
    }
}

uint8_t key_mods_of(uint16_t m) {
    uint8_t r = 0;
    if (m & SDL_KMOD_CTRL) r |= kModCtrl;
    if (m & SDL_KMOD_SHIFT) r |= kModShift;
    if (m & SDL_KMOD_ALT) r |= kModAlt;
    if (m & SDL_KMOD_GUI) r |= kModGui;
    return r;
}

bool parse_key_chord(const std::string& text, KeyChord* out) {
    const std::string t = trim_copy(text);
    KeyChord k;
    if (t.empty() || lower_copy(t) == "none") {
        if (out) *out = k;
        return true;
    }
    // modifiers first, the key last; '+' itself as the key is "Plus" in SDL's names ("Keypad +"), so split on '+' only before the last part
    size_t b = 0;
    for (;;) {
        const size_t e = t.find('+', b);
        const bool last = e == std::string::npos || e + 1 >= t.size();   // "Keypad +": the '+' at the end is the key's own
        const std::string part = trim_copy(last ? t.substr(b) : t.substr(b, e - b));
        if (last) {
            if (part.empty()) return false;
            if (modifier_word(part)) return false;   // a modifier on its own is not a key
            const SDL_Scancode sc = SDL_GetScancodeFromName(part.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN || is_modifier_key(sc)) return false;
            k.sc = sc;
            break;
        }
        const uint8_t m = modifier_word(part);
        if (!m) return false;
        k.mods |= m;
        b = e + 1;
    }
    if (out) *out = k;
    return true;
}

std::string key_chord_text(const KeyChord& k) {
    if (k.empty()) return "none";
    std::string o;
    if (k.mods & kModCtrl) o += "Ctrl+";
    if (k.mods & kModShift) o += "Shift+";
    if (k.mods & kModAlt) o += "Alt+";
    if (k.mods & kModGui) o += "Gui+";
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(k.sc));
    o += (n && *n) ? n : "?";
    return o;
}

KeyToggle::Hit KeyToggle::event(int32_t sc, uint16_t sdl_mods, bool down, bool repeat) {
    if (chord.empty() || sc != chord.sc) return NotOurs;
    if (down) {
        if (repeat) return down_ ? Swallowed : NotOurs;
        if (key_mods_of(sdl_mods) != chord.mods) return NotOurs;   // the same key with other modifiers is the game's
        down_ = true;
        return Fired;
    }
    if (!down_) return NotOurs;   // a release of a press the game had
    down_ = false;
    return Swallowed;
}

KeyChord menu_key_from_config(std::string* warn) {
    std::string key = config_get_str("menu", "key", "");
    const char* from = "[menu] key";
    if (key.empty()) {
        key = config_get_str("gfx", "overlay_key", "F1");
        from = "[gfx] overlay_key";
    }
    KeyChord k;
    if (!parse_key_chord(key, &k) || k.empty()) {
        if (warn) *warn = std::string(from) + " '" + key + "' is not a key: F1 is used";
        k = {SDL_SCANCODE_F1, 0};
    }
    return k;
}

// ---- controller chords -------------------------------------------------------------------------------------------------------------

uint32_t free_pad_buttons(const uint64_t* remap) {
    uint32_t bits = 0;
    for (int i = 0; i < kPadSrcCount; ++i) {
        const PadSrc s = static_cast<PadSrc>(i);
        const bool unmapped_by_default = s == PadSrc::RightPaddle1 || s == PadSrc::LeftPaddle1 || s == PadSrc::RightPaddle2 || s == PadSrc::LeftPaddle2 ||
                                         s == PadSrc::Touchpad || s == PadSrc::TouchpadRight;
        const uint64_t r = remap ? remap[i] : kPadDefault;
        if (r == 0 || (r == kPadDefault && unmapped_by_default)) bits |= pad_src_bit(s);
    }
    return bits;
}

std::string pin_pad_problem(uint32_t chord, uint32_t free_bits) {
    if (!chord) return std::string();
    int n = 0;
    for (uint32_t b = chord; b; b &= b - 1) ++n;
    if (n < 2) return "use two buttons or more, so it never fires by accident";
    if (!(chord & free_bits))
        return "include a rear grip or a trackpad click (a button that presses nothing in the game), so the game never sees the chord";
    return std::string();
}

bool PadToggle::step(uint32_t held, uint32_t* mask) {
    const uint32_t f = chord & free, others = chord & ~free;
    const uint32_t pressed = held & ~prev_;
    prev_ = held;
    if (!chord || !f) return false;
    if (!(held & f)) {
        armed_ = fired_ = false;   // every free button of it is up
        return false;
    }
    // armed by a free button going down while none of the chord's game buttons is held
    if (!armed_ && (pressed & f) && !(held & others)) armed_ = true;
    if (!armed_) return false;
    if (mask) *mask |= held & others;   // held back from the game while the free button is down: the game never sees them
    const bool complete = (held & chord) == chord;
    if (complete && !fired_) {
        fired_ = true;
        return true;
    }
    if (!complete) fired_ = false;
    return false;
}

// ---- conflicts ------------------------------------------------------------------------------------------------------------------------

std::vector<Conflict> toggle_conflicts(const std::vector<ToggleSpec>& t, const std::vector<int32_t>& game_keys, uint32_t quit_pad) {
    std::vector<Conflict> out;
    const int n = static_cast<int>(t.size());
    for (int i = 0; i < n; ++i) {
        if (!t[i].key.empty()) {
            for (int j = i + 1; j < n; ++j)
                if (t[j].key == t[i].key)
                    out.push_back({t[i].name + " and " + t[j].name + " use the same key (" + key_chord_text(t[i].key) + "): " + t[j].name + " is off", i, j});
            if (t[i].key.mods == 0 && std::find(game_keys.begin(), game_keys.end(), t[i].key.sc) != game_keys.end())
                out.push_back({t[i].name + ": the game's controls also use " + key_chord_text(t[i].key) + " (the toggle takes it)", i, -1});
        }
        if (t[i].pad) {
            if (quit_pad && (t[i].pad & quit_pad) == quit_pad)
                out.push_back({t[i].name + ": its controller chord holds the quit chord (" + pad_combo_text(quit_pad) + ")", i, -1});
            for (int j = 0; j < n; ++j) {
                if (j == i || !t[j].pad) continue;
                if (t[j].pad == t[i].pad && j > i)
                    out.push_back({t[i].name + " and " + t[j].name + " use the same controller chord (" + pad_combo_text(t[i].pad) + "): " + t[j].name + " is off", i, j});
                else if (t[j].pad != t[i].pad && (t[i].pad & t[j].pad) == t[j].pad)
                    out.push_back({t[i].name + "'s controller chord also holds " + t[j].name + "'s (" + pad_combo_text(t[j].pad) + ")", i, j});
            }
        }
    }
    return out;
}

}  // namespace nxrt::play
