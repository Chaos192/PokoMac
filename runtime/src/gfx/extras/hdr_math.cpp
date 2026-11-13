// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR and colour math (hdr_math.cpp): the arithmetic behind hdr_math.h. Pure functions, plain float math.
#include "extras/hdr_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nxrt::extras {

namespace {

// SMPTE ST 2084 constants.
constexpr float kPqM1 = 2610.0f / 16384.0f;             // 0.1593017578125
constexpr float kPqM2 = 2523.0f / 4096.0f * 128.0f;     // 78.84375
constexpr float kPqC1 = 3424.0f / 4096.0f;              // 0.8359375
constexpr float kPqC2 = 2413.0f / 4096.0f * 32.0f;      // 18.8515625
constexpr float kPqC3 = 2392.0f / 4096.0f * 32.0f;      // 18.6875
constexpr float kPqPeakNits = 10000.0f;

constexpr float kDefaultPaperWhite = 203.0f;
constexpr float kScrgbNitsPerUnit = 80.0f;

// BT.2087 BT.709 -> BT.2020, and its inverse (hard-coded to 6 digits).
constexpr float kTo2020[3][3] = {
    {0.6274f, 0.3293f, 0.0433f},
    {0.0691f, 0.9195f, 0.0114f},
    {0.0164f, 0.0880f, 0.8956f},
};
constexpr float kTo709[3][3] = {
    {1.660491f, -0.587641f, -0.072850f},
    {-0.124550f, 1.132900f, -0.008349f},
    {-0.018151f, -0.100579f, 1.118730f},
};

void apply3x3(const float m[3][3], const float in[3], float out[3]) {
    // read all inputs first so out may alias in
    const float x = in[0], y = in[1], z = in[2];
    out[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    out[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    out[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z;
}

float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// The shoulder at x (x >= t2 branch of the curve).
float shoulder(float A, float p, float q, float B, float x) {
    const float vv = p * x + q;
    return B + A * vv / (vv + 1.0f);
}

}  // namespace

// ---- transfer functions ----------------------------------------------------------------------------------------------------------

float srgb_oetf(float linear) {
    const float x = std::max(linear, 0.0f);
    if (x <= 0.0031308f) return 12.92f * x;
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

float srgb_eotf(float encoded) {
    const float e = std::max(encoded, 0.0f);
    if (e <= 0.04045f) return e / 12.92f;
    return std::pow((e + 0.055f) / 1.055f, 2.4f);
}

float pq_oetf(float nits) {
    const float y = clampf(nits, 0.0f, kPqPeakNits) / kPqPeakNits;
    const float yp = std::pow(y, kPqM1);
    return std::pow((kPqC1 + kPqC2 * yp) / (1.0f + kPqC3 * yp), kPqM2);
}

float pq_eotf(float signal) {
    const float n = clampf(signal, 0.0f, 1.0f);
    const float np = std::pow(n, 1.0f / kPqM2);
    const float num = std::max(np - kPqC1, 0.0f);
    const float den = kPqC2 - kPqC3 * np;
    return kPqPeakNits * std::pow(num / den, 1.0f / kPqM1);
}

// ---- colour primaries ------------------------------------------------------------------------------------------------------------

void bt709_to_bt2020(const float in[3], float out[3]) { apply3x3(kTo2020, in, out); }

void bt2020_to_bt709(const float in[3], float out[3]) { apply3x3(kTo709, in, out); }

// ---- tone curve ------------------------------------------------------------------------------------------------------------------

float tonemap_curve(const TonemapEval& e, float x) {
    const float* v = e.v;
    if (x >= v[11]) return shoulder(v[12], v[13], v[14], v[15], x);
    if (x >= v[10]) return v[8] * x + v[9];
    return v[6] * x + x * x * (v[4] * x + v[5]) + v[7];
}

bool extend_shoulder(TonemapEval* e, float peak_ratio) {
    if (e == nullptr) return false;
    float* v = e->v;
    const float A = v[12], p = v[13], q = v[14], B = v[15], t2 = v[11];

    const float v0 = p * t2 + q;
    const float y0 = B + A * v0 / (v0 + 1.0f);
    const float s = A * p / ((v0 + 1.0f) * (v0 + 1.0f));
    if (peak_ratio <= y0 + 1e-4f) return false;

    const float new_A = peak_ratio - y0;
    const float new_B = y0;
    const float new_p = s / (peak_ratio - y0);
    const float new_q = -new_p * t2;
    v[12] = new_A;
    v[13] = new_p;
    v[14] = new_q;
    v[15] = new_B;
    return true;
}

float peak_ratio(float peak_nits, float paper_white_nits, float boost) {
    const float paper = paper_white_nits <= 0.0f ? kDefaultPaperWhite : paper_white_nits;
    const float b = clampf(boost, 0.0f, 2.0f);
    return std::max(1.0f + b * (peak_nits / paper - 1.0f), 1.0f);
}

// ---- HDR output encodes ----------------------------------------------------------------------------------------------------------

void encode_hdr10(const float lin709[3], const HdrOut& o, float out[3]) {
    const float clamped[3] = {std::max(lin709[0], 0.0f), std::max(lin709[1], 0.0f), std::max(lin709[2], 0.0f)};
    float bt2020[3];
    bt709_to_bt2020(clamped, bt2020);
    for (int c = 0; c < 3; ++c) {
        out[c] = pq_oetf(std::min(bt2020[c] * o.paper_white_nits, o.peak_nits));
    }
}

void encode_scrgb(const float lin709[3], const HdrOut& o, float out[3]) {
    const float cap = o.peak_nits / kScrgbNitsPerUnit;
    for (int c = 0; c < 3; ++c) {
        out[c] = std::min(lin709[c] * o.paper_white_nits / kScrgbNitsPerUnit, cap);
    }
}

}  // namespace nxrt::extras
