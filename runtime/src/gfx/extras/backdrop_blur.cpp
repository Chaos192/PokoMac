// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's glass panel: the backdrop blur (backdrop_blur.h).
#include "extras/backdrop_blur.h"

#include <algorithm>
#include <cmath>

#include "nxrt/log.h"
#include "vk_gauges.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "glass";

#include "extras/blur_shaders.inc"

struct BlurPush {
    float src[4];   // xy texel size, z tap offset
    float dst[4];   // xy size, zw region origin
    float p[4];     // x mode (0 down, 1 up), zw region size
    float lim[4];   // the source region in source UV (its edge texels' centres)
};

void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                 VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

}  // namespace

int BackdropBlur::levels(float strength) { return std::clamp(static_cast<int>(std::lround(std::clamp(strength, 0.f, 1.f) * kMaxLevels)), 0, kMaxLevels); }
float BackdropBlur::offset(float strength) { return 1.f + 1.5f * std::clamp(strength, 0.f, 1.f); }
int BackdropBlur::margin(float strength) { return 8 << levels(strength); }

bool BackdropBlur::region(const int rect[4], VkExtent2D frame, float strength, int out[4]) {
    constexpr int kAlign = 2 << kMaxLevels;    // 32: the deepest level's texel
    const int m = margin(strength);
    const int w = static_cast<int>(frame.width), h = static_cast<int>(frame.height);
    out[0] = std::clamp(((rect[0] - m) / kAlign) * kAlign, 0, w);
    out[1] = std::clamp(((rect[1] - m) / kAlign) * kAlign, 0, h);
    out[2] = std::clamp(((rect[0] + rect[2] + m + kAlign - 1) / kAlign) * kAlign, 0, w);
    out[3] = std::clamp(((rect[1] + rect[3] + m + kAlign - 1) / kAlign) * kAlign, 0, h);
    return out[2] - out[0] >= 2 && out[3] - out[1] >= 2;
}

bool BackdropBlur::init(gfx::Device& dev) {
    if (dev_) return true;
    dev_ = &dev;
    VkDevice d = dev.device();
    auto fail = [&](const char* what) {
        NXLOG_E(kTag, "%s failed: the panel is drawn solid", what);
        shutdown();
        return false;
    };
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d, &sci, nullptr, &linear_) != VK_SUCCESS) return fail("blur sampler");
    vk_gauge_samplers().add(1);
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_) != VK_SUCCESS) return fail("blur set layout");
    vk_gauge_descriptor_set_layouts().add(1);
    const VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) return fail("blur pipeline layout");
    vk_gauge_pipeline_layouts().add(1);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kBackdropBlurComp;
    mi.pCode = kBackdropBlurComp;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return fail("blur shader module");
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = m;
    cpi.stage.pName = "main";
    cpi.layout = layout_;
    const VkResult pr = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe_);
    vkDestroyShaderModule(d, m, nullptr);
    if (pr != VK_SUCCESS) {
        pipe_ = VK_NULL_HANDLE;
        return fail("blur pipeline");
    }
    vk_gauge_pipelines().add(1);
    const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kMaxLevels}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kMaxLevels}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2 * kMaxLevels;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return fail("blur descriptor pool");
    vk_gauge_descriptor_pools().add(1);
    VkDescriptorSetLayout ls[2 * kMaxLevels];
    for (auto& x : ls) x = set_layout_;
    VkDescriptorSet sets[2 * kMaxLevels] = {};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 2 * kMaxLevels;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(d, &ai, sets) != VK_SUCCESS) return fail("blur descriptor sets");
    vk_gauge_descriptor_sets().add(2 * kMaxLevels);
    for (int i = 0; i < kMaxLevels; ++i) {
        down_[i] = sets[i];
        up_[i] = sets[kMaxLevels + i];
    }
    return true;
}

void BackdropBlur::shutdown() {
    if (!dev_) return;
    VkDevice d = dev_->device();
    dev_->wait_idle();
    for (auto& img : lv_)
        if (img) dev_->destroy_image(img);
    for (auto& img : lv_) img = gfx::Image();
    if (pipe_) { vkDestroyPipeline(d, pipe_, nullptr); vk_gauge_pipelines().sub(1); }
    if (layout_) { vkDestroyPipelineLayout(d, layout_, nullptr); vk_gauge_pipeline_layouts().sub(1); }
    if (pool_) {
        vkDestroyDescriptorPool(d, pool_, nullptr);
        vk_gauge_descriptor_pools().sub(1);
        if (down_[0]) vk_gauge_descriptor_sets().sub(2 * kMaxLevels);
    }
    if (set_layout_) { vkDestroyDescriptorSetLayout(d, set_layout_, nullptr); vk_gauge_descriptor_set_layouts().sub(1); }
    if (linear_) { vkDestroySampler(d, linear_, nullptr); vk_gauge_samplers().sub(1); }
    pipe_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    linear_ = VK_NULL_HANDLE;
    std::fill(std::begin(down_), std::end(down_), VK_NULL_HANDLE);
    std::fill(std::begin(up_), std::end(up_), VK_NULL_HANDLE);
    frame_ = {};
    dev_ = nullptr;
}

bool BackdropBlur::ensure(VkExtent2D extent) {
    if (!dev_ || !pipe_ || !extent.width || !extent.height) return false;
    if (lv_[0] && frame_.width == extent.width && frame_.height == extent.height) return true;
    if (lv_[0]) dev_->wait_idle();
    for (auto& img : lv_)
        if (img) dev_->destroy_image(img);
    for (auto& img : lv_) img = gfx::Image();
    for (int i = 0; i <= kMaxLevels; ++i) {
        const uint32_t div = 2u << i;
        ext_[i] = {std::max(1u, (extent.width + div - 1) / div), std::max(1u, (extent.height + div - 1) / div)};
        gfx::ImageDesc id;
        id.format = VK_FORMAT_R8G8B8A8_UNORM;
        id.width = ext_[i].width;
        id.height = ext_[i].height;
        id.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        id.name = "glass blur level";
        std::string why;
        if (dev_->create_image(id, &lv_[i], &why) != VK_SUCCESS) {
            NXLOG_E(kTag, "the glass blur's level %d (%ux%u): %s: the panel is drawn solid", i, ext_[i].width, ext_[i].height, why.c_str());
            for (auto& img : lv_)
                if (img) dev_->destroy_image(img);
            for (auto& img : lv_) img = gfx::Image();
            frame_ = {};
            return false;
        }
    }
    // the passes' descriptors: down i reads level i and writes i + 1; up i reads i + 1 and writes i
    VkDescriptorImageInfo infos[4 * kMaxLevels];
    VkWriteDescriptorSet w[4 * kMaxLevels];
    int n = 0;
    for (int i = 0; i < kMaxLevels; ++i)
        for (int up = 0; up < 2; ++up) {
            const VkDescriptorSet set = up ? up_[i] : down_[i];
            const gfx::Image& src = up ? lv_[i + 1] : lv_[i];
            const gfx::Image& dst = up ? lv_[i] : lv_[i + 1];
            infos[n] = {linear_, src.view, VK_IMAGE_LAYOUT_GENERAL};
            w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[n].dstSet = set;
            w[n].dstBinding = 0;
            w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[n].pImageInfo = &infos[n];
            ++n;
            infos[n] = {VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};
            w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[n].dstSet = set;
            w[n].dstBinding = 1;
            w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[n].pImageInfo = &infos[n];
            ++n;
        }
    vkUpdateDescriptorSets(dev_->device(), static_cast<uint32_t>(n), w, 0, nullptr);
    frame_ = extent;
    fresh_ = true;
    return true;
}

void BackdropBlur::record(VkCommandBuffer cb, VkImage src, VkExtent2D extent, const int rect[4], float strength) {
    if (!ready() || extent.width != frame_.width || extent.height != frame_.height) return;
    const int n = levels(strength);
    const float off = offset(strength);
    int rg[4];
    if (!region(rect, extent, strength, rg)) return;
    const int x0 = rg[0], y0 = rg[1], x1 = rg[2], y1 = rg[3];

    // the levels: from their first use (UNDEFINED) or from the previous frame's reads (ImGui's fragment shader, these passes) to this
    // frame's blit and passes
    if (fresh_) {
        for (int i = 0; i <= kMaxLevels; ++i)
            image_barrier(cb, lv_[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                              VK_ACCESS_2_SHADER_WRITE_BIT);
        fresh_ = false;
    } else {
        mem_barrier(cb, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
                        VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    // the frame's rectangle at half size into level 0 (a 2:1 linear blit: each texel the mean of its 2 x 2 pixels)
    image_barrier(cb, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {x0, y0, 0};
    blit.srcOffsets[1] = {x1, y1, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x0 / 2, y0 / 2, 0};
    blit.dstOffsets[1] = {x1 / 2, y1 / 2, 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lv_[0].image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    image_barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (n == 0) {
        mem_barrier(cb, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return;
    }
    mem_barrier(cb, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    // the region in a level: the region's texels there (rounded out)
    auto level_region = [&](int level, int out[4]) {
        const int div = 2 << level;
        out[0] = x0 / div;
        out[1] = y0 / div;
        out[2] = std::min<int>((x1 + div - 1) / div, static_cast<int>(ext_[level].width));
        out[3] = std::min<int>((y1 + div - 1) / div, static_cast<int>(ext_[level].height));
    };
    auto pass = [&](VkDescriptorSet set, int src_level, int dst_level, bool up) {
        int dr[4], sr[4];
        level_region(dst_level, dr);
        level_region(src_level, sr);
        const int rx0 = dr[0], ry0 = dr[1], rx1 = dr[2], ry1 = dr[3];
        BlurPush pc = {};
        pc.src[0] = 1.f / static_cast<float>(ext_[src_level].width);
        pc.src[1] = 1.f / static_cast<float>(ext_[src_level].height);
        pc.src[2] = off;
        pc.dst[0] = static_cast<float>(ext_[dst_level].width);
        pc.dst[1] = static_cast<float>(ext_[dst_level].height);
        pc.dst[2] = static_cast<float>(rx0);
        pc.dst[3] = static_cast<float>(ry0);
        pc.p[0] = up ? 1.f : 0.f;
        pc.p[2] = static_cast<float>(rx1 - rx0);
        pc.p[3] = static_cast<float>(ry1 - ry0);
        const float sw = static_cast<float>(ext_[src_level].width), sh = static_cast<float>(ext_[src_level].height);
        pc.lim[0] = (static_cast<float>(sr[0]) + 0.5f) / sw;
        pc.lim[1] = (static_cast<float>(sr[1]) + 0.5f) / sh;
        pc.lim[2] = (static_cast<float>(sr[2]) - 0.5f) / sw;
        pc.lim[3] = (static_cast<float>(sr[3]) - 0.5f) / sh;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, static_cast<uint32_t>((rx1 - rx0 + 7) / 8), static_cast<uint32_t>((ry1 - ry0 + 7) / 8), 1);
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    for (int i = 0; i < n; ++i) pass(down_[i], i, i + 1, false);
    for (int i = n - 1; i >= 0; --i) pass(up_[i], i + 1, i, true);
    mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

}  // namespace nxrt::extras
