// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// blur_ref.h: the backdrop blur on the CPU, the GPU's arithmetic step by step: levels of the same sizes, the same region on each level
// (BackdropBlur::region), every tap clamped into the source level's region, each pass stored as RGBA8.
#include "blur_ref.h"

#include <algorithm>
#include <cmath>

#include "extras/backdrop_blur.h"

namespace nxrt::test {
namespace {

struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> px;              // RGBA8, as the GPU stores each level
    float at(int x, int y, int c) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return px[(static_cast<size_t>(y) * w + x) * 4 + c] / 255.f;
    }
    // bilinear at uv (the sampler: linear, clamp to edge)
    void sample(float u, float v, float out[3]) const {
        const float x = u * w - 0.5f, y = v * h - 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        for (int c = 0; c < 3; ++c)
            out[c] = (at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx) * (1 - fy) + (at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx) * fy;
    }
};

uint8_t q(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

// One pass of kBackdropBlurComp over the destination region `dr` (x0, y0, x1, y1), its taps clamped into the source region `sr`.
void pass(const Img& src, Img& dst, const int sr[4], const int dr[4], float off, bool up) {
    const float hx = off / src.w, hy = off / src.h;
    const float lu0 = (sr[0] + 0.5f) / src.w, lv0 = (sr[1] + 0.5f) / src.h, lu1 = (sr[2] - 0.5f) / src.w, lv1 = (sr[3] - 0.5f) / src.h;
    for (int y = dr[1]; y < dr[3]; ++y)
        for (int x = dr[0]; x < dr[2]; ++x) {
            const float u = (x + 0.5f) / dst.w, v = (y + 0.5f) / dst.h;
            float c[3] = {0, 0, 0}, t[3];
            auto add = [&](float du, float dv, float wgt) {
                src.sample(std::clamp(u + du, lu0, lu1), std::clamp(v + dv, lv0, lv1), t);
                for (int k = 0; k < 3; ++k) c[k] += t[k] * wgt;
            };
            if (!up) {
                add(0, 0, 4);
                add(-hx, -hy, 1);
                add(hx, hy, 1);
                add(hx, -hy, 1);
                add(-hx, hy, 1);
                for (float& k : c) k *= 0.125f;
            } else {
                add(-hx * 2, 0, 1);
                add(-hx, hy, 2);
                add(0, hy * 2, 1);
                add(hx, hy, 2);
                add(hx * 2, 0, 1);
                add(hx, -hy, 2);
                add(0, -hy * 2, 1);
                add(-hx, -hy, 2);
                for (float& k : c) k *= 1.f / 12.f;
            }
            uint8_t* d = &dst.px[(static_cast<size_t>(y) * dst.w + x) * 4];
            d[0] = q(c[0]);
            d[1] = q(c[1]);
            d[2] = q(c[2]);
            d[3] = 255;
        }
}

}  // namespace

std::vector<uint8_t> blur_reference(const std::vector<uint8_t>& rgba, int w, int h, float strength, const int rect[4], int* ow, int* oh) {
    using B = extras::BackdropBlur;
    const int n = B::levels(strength);
    const float off = B::offset(strength);
    const int whole[4] = {0, 0, w, h};
    int rg[4];
    if (!B::region(rect ? rect : whole, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}, strength, rg)) {
        rg[0] = rg[1] = 0;
        rg[2] = w;
        rg[3] = h;
    }
    Img lv[B::kMaxLevels + 1];
    int reg[B::kMaxLevels + 1][4];
    for (int i = 0; i <= B::kMaxLevels; ++i) {
        const int div = 2 << i;
        lv[i].w = std::max(1, (w + div - 1) / div);
        lv[i].h = std::max(1, (h + div - 1) / div);
        lv[i].px.assign(static_cast<size_t>(lv[i].w) * lv[i].h * 4, 0);
        reg[i][0] = rg[0] / div;
        reg[i][1] = rg[1] / div;
        reg[i][2] = std::min((rg[2] + div - 1) / div, lv[i].w);
        reg[i][3] = std::min((rg[3] + div - 1) / div, lv[i].h);
    }
    // level 0: the region's 2 x 2 means (the 2:1 linear blit)
    for (int y = rg[1] / 2; y < rg[3] / 2; ++y)
        for (int x = rg[0] / 2; x < rg[2] / 2; ++x) {
            for (int c = 0; c < 3; ++c) {
                int sum = 0;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) {
                        const int sx = std::min(2 * x + i, w - 1), sy = std::min(2 * y + j, h - 1);
                        sum += rgba[(static_cast<size_t>(sy) * w + sx) * 4 + c];
                    }
                lv[0].px[(static_cast<size_t>(y) * lv[0].w + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
            lv[0].px[(static_cast<size_t>(y) * lv[0].w + x) * 4 + 3] = 255;
        }
    for (int i = 0; i < n; ++i) pass(lv[i], lv[i + 1], reg[i], reg[i + 1], off, false);
    for (int i = n - 1; i >= 0; --i) pass(lv[i + 1], lv[i], reg[i + 1], reg[i], off, true);
    *ow = lv[0].w;
    *oh = lv[0].h;
    return lv[0].px;
}

}  // namespace nxrt::test
