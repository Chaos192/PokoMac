// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Present pass of the NVN-on-Vulkan back end: draws a game's window texture (a VkImageView in SHADER_READ_ONLY_OPTIMAL) onto a
// destination image (the swapchain image or the headless frame image) with dynamic rendering. A full-screen triangle samples
// the source region src_crop, stretched over the whole destination, or letterboxed when keep_aspect is set (black bars).
//
// Filtering: linear when the picture size in destination pixels differs from the crop size, nearest when it is equal (an exact
// 1:1 copy). The source view must be a UNORM view: the texel bytes are shown as written, with no colour space conversion. Alpha
// of the output is forced to 1.
//
// Descriptors: a ring of 16 combined-image-sampler sets, rewritten before each record. The ring must be longer than the number of
// frames in flight (the renderer allows up to 3), so a set is never rewritten while a command buffer still reads it.
// Crops: src_crop = {x, y, w, h} in source texels; w == 0 or h == 0 means the whole source. Crops are clipped to the source.
#pragma once
#include <cstdint>

#include "nxrt/gfx.h"

namespace nxrt::nvn::be {

class PresentPass {
public:
    PresentPass() = default;
    ~PresentPass() { shutdown(); }
    PresentPass(const PresentPass&) = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    // color_format: the format of the destination images (the renderer's format()). Returns false on a Vulkan error (logged).
    bool init(gfx::Device& dev, VkFormat color_format);
    // Destroys the pipeline, samplers and descriptor pool. Call before the device is destroyed (the destructor calls it too).
    void shutdown();
    bool initialised() const { return dev_ != nullptr; }
    // Presents the picture upside down (the lower-left window origin mode of NVN). Applies to the next record() calls.
    void set_flip_y(bool flip) { flip_y_ = flip; }

    // Records into cb, outside any render pass: the destination image must be in COLOR_ATTACHMENT_OPTIMAL. Begins dynamic rendering
    // on dst_view (clear to black when keep_aspect, else don't-care: the triangle covers the target), draws, ends rendering.
    // src_view must be a view of an image in SHADER_READ_ONLY_OPTIMAL. src_crop may be null (whole source).
    void record(VkCommandBuffer cb, VkImageView dst_view, VkExtent2D dst_extent, VkImageView src_view, VkExtent2D src_extent,
                const int32_t src_crop[4], bool keep_aspect);

    static constexpr uint32_t kSetRing = 16;

private:
    gfx::Device* dev_ = nullptr;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kSetRing] = {};
    uint32_t next_set_ = 0;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool flip_y_ = false;
};

}  // namespace nxrt::nvn::be
