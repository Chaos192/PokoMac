// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// soft_raster.h: Dear ImGui draw data on the CPU. Vertex positions go to 8-bit sub-pixel fixed point so the edge functions are exact and a
// shared edge belongs to exactly one of its two triangles (ImGui draws every rectangle as two triangles: a translucent panel must not get
// its diagonal blended twice). Pixel centres are at +0.5, as on the GPU.
#include "soft_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "play/cheats_theme.h"

namespace nxrt::test {
namespace {

struct Tex {
    const uint8_t* px = nullptr;
    int w = 0, h = 0, bpp = 4;
};

Tex tex_of(ImTextureID id) {
    Tex t;
    auto* td = reinterpret_cast<ImTextureData*>(static_cast<intptr_t>(id));
    if (!td || !td->Pixels) return t;
    t.px = td->Pixels;
    t.w = td->Width;
    t.h = td->Height;
    t.bpp = td->BytesPerPixel;
    return t;
}

// Bilinear, clamped to the edge (the sampler imgui_impl_vulkan uses).
void sample(const Tex& t, float u, float v, float out[4]) {
    if (!t.px) { out[0] = out[1] = out[2] = out[3] = 1.f; return; }
    const float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    float acc[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) {
        const int xi = std::clamp(x0 + (k & 1), 0, t.w - 1), yi = std::clamp(y0 + (k >> 1), 0, t.h - 1);
        const float wgt = ((k & 1) ? fx : 1.f - fx) * ((k >> 1) ? fy : 1.f - fy);
        const uint8_t* p = t.px + (static_cast<size_t>(yi) * t.w + xi) * t.bpp;
        if (t.bpp == 1) {
            acc[0] += wgt; acc[1] += wgt; acc[2] += wgt; acc[3] += wgt * p[0] / 255.f;
        } else {
            for (int c = 0; c < 4; ++c) acc[c] += wgt * p[c] / 255.f;
        }
    }
    std::memcpy(out, acc, sizeof acc);
}

using I64 = long long;
constexpr int kSub = 256;   // sub-pixel steps

inline I64 edge(I64 ax, I64 ay, I64 bx, I64 by, I64 px, I64 py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
// Whether a pixel exactly on the edge a -> b is inside: true for one direction of every edge only, so two triangles that share it never
// both take it.
inline bool owns(I64 ax, I64 ay, I64 bx, I64 by) { return (by - ay) > 0 || ((by - ay) == 0 && (bx - ax) < 0); }

void draw_triangle(const ImDrawVert* v[3], const Tex& tex, const ImVec4& clip, std::vector<uint8_t>& rgba, int w, int h) {
    I64 X[3], Y[3];
    for (int i = 0; i < 3; ++i) {
        X[i] = static_cast<I64>(std::llround(v[i]->pos.x * kSub));
        Y[i] = static_cast<I64>(std::llround(v[i]->pos.y * kSub));
    }
    I64 area = edge(X[0], Y[0], X[1], Y[1], X[2], Y[2]);
    if (area == 0) return;
    int o[3] = {0, 1, 2};
    if (area < 0) { std::swap(o[1], o[2]); area = -area; }
    const I64 x0 = X[o[0]], y0 = Y[o[0]], x1 = X[o[1]], y1 = Y[o[1]], x2 = X[o[2]], y2 = Y[o[2]];
    const ImDrawVert* a = v[o[0]];
    const ImDrawVert* b = v[o[1]];
    const ImDrawVert* c = v[o[2]];
    // the bounding box, inside the clip rectangle and the image
    const float minx = std::max({std::min({a->pos.x, b->pos.x, c->pos.x}), clip.x, 0.f});
    const float miny = std::max({std::min({a->pos.y, b->pos.y, c->pos.y}), clip.y, 0.f});
    const float maxx = std::min({std::max({a->pos.x, b->pos.x, c->pos.x}), clip.z, static_cast<float>(w)});
    const float maxy = std::min({std::max({a->pos.y, b->pos.y, c->pos.y}), clip.w, static_cast<float>(h)});
    if (minx >= maxx || miny >= maxy) return;
    const int px0 = static_cast<int>(std::floor(minx)), py0 = static_cast<int>(std::floor(miny));
    const int px1 = static_cast<int>(std::ceil(maxx)), py1 = static_cast<int>(std::ceil(maxy));
    const bool own0 = owns(x1, y1, x2, y2), own1 = owns(x2, y2, x0, y0), own2 = owns(x0, y0, x1, y1);
    auto unpack = [](ImU32 col, float out[4]) {
        for (int k = 0; k < 4; ++k) out[k] = static_cast<float>((col >> (8 * k)) & 0xFF) / 255.f;
    };
    float ca[4], cb[4], cc[4];
    unpack(a->col, ca);
    unpack(b->col, cb);
    unpack(c->col, cc);
    // a solid colour (every vertex the same colour, every uv the atlas's white texel) needs no sampling per pixel
    const bool flat = a->col == b->col && a->col == c->col && a->uv.x == b->uv.x && a->uv.x == c->uv.x && a->uv.y == b->uv.y && a->uv.y == c->uv.y;
    float flat_tex[4] = {1, 1, 1, 1};
    if (flat) sample(tex, a->uv.x, a->uv.y, flat_tex);
    const float inv = 1.f / static_cast<float>(area);
    for (int py = std::max(py0, 0); py < std::min(py1, h); ++py) {
        const float cy = py + 0.5f;
        if (cy < clip.y || cy >= clip.w) continue;
        const I64 sy = static_cast<I64>(py) * kSub + kSub / 2;
        for (int px = std::max(px0, 0); px < std::min(px1, w); ++px) {
            const float cx = px + 0.5f;
            if (cx < clip.x || cx >= clip.z) continue;
            const I64 sx = static_cast<I64>(px) * kSub + kSub / 2;
            const I64 w0 = edge(x1, y1, x2, y2, sx, sy), w1 = edge(x2, y2, x0, y0, sx, sy), w2 = edge(x0, y0, x1, y1, sx, sy);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2)) continue;
            float src[4];
            if (flat) {
                for (int k = 0; k < 4; ++k) src[k] = ca[k] * flat_tex[k];
            } else {
                const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
                float t[4];
                sample(tex, l0 * a->uv.x + l1 * b->uv.x + l2 * c->uv.x, l0 * a->uv.y + l1 * b->uv.y + l2 * c->uv.y, t);
                for (int k = 0; k < 4; ++k) src[k] = (l0 * ca[k] + l1 * cb[k] + l2 * cc[k]) * t[k];
            }
            const float al = std::clamp(src[3], 0.f, 1.f);
            if (al <= 0.f) continue;
            uint8_t* d = &rgba[(static_cast<size_t>(py) * w + px) * 4];
            for (int k = 0; k < 3; ++k) {
                const float out = std::clamp(src[k], 0.f, 1.f) * al + (d[k] / 255.f) * (1.f - al);
                d[k] = static_cast<uint8_t>(std::lround(out * 255.f));
            }
            d[3] = static_cast<uint8_t>(std::lround((al + (d[3] / 255.f) * (1.f - al)) * 255.f));
        }
    }
}

}  // namespace

void soft_init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "nxrt_soft";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
}

void soft_render(ImDrawData* dd, std::vector<uint8_t>& rgba, int w, int h) {
    if (dd->Textures)
        for (ImTextureData* t : *dd->Textures) {
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
                t->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(t)));
                t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    const ImVec2 off = dd->DisplayPos, sc = dd->FramebufferScale;
    for (const ImDrawList* dl : dd->CmdLists) {
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;     // ImDrawCallback_ResetRenderState and friends: nothing to reset here
            const ImVec4 clip((cmd.ClipRect.x - off.x) * sc.x, (cmd.ClipRect.y - off.y) * sc.y, (cmd.ClipRect.z - off.x) * sc.x,
                              (cmd.ClipRect.w - off.y) * sc.y);
            if (clip.z <= clip.x || clip.w <= clip.y) continue;
            const Tex tex = tex_of(cmd.GetTexID());
            for (unsigned i = 0; i + 2 < cmd.ElemCount + 0u; i += 3) {
                const ImDrawVert* v[3];
                ImDrawVert tv[3];
                for (int k = 0; k < 3; ++k) {
                    tv[k] = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + k]];
                    tv[k].pos.x = (tv[k].pos.x - off.x) * sc.x;
                    tv[k].pos.y = (tv[k].pos.y - off.y) * sc.y;
                    v[k] = &tv[k];
                }
                draw_triangle(v, tex, clip, rgba, w, h);
            }
        }
    }
}

bool theme_guard(ImDrawData* dd, const char* what) {
    namespace th = nxrt::cheats::ui::theme;
    std::string why;
    if (!th::style_check(&why)) {
        std::printf("FAILED %s: theme guard: %s\n", what, why.c_str());
        return false;
    }
    int stock = 0, verts = 0;
    ImU32 first = 0;
    if (dd)
        for (const ImDrawList* dl : dd->CmdLists)
            for (const ImDrawVert& v : dl->VtxBuffer) {
                ++verts;
                if ((v.col >> IM_COL32_A_SHIFT) && th::is_stock_colour(v.col)) {
                    if (!stock++) first = v.col;
                }
            }
    if (stock) {
        std::printf("FAILED %s: theme guard: %d of %d vertices have ImGui's stock colours (first #%06x)\n", what, stock, verts,
                    static_cast<unsigned>(((first & 0xFF) << 16) | (first & 0xFF00) | ((first >> 16) & 0xFF)));
        return false;
    }
    return true;
}

const char* background_name(int kind) {
    static const char* const k[] = {"scene", "light", "dark", "busy"};
    return kind >= 0 && kind < 4 ? k[kind] : "scene";
}

void background(int kind, std::vector<uint8_t>& px, int w, int h) {
    px.assign(static_cast<size_t>(w) * h * 4, 255);
    uint32_t seed = 0x9e3779b9u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float fy = static_cast<float>(y) / h, fx = static_cast<float>(x) / w;
            float r = 0, g = 0, b = 0;
            if (kind == 1) {          // light: a pale sky over snow, a white sun
                r = 0.86f + 0.10f * fy; g = 0.92f + 0.06f * fy; b = 0.98f;
                const float sx = fx - 0.25f, sy = fy - 0.25f;
                if (sx * sx * 3.16f + sy * sy < 0.012f) { r = g = b = 1.f; }
                if (fy > 0.66f + 0.05f * std::sin(fx * 5.f)) { r = 0.97f; g = 0.98f; b = 1.f; }
            } else if (kind == 2) {   // dark: a night sky with a few stars over black hills
                r = 0.02f + 0.03f * fy; g = 0.03f + 0.03f * fy; b = 0.07f + 0.05f * fy;
                seed = seed * 1664525u + 1013904223u;
                if ((seed >> 24) == 7 && fy < 0.6f) { r = g = b = 0.85f; }
                if (fy > 0.72f + 0.06f * std::sin(fx * 9.f)) { r = 0.01f; g = 0.01f; b = 0.015f; }
            } else {                  // busy: saturated diagonal stripes over a checkerboard of dark and bright squares (sharp edges everywhere)
                const int u = std::max(1, w / 1280);
                const int stripe = ((x + y) / (14 * u)) % 6;
                static const float c6[6][3] = {{1, .2f, .2f}, {1, .85f, .1f}, {.1f, .8f, .3f}, {.1f, .5f, 1}, {.8f, .2f, 1}, {1, 1, 1}};
                r = c6[stripe][0]; g = c6[stripe][1]; b = c6[stripe][2];
                if (((x / (40 * u)) + (y / (40 * u))) % 2 == 0) { r *= 0.3f; g *= 0.3f; b *= 0.3f; }
            }
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(r * 255);
            p[1] = static_cast<uint8_t>(g * 255);
            p[2] = static_cast<uint8_t>(b * 255);
        }
}

}  // namespace nxrt::test
