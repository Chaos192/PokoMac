// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNformat -> storage facts and Vulkan formats (S4d). The table is nvn_format.cpp.
#pragma once
#include <cstdint>

#include <volk.h>
#include <nvn/nvn.h>

namespace nxrt::nvn {

enum FormatAspect : uint8_t { kAspectColor = 0, kAspectDepth = 1, kAspectStencil = 2, kAspectDepthStencil = 3 };

struct FormatInfo {
    const char* name;            // NVN enum name without the NVN_FORMAT_ prefix, e.g. "RGBA8", "RGBA_ASTC_5x4"
    int nvn;                     // the NVNformat value
    uint8_t bytes;               // bytes of one texel (uncompressed) or of one block (compressed): the NVN storage unit
    uint8_t block_w, block_h;    // texels per block (1x1 for uncompressed)
    uint8_t aspect;              // FormatAspect
    uint8_t channels;            // 1..4
    bool srgb;                   // sRGB encoded colour
    bool compressed;             // block compressed (BCn, ASTC, ETC)
    bool astc;                   // ASTC LDR (astc_w x astc_h are block_w x block_h)
    bool bc;                     // BC1..BC7
    bool is_float, is_snorm, is_int, is_uint;   // numeric class of a colour format (all false: unorm)
    VkFormat vk;                 // the Vulkan format with the same texel layout and numeric class; VK_FORMAT_UNDEFINED if none
                                 // (for ASTC this is the ASTC Vulkan format; the BC7 stand-in is chosen by the image code)
    VkFormat vk_fallback;        // used when the GPU lacks `vk` (D24S8 -> D32_SFLOAT_S8_UINT, ...); UNDEFINED if none
};

// Null for NVN_FORMAT_NONE, out-of-range values and values the table does not know.
const FormatInfo* format_info(int nvn_format);
// Number of formats in the table (every NVN_FORMAT_* of nvn.h except NONE and LARGE).
int format_count();
const FormatInfo* format_at(int index);

// The format of a vertex attribute (nvnVertexAttribStateSetFormat): the Vulkan vertex-buffer format, VK_FORMAT_UNDEFINED
// for a value that is not a vertex format. Includes the integer, scaled (USCALED/SSCALED) and normalised forms NVN offers.
VkFormat vertex_format(int nvn_format);
// A Vulkan integer colour/vertex format (UINT or SINT of 8, 16 or 32 bits and the packed 2-10-10-10 forms), and whether it is the signed one.
bool vk_format_is_integer(VkFormat f);
bool vk_format_is_signed_int(VkFormat f);

// The unit a buffer <-> image copy of format f moves for `aspect` (VkBufferImageCopy addresses the buffer in it): bytes and texels of one
// block (1x1 for uncompressed formats; the depth or stencil part alone for a depth/stencil aspect). bytes = 0: a format the NVN table
// does not use.
struct CopyBlock {
    uint32_t bytes = 0, w = 1, h = 1;
};
CopyBlock vk_copy_block(VkFormat f, VkImageAspectFlags aspect);
// A buffer copy of an image in format `image` addresses the buffer like one of an image in format `nvn` (the NVN format's own Vulkan
// format, whose layout the game's buffer has): same block size and bytes. False when either is unknown.
bool vk_copy_layout_matches(VkFormat image, VkFormat nvn, VkImageAspectFlags aspect);

}  // namespace nxrt::nvn
