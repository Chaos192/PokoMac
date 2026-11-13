// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR and colour math (hdr_math.h): pure functions with no state. Covers the sRGB and SMPTE ST 2084 (PQ) transfer functions,
// the BT.709 <-> BT.2020 primaries conversion, the game's tone-map curve (cubic toe, linear middle, rational shoulder) with the
// shoulder extension that moves its asymptote to a display's peak, and the HDR10 and scRGB encodes of a scene-linear colour
// where 1.0 is paper white. Plain float math; the only includes are the standard headers the .cpp needs.
#pragma once

namespace nxrt::extras {

// ---- transfer functions (scalar) -------------------------------------------------------------------------------------------------

// IEC 61966-2-1 sRGB encode. Input clamped to >= 0; values above 1 continue by the same formula (no upper clamp).
float srgb_oetf(float linear);
// Inverse of srgb_oetf, same conventions.
float srgb_eotf(float encoded);

// SMPTE ST 2084 inverse EOTF: absolute luminance in cd/m2 (clamped to 0..10000) -> signal 0..1.
float pq_oetf(float nits);
// SMPTE ST 2084 EOTF: signal (clamped to 0..1) -> cd/m2.
float pq_eotf(float signal);

// ---- colour primaries (linear light; row-major matrices applied to column vectors) ------------------------------------------------

// ITU-R BT.2087 BT.709 -> BT.2020.
void bt709_to_bt2020(const float in[3], float out[3]);
// The matrix inverse of the above (BT.2020 -> BT.709).
void bt2020_to_bt709(const float in[3], float out[3]);

// ---- the game's tone curve --------------------------------------------------------------------------------------------------------

// The tone-map fragment shader's 20 "eval params" (5 vec4 rows). Index into v[20]:
//   v[0]   exposure scale applied to the scene colour before the curve
//   v[1]   inverse exposure (unused by the curve)
//   v[4..7] cubic toe, x < t1:        y = c*x + x*x*(a*x + b) + d   (a = v[4], b = v[5], c = v[6], d = v[7])
//   v[8..9] linear, t1 <= x < t2:     y = m*x + k                   (m = v[8], k = v[9])
//   v[10]  t1, v[11] t2 (thresholds)
//   v[12..15] shoulder, x >= t2:      vv = p*x + q; y = B + A*vv/(vv+1)   (A = v[12], p = v[13], q = v[14], B = v[15])
//   v[16..19] 3D grading lookup coordinate = y*scale + bias: lut0 bias v[16], lut0 scale v[17], lut1 bias v[18], lut1 scale v[19]
struct TonemapEval {
    float v[20];
};

// The per-channel curve for an input x that is ALREADY multiplied by the exposure. Segment chosen by the thresholds:
// x >= t2 -> shoulder, else x >= t1 -> linear, else cubic.
float tonemap_curve(const TonemapEval& e, float x);

// Extends the shoulder so its asymptote is peak_ratio (> 1) instead of about A+B, keeping the value and the slope at x = t2.
// Returns false and leaves *e unchanged when peak_ratio <= (the value at t2) + 1e-4.
bool extend_shoulder(TonemapEval* e, float peak_ratio);

// Peak ratio from display numbers: R = 1 + boost*(peak_nits/paper_white_nits - 1), never below 1.
// paper_white_nits <= 0 is treated as 203; boost is clamped to 0..2.
float peak_ratio(float peak_nits, float paper_white_nits, float boost);

// ---- HDR output encodes of a scene-linear colour where 1.0 = paper white ----------------------------------------------------------

struct HdrOut {
    float paper_white_nits = 203.0f;
    float peak_nits = 1000.0f;
};

// HDR10: out_c = pq_oetf(min(bt2020_c * paper_white_nits, peak_nits)), where bt2020 = bt709_to_bt2020(max(lin, 0)).
void encode_hdr10(const float lin709[3], const HdrOut& o, float out[3]);
// scRGB: out_c = lin_c * paper_white_nits / 80 (1.0 = 80 nits, BT.709 primaries). No lower clamp; the result is capped at
// peak_nits / 80 per channel.
void encode_scrgb(const float lin709[3], const HdrOut& o, float out[3]);

}  // namespace nxrt::extras
