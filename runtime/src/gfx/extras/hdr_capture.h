// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR capture decoder (hdr_capture.h): turns a read-back frame (HDR10 PQ, scRGB half floats or 8-bit sRGB) into scene-linear
// floats, 8-bit previews (SDR and false colour) and brightness statistics, and writes linear data out as a PFM file.
// No Vulkan types here: the caller describes the pixel layout with CaptureFormat.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::extras {

enum class CaptureFormat : uint8_t {
    A2B10G10R10,   // 32-bit word per pixel, little endian: bits 0-9 = R, 10-19 = G, 20-29 = B, 30-31 = A (VK_FORMAT_A2B10G10R10_UNORM_PACK32), PQ-encoded BT.2020 (HDR10)
    A2R10G10B10,   // bits 0-9 = B, 10-19 = G, 20-29 = R, 30-31 = A, PQ-encoded BT.2020
    Rgba16F,       // 4 IEEE half floats per pixel (R,G,B,A), scRGB: linear BT.709, 1.0 = 80 nits
    Rgba8,         // 4 bytes R,G,B,A, SDR sRGB-encoded (decoded with srgb_eotf; 1.0 = paper white)
};

struct HdrCaptureStats {
    double max_nits = 0;            // brightest channel value found, in nits
    double mean_nits = 0;           // mean of the per-pixel maximum channel, in nits
    double frac_above_paper = 0;    // fraction of pixels whose maximum channel is above paper white
    double frac_above_2x = 0;       // ... above 2 x paper white
    double frac_above_4x = 0;       // ... above 4 x paper white
    uint32_t width = 0, height = 0;
};

// Decodes raw pixels (tightly packed rows, no padding) into:
//  - linear_rgb: scene-linear BT.709, 3 floats per pixel, 1.0 = paper_white_nits (may exceed 1); may be null
//  - sdr_rgba:   8-bit RGBA preview: the linear value clamped to 0..1, sRGB-encoded, alpha 255; may be null
//  - map_rgba:   8-bit RGBA false-colour map of the pixel's maximum channel in paper-white units v: v<=0.25 dark grey ramp (v*4*64),
//                0.25<v<=1 grey ramp 64..255, 1<v<=2 yellow ramp (255,255,0)->(255,160,0), 2<v<=4 orange->red ramp
//                (255,160,0)->(255,0,0), v>4 magenta (255,0,255); alpha 255; may be null
//  - stats:      may be null
// Returns false when raw is null, w or h is 0 or paper_white_nits <= 0. Negative values decode as 0 for the previews.
bool decode_hdr_capture(CaptureFormat fmt, const uint8_t* raw, uint32_t w, uint32_t h, float paper_white_nits,
                        std::vector<float>* linear_rgb, std::vector<uint8_t>* sdr_rgba, std::vector<uint8_t>* map_rgba, HdrCaptureStats* stats);

// Bytes per pixel of a format (4 or 8).
uint32_t capture_bytes_per_pixel(CaptureFormat fmt);

// Writes a PFM (Portable Float Map, 3 channels, little endian: header "PF\n<w> <h>\n-1.0\n", rows bottom to top) of linear_rgb scaled by
// `scale` (so scale = paper_white_nits/100 gives cd/m2 / 100). Returns false on I/O error (or when linear_rgb is not w*h*3 floats).
bool write_pfm(const std::string& path, const std::vector<float>& linear_rgb, uint32_t w, uint32_t h, float scale);

// Writes the stats as one line of text: "max_nits=... mean_nits=... above_paper=..% above_2x=..% above_4x=..%".
std::string capture_stats_line(const HdrCaptureStats& s);

}  // namespace nxrt::extras
