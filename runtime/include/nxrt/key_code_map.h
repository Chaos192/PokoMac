// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// nn::kpr::KeyCodeMap, the table settings (set 7 GetKeyCodeMap, 9 GetKeyCodeMap2, 12 GetKeyCodeMapByPort) gives a game: how the SDK's
// nn::kpr::GetKeyCode turns a keyboard key (HID usage id) and the modifiers into a UTF-16 character.
//
// The layout, read from the SDK's own reader (nn::kpr::detail::GetKeyCode and IsKeyCodeMapModeSupported; Super Mario Galaxy 2's sdk
// 0x4fb190 / 0x4fb160):
//   +0x00 u32  the modes the map has: bit n = KeyCodeMapMode n is supported (0 the default Latin layer; Hiragana, Katakana, Cyrillic,
//              Hangul and the two Zhuyin modes above it), bit 24 = an AltGr layer exists (Control+LeftAlt or RightAlt select it),
//              bit 25 = that layer has a shifted column too
//   +0x04 u32  u16 words per entry (the flags word, then the characters)
//   +0x08 u32  entries: usage ids 4 .. 4 + count - 1
//   +0x0C u32  the word index of the AltGr layer in an entry
//   +0x40      the entries: u16 flags, then normal, shifted, (AltGr, AltGr shifted, other modes' pairs)
// flags >> 12 is the entry's kind (1: an ordinary key, 2: a keypad key NumLock turns into a digit), bit 0 says CapsLock (kind 1) or
// NumLock (kind 2) applies to the default layer, bit 1 says CapsLock applies to the AltGr layer. A keypad key's digit is its shifted
// character: NumLock on and no Shift gives it, as on a PC.
//
// The map served is built here from the host's own keyboard layout (the play layer reads it from SDL: what each physical key prints, with
// and without Shift), so a game's text entry types what the player's keyboard says; with no host layout (headless runs, tests) it is a
// US English layout. Keys that print nothing (Enter, Tab, Backspace, the function and arrow keys) have no entry, as on the console.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nxrt::hid {

constexpr size_t kKeyCodeMapBytes = 0x1000;
constexpr int kKeyCodeMapFirstUsage = 4;        // KeyboardKey::A
constexpr int kKeyCodeMapEntries = 100;         // usage ids 4 .. 0x67 (KeyboardKey::NumPadEquals), as the console's Latin maps

// What one physical key prints: the character alone and with Shift (UTF-16, 0 = nothing). Indexed by HID usage id (0..255).
struct KeyChars {
    char16_t normal = 0, shift = 0;
};

// The host's layout for usage ids 0..255 (256 entries); nullptr returns to the US English layout. Any thread; copied.
void set_host_key_chars(const KeyChars* chars);
// The US English layout (256 entries), the default.
const KeyChars* us_key_chars();
// Builds the map from a layout (256 entries). Keypad keys (0x54..0x63, 0x67) are the same on every layout and are not taken from it.
void build_key_code_map(const KeyChars* chars, uint8_t out[kKeyCodeMapBytes]);
// The map the settings service serves now (the host layout's, or US English).
void current_key_code_map(uint8_t out[kKeyCodeMapBytes]);
// What nn::kpr::GetKeyCode reads from `map` for a usage id and nn::hid::KeyboardModifier bits in the default mode (0 = nothing): the
// SDK's rule, here for the tests.
char16_t key_code_of(const uint8_t map[kKeyCodeMapBytes], int usage, uint32_t modifiers);

}  // namespace nxrt::hid
