// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Depth comparison on a colour texture: the value the console compares against, and the small by-value cache of stand-in images.
// The game binds its default 1x1 / 8x8 black and white textures to the shadow samplers of lights without shadows (VUID-vkCmdDrawIndexed-None-06479).
// The console's hardware compares with the RED channel of the texel, so the stand-in is a 1x1 depth image cleared to the red value of texel 0.
// Design from pokopia-native (MIT), windows/src/nxrt/vk/vk_replay.c dcmp_view() @7c3655d. Pure functions, no Vulkan: tested in test_nvn_dcmp.cpp.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include <volk.h>

namespace nxrt::nvn::be {

constexpr int kDcmpMaxValues = 8;      // distinct stand-in values kept (each is one 1x1x6 image with four views)

// Red of an IEEE half.
inline float dcmp_half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}

inline float dcmp_srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// The red channel of the first texel (px: its bytes, as stored in the format `f` the texture's image would have). Clamped to [0, 1], the range a depth
// compare reference takes. `ok` false: the format is not one the default textures use (compressed, integer, packed): the caller keeps 1.0 (lit).
inline float dcmp_red(VkFormat f, const uint8_t* px, bool* ok) {
    *ok = true;
    auto u16 = [&](int i) { uint16_t v; std::memcpy(&v, px + 2 * i, 2); return v; };
    auto u32 = [&](int i) { uint32_t v; std::memcpy(&v, px + 4 * i, 4); return v; };
    float r = 1.0f;
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM: r = px[0] / 255.0f; break;
        case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB: r = dcmp_srgb_to_linear(px[0] / 255.0f); break;
        case VK_FORMAT_B8G8R8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: r = px[2] / 255.0f; break;
        case VK_FORMAT_B8G8R8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: r = dcmp_srgb_to_linear(px[2] / 255.0f); break;
        case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8B8A8_SNORM: r = static_cast<int8_t>(px[0]) / 127.0f; break;
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM: r = u16(0) / 65535.0f; break;
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: r = dcmp_half(u16(0)); break;
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: { const uint32_t b = u32(0); std::memcpy(&r, &b, 4); break; }
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: r = (u32(0) & 1023u) / 1023.0f; break;
        default: *ok = false; return 1.0f;
    }
    if (!(r == r)) return 1.0f;
    return r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
}

// The distinct values that have a stand-in image. slot() gives the index of the entry holding v (adding it while there is room); once all
// kDcmpMaxValues are taken a new value maps to the nearest kept one (the compare outcome only differs for depths between the two).
struct DcmpValues {
    float v[kDcmpMaxValues];
    int n = 0;
    int slot(float x, bool* added = nullptr) {
        if (added) *added = false;
        for (int i = 0; i < n; ++i) if (v[i] == x) return i;
        if (n < kDcmpMaxValues) { v[n] = x; if (added) *added = true; return n++; }
        int best = 0;
        for (int i = 1; i < n; ++i) if (std::fabs(v[i] - x) < std::fabs(v[best] - x)) best = i;
        return best;
    }
};

}  // namespace nxrt::nvn::be
