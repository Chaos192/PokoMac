// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// A CPU renderer of Dear ImGui draw data, for screenshot tests without a GPU: textured, vertex-coloured triangles with ImGui's
// clip rectangles, bilinear texture sampling and straight alpha blending (src alpha, one minus src alpha), as imgui_impl_vulkan draws them.
// It is a renderer backend with ImGuiBackendFlags_RendererHasTextures: it samples ImTextureData's own pixels (the atlas keeps them).
#pragma once
#include <imgui.h>

#include <cstdint>
#include <vector>

namespace nxrt::test {

// Declares the backend in the current context (call once after CreateContext).
void soft_init();
// Honours the frame's texture requests (create / update / destroy) and draws the draw data over `rgba` (w x h, RGBA8, row major).
void soft_render(ImDrawData* dd, std::vector<uint8_t>& rgba, int w, int h);
// The theme guard of a drawn frame (tools/dev/overlay_theme_guard.py is its static half): the style in force is the overlay's theme
// (theme::style_check: every colour set, the default font Inter) and no vertex of the draw data has one of ImGui's stock colours
// (theme::is_stock_colour). Prints a FAILED line naming `what` and returns false otherwise.
bool theme_guard(ImDrawData* dd, const char* what);
// The backgrounds every overlay screen is checked on: 0 the test's own scene, 1 light (snow and a pale sky), 2 dark (night), 3 busy
// (sharp, saturated stripes, checks and noise: the hardest to read text over).
const char* background_name(int kind);
void background(int kind, std::vector<uint8_t>& px, int w, int h);

}  // namespace nxrt::test
