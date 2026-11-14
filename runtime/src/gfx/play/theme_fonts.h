// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The overlay's CJK fallback (cheats_theme.cpp load_fonts): the system's Chinese, Japanese and Korean faces, mapped into memory read-only
// (pages come in only for the glyphs a frame draws), merged after Inter and the symbols. A packaged fonts/ folder may bring its own
// (Noto Sans CJK); [overlay] cjk_font names a file of the player's; [overlay] cjk = false turns the fallback off.
#pragma once
#include <cstddef>
#include <vector>

namespace nxrt::cheats::ui::theme::fonts {

struct Mapped {
    const unsigned char* data = nullptr;   // the file, mapped for the life of the process
    size_t size = 0;
    int index = 0;                         // the face in a collection (.ttc)
    float em = 1.f;                        // em_per_height: ImFontConfig::ExtraSizeScale = Inter's em / this gives Inter's em size
    char name[28] = {};                    // the file name, for ImFontConfig::Name
};
// Found and mapped once per process (the first call), in fallback order.
const std::vector<Mapped>& system_cjk();
// "msyh.ttc, YuGothR.ttc, malgun.ttf" or "none".
const char* system_cjk_names();
// The em of a face per pixel of ImGui's size (unitsPerEm / (hhea ascender - descender)); 0 when the data is not a font.
float em_per_height(const unsigned char* data, size_t size, int index);

}  // namespace nxrt::cheats::ui::theme::fonts
