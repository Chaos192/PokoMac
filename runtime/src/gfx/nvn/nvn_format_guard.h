// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Format-feature guards: what a Vulkan format can do on this device, asked once per format and cached, and the decisions that depend
// on it. Blend, linear filtering and vertex fetch are optional per format (Intel, older AMD / NVIDIA lack some of them for float and
// packed formats); using them anyway is undefined behaviour (blend: VUID-vkCmdDraw-None-06479 family, filter: magFilter-04553 family,
// vertex: VUID-VkVertexInputAttributeDescription-format-00623). Every decision is a pure function of the device, so the pipeline keys,
// the pipeline cache and the prewarm lists stay as they are: the guards act where the Vulkan structs are filled.
// Design from pokopia-native (MIT), windows/src/nxrt/vk/vk_replay.c (blend, filter and vertex checks) @7c3655d.
// Each guard logs ONE line per (guard, format) the first time it changes something; on a device with the features nothing is logged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include <volk.h>

namespace nxrt::nvn {

struct FormatFeatureBits {
    VkFormatFeatureFlags optimal = 0;   // optimalTilingFeatures
    VkFormatFeatureFlags buffer = 0;    // bufferFeatures (vertex fetch)
};

class FormatGuard {
public:
    // nullopt: the device is not known yet; the answer is then "everything allowed" and is not cached.
    using Probe = std::function<std::optional<FormatFeatureBits>(VkFormat)>;
    explicit FormatGuard(Probe probe) : probe_(std::move(probe)) {}

    // The features of f, asked of the probe on first use and cached. VK_FORMAT_UNDEFINED has none.
    FormatFeatureBits features(VkFormat f);

    // A colour attachment of format f may enable blending. UNDEFINED (no attachment) is allowed. False: the draw writes without blending.
    bool blend_allowed(VkFormat color_format);
    // Sampled image format f may be read with linear min / mag filters. False: the sampler uses nearest.
    bool linear_filter_allowed(VkFormat image_format);
    // Rewrites NVN min / mag filter values (NVNminFilter, NVNmagFilter) to their nearest counterparts (the mip part is kept) when the
    // sampler filters linearly and linear_filter_allowed(image_format) is false; anisotropy drops to 1 then. True when anything changed.
    bool limit_sampler(VkFormat image_format, int* nvn_min_filter, int* nvn_mag_filter, float* max_anisotropy);
    // The format to fetch a vertex attribute of format f in: f itself when the device can; the 4-component sibling of a 3-component
    // 8 / 16 / 32-bit format when that one can (the extra component is not read by the shader); else VK_FORMAT_UNDEFINED (the
    // attribute is left out of the vertex input).
    VkFormat vertex_fetch_format(VkFormat f);

    // Number of distinct (guard, format) pairs that changed something so far.
    size_t triggered() const;

private:
    void note(int kind, VkFormat f, const char* what);

    Probe probe_;
    mutable std::mutex mu_;
    std::unordered_map<int, FormatFeatureBits> cache_;
    std::set<std::pair<int, int>> noted_;
};

// The 4-component sibling of a 3-component vertex format (R8G8B8_*, R16G16B16_*, R32G32B32_*); UNDEFINED for any other format.
VkFormat vertex_four_component(VkFormat f);

// The guard of the running device (probe: vkGetPhysicalDeviceFormatProperties of the render device). Until the device exists every
// answer is "allowed", so nothing is rewritten.
FormatGuard& format_guard();

}  // namespace nxrt::nvn
