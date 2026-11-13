// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The controller-button icons of the shared Nintendo-extension font (pl:u font 5, src/services/pl_ext_font.cpp) follow the controller the
// player holds. Play mode's prompt style (src/gfx/play/prompts.cpp: [prompts] style, "auto" = the device used last) says what each Switch
// button shows; the font draws that (an Xbox B for the Switch A on an Xbox pad, as the pad map presses it; a cross on a PlayStation pad;
// the bound key on a keyboard), so every game's text prompts match, with no game code.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::services {

// The Switch buttons the font has icons for.
enum class FaceRole : uint8_t { A, B, X, Y, L, R, ZL, ZR, SL, SR, Plus, Minus, Up, Down, Left, Right, LStick, RStick, kCount };
constexpr size_t kFaceRoleCount = static_cast<size_t>(FaceRole::kCount);

struct ButtonFace {
    // Per role: a prompt-pack name of the physical input ("xbox/b", "ps/circle", "steam/lb", "deck/menu", "xbox/dpad_up", "kbm/space",
    // "kbm/left ctrl", "kbm/mouse1"), or "" for the Switch button itself.
    std::array<std::string, kFaceRoleCount> name;
    std::string style = "switch";   // for the log
    bool operator==(const ButtonFace& o) const { return name == o.name; }
    bool operator!=(const ButtonFace& o) const { return !(*this == o); }
};

// The icons to show from now on. Before the game has the shared font they are what it gets; after, font 5 is rewritten in place (its
// size and table layout never change): glyphs the game draws from then on use them, glyphs it already has in its own glyph cache keep
// their old look until it draws them again (a full change: the next start).
void shared_font_set_face(const ButtonFace& face);
ButtonFace shared_font_face();

// Asked once, when the shared font memory is first built, for the icons to start with (play mode registers it; none: the Switch's).
void shared_font_set_face_provider(ButtonFace (*fn)());

// The same icons as pictures, for button images a game draws from its own textures (src/gfx/play/prompt_images.cpp). The coverage
// (0..255) of the icon `name` stands for (a ButtonFace name; "" = the Switch's own for `role`), fitted into w x h with its aspect kept and
// centred. Which part:
//   Whole  the icon as the font draws it (a ring with the letter or mark, a plate with its text)
//   Mark   only what is inside the ring or plate (the letter, the PlayStation shape), at the size it has in the whole icon
//   Disc   the ring or plate filled, with the mark cut out of it (a button drawn as a solid disc with its letter knocked out)
// bottom_up: row 0 is the bottom row of the picture (textures stored upside down). False when w or h is 0 or above 4096.
enum class IconPart : uint8_t { Whole, Mark, Disc };
bool shared_font_icon_coverage(FaceRole role, const std::string& name, IconPart part, uint32_t w, uint32_t h, bool bottom_up,
                               std::vector<uint8_t>& coverage);

}  // namespace nxrt::services
