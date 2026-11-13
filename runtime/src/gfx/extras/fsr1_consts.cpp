// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of the FidelityFX SDK v1.1.4 (MIT) CPU constant code in third_party/ffx_fsr1/fsr1/ffx_fsr1.h. The arithmetic keeps the
// header's operand order so the float results are bit-identical; float<->uint32 casts go through memcpy.
#include "fsr1_consts.h"

#include <cmath>
#include <cstring>

namespace nxrt::extras {
namespace {

uint32_t as_uint(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

float reciprocal(float x) { return 1.0f / x; }   // ffxReciprocal

// ffxF32ToF16 in the SDK (ffx_core_cpu.h) is a table lookup that truncates toward zero, saturates to the largest finite half
// (0x7bff) and flushes values below 2^-24 to zero. This bit-level version does the same; checked against the SDK table over
// every positive finite float, so it is not round-to-nearest.
uint16_t f32_to_f16(float f) {
    uint32_t x = as_uint(f);
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t exp = (x >> 23) & 0xffu;
    uint32_t man = x & 0x7fffffu;
    int32_t e = static_cast<int32_t>(exp) - 127 + 15;
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7bffu);   // saturate, as the SDK table does
    if (e <= 0) {                                                // half subnormal, or flushed to zero
        if (e < -10) return static_cast<uint16_t>(sign);
        man |= 0x800000u;
        return static_cast<uint16_t>(sign | (man >> static_cast<uint32_t>(14 - e)));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | (man >> 13));
}

uint32_t pack_half2x16(float x, float y) { return static_cast<uint32_t>(f32_to_f16(x)) + (static_cast<uint32_t>(f32_to_f16(y)) << 16); }

}  // namespace

void fsr1_easu_consts(uint32_t con0[4], uint32_t con1[4], uint32_t con2[4], uint32_t con3[4], float in_view_w, float in_view_h,
                      float in_tex_w, float in_tex_h, float out_w, float out_h) {
    // Output integer position to a pixel position in viewport.
    con0[0] = as_uint(in_view_w * reciprocal(out_w));
    con0[1] = as_uint(in_view_h * reciprocal(out_h));
    con0[2] = as_uint(0.5f * in_view_w * reciprocal(out_w) - 0.5f);
    con0[3] = as_uint(0.5f * in_view_h * reciprocal(out_h) - 0.5f);

    // Viewport pixel position to normalized image space.
    con1[0] = as_uint(reciprocal(in_tex_w));
    con1[1] = as_uint(reciprocal(in_tex_h));
    // Centers of gather4, first offset from upper-left of 'F'.
    con1[2] = as_uint(1.0f * reciprocal(in_tex_w));
    con1[3] = as_uint(-1.0f * reciprocal(in_tex_h));

    // These are from (0) instead of 'F'.
    con2[0] = as_uint(-1.0f * reciprocal(in_tex_w));
    con2[1] = as_uint(2.0f * reciprocal(in_tex_h));
    con2[2] = as_uint(1.0f * reciprocal(in_tex_w));
    con2[3] = as_uint(2.0f * reciprocal(in_tex_h));
    con3[0] = as_uint(0.0f * reciprocal(in_tex_w));
    con3[1] = as_uint(4.0f * reciprocal(in_tex_h));
    con3[2] = con3[3] = 0;
}

void fsr1_rcas_consts(uint32_t con[4], float sharpness_stops) {
    // Transform from stops to linear value.
    const float sharpness = std::exp2(-sharpness_stops);
    con[0] = as_uint(sharpness);
    con[1] = pack_half2x16(sharpness, sharpness);
    con[2] = 0;
    con[3] = 0;
}

}  // namespace nxrt::extras
