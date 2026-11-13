// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Masked colour clear of the NVN-on-Vulkan back end (src/gfx/nvn/nvn_mask_clear.cpp). vkCmdClearAttachments ignores the colour
// write mask, so a partial NVN ClearColor cannot use it. This pass draws a full-screen triangle with a constant colour, a pipeline
// whose colour write mask is the NVN mask, a scissor = the clear rectangle and no blending, depth or stencil, so only the channels
// in the mask change.
//
// Pipelines are created on demand per (colour format, attachment count, attachment index, mask, kind, sample count) and cached.
// The fragment shader writes the colour to the one output location `attachment` (vec4 for kind 0, ivec4 for kind 1, uvec4 for kind 2).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <tuple>

#include "nxrt/gfx.h"

namespace nxrt::nvn::be {

class MaskClear {
public:
    MaskClear() = default;
    ~MaskClear() { shutdown(); }
    MaskClear(const MaskClear&) = delete;
    MaskClear& operator=(const MaskClear&) = delete;

    // Shader modules, the pipeline layout (a 32-byte fragment push constant range, no descriptors). Returns false on a Vulkan error.
    bool init(gfx::Device& dev);
    // Destroys the cached pipelines, the layout and the shader modules. Call before the device is destroyed (the destructor calls it too).
    void shutdown();
    bool initialised() const { return dev_ != nullptr; }

    // Inside a dynamic-rendering pass that is open on cb: clears the channels in `mask` (bit 0 R, 1 G, 2 B, 3 A) of colour attachment
    // `attachment` (0..7 and below total_attachments) to `value` inside `rect`; the other channels and the other attachments are not
    // written. value holds 4 x 32-bit words: floats when kind == 0, int32 when kind == 1, uint32 when kind == 2. color_format is the
    // VkFormat of that attachment, total_attachments the number of colour attachments of the pass. Every colour attachment of the
    // pass must have color_format: the pipeline lists it for all of them (the device does not enable
    // dynamicRenderingUnusedAttachments, so an UNDEFINED entry is a validation error). A mask of 0 (or a rect with no area) draws
    // nothing. Returns false if the pipeline cannot be created or the arguments are out of range.
    bool record(VkCommandBuffer cb, VkFormat color_format, uint32_t total_attachments, uint32_t attachment, uint32_t mask, uint32_t kind,
                const uint32_t value[4], VkRect2D rect, uint32_t sample_count);

    // Inside a dynamic-rendering pass open on cb: sets the stencil bits in `stencil_mask` to the same bits of `stencil_value` and, when
    // write_depth, depth to `depth`, inside `rect`. vkCmdClearAttachments takes no stencil write mask, so a partial NVN stencil clear is
    // a full-screen triangle with stencil op REPLACE (write mask, reference), depth test ALWAYS (constant viewport depth) and every
    // colour write mask 0. color_formats lists the formats of all colour attachments of the pass (a pipeline names them all). The depth
    // and stencil format of the pass is depth_format; has_stencil says whether it has a stencil aspect. False: nothing was drawn.
    bool record_depth_stencil(VkCommandBuffer cb, const VkFormat* color_formats, uint32_t color_count, VkFormat depth_format, bool has_stencil,
                              uint32_t sample_count, bool write_depth, float depth, uint32_t stencil_mask, uint32_t stencil_value, VkRect2D rect);

    static constexpr uint32_t kMaxAttachments = 8;
    static constexpr uint32_t kPushBytes = 32;   // size of the push constant range; the shaders read the first 16

private:
    using Key = std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;   // format, count, index, mask, kind, samples
    VkPipeline pipeline_for(const Key& key);
    VkPipeline ds_pipeline_for(const VkFormat* color_formats, uint32_t color_count, VkFormat depth_format, bool has_stencil, uint32_t samples,
                               bool write_depth);

    gfx::Device* dev_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vert_ = VK_NULL_HANDLE;
    VkShaderModule frag_[3][kMaxAttachments] = {};   // [kind][location]
    std::map<Key, VkPipeline> pipelines_;
    std::map<std::string, VkPipeline> ds_pipelines_;
};

}  // namespace nxrt::nvn::be
