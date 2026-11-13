// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The backdrop blur's arithmetic on the CPU (extras/backdrop_blur.cpp and its shader, scripts/gen_blur_shaders.py): level 0 is the frame at
// half size (the mean of each 2 x 2 block, as the 2:1 linear blit gives), then the dual Kawase passes down and back up with bilinear,
// clamp-to-edge taps, each pass stored as RGBA8. The screenshot test draws the glass panel with it, and the GPU test compares the GPU's
// level 0 with it.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::test {

// `rgba`: the frame (w x h RGBA8); `rect`: the panel's rectangle (x, y, w, h; null: the whole frame). Returns level 0 (*ow x *oh RGBA8,
// half the frame's size, rounded up), zero outside the region the blur works on.
std::vector<uint8_t> blur_reference(const std::vector<uint8_t>& rgba, int w, int h, float strength, const int rect[4], int* ow, int* oh);

}  // namespace nxrt::test
