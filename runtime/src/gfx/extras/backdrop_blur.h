// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's glass panel: a blurred copy of the game frame under the panel (overlay.cpp records it each frame the panel is glass,
// and only then: nothing runs while the overlay is closed or solid).
//   The panel's rectangle (with a margin the blur reaches into) is blitted from the frame image at half size into level 0, then the dual
//   Kawase blur (kBackdropBlurComp, blur_shaders.inc; scripts/gen_blur_shaders.py) halves it `levels` times and doubles it back into
//   level 0, which ImGui samples behind the panel's tint. Only the rectangle's texels are touched; the images have the frame's size / 2,
//   / 4 ... so a panel that moves needs no new images. The CPU reference of the same arithmetic is tests/cheats_ui/blur_ref.cpp.
#pragma once
#include <volk.h>

#include <cstdint>

#include "nxrt/gfx.h"

namespace nxrt::extras {

class BackdropBlur {
public:
    static constexpr int kMaxLevels = 4;

    BackdropBlur() = default;
    ~BackdropBlur() { shutdown(); }
    BackdropBlur(const BackdropBlur&) = delete;
    BackdropBlur& operator=(const BackdropBlur&) = delete;

    bool init(gfx::Device& dev);
    void shutdown();
    bool initialised() const { return dev_ != nullptr; }

    // The images for a frame of `extent` (level 0 is half its size). Recreates them (after waiting for the device) when the size changed.
    bool ensure(VkExtent2D extent);
    // Level 0, the blurred picture: sampled in VK_IMAGE_LAYOUT_GENERAL with sampler(), at UV = frame pixel / frame size.
    VkImageView view() const { return lv_[0].view; }
    VkImage image(int level) const { return lv_[level].image; }
    VkExtent2D level_extent(int level) const { return ext_[level]; }
    VkSampler sampler() const { return linear_; }
    bool ready() const { return dev_ && pipe_ && lv_[0]; }

    // Records the blur of `rect` (x, y, w, h in pixels of the frame image `src`, which is in COLOR_ATTACHMENT_OPTIMAL and is left so; it
    // needs TRANSFER_SRC usage). `strength` 0 .. 1: 0 is a plain half-size copy, 1 the widest blur. After it, level 0 is ready for a
    // fragment shader read.
    void record(VkCommandBuffer cb, VkImage src, VkExtent2D extent, const int rect[4], float strength);

    // How a strength maps to passes: the number of levels down (and back up) and the tap offset in texels.
    static int levels(float strength);
    static float offset(float strength);
    // The margin, in frame pixels, around the rectangle that the blur takes in (the panel's edges blur the picture around it too).
    static int margin(float strength);
    // The frame region the blur works on: the rectangle with the margin, out to multiples of 32 pixels (so every level's region starts on
    // a texel of it), inside the frame: x0, y0, x1, y1. Each level i covers [x0 / 2^(i+1), ceil(x1 / 2^(i+1))) and every tap is clamped
    // into the region of its level: the region blurs as an image of its own. False when it is empty.
    static bool region(const int rect[4], VkExtent2D frame, float strength, int out[4]);

private:
    gfx::Device* dev_ = nullptr;
    VkSampler linear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet down_[kMaxLevels] = {}, up_[kMaxLevels] = {};
    gfx::Image lv_[kMaxLevels + 1];
    VkExtent2D ext_[kMaxLevels + 1] = {};
    VkExtent2D frame_ = {};
    bool fresh_ = true;                   // the images were just made (UNDEFINED): the first record moves them to GENERAL
};

}  // namespace nxrt::extras
