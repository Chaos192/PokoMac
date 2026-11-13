// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Format-feature guards (nvn_format_guard.h).
#include "nvn_format_guard.h"

#include <nvn/nvn.h>

#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn {

namespace {
constexpr const char* kTag = "nvn.fmt";
enum Kind { kBlend, kFilter, kVertex };

const char* kind_name(int k) { return k == kBlend ? "blend" : k == kFilter ? "linear filter" : "vertex fetch"; }

int nearest_min(int f) {
    switch (f) {
        case NVN_MIN_FILTER_LINEAR: return NVN_MIN_FILTER_NEAREST;
        case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: return NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
        case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: return NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR;
        default: return f;
    }
}
}  // namespace

FormatFeatureBits FormatGuard::features(VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return {};
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = cache_.find(static_cast<int>(f));
        if (it != cache_.end()) return it->second;
    }
    const std::optional<FormatFeatureBits> b = probe_(f);   // outside the lock: the probe calls into the driver
    if (!b) {
        FormatFeatureBits all;
        all.optimal = all.buffer = ~0u;
        return all;
    }
    std::lock_guard<std::mutex> lk(mu_);
    return cache_.emplace(static_cast<int>(f), *b).first->second;
}

void FormatGuard::note(int kind, VkFormat f, const char* what) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!noted_.emplace(kind, static_cast<int>(f)).second) return;
    }
    NXLOG_W(kTag, "VkFormat %d has no %s support on this device: %s", static_cast<int>(f), kind_name(kind), what);
}

size_t FormatGuard::triggered() const {
    std::lock_guard<std::mutex> lk(mu_);
    return noted_.size();
}

bool FormatGuard::blend_allowed(VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return true;
    if (features(f).optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) return true;
    note(kBlend, f, "blending is off for targets of this format");
    return false;
}

bool FormatGuard::linear_filter_allowed(VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return true;
    if (features(f).optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) return true;
    note(kFilter, f, "samplers reading it use nearest");
    return false;
}

bool FormatGuard::limit_sampler(VkFormat f, int* min_filter, int* mag_filter, float* max_anisotropy) {
    const bool lin_min = *min_filter == NVN_MIN_FILTER_LINEAR || *min_filter == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST ||
                         *min_filter == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR;
    const bool lin_mag = *mag_filter == NVN_MAG_FILTER_LINEAR;
    if (!lin_min && !lin_mag) return false;
    if (linear_filter_allowed(f)) return false;
    *min_filter = nearest_min(*min_filter);
    *mag_filter = NVN_MAG_FILTER_NEAREST;
    *max_anisotropy = 1.0f;
    return true;
}

VkFormat vertex_four_component(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
        case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
        case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
        case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
        case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
        case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
        case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
        case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
        case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
        case VK_FORMAT_R32G32B32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
        case VK_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
        case VK_FORMAT_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat FormatGuard::vertex_fetch_format(VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return f;
    if (features(f).buffer & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) return f;
    const VkFormat alt = vertex_four_component(f);
    if (alt != VK_FORMAT_UNDEFINED && (features(alt).buffer & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        note(kVertex, f, "fetched as the 4-component format");
        return alt;
    }
    note(kVertex, f, "the attribute is left out of the vertex input");
    return VK_FORMAT_UNDEFINED;
}

FormatGuard& format_guard() {
    static FormatGuard g([](VkFormat f) -> std::optional<FormatFeatureBits> {
        const VkPhysicalDevice pd = be::gpu::device().physical();
        if (pd == VK_NULL_HANDLE) return std::nullopt;
        VkFormatProperties p = {};
        vkGetPhysicalDeviceFormatProperties(pd, f, &p);
        return FormatFeatureBits{p.optimalTilingFeatures, p.bufferFeatures};
    });
    return g;
}

}  // namespace nxrt::nvn
