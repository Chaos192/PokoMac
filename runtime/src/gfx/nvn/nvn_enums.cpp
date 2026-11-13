// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVN enum values -> Vulkan enum values (S4d). The switch tables behind nvn_enums.h.
// Every default branch counts the value in one atomic counter, so a wrong value shows up in the log and in the tests.
#include "nvn_enums.h"

#include <atomic>

#include <nvn/nvn.h>

namespace nxrt::nvn {
namespace {

std::atomic<uint64_t> g_unknown_enum_count{0};

void note_unknown() { g_unknown_enum_count.fetch_add(1, std::memory_order_relaxed); }

}  // namespace

uint64_t unknown_enum_count() { return g_unknown_enum_count.load(std::memory_order_relaxed); }
void reset_unknown_enum_count() { g_unknown_enum_count.store(0, std::memory_order_relaxed); }

VkBlendFactor vk_blend_factor(int nvn_blend_func) {
    switch (nvn_blend_func) {
        case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
        case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
        case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: note_unknown(); return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp vk_blend_op(int nvn_blend_equation) {
    switch (nvn_blend_equation) {
        case NVN_BLEND_EQUATION_ADD: return VK_BLEND_OP_ADD;
        case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
        case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
        case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
        default: note_unknown(); return VK_BLEND_OP_ADD;
    }
}

// NVNdepthFunc, NVNstencilFunc, NVNcompareFunc and NVNalphaFunc all use the same values 1..8 (NEVER..ALWAYS).
VkCompareOp vk_compare_op(int nvn_compare) {
    switch (nvn_compare) {
        case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
        case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
        case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
        case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
        case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case NVN_COMPARE_FUNC_ALWAYS: return VK_COMPARE_OP_ALWAYS;
        default: note_unknown(); return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp vk_stencil_op(int nvn_stencil_op) {
    switch (nvn_stencil_op) {
        case NVN_STENCIL_OP_KEEP: return VK_STENCIL_OP_KEEP;
        case NVN_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
        case NVN_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
        case NVN_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case NVN_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case NVN_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
        case NVN_STENCIL_OP_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case NVN_STENCIL_OP_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: note_unknown(); return VK_STENCIL_OP_KEEP;
    }
}

VkCullModeFlags vk_cull_mode(int nvn_face) {
    switch (nvn_face) {
        case NVN_FACE_NONE: return VK_CULL_MODE_NONE;
        case NVN_FACE_FRONT: return VK_CULL_MODE_FRONT_BIT;
        case NVN_FACE_BACK: return VK_CULL_MODE_BACK_BIT;
        case NVN_FACE_FRONT_AND_BACK: return VK_CULL_MODE_FRONT_AND_BACK;
        default: note_unknown(); return VK_CULL_MODE_NONE;
    }
}

VkFrontFace vk_front_face(int nvn_front_face) {
    switch (nvn_front_face) {
        case NVN_FRONT_FACE_CW: return VK_FRONT_FACE_CLOCKWISE;
        case NVN_FRONT_FACE_CCW: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
        default: note_unknown(); return VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
}

VkPolygonMode vk_polygon_mode(int nvn_polygon_mode) {
    switch (nvn_polygon_mode) {
        case NVN_POLYGON_MODE_POINT: return VK_POLYGON_MODE_POINT;
        case NVN_POLYGON_MODE_LINE: return VK_POLYGON_MODE_LINE;
        case NVN_POLYGON_MODE_FILL: return VK_POLYGON_MODE_FILL;
        default: note_unknown(); return VK_POLYGON_MODE_FILL;
    }
}

VkLogicOp vk_logic_op(int nvn_logic_op) {
    switch (nvn_logic_op) {
        case NVN_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
        case NVN_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
        case NVN_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
        case NVN_LOGIC_OP_COPY: return VK_LOGIC_OP_COPY;
        case NVN_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
        case NVN_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
        case NVN_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
        case NVN_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
        case NVN_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
        case NVN_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
        case NVN_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
        case NVN_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
        case NVN_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
        case NVN_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
        case NVN_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
        case NVN_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
        default: note_unknown(); return VK_LOGIC_OP_COPY;
    }
}

VkSampleCountFlagBits vk_sample_count(int samples) {
    switch (samples) {
        case 0:
        case 1: return VK_SAMPLE_COUNT_1_BIT;
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        default: note_unknown(); return VK_SAMPLE_COUNT_1_BIT;
    }
}

VkIndexType vk_index_type(int nvn_index_type) {
    switch (nvn_index_type) {
        case NVN_INDEX_TYPE_UNSIGNED_BYTE: return VK_INDEX_TYPE_UINT8;
        case NVN_INDEX_TYPE_UNSIGNED_SHORT: return VK_INDEX_TYPE_UINT16;
        case NVN_INDEX_TYPE_UNSIGNED_INT: return VK_INDEX_TYPE_UINT32;
        default: note_unknown(); return VK_INDEX_TYPE_UINT16;
    }
}

uint32_t index_size_bytes(int nvn_index_type) {
    switch (nvn_index_type) {
        case NVN_INDEX_TYPE_UNSIGNED_BYTE: return 1;
        case NVN_INDEX_TYPE_UNSIGNED_SHORT: return 2;
        case NVN_INDEX_TYPE_UNSIGNED_INT: return 4;
        default: note_unknown(); return 2;
    }
}

VkPrimitiveTopology vk_topology(int nvn_draw_primitive) {
    switch (nvn_draw_primitive) {
        case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;  // closed by the replay (PrimConversion::LineLoop)
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case NVN_DRAW_PRIMITIVE_QUADS: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // converted by the replay (PrimConversion::Quads)
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // converted (PrimConversion::QuadStrip)
        case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // converted (PrimConversion::Polygon)
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: note_unknown(); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

PrimConversion prim_conversion(int nvn_draw_primitive) {
    switch (nvn_draw_primitive) {
        case NVN_DRAW_PRIMITIVE_POINTS:
        case NVN_DRAW_PRIMITIVE_LINES:
        case NVN_DRAW_PRIMITIVE_LINE_STRIP:
        case NVN_DRAW_PRIMITIVE_TRIANGLES:
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY:
        case NVN_DRAW_PRIMITIVE_PATCHES: return PrimConversion::None;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return PrimConversion::LineLoop;
        case NVN_DRAW_PRIMITIVE_QUADS: return PrimConversion::Quads;
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return PrimConversion::QuadStrip;
        case NVN_DRAW_PRIMITIVE_POLYGON: return PrimConversion::Polygon;
        default: note_unknown(); return PrimConversion::None;
    }
}

uint64_t primitives_for(int nvn_draw_primitive, uint32_t count) {
    const int64_t n = count;
    auto pos = [](int64_t v) -> uint64_t { return v > 0 ? static_cast<uint64_t>(v) : 0; };
    switch (nvn_draw_primitive) {
        case NVN_DRAW_PRIMITIVE_POINTS: return count;
        case NVN_DRAW_PRIMITIVE_LINES: return count / 2;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return count >= 2 ? count : 0;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return pos(n - 1);
        case NVN_DRAW_PRIMITIVE_TRIANGLES: return count / 3;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return pos(n - 2);
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return pos(n - 2);
        case NVN_DRAW_PRIMITIVE_QUADS: return count / 4;
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return pos((n - 2) / 2);
        case NVN_DRAW_PRIMITIVE_POLYGON: return pos(n - 2);
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return count / 4;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return pos(n - 3);
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return count / 6;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return pos((n - 4) / 2);
        case NVN_DRAW_PRIMITIVE_PATCHES: return count / 3;   // patch size is not known to the replay: 3
        default: note_unknown(); return 0;
    }
}

VkFilter vk_mag_filter(int nvn_mag_filter) {
    switch (nvn_mag_filter) {
        case NVN_MAG_FILTER_NEAREST: return VK_FILTER_NEAREST;
        case NVN_MAG_FILTER_LINEAR: return VK_FILTER_LINEAR;
        default: note_unknown(); return VK_FILTER_LINEAR;
    }
}

// The texel filter of the min value. The mip filter is vk_mipmap_mode.
VkFilter vk_min_filter(int nvn_min_filter) {
    switch (nvn_min_filter) {
        case NVN_MIN_FILTER_NEAREST: return VK_FILTER_NEAREST;
        case NVN_MIN_FILTER_LINEAR: return VK_FILTER_LINEAR;
        case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST: return VK_FILTER_NEAREST;
        case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: return VK_FILTER_LINEAR;
        case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR: return VK_FILTER_NEAREST;
        case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: return VK_FILTER_LINEAR;
        default: note_unknown(); return VK_FILTER_LINEAR;
    }
}

// The second word of a *_MIPMAP_* value is the mip filter. The two no-mip values give NEAREST.
VkSamplerMipmapMode vk_mipmap_mode(int nvn_min_filter) {
    switch (nvn_min_filter) {
        case NVN_MIN_FILTER_NEAREST:
        case NVN_MIN_FILTER_LINEAR:
        case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST:
        case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR:
        case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        default: note_unknown(); return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

bool min_filter_uses_mips(int nvn_min_filter) {
    switch (nvn_min_filter) {
        case NVN_MIN_FILTER_NEAREST:
        case NVN_MIN_FILTER_LINEAR: return false;
        case NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST:
        case NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST:
        case NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR:
        case NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR: return true;
        default: note_unknown(); return false;
    }
}

// Vulkan has no mirror-clamp-to-border address mode, so MIRROR_CLAMP_TO_BORDER becomes MIRROR_CLAMP_TO_EDGE.
VkSamplerAddressMode vk_address_mode(int nvn_wrap_mode) {
    switch (nvn_wrap_mode) {
        case NVN_WRAP_MODE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case NVN_WRAP_MODE_MIRROR_CLAMP: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case NVN_WRAP_MODE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default: note_unknown(); return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkSamplerReductionMode vk_reduction_mode(int nvn_sampler_reduction) {
    switch (nvn_sampler_reduction) {
        case NVN_SAMPLER_REDUCTION_AVERAGE: return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
        case NVN_SAMPLER_REDUCTION_MIN: return VK_SAMPLER_REDUCTION_MODE_MIN;
        case NVN_SAMPLER_REDUCTION_MAX: return VK_SAMPLER_REDUCTION_MODE_MAX;
        default: note_unknown(); return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    }
}

VkComponentSwizzle vk_swizzle(int nvn_texture_swizzle) {
    switch (nvn_texture_swizzle) {
        case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
        case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
        case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
        case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
        case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
        case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
        default: note_unknown(); return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// BUFFER is a VkBufferView, not an image. Its view type is 1D, so the image type for it is listed under 2D below.
VkImageViewType vk_view_type(int nvn_texture_target) {
    switch (nvn_texture_target) {
        case NVN_TEXTURE_TARGET_1D: return VK_IMAGE_VIEW_TYPE_1D;
        case NVN_TEXTURE_TARGET_2D: return VK_IMAGE_VIEW_TYPE_2D;
        case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_VIEW_TYPE_3D;
        case NVN_TEXTURE_TARGET_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case NVN_TEXTURE_TARGET_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: return VK_IMAGE_VIEW_TYPE_2D;
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case NVN_TEXTURE_TARGET_RECTANGLE: return VK_IMAGE_VIEW_TYPE_2D;
        case NVN_TEXTURE_TARGET_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case NVN_TEXTURE_TARGET_BUFFER: return VK_IMAGE_VIEW_TYPE_1D;
        default: note_unknown(); return VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkImageType vk_image_type(int nvn_texture_target) {
    switch (nvn_texture_target) {
        case NVN_TEXTURE_TARGET_1D:
        case NVN_TEXTURE_TARGET_1D_ARRAY: return VK_IMAGE_TYPE_1D;
        case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_TYPE_3D;
        case NVN_TEXTURE_TARGET_2D:
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
        case NVN_TEXTURE_TARGET_RECTANGLE:
        case NVN_TEXTURE_TARGET_CUBEMAP:
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return VK_IMAGE_TYPE_2D;
        case NVN_TEXTURE_TARGET_BUFFER: return VK_IMAGE_TYPE_1D;   // shown as a 1D image (view type 1D)
        default: note_unknown(); return VK_IMAGE_TYPE_2D;
    }
}

bool target_is_array(int nvn_texture_target) {
    switch (nvn_texture_target) {
        case NVN_TEXTURE_TARGET_1D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return true;
        case NVN_TEXTURE_TARGET_1D:
        case NVN_TEXTURE_TARGET_2D:
        case NVN_TEXTURE_TARGET_3D:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE:
        case NVN_TEXTURE_TARGET_RECTANGLE:
        case NVN_TEXTURE_TARGET_CUBEMAP:
        case NVN_TEXTURE_TARGET_BUFFER: return false;
        default: note_unknown(); return false;
    }
}

bool target_is_cube(int nvn_texture_target) {
    switch (nvn_texture_target) {
        case NVN_TEXTURE_TARGET_CUBEMAP:
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return true;
        case NVN_TEXTURE_TARGET_1D:
        case NVN_TEXTURE_TARGET_2D:
        case NVN_TEXTURE_TARGET_3D:
        case NVN_TEXTURE_TARGET_1D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
        case NVN_TEXTURE_TARGET_RECTANGLE:
        case NVN_TEXTURE_TARGET_BUFFER: return false;
        default: note_unknown(); return false;
    }
}

bool target_is_multisample(int nvn_texture_target) {
    switch (nvn_texture_target) {
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE:
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return true;
        case NVN_TEXTURE_TARGET_1D:
        case NVN_TEXTURE_TARGET_2D:
        case NVN_TEXTURE_TARGET_3D:
        case NVN_TEXTURE_TARGET_1D_ARRAY:
        case NVN_TEXTURE_TARGET_2D_ARRAY:
        case NVN_TEXTURE_TARGET_RECTANGLE:
        case NVN_TEXTURE_TARGET_CUBEMAP:
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
        case NVN_TEXTURE_TARGET_BUFFER: return false;
        default: note_unknown(); return false;
    }
}

}  // namespace nxrt::nvn
