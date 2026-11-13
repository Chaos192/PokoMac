// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNblendState, NVNcolorState and NVNchannelMaskState: plain data in the guest object (8, 4 and 4 bytes). The bit layout
// follows the one smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0) uses for the same objects (checked there against a real game).
// smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0), layout/defaults checked, no code copied.
//
// NVNblendState (8 bytes), one blend state per object:
//   byte 0  bits 0..2 render target, bits 3..4 advanced overlap, bit 5 advanced premultiplied src, bit 6 advanced normalized dst
//   byte 1  blend func src RGB, byte 2 dst RGB, byte 3 src alpha, byte 4 dst alpha (NVNblendFunc)
//   byte 5  blend equation RGB, byte 6 blend equation alpha (NVNblendEquation)
//   byte 7  advanced blend mode (NVNblendAdvancedMode)
// NVNcolorState (4 bytes):
//   byte 0  blend enable mask, bit n set = render target n blends
//   byte 1  logic op (NVNlogicOp), byte 2 alpha test func (NVNalphaFunc), byte 3 unused
// NVNchannelMaskState (4 bytes): one 32-bit word, 4 bits per render target (bit 0 R, 1 G, 2 B, 3 A), target 0 in the low bits.
//
// Defaults (NVN defaults, as smo sets them): target 0, funcs ONE/ZERO for colour and alpha, equations ADD, advanced mode
// NONE, overlap UNCORRELATED, premultiplied src TRUE, normalized dst FALSE. Colour: blending off for every target, logic op
// COPY, alpha test ALWAYS. Channel mask: all four channels on for every target.
// Render targets outside 0..7 are ignored by the setters and read as 0 / FALSE by the getters.
#include <cstring>

#include "nvn_impl.h"

namespace nxrt::nvn {
namespace {

struct BlendLayout {
    uint8_t flags;      // target bits 0..2, overlap bits 3..4, premultiplied src bit 5, normalized dst bit 6
    uint8_t func[4];    // src RGB, dst RGB, src alpha, dst alpha
    uint8_t eq[2];      // equation RGB, equation alpha
    uint8_t adv_mode;
};
static_assert(sizeof(BlendLayout) <= sizeof(NVNblendState), "blend state layout fits the NVN object");

struct ColorLayout {
    uint8_t blend_enable;   // bit n: render target n blends
    uint8_t logic_op;
    uint8_t alpha_test;
    uint8_t reserved;
};
static_assert(sizeof(ColorLayout) <= sizeof(NVNcolorState), "colour state layout fits the NVN object");
static_assert(sizeof(uint32_t) <= sizeof(NVNchannelMaskState), "channel mask layout fits the NVN object");

constexpr int kTargets = 8;
constexpr uint8_t kTargetBits = 0x07;
constexpr uint8_t kOverlapBits = 0x18;   // bits 3..4
constexpr uint8_t kPremultBit = 0x20;    // bit 5
constexpr uint8_t kNormDstBit = 0x40;    // bit 6

inline bool target_ok(int target) { return target >= 0 && target < kTargets; }

inline BlendLayout* blend(NVNblendState* s) { return reinterpret_cast<BlendLayout*>(s); }
inline const BlendLayout* blend(const NVNblendState* s) { return reinterpret_cast<const BlendLayout*>(s); }
inline ColorLayout* color(NVNcolorState* s) { return reinterpret_cast<ColorLayout*>(s); }
inline const ColorLayout* color(const NVNcolorState* s) { return reinterpret_cast<const ColorLayout*>(s); }

inline uint32_t rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }

// ---- NVNblendState -----------------------------------------------------------------------------------------------------------
void nvnBlendStateSetDefaults(NVNblendState* s) {
    BlendLayout d{};
    d.flags = kPremultBit;
    d.func[0] = static_cast<uint8_t>(NVN_BLEND_FUNC_ONE);    // src RGB
    d.func[1] = static_cast<uint8_t>(NVN_BLEND_FUNC_ZERO);   // dst RGB
    d.func[2] = static_cast<uint8_t>(NVN_BLEND_FUNC_ONE);    // src alpha
    d.func[3] = static_cast<uint8_t>(NVN_BLEND_FUNC_ZERO);   // dst alpha
    d.eq[0] = static_cast<uint8_t>(NVN_BLEND_EQUATION_ADD);
    d.eq[1] = static_cast<uint8_t>(NVN_BLEND_EQUATION_ADD);
    d.adv_mode = static_cast<uint8_t>(NVN_BLEND_ADVANCED_MODE_NONE);
    *blend(s) = d;
}
void nvnBlendStateSetBlendTarget(NVNblendState* s, int target) {
    if (!target_ok(target)) return;
    BlendLayout* b = blend(s);
    b->flags = static_cast<uint8_t>((b->flags & ~kTargetBits) | target);
}
void nvnBlendStateSetBlendFunc(NVNblendState* s, NVNblendFunc src_rgb, NVNblendFunc dst_rgb, NVNblendFunc src_a,
                               NVNblendFunc dst_a) {
    BlendLayout* b = blend(s);
    b->func[0] = static_cast<uint8_t>(src_rgb);
    b->func[1] = static_cast<uint8_t>(dst_rgb);
    b->func[2] = static_cast<uint8_t>(src_a);
    b->func[3] = static_cast<uint8_t>(dst_a);
}
void nvnBlendStateSetBlendEquation(NVNblendState* s, NVNblendEquation rgb, NVNblendEquation alpha) {
    BlendLayout* b = blend(s);
    b->eq[0] = static_cast<uint8_t>(rgb);
    b->eq[1] = static_cast<uint8_t>(alpha);
}
void nvnBlendStateSetAdvancedMode(NVNblendState* s, NVNblendAdvancedMode mode) {
    blend(s)->adv_mode = static_cast<uint8_t>(mode);
}
void nvnBlendStateSetAdvancedOverlap(NVNblendState* s, NVNblendAdvancedOverlap overlap) {
    BlendLayout* b = blend(s);
    b->flags = static_cast<uint8_t>((b->flags & ~kOverlapBits) | ((static_cast<uint32_t>(overlap) & 3u) << 3));
}
void nvnBlendStateSetAdvancedPremultipliedSrc(NVNblendState* s, NVNboolean v) {
    BlendLayout* b = blend(s);
    b->flags = static_cast<uint8_t>(v != 0 ? (b->flags | kPremultBit) : (b->flags & ~kPremultBit));
}
void nvnBlendStateSetAdvancedNormalizedDst(NVNblendState* s, NVNboolean v) {
    BlendLayout* b = blend(s);
    b->flags = static_cast<uint8_t>(v != 0 ? (b->flags | kNormDstBit) : (b->flags & ~kNormDstBit));
}
int nvnBlendStateGetBlendTarget(const NVNblendState* s) { return blend(s)->flags & kTargetBits; }
void nvnBlendStateGetBlendFunc(const NVNblendState* s, NVNblendFunc* src_rgb, NVNblendFunc* dst_rgb, NVNblendFunc* src_a,
                               NVNblendFunc* dst_a) {
    const BlendLayout* b = blend(s);
    if (src_rgb) *src_rgb = static_cast<NVNblendFunc>(b->func[0]);
    if (dst_rgb) *dst_rgb = static_cast<NVNblendFunc>(b->func[1]);
    if (src_a) *src_a = static_cast<NVNblendFunc>(b->func[2]);
    if (dst_a) *dst_a = static_cast<NVNblendFunc>(b->func[3]);
}
void nvnBlendStateGetBlendEquation(const NVNblendState* s, NVNblendEquation* rgb, NVNblendEquation* alpha) {
    const BlendLayout* b = blend(s);
    if (rgb) *rgb = static_cast<NVNblendEquation>(b->eq[0]);
    if (alpha) *alpha = static_cast<NVNblendEquation>(b->eq[1]);
}
NVNblendAdvancedMode nvnBlendStateGetAdvancedMode(const NVNblendState* s) {
    return static_cast<NVNblendAdvancedMode>(blend(s)->adv_mode);
}
NVNblendAdvancedOverlap nvnBlendStateGetAdvancedOverlap(const NVNblendState* s) {
    return static_cast<NVNblendAdvancedOverlap>((blend(s)->flags & kOverlapBits) >> 3);
}
NVNboolean nvnBlendStateGetAdvancedPremultipliedSrc(const NVNblendState* s) {
    return static_cast<NVNboolean>((blend(s)->flags & kPremultBit) ? 1 : 0);
}
NVNboolean nvnBlendStateGetAdvancedNormalizedDst(const NVNblendState* s) {
    return static_cast<NVNboolean>((blend(s)->flags & kNormDstBit) ? 1 : 0);
}

// ---- NVNcolorState -----------------------------------------------------------------------------------------------------------
void nvnColorStateSetDefaults(NVNcolorState* s) {
    ColorLayout d{};
    d.logic_op = static_cast<uint8_t>(NVN_LOGIC_OP_COPY);
    d.alpha_test = static_cast<uint8_t>(NVN_ALPHA_FUNC_ALWAYS);
    *color(s) = d;
}
void nvnColorStateSetBlendEnable(NVNcolorState* s, int target, NVNboolean enable) {
    if (!target_ok(target)) return;
    ColorLayout* c = color(s);
    const uint8_t bit = static_cast<uint8_t>(1u << target);
    c->blend_enable = static_cast<uint8_t>(enable != 0 ? (c->blend_enable | bit) : (c->blend_enable & ~bit));
}
void nvnColorStateSetLogicOp(NVNcolorState* s, NVNlogicOp op) { color(s)->logic_op = static_cast<uint8_t>(op); }
void nvnColorStateSetAlphaTest(NVNcolorState* s, NVNalphaFunc func) { color(s)->alpha_test = static_cast<uint8_t>(func); }
NVNboolean nvnColorStateGetBlendEnable(const NVNcolorState* s, int target) {
    if (!target_ok(target)) return static_cast<NVNboolean>(0);
    return static_cast<NVNboolean>((color(s)->blend_enable >> target) & 1);
}
NVNlogicOp nvnColorStateGetLogicOp(const NVNcolorState* s) { return static_cast<NVNlogicOp>(color(s)->logic_op); }
NVNalphaFunc nvnColorStateGetAlphaTest(const NVNcolorState* s) { return static_cast<NVNalphaFunc>(color(s)->alpha_test); }

// ---- NVNchannelMaskState -----------------------------------------------------------------------------------------------------
void nvnChannelMaskStateSetDefaults(NVNchannelMaskState* s) { wr32(s, 0xffffffffu); }
void nvnChannelMaskStateSetChannelMask(NVNchannelMaskState* s, int target, NVNboolean r, NVNboolean g, NVNboolean b,
                                       NVNboolean a) {
    if (!target_ok(target)) return;
    const uint32_t bits = (r != 0 ? 1u : 0u) | (g != 0 ? 2u : 0u) | (b != 0 ? 4u : 0u) | (a != 0 ? 8u : 0u);
    const uint32_t shift = static_cast<uint32_t>(target) * 4;
    wr32(s, (rd32(s) & ~(0xfu << shift)) | (bits << shift));
}
void nvnChannelMaskStateGetChannelMask(const NVNchannelMaskState* s, int target, NVNboolean* r, NVNboolean* g, NVNboolean* b,
                                       NVNboolean* a) {
    const uint32_t m = target_ok(target) ? (rd32(s) >> (static_cast<uint32_t>(target) * 4)) & 0xfu : 0u;
    if (r) *r = static_cast<NVNboolean>(m & 1);
    if (g) *g = static_cast<NVNboolean>((m >> 1) & 1);
    if (b) *b = static_cast<NVNboolean>((m >> 2) & 1);
    if (a) *a = static_cast<NVNboolean>((m >> 3) & 1);
}

}  // namespace

// ---- decode() ----------------------------------------------------------------------------------------------------------------
BlendInfo decode(const NVNblendState* s) {
    const BlendLayout* b = blend(s);
    BlendInfo i;
    i.target = b->flags & kTargetBits;
    i.src_rgb = b->func[0];
    i.dst_rgb = b->func[1];
    i.src_a = b->func[2];
    i.dst_a = b->func[3];
    i.eq_rgb = b->eq[0];
    i.eq_a = b->eq[1];
    i.advanced_mode = b->adv_mode;
    i.advanced_overlap = (b->flags & kOverlapBits) >> 3;
    i.advanced_premultiplied_src = (b->flags & kPremultBit) != 0;
    i.advanced_normalized_dst = (b->flags & kNormDstBit) != 0;
    return i;
}

ColorInfo decode(const NVNcolorState* s) {
    const ColorLayout* c = color(s);
    ColorInfo i;
    i.blend_enable_mask = c->blend_enable;
    i.logic_op = c->logic_op;
    i.alpha_test = c->alpha_test;
    return i;
}

ChannelMaskInfo decode(const NVNchannelMaskState* s) {
    const uint32_t w = rd32(s);
    ChannelMaskInfo i;
    for (int t = 0; t < kTargets; t++) i.mask[t] = static_cast<uint8_t>((w >> (t * 4)) & 0xfu);
    return i;
}

void register_api_blend_color() {
    NVN_REG(nvnBlendStateSetDefaults);
    NVN_REG(nvnBlendStateSetBlendTarget);
    NVN_REG(nvnBlendStateSetBlendFunc);
    NVN_REG(nvnBlendStateSetBlendEquation);
    NVN_REG(nvnBlendStateSetAdvancedMode);
    NVN_REG(nvnBlendStateSetAdvancedOverlap);
    NVN_REG(nvnBlendStateSetAdvancedPremultipliedSrc);
    NVN_REG(nvnBlendStateSetAdvancedNormalizedDst);
    NVN_REG(nvnBlendStateGetBlendTarget);
    NVN_REG(nvnBlendStateGetBlendFunc);
    NVN_REG(nvnBlendStateGetBlendEquation);
    NVN_REG(nvnBlendStateGetAdvancedMode);
    NVN_REG(nvnBlendStateGetAdvancedOverlap);
    NVN_REG(nvnBlendStateGetAdvancedPremultipliedSrc);
    NVN_REG(nvnBlendStateGetAdvancedNormalizedDst);
    NVN_REG(nvnColorStateSetDefaults);
    NVN_REG(nvnColorStateSetBlendEnable);
    NVN_REG(nvnColorStateSetLogicOp);
    NVN_REG(nvnColorStateSetAlphaTest);
    NVN_REG(nvnColorStateGetBlendEnable);
    NVN_REG(nvnColorStateGetLogicOp);
    NVN_REG(nvnColorStateGetAlphaTest);
    NVN_REG(nvnChannelMaskStateSetDefaults);
    NVN_REG(nvnChannelMaskStateSetChannelMask);
    NVN_REG(nvnChannelMaskStateGetChannelMask);
}

}  // namespace nxrt::nvn
