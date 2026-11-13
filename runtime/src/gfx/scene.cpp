// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The test scene (declarations in include/nxrt/scene.h): a clear colour and two textured quads, the texture going through the
// block-linear tiling library of tools/tiling (tegra_tiling.h) before it is uploaded.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/scene.h"
#include "tegra_tiling.h"
#include "vk_gauges.h"

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "scene";
constexpr uint32_t kTexSize = 256;
constexpr uint32_t kTexLevels = 9;   // 256 .. 1

#include "scene_shaders.inc"

struct PushConstants {
    float center[2];
    float half_size[2];
    float angle;
    float aspect;
    float lod;
    float pad;
};

// Level 0 of the test image: asymmetric on purpose (a flipped or swapped image shows). Integer maths only.
void paint_level0(uint8_t* px) {
    for (uint32_t y = 0; y < kTexSize; ++y) {
        for (uint32_t x = 0; x < kTexSize; ++x) {
            uint8_t* p = px + (static_cast<size_t>(y) * kTexSize + x) * 4;
            const bool check = (((x / 32) + (y / 32)) & 1) != 0;
            p[0] = static_cast<uint8_t>(x);
            p[1] = static_cast<uint8_t>(y);
            p[2] = check ? 230 : 40;
            p[3] = 255;
            const int dx = static_cast<int>(x) - 128, dy = static_cast<int>(y) - 128;
            const int d2 = dx * dx + dy * dy;
            if (d2 >= 84 * 84 && d2 <= 92 * 92) { p[0] = 250; p[1] = 220; p[2] = 20; }           // ring
            if (x < 28 && y < 28) { p[0] = p[1] = p[2] = 255; }                                    // white corner marker, top left
            if (x >= 228 && y >= 228 && x - 228 == y - 228) { p[0] = p[1] = p[2] = 0; }            // black diagonal, bottom right
        }
    }
}

}  // namespace

std::vector<uint8_t> TestScene::make_texture_levels(uint32_t* level_count) {
    // all levels back to back, tight rows, level 0 first; box-filtered mips
    std::vector<uint8_t> out;
    std::vector<uint8_t> cur(static_cast<size_t>(kTexSize) * kTexSize * 4);
    paint_level0(cur.data());
    uint32_t w = kTexSize;
    for (uint32_t l = 0; l < kTexLevels; ++l) {
        out.insert(out.end(), cur.begin(), cur.end());
        if (l + 1 == kTexLevels) break;
        const uint32_t nw = std::max(1u, w / 2);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nw * 4);
        for (uint32_t y = 0; y < nw; ++y)
            for (uint32_t x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                    const uint32_t y0 = std::min(2 * y, w - 1), y1 = std::min(2 * y + 1, w - 1);
                    const uint32_t sum = cur[(static_cast<size_t>(y0) * w + x0) * 4 + c] + cur[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                         cur[(static_cast<size_t>(y1) * w + x0) * 4 + c] + cur[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
        cur.swap(next);
        w = nw;
    }
    if (level_count) *level_count = kTexLevels;
    return out;
}

struct TestScene::Impl {
    Device* dev = nullptr;
    Image texture;
    Buffer staging;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t tiled = 0;
    bool round_trip = false;

    ~Impl() {
        VkDevice d = dev->device();
        dev->wait_idle();
        if (pipeline) { vkDestroyPipeline(d, pipeline, nullptr); vk_gauge_pipelines().sub(1); }
        if (layout) { vkDestroyPipelineLayout(d, layout, nullptr); vk_gauge_pipeline_layouts().sub(1); }
        if (pool) {   // frees the set
            vkDestroyDescriptorPool(d, pool, nullptr);
            vk_gauge_descriptor_pools().sub(1);
            if (set) vk_gauge_descriptor_sets().sub(1);
        }
        if (set_layout) { vkDestroyDescriptorSetLayout(d, set_layout, nullptr); vk_gauge_descriptor_set_layouts().sub(1); }
        if (sampler) { vkDestroySampler(d, sampler, nullptr); vk_gauge_samplers().sub(1); }
        dev->destroy_buffer(staging);
        dev->destroy_image(texture);
    }
};

std::unique_ptr<TestScene> TestScene::create(Device& dev, VkFormat color_format, std::string* error) {
    std::unique_ptr<TestScene> sc(new TestScene());
    sc->impl_ = new Impl();
    Impl& s = *sc->impl_;
    s.dev = &dev;
    auto fail = [&](const std::string& m) -> std::unique_ptr<TestScene> {
        if (error) *error = m;
        NXLOG_E(kTag, "%s", m.c_str());
        return nullptr;
    };

    // ---- the texture: author linear, tile (as the console stores it), untile (as the runtime reads it), upload
    uint32_t levels = 0;
    const std::vector<uint8_t> linear = make_texture_levels(&levels);
    tegra::TextureDesc td;
    td.width = td.height = kTexSize;
    td.levels = levels;
    td.block_w = td.block_h = 1;
    td.bytes_per_block = 4;
    td.gob_blocks_y = tegra::default_gob_blocks_y(kTexSize);
    td.linear_stride_align = 1;   // tight rows, as make_texture_levels produces
    const tegra::Layout lay = tegra::compute_layout(td);
    if (!lay.valid || lay.linear_size != linear.size()) return fail("tiling layout does not match the texture data");
    std::vector<uint8_t> tiled(lay.tiled_size, 0), back(lay.linear_size, 0);
    if (!tegra::tile(td, lay, linear.data(), tiled.data())) return fail("tegra::tile failed");
    if (!tegra::untile(td, lay, tiled.data(), back.data())) return fail("tegra::untile failed");
    s.tiled = lay.tiled_size;
    s.round_trip = back == linear;
    if (!s.round_trip) return fail("tiling round trip changed the texture");
    if (std::memcmp(tiled.data(), linear.data(), std::min(tiled.size(), linear.size())) == 0) return fail("tile() left the texture in linear order");
    NXLOG_I(kTag, "test texture %ux%u, %u levels: %llu linear bytes, %llu tiled bytes (block height %u GOBs), round trip ok", kTexSize, kTexSize,
            levels, static_cast<unsigned long long>(lay.linear_size), static_cast<unsigned long long>(lay.tiled_size), td.gob_blocks_y);

    ImageDesc id;
    id.format = VK_FORMAT_R8G8B8A8_UNORM;
    id.width = id.height = kTexSize;
    id.mip_levels = levels;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.name = "test texture";
    std::string why;
    if (dev.create_image(id, &s.texture, &why) != VK_SUCCESS) return fail("texture image: " + why);
    if (dev.create_buffer(back.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUse::Upload, &s.staging, "texture staging") != VK_SUCCESS) return fail("staging buffer");
    std::memcpy(s.staging.mapped, back.data(), back.size());
    dev.flush_buffer(s.staging);

    std::vector<VkBufferImageCopy2> regions;
    for (uint32_t l = 0; l < levels; ++l) {
        const tegra::LevelLayout& lv = lay.levels[l];
        VkBufferImageCopy2 r = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        r.bufferOffset = lv.linear_offset;
        r.bufferRowLength = static_cast<uint32_t>(lv.linear_row_stride / 4);
        r.bufferImageHeight = lv.height;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        r.imageExtent = {lv.width, lv.height, 1};
        regions.push_back(r);
    }
    const bool uploaded = dev.submit_once([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s.texture.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        VkCopyBufferToImageInfo2 ci = {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        ci.srcBuffer = s.staging.buffer;
        ci.dstImage = s.texture.image;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = static_cast<uint32_t>(regions.size());
        ci.pRegions = regions.data();
        vkCmdCopyBufferToImage2(cmd, &ci);
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &di);
    });
    if (!uploaded) return fail("texture upload failed");

    // ---- descriptors, pipeline
    VkDevice d = dev.device();
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = static_cast<float>(levels);
    if (vkCreateSampler(d, &sci, nullptr, &s.sampler) != VK_SUCCESS) return fail("sampler");
    vk_gauge_samplers().add(1);
    VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    if (vkCreateDescriptorSetLayout(d, &dli, nullptr, &s.set_layout) != VK_SUCCESS) return fail("descriptor set layout");
    vk_gauge_descriptor_set_layouts().add(1);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dpi, nullptr, &s.pool) != VK_SUCCESS) return fail("descriptor pool");
    vk_gauge_descriptor_pools().add(1);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.set_layout;
    if (vkAllocateDescriptorSets(d, &dai, &s.set) != VK_SUCCESS) return fail("descriptor set");
    vk_gauge_descriptor_sets().add(1);
    VkDescriptorImageInfo dii = {s.sampler, s.texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &s.layout) != VK_SUCCESS) return fail("pipeline layout");
    vk_gauge_pipeline_layouts().add(1);

    auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes;
        mi.pCode = code;
        const bool ok = vkCreateShaderModule(d, &mi, nullptr, out) == VK_SUCCESS;
        if (ok) vk_gauge_shader_modules().add(1);
        return ok;
    };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (!make_module(kSceneVert, sizeof kSceneVert, &vs) || !make_module(kSceneFrag, sizeof kSceneFrag, &fs)) return fail("shader module");
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
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
    pri.pColorAttachmentFormats = &color_format;
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
    gpi.layout = s.layout;
    const VkResult pr = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &s.pipeline);
    if (pr == VK_SUCCESS) vk_gauge_pipelines().add(1);
    vkDestroyShaderModule(d, vs, nullptr);
    vk_gauge_shader_modules().sub(1);
    vkDestroyShaderModule(d, fs, nullptr);
    vk_gauge_shader_modules().sub(1);
    if (pr != VK_SUCCESS) return fail("graphics pipeline");
    return sc;
}

TestScene::~TestScene() { delete impl_; }

uint64_t TestScene::tiled_bytes() const { return impl_->tiled; }
bool TestScene::tiling_round_trip_ok() const { return impl_->round_trip; }

void TestScene::record(const FrameInfo& f) {
    Impl& s = *impl_;
    VkClearValue clear;
    clear.color = {{0.05f, 0.08f, 0.14f, 1.0f}};
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = f.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue = clear;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, f.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(f.cmd, &ri);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &s.set, 0, nullptr);
    const VkViewport vpt = {0, 0, static_cast<float>(f.extent.width), static_cast<float>(f.extent.height), 0, 1};
    const VkRect2D sc = {{0, 0}, f.extent};
    vkCmdSetViewport(f.cmd, 0, 1, &vpt);
    vkCmdSetScissor(f.cmd, 0, 1, &sc);

    const float aspect = static_cast<float>(f.extent.width) / static_cast<float>(f.extent.height);
    const float t = static_cast<float>(f.frame_number);
    // big quad: rotates, breathes
    PushConstants pc = {};
    pc.center[0] = 0.0f;
    pc.center[1] = 0.0f;
    pc.half_size[0] = pc.half_size[1] = 0.62f + 0.06f * std::sin(t * 0.05f);
    pc.angle = t * 0.02f;
    pc.aspect = aspect;
    pc.lod = 0.0f;
    vkCmdPushConstants(f.cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(f.cmd, 4, 1, 0, 0);
    // small quad in the top right corner at level 2: shows the mip chain came through the tiling library intact
    pc.center[0] = 0.78f;
    pc.center[1] = -0.72f;
    pc.half_size[0] = pc.half_size[1] = 0.16f;
    pc.angle = 0.0f;
    pc.lod = 2.0f;
    vkCmdPushConstants(f.cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(f.cmd, 4, 1, 0, 0);
    vkCmdEndRendering(f.cmd);
}

}  // namespace nxrt::gfx
