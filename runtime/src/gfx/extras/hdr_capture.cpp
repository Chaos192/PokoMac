// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR capture decoder (hdr_capture.cpp): pixel unpacking (PQ 10-bit words, IEEE half floats, 8-bit sRGB), the previews, the
// statistics and the PFM writer behind hdr_capture.h. Uses the transfer functions and primaries from hdr_math.h.
#include "extras/hdr_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "extras/hdr_math.h"

namespace nxrt::extras {

namespace {

constexpr float kScrgbNitsPerUnit = 80.0f;
constexpr float kHalfMax = 65504.0f;

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16_le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// IEEE binary16 -> float, by hand. Zero and subnormals are exact; NaN -> 0; +-Inf -> +-65504 (the largest finite half).
float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    float mag;
    if (exp == 0x1Fu) {
        mag = (man != 0) ? 0.0f : kHalfMax;
    } else if (exp == 0) {
        mag = std::ldexp(static_cast<float>(man), -24);                                   // subnormal: man * 2^-24
    } else {
        mag = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);  // normal: (1.man) * 2^(exp-15)
    }
    return sign ? -mag : mag;
}

// Unpacks one raw pixel to scene-linear BT.709, 1.0 = paper white (negatives are kept).
void decode_pixel(CaptureFormat fmt, const uint8_t* p, float paper_white_nits, float out[3]) {
    switch (fmt) {
        case CaptureFormat::A2B10G10R10:
        case CaptureFormat::A2R10G10B10: {
            const uint32_t word = read_u32_le(p);
            uint32_t code[3];                                    // BT.2020 R, G, B
            if (fmt == CaptureFormat::A2B10G10R10) {
                code[0] = word & 0x3FFu;
                code[1] = (word >> 10) & 0x3FFu;
                code[2] = (word >> 20) & 0x3FFu;
            } else {
                code[2] = word & 0x3FFu;
                code[1] = (word >> 10) & 0x3FFu;
                code[0] = (word >> 20) & 0x3FFu;
            }
            float bt2020[3];
            for (int c = 0; c < 3; ++c) {
                bt2020[c] = pq_eotf(static_cast<float>(code[c]) / 1023.0f) / paper_white_nits;
            }
            bt2020_to_bt709(bt2020, out);
            return;
        }
        case CaptureFormat::Rgba16F: {
            const float scale = kScrgbNitsPerUnit / paper_white_nits;   // scRGB 1.0 = 80 nits
            for (int c = 0; c < 3; ++c) out[c] = half_to_float(read_u16_le(p + 2 * c)) * scale;
            return;
        }
        case CaptureFormat::Rgba8:
            for (int c = 0; c < 3; ++c) out[c] = srgb_eotf(static_cast<float>(p[c]) / 255.0f);
            return;
    }
}

uint8_t to_u8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); }

// False-colour ramp of the maximum channel v (paper-white units, already >= 0).
void false_colour(float v, uint8_t rgb[3]) {
    float r, g, b;
    if (v <= 0.25f) {
        r = g = b = v * 4.0f * 64.0f;
    } else if (v <= 1.0f) {
        r = g = b = 64.0f + (v - 0.25f) / 0.75f * (255.0f - 64.0f);
    } else if (v <= 2.0f) {
        const float t = v - 1.0f;
        r = 255.0f;
        g = 255.0f + t * (160.0f - 255.0f);
        b = 0.0f;
    } else if (v <= 4.0f) {
        const float t = (v - 2.0f) / 2.0f;
        r = 255.0f;
        g = 160.0f + t * (0.0f - 160.0f);
        b = 0.0f;
    } else {
        r = 255.0f;
        g = 0.0f;
        b = 255.0f;
    }
    rgb[0] = to_u8(r);
    rgb[1] = to_u8(g);
    rgb[2] = to_u8(b);
}

}  // namespace

bool decode_hdr_capture(CaptureFormat fmt, const uint8_t* raw, uint32_t w, uint32_t h, float paper_white_nits,
                        std::vector<float>* linear_rgb, std::vector<uint8_t>* sdr_rgba, std::vector<uint8_t>* map_rgba,
                        HdrCaptureStats* stats) {
    if (raw == nullptr || w == 0 || h == 0 || !(paper_white_nits > 0.0f)) return false;

    const size_t count = static_cast<size_t>(w) * h;
    const size_t bpp = capture_bytes_per_pixel(fmt);
    std::vector<float> lin(count * 3);
    std::vector<uint8_t> sdr, map;
    if (sdr_rgba) sdr.resize(count * 4);
    if (map_rgba) map.resize(count * 4);

    double sum = 0.0, max_v = 0.0;
    uint64_t above1 = 0, above2 = 0, above4 = 0;

    for (size_t i = 0; i < count; ++i) {
        float* px = &lin[i * 3];
        decode_pixel(fmt, raw + i * bpp, paper_white_nits, px);

        // Previews: negatives are 0.
        const float clamped_max = std::max({px[0], px[1], px[2], 0.0f});
        if (sdr_rgba) {
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(px[c], 0.0f, 1.0f);
                sdr[i * 4 + c] = to_u8(srgb_oetf(v) * 255.0f);
            }
            sdr[i * 4 + 3] = 255;
        }
        if (map_rgba) {
            uint8_t rgb[3];
            false_colour(clamped_max, rgb);
            map[i * 4 + 0] = rgb[0];
            map[i * 4 + 1] = rgb[1];
            map[i * 4 + 2] = rgb[2];
            map[i * 4 + 3] = 255;
        }

        // Statistics in paper-white units.
        const double v = clamped_max;
        sum += v;
        max_v = std::max(max_v, v);
        if (v > 1.0) ++above1;
        if (v > 2.0) ++above2;
        if (v > 4.0) ++above4;
    }

    if (linear_rgb) *linear_rgb = std::move(lin);
    if (sdr_rgba) *sdr_rgba = std::move(sdr);
    if (map_rgba) *map_rgba = std::move(map);
    if (stats) {
        const double paper = paper_white_nits;
        const double n = static_cast<double>(count);
        stats->max_nits = max_v * paper;
        stats->mean_nits = (sum / n) * paper;
        stats->frac_above_paper = static_cast<double>(above1) / n;
        stats->frac_above_2x = static_cast<double>(above2) / n;
        stats->frac_above_4x = static_cast<double>(above4) / n;
        stats->width = w;
        stats->height = h;
    }
    return true;
}

uint32_t capture_bytes_per_pixel(CaptureFormat fmt) {
    return fmt == CaptureFormat::Rgba16F ? 8u : 4u;
}

bool write_pfm(const std::string& path, const std::vector<float>& linear_rgb, uint32_t w, uint32_t h, float scale) {
    const size_t row_floats = static_cast<size_t>(w) * 3;
    if (w == 0 || h == 0 || linear_rgb.size() != row_floats * h) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;

    char header[64];
    const int hl = std::snprintf(header, sizeof header, "PF\n%u %u\n-1.0\n", static_cast<unsigned>(w), static_cast<unsigned>(h));
    bool ok = hl > 0 && std::fwrite(header, 1, static_cast<size_t>(hl), f) == static_cast<size_t>(hl);

    std::vector<uint8_t> row(row_floats * 4);
    for (uint32_t y = h; ok && y-- > 0;) {                          // bottom row of the image first
        for (size_t i = 0; i < row_floats; ++i) {
            const float v = linear_rgb[static_cast<size_t>(y) * row_floats + i] * scale;
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            row[i * 4 + 0] = static_cast<uint8_t>(bits & 0xFFu);   // little endian, byte by byte
            row[i * 4 + 1] = static_cast<uint8_t>((bits >> 8) & 0xFFu);
            row[i * 4 + 2] = static_cast<uint8_t>((bits >> 16) & 0xFFu);
            row[i * 4 + 3] = static_cast<uint8_t>((bits >> 24) & 0xFFu);
        }
        ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    }

    if (std::fclose(f) != 0) ok = false;
    return ok;
}

std::string capture_stats_line(const HdrCaptureStats& s) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "max_nits=%.1f mean_nits=%.1f above_paper=%.2f%% above_2x=%.2f%% above_4x=%.2f%%",
                  s.max_nits, s.mean_nits, s.frac_above_paper * 100.0, s.frac_above_2x * 100.0, s.frac_above_4x * 100.0);
    return buf;
}

}  // namespace nxrt::extras
