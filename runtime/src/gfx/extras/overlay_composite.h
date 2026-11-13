// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR overlay composite (overlay.cpp draws ImGui here when the swapchain is HDR).
//   The ImGui overlay is rendered into an RGBA8 UNORM target (cleared to 0, so the texels are premultiplied), then one full-screen
//   pass reads that target, maps UI white (sRGB 1.0) to paper white, encodes the colour as HDR10 (ST 2084) or scRGB and blends it
//   over the swapchain image. The shaders and push-constant layout are in extras_shaders.inc (kOverlayCompositeFrag).
#pragma once
#include <volk.h>

#include <cstdint>
#include <map>

#include "nxrt/extras.h"
#include "nxrt/gfx.h"

namespace nxrt::extras {

class OverlayComposite {
public:
    OverlayComposite() = default;
    ~OverlayComposite() { shutdown(); }
    OverlayComposite(const OverlayComposite&) = delete;
    OverlayComposite& operator=(const OverlayComposite&) = delete;

    bool init(gfx::Device& dev);
    void shutdown();
    bool initialised() const { return dev_ != nullptr; }

    // Creates (or, when the size changed, recreates) the RGBA8 target. Waits for the device when it has to free the old one.
    bool ensure_target(VkExtent2D extent);
    VkImage target_image() const { return target_.image; }
    VkImageView target_view() const { return target_.view; }

    // Records the transition of the target to COLOR_ATTACHMENT_OPTIMAL. The caller then renders the ImGui overlay into target_view().
    void begin_overlay(VkCommandBuffer cb);
    // Records the transition of the target to SHADER_READ_ONLY_OPTIMAL and one full-screen triangle into dst_view (the swapchain image,
    // in COLOR_ATTACHMENT_OPTIMAL), blended over it. kind must be Hdr10 or ScRgb; paper white and peak are in nits.
    void composite(VkCommandBuffer cb, VkImageView dst_view, VkFormat dst_format, VkExtent2D dst_extent, OutputKind kind, float paper_white_nits,
                   float peak_nits);

    static constexpr uint32_t kSetRing = 8;

private:
    VkPipeline pipeline(VkFormat fmt);

    gfx::Device* dev_ = nullptr;
    VkSampler linear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kSetRing] = {};
    uint32_t next_set_ = 0;
    std::map<VkFormat, VkPipeline> pipes_;

    gfx::Image target_;
    VkExtent2D extent_ = {};
    bool target_read_ = false;            // the target is in SHADER_READ_ONLY_OPTIMAL (after composite), else UNDEFINED / COLOR_ATTACHMENT
};

}  // namespace nxrt::extras
