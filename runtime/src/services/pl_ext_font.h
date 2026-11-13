// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// A stand-in for the system's Nintendo-extension shared font (SharedFontType 5): controller-button icons at the private-use code points
// games put in their text, drawn here from simple vector shapes (no Nintendo data). pl.cpp serves it in slot 5 unless [pl] font_dir holds
// the player's own FontNintendoExtended.bfttf.
#pragma once
#include <cstdint>
#include <vector>

#include "nxrt/shared_font.h"

namespace nxrt::services {

// A complete TrueType font (glyf outlines, cmap format 4) mapping U+E000..U+E1FF: the button, D-pad and stick icons at the code points
// below, drawn for `face` (the Switch's own buttons by default; an Xbox, PlayStation or Steam pad's, or key caps), and a plain ring for
// every other code point of that block (a game may take any of them without a null check).
//   U+E0A0..E0A3 / U+E0E0..E0E3  A B X Y          U+E0A4..E0A9 / U+E0E4..E0E9  L R ZL ZR SL SR
//   U+E0B5 / U+E0EF  plus   U+E0B6 / U+E0F0  minus      U+E0EA  D-pad      U+E0AB..E0AE / U+E0EB..E0EE  D-pad right, down, up, left
//   U+E090..E097  stick arrows (right, left, up, down, right-up, right-down, left-down, left-up)
//   U+E100 stick, U+E101 / U+E0C1 left stick, U+E102 / U+E0C2 right stick
// (Super Mario Bros. Wonder's GameMsg/Key.msbt uses U+E0E0..E0F0, U+E0AB..E0AE, U+E090..E097 and U+E100/E101; nxnative
// docs/reference/services.md "Nintendo-extension glyphs for button icons".)
// Every face gives a font of the same size and table layout, so one can be written over another in place.
std::vector<uint8_t> make_extension_font(const ButtonFace& face = ButtonFace());

}  // namespace nxrt::services
