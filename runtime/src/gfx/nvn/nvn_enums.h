// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVN enum values -> Vulkan enum values (S4d). The switch tables are nvn_enums.cpp.
// Every function takes the raw NVN enum value (an int, so the caller needs no NVN type) and never fails: an unknown value
// gives the documented default and bumps unknown_enum_count() (so a test or the log can show it).
#pragma once
#include <cstdint>

#include <volk.h>

namespace nxrt::nvn {

VkBlendFactor vk_blend_factor(int nvn_blend_func);        // NVNblendFunc; default ONE
VkBlendOp vk_blend_op(int nvn_blend_equation);            // NVNblendEquation; default ADD
VkCompareOp vk_compare_op(int nvn_compare);               // NVNdepthFunc / NVNstencilFunc / NVNcompareFunc / NVNalphaFunc (same values 1..8: NEVER..ALWAYS); default ALWAYS
VkStencilOp vk_stencil_op(int nvn_stencil_op);            // NVNstencilOp; default KEEP
VkCullModeFlags vk_cull_mode(int nvn_face);               // NVNface (NONE, FRONT, BACK, FRONT_AND_BACK); default NONE
VkFrontFace vk_front_face(int nvn_front_face);            // NVNfrontFace; the caller flips for a mirrored viewport itself
VkPolygonMode vk_polygon_mode(int nvn_polygon_mode);      // NVNpolygonMode (POINT, LINE, FILL)
VkLogicOp vk_logic_op(int nvn_logic_op);                  // NVNlogicOp (CLEAR..SET), default COPY
VkSampleCountFlagBits vk_sample_count(int samples);       // 0 or 1 -> 1, 2, 4, 8, 16; anything else -> 1
VkIndexType vk_index_type(int nvn_index_type);            // NVNindexType (UNSIGNED_BYTE -> UINT8, SHORT -> UINT16, INT -> UINT32)
uint32_t index_size_bytes(int nvn_index_type);            // 1, 2, 4

// Primitive topology of a draw. Some NVN primitives have no Vulkan equivalent: LINE_LOOP, TRIANGLE_FAN (supported, listed),
// QUADS, QUAD_STRIP and POLYGON are not (the replay converts the draw, see needs_conversion).
VkPrimitiveTopology vk_topology(int nvn_draw_primitive);  // NVNdrawPrimitive; unsupported ones return TRIANGLE_LIST
enum class PrimConversion : uint8_t { None, LineLoop, Quads, QuadStrip, Polygon };
PrimConversion prim_conversion(int nvn_draw_primitive);   // None for everything Vulkan has a topology for (fans included)

// Primitives a draw of `count` vertices in this NVN mode generates, for the NVN_COUNTER_TYPE_PRIMITIVES_GENERATED counter (one instance).
// The replay multiplies by the instance count. Unknown modes give 0.
uint64_t primitives_for(int nvn_draw_primitive, uint32_t count);

// Samplers
VkFilter vk_mag_filter(int nvn_mag_filter);                         // NVNmagFilter
VkFilter vk_min_filter(int nvn_min_filter);                         // NVNminFilter: the min part of the 6 values
VkSamplerMipmapMode vk_mipmap_mode(int nvn_min_filter);             // NEAREST / LINEAR in the *_MIPMAP_* values; NEAREST for the two no-mip values
bool min_filter_uses_mips(int nvn_min_filter);                      // the four *_MIPMAP_* values
VkSamplerAddressMode vk_address_mode(int nvn_wrap_mode);            // NVNwrapMode: CLAMP -> CLAMP_TO_EDGE, MIRROR_CLAMP* -> MIRROR_CLAMP_TO_EDGE
VkSamplerReductionMode vk_reduction_mode(int nvn_sampler_reduction);

// Texture views / swizzle
VkComponentSwizzle vk_swizzle(int nvn_texture_swizzle);             // NVNtextureSwizzle ZERO, ONE, R, G, B, A
VkImageViewType vk_view_type(int nvn_texture_target);               // NVNtextureTarget; BUFFER -> 1D, RECTANGLE -> 2D, MULTISAMPLE -> 2D / 2D_ARRAY; 1D targets are returned as 1D (the image code decides about 2D stand-ins)
VkImageType vk_image_type(int nvn_texture_target);                  // 1D*, 2D* / cube / rectangle, 3D
bool target_is_array(int nvn_texture_target);                       // 1D_ARRAY, 2D_ARRAY, 2D_MULTISAMPLE_ARRAY, CUBEMAP_ARRAY
bool target_is_cube(int nvn_texture_target);
bool target_is_multisample(int nvn_texture_target);

// Number of values the functions above did not know since start (or reset), for the log and the tests.
uint64_t unknown_enum_count();
void reset_unknown_enum_count();

}  // namespace nxrt::nvn
