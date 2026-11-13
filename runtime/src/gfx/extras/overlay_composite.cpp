// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR overlay composite (overlay_composite.h).
#include "extras/overlay_composite.h"

#include <algorithm>

#include "nxrt/log.h"
#include "vk_gauges.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "ovlcomp";

#include "extras/extras_shaders.inc"

struct CompositePush {
    float p[4];     // x = mode, y = paper white nits, z = peak nits
};

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

VkShaderModule make_module(VkDevice d, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_gauge_shader_modules().add(1);
    return m;
}

}  // namespace

bool OverlayComposite::init(gfx::Device& dev) {
    if (dev_) return true;
    dev_ = &dev;
    VkDevice d = dev.device();
    auto fail = [&](const char* what) {
        NXLOG_E(kTag, "%s failed", what);
        shutdown();
        return false;
    };

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d, &sci, nullptr, &linear_) != VK_SUCCESS) return fail("overlay sampler");
    vk_gauge_samplers().add(1);

    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_) != VK_SUCCESS) return fail("overlay set layout");
    vk_gauge_descriptor_set_layouts().add(1);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CompositePush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) return fail("overlay pipeline layout");
    vk_gauge_pipeline_layouts().add(1);

    const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetRing};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kSetRing;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &pool_) != VK_SUCCESS) return fail("overlay descriptor pool");
    vk_gauge_descriptor_pools().add(1);
    VkDescriptorSetLayout ls[kSetRing];
    for (auto& x : ls) x = set_layout_;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSetRing;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(d, &ai, sets_) != VK_SUCCESS) return fail("overlay descriptor sets");
    vk_gauge_descriptor_sets().add(kSetRing);   // shutdown subtracts them only while sets_[0] is set

    vs_ = make_module(d, kOutVert, sizeof kOutVert);
    fs_ = make_module(d, kOverlayCompositeFrag, sizeof kOverlayCompositeFrag);
    if (!vs_ || !fs_) return fail("overlay shader modules");
    return true;
}

void OverlayComposite::shutdown() {
    if (!dev_) return;
    VkDevice d = dev_->device();
    dev_->wait_idle();
    if (target_) dev_->destroy_image(target_);
    target_ = gfx::Image();
    for (auto& [f, p] : pipes_)
        if (p) { vkDestroyPipeline(d, p, nullptr); vk_gauge_pipelines().sub(1); }
    pipes_.clear();
    if (layout_) { vkDestroyPipelineLayout(d, layout_, nullptr); vk_gauge_pipeline_layouts().sub(1); }
    if (pool_) { vkDestroyDescriptorPool(d, pool_, nullptr); vk_gauge_descriptor_pools().sub(1); }
    if (sets_[0]) vk_gauge_descriptor_sets().sub(kSetRing);
    if (set_layout_) { vkDestroyDescriptorSetLayout(d, set_layout_, nullptr); vk_gauge_descriptor_set_layouts().sub(1); }
    if (vs_) { vkDestroyShaderModule(d, vs_, nullptr); vk_gauge_shader_modules().sub(1); }
    if (fs_) { vkDestroyShaderModule(d, fs_, nullptr); vk_gauge_shader_modules().sub(1); }
    if (linear_) { vkDestroySampler(d, linear_, nullptr); vk_gauge_samplers().sub(1); }
    linear_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    std::fill(std::begin(sets_), std::end(sets_), VK_NULL_HANDLE);
    next_set_ = 0;
    extent_ = {};
    target_read_ = false;
    dev_ = nullptr;
}

bool OverlayComposite::ensure_target(VkExtent2D extent) {
    if (!dev_) return false;
    if (target_ && extent_.width == extent.width && extent_.height == extent.height) return true;
    if (target_) {
        dev_->wait_idle();
        dev_->destroy_image(target_);
        target_ = gfx::Image();
    }
    gfx::ImageDesc id;
    id.format = VK_FORMAT_R8G8B8A8_UNORM;
    id.width = extent.width;
    id.height = extent.height;
    id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    id.name = "overlay composite";
    std::string why;
    if (dev_->create_image(id, &target_, &why) != VK_SUCCESS) {
        NXLOG_E(kTag, "overlay composite target: %s", why.c_str());
        target_ = gfx::Image();
        return false;
    }
    extent_ = extent;
    target_read_ = false;
    return true;
}

VkPipeline OverlayComposite::pipeline(VkFormat fmt) {
    auto it = pipes_.find(fmt);
    if (it != pipes_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // premultiplied-alpha "over" of the encoded colour onto the swapchain: colour SRC_ALPHA / ONE_MINUS_SRC_ALPHA, alpha ONE / ONE_MINUS_SRC_ALPHA
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pri.colorAttachmentCount = 1;
    pri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_->device(), VK_NULL_HANDLE, 1, &gpi, nullptr, &p) != VK_SUCCESS) {
        NXLOG_E(kTag, "overlay composite pipeline for format %d failed", static_cast<int>(fmt));
        p = VK_NULL_HANDLE;
    }
    if (p) vk_gauge_pipelines().add(1);
    pipes_[fmt] = p;
    return p;
}

void OverlayComposite::begin_overlay(VkCommandBuffer cb) {
    if (!target_) return;
    // the previous frame's read of the target (composite) is the source of the hazard; the first use starts from UNDEFINED
    if (target_read_) {
        image_barrier(cb, target_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    } else {
        image_barrier(cb, target_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    target_read_ = false;
}

void OverlayComposite::composite(VkCommandBuffer cb, VkImageView dst_view, VkFormat dst_format, VkExtent2D dst_extent, OutputKind kind, float paper_white_nits,
                                 float peak_nits) {
    if (!dev_ || !target_ || kind == OutputKind::Sdr8 || !dst_extent.width || !dst_extent.height) return;
    VkPipeline pipe = pipeline(dst_format);
    if (!pipe) return;

    // the overlay rendered into the target: make it readable by the fragment shader
    image_barrier(cb, target_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target_read_ = true;

    CompositePush pc = {};
    pc.p[0] = kind == OutputKind::Hdr10 ? 2.0f : 3.0f;
    pc.p[1] = paper_white_nits;
    pc.p[2] = peak_nits;

    VkDescriptorSet set = sets_[next_set_];
    next_set_ = (next_set_ + 1) % kSetRing;
    VkDescriptorImageInfo dii = {linear_, target_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_->device(), 1, &w, 0, nullptr);

    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = dst_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, dst_extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    const VkViewport vpt = {0, 0, static_cast<float>(dst_extent.width), static_cast<float>(dst_extent.height), 0, 1};
    const VkRect2D scissor = {{0, 0}, dst_extent};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

}  // namespace nxrt::extras
