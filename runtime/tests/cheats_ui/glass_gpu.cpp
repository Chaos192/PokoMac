// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The glass panel's GPU blur against its CPU reference (nxrt_cheats_ui_test --gpu): a headless device, a frame image holding the test's game
// picture, BackdropBlur::record over a panel's rectangle, level 0 read back and compared with blur_ref.h inside the rectangle; then the GPU
// time of one blur (timestamps, the mean of 20) at each panel size. Only inside the GPU slot (gpu_slot.sh sets GPU_SLOT_HELD): a device
// is never made outside it.
#include <volk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "blur_ref.h"
#include "extras/backdrop_blur.h"
#include "nxrt/gfx.h"

namespace nxrt::test {

void game_frame_rgba(std::vector<uint8_t>& px, int w, int h);   // cheats_ui_shots.cpp

namespace {

bool slot_held() {
    const char* held = std::getenv("GPU_SLOT_HELD");
    if (held && *held) return true;
    const char* bypass = std::getenv("GPU_SLOT_BYPASS");
    return bypass && !std::strcmp(bypass, "1");
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
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

// Returns the number of failures (0: all ok); a skip outside the slot is not a failure.
int run_gpu_glass_test(const std::string& out_dir) {
    if (!slot_held()) {
        std::printf("skip   glass GPU test: no GPU device outside the GPU slot (run it through gpu_slot.sh)\n");
        return 0;
    }
    int failures = 0;
    gfx::DeviceDesc dd;
    dd.app_name = "nxrt_cheats_ui_test";
    dd.headless = 1;
    std::string err;
    auto dev = gfx::Device::create(dd, &err);
    if (!dev) {
        std::printf("FAILED glass GPU test: no device: %s\n", err.c_str());
        return 1;
    }
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(dev->physical(), &props);
    std::printf("gpu    %s (driver %u.%u, sub-texel precision %u bits)\n", props.deviceName, VK_API_VERSION_MAJOR(props.driverVersion),
                VK_API_VERSION_MINOR(props.driverVersion), props.limits.subTexelPrecisionBits);
    struct Size { int w, h; const char* tag; };
    const Size sizes[] = {{1280, 720, "720p"}, {1920, 1080, "1080p"}, {2560, 1440, "1440p"}, {3840, 2160, "4K"}};
    for (const Size& sz : sizes) {
        // the frame: the test's game picture in an image the blur reads as it reads the swapchain
        std::vector<uint8_t> frame;
        game_frame_rgba(frame, sz.w, sz.h);
        gfx::ImageDesc id;
        id.format = VK_FORMAT_R8G8B8A8_UNORM;
        id.width = static_cast<uint32_t>(sz.w);
        id.height = static_cast<uint32_t>(sz.h);
        id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        id.name = "glass test frame";
        gfx::Image img;
        gfx::Buffer up, down;
        if (dev->create_image(id, &img) != VK_SUCCESS ||
            dev->create_buffer(frame.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryUse::Upload, &up, "glass test upload") != VK_SUCCESS) {
            std::printf("FAILED glass GPU test %s: allocation\n", sz.tag);
            ++failures;
            continue;
        }
        std::memcpy(up.mapped, frame.data(), frame.size());
        extras::BackdropBlur blur;
        if (!blur.init(*dev) || !blur.ensure({static_cast<uint32_t>(sz.w), static_cast<uint32_t>(sz.h)})) {
            std::printf("FAILED glass GPU test %s: the blur did not come up\n", sz.tag);
            ++failures;
            dev->destroy_image(img);
            dev->destroy_buffer(up);
            continue;
        }
        const VkExtent2D lv0 = blur.level_extent(0);
        dev->create_buffer(static_cast<VkDeviceSize>(lv0.width) * lv0.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &down,
                           "glass test readback");
        // the panel: centred, as the overlay places it (64 % x 84 % of the frame)
        const int rect[4] = {sz.w * 18 / 100, sz.h * 8 / 100, sz.w * 64 / 100, sz.h * 84 / 100};
        const float strength = 0.7f;      // Frost's
        VkQueryPool qp = VK_NULL_HANDLE;
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        vkCreateQueryPool(dev->device(), &qi, nullptr, &qp);
        const VkExtent2D ext = {static_cast<uint32_t>(sz.w), static_cast<uint32_t>(sz.h)};
        // upload, blur once, read level 0 back
        dev->submit_once([&](VkCommandBuffer cb) {
            barrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c = {};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {static_cast<uint32_t>(sz.w), static_cast<uint32_t>(sz.h), 1};
            vkCmdCopyBufferToImage(cb, up.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            blur.record(cb, img.image, ext, rect, strength);
            VkBufferImageCopy r = {};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {lv0.width, lv0.height, 1};
            vkCmdCopyImageToBuffer(cb, blur.image(0), VK_IMAGE_LAYOUT_GENERAL, down.buffer, 1, &r);
        });
        // compare with the CPU reference inside the rectangle (level 0 coordinates)
        int rw = 0, rh = 0;
        const std::vector<uint8_t> ref = blur_reference(frame, sz.w, sz.h, strength, rect, &rw, &rh);
        const uint8_t* got = static_cast<const uint8_t*>(down.mapped);
        int worst = 0;
        double sum = 0;
        size_t n = 0, over = 0;
        for (int y = rect[1] / 2; y < (rect[1] + rect[3]) / 2; ++y)
            for (int x = rect[0] / 2; x < (rect[0] + rect[2]) / 2; ++x)
                for (int c = 0; c < 3; ++c) {
                    const int d = std::abs(static_cast<int>(got[(static_cast<size_t>(y) * lv0.width + x) * 4 + c]) - ref[(static_cast<size_t>(y) * rw + x) * 4 + c]);
                    worst = std::max(worst, d);
                    sum += d;
                    ++n;
                    over += d > 3;
                }
        const double mean = n ? sum / static_cast<double>(n) : 0.0;
        // a GPU filters with limited sub-texel precision (VkPhysicalDeviceLimits::subTexelPrecisionBits, at least 4), so after the passes a
        // texel may differ from the exact arithmetic by a level or two: allowed up to 4, below 1 on average
        const bool ok = n && mean < 1.0 && worst <= 4 && over * 1000 < n;
        if (!ok) ++failures;
        // the GPU time of one blur, the mean of 20
        double ms = 0;
        int timed = 0;
        for (int it = 0; it < 20; ++it) {
            dev->submit_once([&](VkCommandBuffer cb) {
                vkCmdResetQueryPool(cb, qp, 0, 2);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 0);
                blur.record(cb, img.image, ext, rect, strength);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 1);
            });
            uint64_t ts[2] = {};
            if (vkGetQueryPoolResults(dev->device(), qp, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
                ts[1] > ts[0]) {
                ms += static_cast<double>(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
                ++timed;
            }
        }
        std::printf("%-6s glass GPU blur %s, panel %dx%d: mean diff %.3f, worst %d, %.3f%% off by more than 3 (vs the CPU reference); %.3f ms per frame\n",
                    ok ? "ok" : "FAILED", sz.tag, rect[2], rect[3], mean, worst, n ? 100.0 * static_cast<double>(over) / static_cast<double>(n) : 0.0,
                    timed ? ms / timed : -1.0);
        if (sz.w == 1280) {   // the GPU's picture, for a look (level 0, half size)
            std::vector<uint8_t> pic(got, got + static_cast<size_t>(lv0.width) * lv0.height * 4);
            gfx::save_png_rgba(out_dir + "/glass_gpu_level0.png", pic.data(), lv0.width, lv0.height);
            gfx::save_png_rgba(out_dir + "/glass_cpu_level0.png", ref.data(), static_cast<uint32_t>(rw), static_cast<uint32_t>(rh));
        }
        vkDestroyQueryPool(dev->device(), qp, nullptr);
        blur.shutdown();
        dev->destroy_buffer(down);
        dev->destroy_buffer(up);
        dev->destroy_image(img);
    }
    return failures;
}

}  // namespace nxrt::test
