// SPDX-License-Identifier: GPL-3.0-or-later
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

}  // namespace nxrt::test
