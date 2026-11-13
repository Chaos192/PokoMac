// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The game's icon for the cheats overlay's header (cheats_ui_internal.h game_icon): the title folder's icon.png or icon.jpg (what the app's
// library keeps beside each title: icon.jpg from the catalogue or the control data, icon.png from some), decoded once, made smaller with a
// box filter (the header shows it at 40 UI units: a 256-pixel icon sampled down without mipmaps would shimmer), and registered as an
// ImGui user texture (the renderer backend uploads it like the font atlas).
// JPEG through stb_image (Sean Barrett, public domain or MIT: runtime/third_party/stb, v2.30, THIRD_PARTY_NOTICES.md), JPEG only, from
// memory; PNG through the runtime's own loader (gfx::load_png_rgba, SDL).
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cheats_ui_internal.h"
#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "../../../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nxrt::cheats::ui {
namespace fs = std::filesystem;
namespace {

struct Icon {
    bool tried = false;
    std::vector<uint8_t> rgba;          // decoded (and made smaller) once
    int w = 0, h = 0;
    ImGuiContext* ctx = nullptr;        // the context the texture is registered in (a new context gets its own)
    ImTextureData* tex = nullptr;
};
Icon g_icon;

std::vector<uint8_t> read_all(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Halves the picture (2x2 box) while it is at least `keep` pixels wide: 256 -> 128 for the header.
void shrink(std::vector<uint8_t>& px, int& w, int& h, int keep) {
    while (w >= keep * 2 && h >= keep * 2) {
        const int nw = w / 2, nh = h / 2;
        std::vector<uint8_t> o(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int k = 0; k < 4; ++k) {
                    const auto at = [&](int xx, int yy) { return px[(static_cast<size_t>(yy) * w + xx) * 4 + k]; };
                    o[(static_cast<size_t>(y) * nw + x) * 4 + k] =
                        static_cast<uint8_t>((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
        px.swap(o);
        w = nw;
        h = nh;
    }
}

void load() {
    Icon& ic = g_icon;
    ic.tried = true;
    for (const char* name : {"icon.png", "icon.jpg", "icon.jpeg"}) {
        const std::string p = name;   // PokoMac: the game folder is the working directory (AnySwitch: title_path)
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(fs::u8path(p), ec)) continue;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (std::strstr(name, ".png")) {
            uint32_t pw = 0, ph = 0;
            if (!gfx::load_png_rgba(p, &px, &pw, &ph)) continue;
            w = static_cast<int>(pw);
            h = static_cast<int>(ph);
        } else {
            const std::vector<uint8_t> bytes = read_all(p);
            if (bytes.empty() || bytes.size() > (16u << 20)) continue;
            int n = 0;
            stbi_uc* d = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
            if (!d) {
                NXLOG_W("cheats", "the game's icon %s could not be read as a JPEG", p.c_str());
                continue;
            }
            px.assign(d, d + static_cast<size_t>(w) * h * 4);
            stbi_image_free(d);
        }
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) continue;
        shrink(px, w, h, 96);
        ic.rgba = std::move(px);
        ic.w = w;
        ic.h = h;
        NXLOG_I("cheats", "the game's icon for the overlay: %s (%dx%d)", p.c_str(), w, h);
        return;
    }
}

}  // namespace

ImTextureData* game_icon() {
    Icon& ic = g_icon;
    if (!ic.tried) load();
    if (ic.rgba.empty()) return nullptr;
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ic.ctx != ctx || !ic.tex) {
        ic.tex = IM_NEW(ImTextureData)();
        ic.tex->Create(ImTextureFormat_RGBA32, ic.w, ic.h);
        std::memcpy(ic.tex->GetPixels(), ic.rgba.data(), ic.rgba.size());
        ic.tex->UseColors = true;
        ImGui::RegisterUserTexture(ic.tex);     // the backend creates it with the frame's other texture requests
        ic.ctx = ctx;
    }
    return ic.tex;
}

}  // namespace nxrt::cheats::ui
