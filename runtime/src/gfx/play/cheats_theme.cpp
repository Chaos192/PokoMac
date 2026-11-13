// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// cheats_theme.h: the overlay's tokens, style, fonts, icons and shared widgets. The values are the launcher's (launcher/src/theme.css).
#include "cheats_theme.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::cheats::ui::theme {
namespace {

#if defined(NXRT_HAVE_CHEATS_FONTS)
// Inter (SIL Open Font License 1.1, runtime/assets/fonts/LICENSE-Inter.txt): the launcher's typeface, static instances made from its
// @fontsource-variable/inter files and subset to Latin (tools: scratch mkfont.py, see runtime/assets/fonts/README-Inter.md).
alignas(4) const unsigned char kInterRegular[] = {
#include "cheats_font_inter_regular.inc"
};
alignas(4) const unsigned char kInterRegularExt[] = {
#include "cheats_font_inter_regular_ext.inc"
};
alignas(4) const unsigned char kInterSemiBold[] = {
#include "cheats_font_inter_semibold.inc"
};
#endif

ImFont* g_body = nullptr;     // Inter Regular (+ Latin Extended)
ImFont* g_strong = nullptr;   // Inter SemiBold
ImFont* g_mono = nullptr;     // ImGui's default vector font (monospaced)
Palette g_pal;
float g_k = 1.f;
ThemeSpec g_theme;            // the theme in force (set_theme; the default Sunrise until then)
bool g_glass = false;         // after apply(): the panel is glass this frame
float g_blur = 0.6f, g_tint = 0.55f;

constexpr ImU32 rgb(int r, int g, int b, float a = 1.f) {
    return IM_COL32(r, g, b, static_cast<int>(a * 255.f + 0.5f));
}

struct Preset { const char* name; ImU32 rgb; };
const Preset kPresets[] = {
    {"sunrise", rgb(255, 194, 61)}, {"blue", rgb(106, 169, 255)}, {"green", rgb(126, 214, 150)}, {"amber", rgb(255, 179, 92)},
    {"violet", rgb(176, 148, 255)}, {"red", rgb(255, 122, 107)},  {"teal", rgb(126, 232, 193)},  {"slate", rgb(160, 176, 196)},
};

float lum(ImU32 c) {
    auto ch = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const ImVec4 f = ImGui::ColorConvertU32ToFloat4(c);
    return 0.2126f * ch(f.x) + 0.7152f * ch(f.y) + 0.0722f * ch(f.z);
}
ImU32 shade(ImU32 c, float f) {   // f > 1 lighter, < 1 darker
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    if (f >= 1.f) { v.x += (1.f - v.x) * (f - 1.f); v.y += (1.f - v.y) * (f - 1.f); v.z += (1.f - v.z) * (f - 1.f); }
    else { v.x *= f; v.y *= f; v.z *= f; }
    return ImGui::ColorConvertFloat4ToU32(v);
}

// `auto`: the frame's hue with a saturation and lightness that keep the focus ring and the selection readable on the dark panel whatever
// the game shows (a grey frame gives slate).
ImU32 auto_accent(const unsigned char rgba[4]) {
    float r = rgba[0] / 255.f, g = rgba[1] / 255.f, b = rgba[2] / 255.f;
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
    if (s < 0.12f) return kPresets[7].rgb;
    s = std::clamp(s, 0.45f, 0.70f);
    v = 0.92f;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.f));
}

}  // namespace

const Palette& pal() { return g_pal; }
float k() { return g_k; }

ImU32 with_alpha(ImU32 c, float a) {
    const int al = static_cast<int>(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(al) << IM_COL32_A_SHIFT);
}

ImU32 accent_colour(const char* accent, const unsigned char* frame_rgba) {
    const char* want = accent && *accent ? accent : "theme";
    if (std::strcmp(want, "theme") == 0) return g_theme.accent;
    if (std::strcmp(want, "auto") == 0) return frame_rgba ? auto_accent(frame_rgba) : kPresets[7].rgb;
    for (const Preset& p : kPresets)
        if (std::strcmp(p.name, want) == 0) return p.rgb;
    ImU32 c = 0;
    if (parse_colour(want, &c)) return c | IM_COL32_A_MASK;   // the player's own colour (an accent is always opaque)
    return g_theme.accent;
}

const ThemeSpec& theme() { return g_theme; }
void set_theme(const ThemeSpec& t) { g_theme = t; }
bool glass() { return g_glass; }
float glass_blur() { return g_blur; }
float glass_tint() { return g_tint; }

// ---- fonts ---------------------------------------------------------------------------------------------------------------------------
void load_fonts(ImFontAtlas* atlas) {
    // once per atlas, found by name: a new atlas (a new ImGui context) may sit at the address of a destroyed one, so a pointer says nothing
    if (!atlas) return;
    g_body = g_strong = nullptr;
    g_mono = atlas->Fonts.Size > 0 ? atlas->Fonts[0] : nullptr;
    for (ImFont* f : atlas->Fonts) {
        if (std::strcmp(f->GetDebugName(), "Inter Regular") == 0 && !g_body) g_body = f;
        if (std::strcmp(f->GetDebugName(), "Inter SemiBold") == 0 && !g_strong) g_strong = f;
        if (std::strcmp(f->GetDebugName(), "Theme font") == 0) g_body = g_strong = f;
    }
    if (g_body) return;
#if defined(NXRT_HAVE_CHEATS_FONTS)
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;     // the bytes are the program's own (static)
    std::snprintf(cfg.Name, sizeof cfg.Name, "Inter Regular");
    g_body = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kInterRegular), static_cast<int>(sizeof kInterRegular), 16.f, &cfg);
    ImFontConfig ext = cfg;
    ext.MergeMode = true;                 // the Latin Extended letters into the same face
    std::snprintf(ext.Name, sizeof ext.Name, "Inter Regular Ext");
    if (g_body) atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kInterRegularExt), static_cast<int>(sizeof kInterRegularExt), 16.f, &ext);
    ImFontConfig sb = cfg;
    std::snprintf(sb.Name, sizeof sb.Name, "Inter SemiBold");
    g_strong = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kInterSemiBold), static_cast<int>(sizeof kInterSemiBold), 16.f, &sb);
    if (g_strong) {                       // its accented letters come from the regular face
        ImFontConfig sbx = ext;
        atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kInterRegularExt), static_cast<int>(sizeof kInterRegularExt), 16.f, &sbx);
    }
#endif
    // the theme ([cheats] theme) may bring its own font: it replaces Inter for the text (headings too); a font that does not load keeps Inter
    g_theme = load_theme(config_get_str("cheats", "theme", ""), nullptr);
    if (!g_theme.font.empty()) {
        ImFontConfig own;
        std::snprintf(own.Name, sizeof own.Name, "Theme font");
        if (ImFont* f = atlas->AddFontFromFileTTF(g_theme.font.c_str(), 16.f, &own)) {
            g_body = f;
            g_strong = f;
        } else {
            NXLOG_W("cheats", "the theme's font %s did not load: the overlay keeps Inter", g_theme.font.c_str());
        }
    }
}

float size_of(Type t) {
    const float b = g_theme.font_size > 0.f ? g_theme.font_size : 16.f;   // the theme's body size (16, the launcher's 1rem, by default)
    switch (t) {
        case Type::Small: return b * 0.84375f;
        case Type::Title: return b * 1.25f;
        case Type::Display: return b * 1.875f;
        case Type::Mono: return 15.f;
        default: return b;
    }
}

void push(Type t) {
    ImFont* f = nullptr;
    switch (t) {
        case Type::Strong: case Type::Title: case Type::Display: f = g_strong ? g_strong : g_body; break;
        case Type::Mono: f = g_mono; break;
        default: f = g_body; break;
    }
    ImGui::PushFont(f, size_of(t));
}
void push(Type t, float scale) {
    ImFont* f = nullptr;
    switch (t) {
        case Type::Strong: case Type::Title: case Type::Display: f = g_strong ? g_strong : g_body; break;
        case Type::Mono: f = g_mono; break;
        default: f = g_body; break;
    }
    ImGui::PushFont(f, size_of(t) * scale);
}
void pop() { ImGui::PopFont(); }

// ---- style ---------------------------------------------------------------------------------------------------------------------------
void apply(const Look& look, const unsigned char* frame_rgba, bool blur_ok) {
    if (!g_theme.panel) g_theme = load_theme(std::string(), nullptr);
    ImGuiStyle& s = ImGui::GetStyle();
    const float dpi = s.FontScaleDpi > 0.f ? s.FontScaleDpi : 1.f;
    const float user = look.scale > 0.f ? look.scale : 1.f;
    g_k = dpi * user;
    const ThemeSpec& t = g_theme;
    const LookSpec& spec = look_spec();
    // the player's look over the theme's: style, panel opacity, blur, tint
    const bool want_glass = look.style < 0 ? t.glass : look.style == 1;
    g_glass = want_glass && blur_ok;
    const float o = std::clamp(look.opacity >= 0.f ? look.opacity : t.opacity, spec.opacity.min / 100.f, spec.opacity.max / 100.f);
    g_blur = std::clamp(look.blur >= 0.f ? look.blur : t.blur, 0.f, 1.f);
    g_tint = std::clamp(look.tint >= 0.f ? look.tint : t.tint, 0.f, 1.f);

    // the palette: the theme's colours, the derived ones as the launcher derives its tones (theme.css), the chosen accent
    Palette& p = g_pal;
    const bool dark = lum(t.panel) < 0.35f;
    p.dark = dark;
    auto pick = [](ImU32 given, ImU32 derived) { return given ? given : derived; };
    const ImU32 ac = accent_colour(look.accent.c_str(), frame_rgba);
    p.accent = ac;
    p.accent_hover = shade(ac, dark ? 1.18f : 0.88f);
    p.accent_press = shade(ac, dark ? 0.9f : 0.78f);
    // the theme's on-accent goes with the theme's accent; another accent gets the readable one of dark and white
    p.on_accent = (look.accent.empty() || look.accent == "theme") && t.on_accent ? t.on_accent : lum(ac) > 0.35f ? rgb(24, 16, 4) : rgb(255, 255, 255);
    p.accent_tint = with_alpha(ac, dark ? 0.16f : 0.13f);
    const float scrim_a = ((t.scrim >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    p.scrim_top = with_alpha(t.scrim, scrim_a * 0.55f);
    p.scrim_bottom = t.scrim;
    p.modal_dim = with_alpha(t.scrim, std::max(scrim_a, dark ? 0.62f : 0.36f));
    // the panel: solid at the opacity, or glass (the game blurred behind it, overlay.cpp) under the tint
    p.panel = with_alpha(t.panel, g_glass ? g_tint : o);
    p.solid = with_alpha(t.dialog, 0.98f);
    const float s1 = dark ? 0.05f : 0.04f, s2 = dark ? 0.085f : 0.065f, s3 = dark ? 0.14f : 0.11f;
    p.surface1 = with_alpha(t.surface, s1);
    p.surface2 = with_alpha(t.surface, s2);
    p.surface3 = with_alpha(t.surface, s3);
    p.card = pick(t.card, with_alpha(t.surface, 0.045f));
    p.sunken = t.well;
    p.sunken_hover = with_alpha(t.well, std::min(1.f, ((t.well >> IM_COL32_A_SHIFT) & 0xFF) / 255.f * 1.3f + 0.02f));
    p.line = pick(t.line, with_alpha(t.surface, dark ? 0.09f : 0.1f));
    p.line_strong = pick(t.line_strong, with_alpha(t.surface, dark ? 0.2f : 0.22f));
    p.text = t.text;
    p.text2 = pick(t.text2, with_alpha(t.text, 0.72f));
    p.text3 = pick(t.text3, with_alpha(t.text, dark ? 0.5f : 0.54f));
    p.ok = t.ok;                         // the default themes' ok is blue, so it stays apart from warn and danger for red-green colour blindness
    p.warn = t.warn;
    p.danger = t.danger;
    p.danger_solid = pick(t.danger_solid, shade(t.danger, 0.85f));
    p.on_danger = rgb(255, 255, 255);
    p.ok_tint = with_alpha(t.ok, dark ? 0.12f : 0.1f);
    p.warn_tint = with_alpha(t.warn, dark ? 0.14f : 0.1f);
    p.danger_tint = with_alpha(t.danger, dark ? 0.14f : 0.1f);
    p.control_off = with_alpha(t.surface, dark ? 0.18f : 0.2f);
    p.row_alt = with_alpha(t.surface, dark ? 0.025f : 0.02f);
    p.cap = t.text;
    p.on_cap = t.panel | IM_COL32_A_MASK;
    p.shadow = dark ? rgb(0, 0, 0, 0.35f) : rgb(0, 0, 0, 0.14f);

    s = ImGuiStyle();                    // from ImGui's unscaled defaults, so nothing is scaled twice
    s.FontSizeBase = size_of(Type::Body);
    s.FontScaleDpi = dpi;
    s.FontScaleMain = user;
    const float rk = std::clamp(t.radius, 0.f, 40.f) / kRadiusLg;   // the theme's corners, in the launcher's proportions
    s.WindowRounding = kRadiusLg * rk;
    s.ChildRounding = kRadiusMd * rk;
    s.FrameRounding = kRadiusSm * rk;
    s.PopupRounding = kRadiusMd * rk;
    s.GrabRounding = 999.f;
    s.TabRounding = kRadiusSm;
    s.ScrollbarRounding = 999.f;
    s.WindowBorderSize = 1.f;
    s.ChildBorderSize = 0.f;
    s.FrameBorderSize = 1.f;
    s.PopupBorderSize = 1.f;
    s.WindowPadding = ImVec2(S6, S5);
    s.FramePadding = ImVec2(S3, 9.f);
    s.ItemSpacing = ImVec2(S3, S2 + 2.f);
    s.ItemInnerSpacing = ImVec2(S2, S2);
    s.CellPadding = ImVec2(S3, 7.f);
    s.IndentSpacing = S5;
    s.ScrollbarSize = 12.f;
    s.GrabMinSize = 14.f;
    s.WindowTitleAlign = ImVec2(0.f, 0.5f);
    s.SeparatorTextBorderSize = 1.f;
    s.SeparatorTextPadding = ImVec2(0.f, S1);
    s.SelectableTextAlign = ImVec2(0.f, 0.5f);
    s.DisabledAlpha = 0.45f;
    s.ScaleAllSizes(g_k);

    ImVec4* c = s.Colors;
    const ImVec4 a = v4(ac);
    c[ImGuiCol_Text] = v4(p.text);
    c[ImGuiCol_TextDisabled] = v4(p.text3);
    c[ImGuiCol_WindowBg] = v4(p.panel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v4(p.solid);
    c[ImGuiCol_Border] = v4(p.line);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v4(p.sunken);
    c[ImGuiCol_FrameBgHovered] = v4(p.sunken_hover);
    c[ImGuiCol_FrameBgActive] = v4(p.sunken_hover);
    c[ImGuiCol_TitleBg] = v4(p.solid);
    c[ImGuiCol_TitleBgActive] = v4(p.solid);
    c[ImGuiCol_TitleBgCollapsed] = v4(p.solid);
    c[ImGuiCol_MenuBarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v4(p.surface3);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(p.line_strong);
    c[ImGuiCol_ScrollbarGrabActive] = a;
    c[ImGuiCol_CheckMark] = a;
    c[ImGuiCol_SliderGrab] = a;
    c[ImGuiCol_SliderGrabActive] = v4(p.accent_hover);
    c[ImGuiCol_Button] = v4(p.surface2);
    c[ImGuiCol_ButtonHovered] = v4(p.surface3);
    c[ImGuiCol_ButtonActive] = v4(with_alpha(ac, 0.45f));
    c[ImGuiCol_Header] = v4(p.accent_tint);
    c[ImGuiCol_HeaderHovered] = v4(p.surface2);
    c[ImGuiCol_HeaderActive] = v4(with_alpha(ac, 0.28f));
    c[ImGuiCol_Separator] = v4(p.line);
    c[ImGuiCol_SeparatorHovered] = v4(p.line_strong);
    c[ImGuiCol_SeparatorActive] = a;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = v4(with_alpha(ac, 0.5f));
    c[ImGuiCol_ResizeGripActive] = a;
    c[ImGuiCol_InputTextCursor] = a;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = v4(p.surface2);
    c[ImGuiCol_TabSelected] = v4(p.accent_tint);
    c[ImGuiCol_TabSelectedOverline] = a;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = v4(p.surface2);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PlotLines] = a;
    c[ImGuiCol_PlotLinesHovered] = v4(p.accent_hover);
    c[ImGuiCol_PlotHistogram] = a;
    c[ImGuiCol_PlotHistogramHovered] = v4(p.accent_hover);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderStrong] = v4(p.line);
    c[ImGuiCol_TableBorderLight] = v4(p.line);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = v4(p.row_alt);
    c[ImGuiCol_TextLink] = a;
    c[ImGuiCol_TextSelectedBg] = v4(with_alpha(ac, 0.3f));
    c[ImGuiCol_TreeLines] = v4(p.line);
    c[ImGuiCol_DragDropTarget] = a;
    c[ImGuiCol_NavCursor] = a;           // the controller / keyboard focus ring: the launcher's one ring, in the accent
    c[ImGuiCol_NavWindowingHighlight] = a;
    c[ImGuiCol_NavWindowingDimBg] = v4(p.scrim_bottom);
    c[ImGuiCol_ModalWindowDimBg] = v4(p.modal_dim);
}

// ---- icons ---------------------------------------------------------------------------------------------------------------------------
void icon(ImDrawList* dl, Icon ic, ImVec2 c, float size, ImU32 col) {
    if (ic == Icon::None) return;
    const float u = size / 24.f;
    const float t = std::max(1.25f, 2.f * u);
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12.f) * u, c.y + (y - 12.f) * u); };
    auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, t); };
    auto poly = [&](std::initializer_list<float> xy, bool closed = false) {
        ImVec2 pts[16];
        int n = 0;
        for (auto it = xy.begin(); it != xy.end() && n < 16; it += 2) pts[n++] = P(*it, *(it + 1));
        dl->AddPolyline(pts, n, col, closed ? ImDrawFlags_Closed : ImDrawFlags_None, t);
    };
    auto circle = [&](float x, float y, float r) { dl->AddCircle(P(x, y), r * u, col, 0, t); };
    auto dot = [&](float x, float y, float r) { dl->AddCircleFilled(P(x, y), r * u, col); };
    auto rect = [&](float x0, float y0, float x1, float y1, float r) { dl->AddRect(P(x0, y0), P(x1, y1), col, r * u, 0, t); };
    switch (ic) {
        case Icon::Cheats:   // a wand with sparkles
            line(4, 20, 15, 9);
            poly({15, 3, 16.2f, 6.8f, 20, 8, 16.2f, 9.2f, 15, 13, 13.8f, 9.2f, 10, 8, 13.8f, 6.8f}, true);
            line(6, 4, 6, 8); line(4, 6, 8, 6);
            break;
        case Icon::Search: circle(11, 11, 7); line(16, 16, 21, 21); break;
        case Icon::Table:
            dot(4, 6, 1.4f); dot(4, 12, 1.4f); dot(4, 18, 1.4f);
            line(8.5f, 6, 21, 6); line(8.5f, 12, 21, 12); line(8.5f, 18, 21, 18);
            break;
        case Icon::Tools:   // sliders
            line(6, 3, 6, 21); line(12, 3, 12, 21); line(18, 3, 18, 21);
            dl->AddCircleFilled(P(6, 15), 2.6f * u, col); dl->AddCircleFilled(P(12, 8), 2.6f * u, col); dl->AddCircleFilled(P(18, 16), 2.6f * u, col);
            break;
        case Icon::Close: line(6, 6, 18, 18); line(18, 6, 6, 18); break;
        case Icon::Monitor: rect(2.5f, 4, 21.5f, 16, 2); line(12, 16, 12, 20); line(7.5f, 20.5f, 16.5f, 20.5f); break;
        case Icon::Volume:
            poly({3.5f, 9.5f, 7.5f, 9.5f, 12.5f, 5, 12.5f, 19, 7.5f, 14.5f, 3.5f, 14.5f}, true);
            poly({16, 9, 17.3f, 12, 16, 15}); poly({18.8f, 6.2f, 21, 12, 18.8f, 17.8f});
            break;
        case Icon::Expand:
            poly({4, 9, 4, 4, 9, 4}); line(4, 4, 10, 10); poly({15, 4, 20, 4, 20, 9}); line(20, 4, 14, 10);
            poly({4, 15, 4, 20, 9, 20}); line(4, 20, 10, 14); poly({15, 20, 20, 20, 20, 15}); line(20, 20, 14, 14);
            break;
        case Icon::Check: poly({5, 12.5f, 10, 17.5f, 19, 7}); break;
        case Icon::Snow:
            line(12, 2.5f, 12, 21.5f); line(3.8f, 7.25f, 20.2f, 16.75f); line(3.8f, 16.75f, 20.2f, 7.25f);
            poly({9.5f, 4, 12, 6.5f, 14.5f, 4}); poly({9.5f, 20, 12, 17.5f, 14.5f, 20});
            break;
        case Icon::Keyboard:
            rect(2.5f, 6, 21.5f, 18, 2);
            for (float x : {6.f, 9.5f, 13.f, 16.5f}) dot(x, 10, 1.1f);
            line(7.5f, 14.5f, 16.5f, 14.5f);
            break;
        case Icon::Plus: line(12, 5, 12, 19); line(5, 12, 19, 12); break;
        case Icon::Trash:
            line(3.5f, 6.5f, 20.5f, 6.5f); poly({9, 6.5f, 9, 3.5f, 15, 3.5f, 15, 6.5f});
            poly({5.5f, 6.5f, 6.8f, 20.5f, 17.2f, 20.5f, 18.5f, 6.5f});
            line(10, 10.5f, 10, 16.5f); line(14, 10.5f, 14, 16.5f);
            break;
        case Icon::Undo:
            dl->PathArcTo(P(13, 13), 7 * u, IM_PI * 1.05f, IM_PI * 2.55f, 20);
            dl->PathStroke(col, 0, t);
            poly({3, 8, 6.2f, 12.6f, 10.6f, 9.4f});
            break;
        case Icon::Edit: poly({4, 20, 5, 15.5f, 16.5f, 4, 20, 7.5f, 8.5f, 19, 4, 20}); line(14, 6.5f, 17.5f, 10); break;
        case Icon::Play: dl->AddTriangleFilled(P(7, 4.5f), P(19.5f, 12), P(7, 19.5f), col); break;
        case Icon::Pause: dl->AddRectFilled(P(6.5f, 5), P(10, 19), col, u); dl->AddRectFilled(P(14, 5), P(17.5f, 19), col, u); break;
        case Icon::Gear:
            circle(12, 12, 3.2f);
            circle(12, 12, 7.2f);
            for (int i = 0; i < 8; ++i) {
                const float an = i * IM_PI / 4.f;
                line(12 + std::cos(an) * 7.2f, 12 + std::sin(an) * 7.2f, 12 + std::cos(an) * 9.8f, 12 + std::sin(an) * 9.8f);
            }
            break;
        case Icon::Info: circle(12, 12, 9.5f); line(12, 11, 12, 16.5f); dot(12, 7.6f, 1.3f); break;
        case Icon::Warn: poly({12, 3.5f, 21.5f, 20, 2.5f, 20}, true); line(12, 9.5f, 12, 14); dot(12, 17, 1.2f); break;
        case Icon::Up: line(12, 19, 12, 5); poly({6, 11, 12, 5, 18, 11}); break;
        case Icon::Down: line(12, 5, 12, 19); poly({6, 13, 12, 19, 18, 13}); break;
        case Icon::Changed: line(8, 4, 8, 19); poly({4, 8, 8, 4, 12, 8}); line(16, 5, 16, 20); poly({12, 16, 16, 20, 20, 16}); break;
        case Icon::Same: line(5, 9, 19, 9); line(5, 15, 19, 15); break;
        case Icon::Folder: poly({3, 6, 3, 19, 21, 19, 21, 8.5f, 12, 8.5f, 10, 5, 3, 5, 3, 6}); break;
        case Icon::Code: poly({8, 6, 2.5f, 12, 8, 18}); poly({16, 6, 21.5f, 12, 16, 18}); break;
        case Icon::Eye:
            dl->PathArcTo(P(12, 21), 12 * u, IM_PI * 1.2f, IM_PI * 1.8f, 16); dl->PathStroke(col, 0, t);
            dl->PathArcTo(P(12, 3), 12 * u, IM_PI * 0.2f, IM_PI * 0.8f, 16); dl->PathStroke(col, 0, t);
            circle(12, 12, 3);
            break;
        case Icon::Bolt: poly({13, 2.5f, 4, 13.5f, 12, 13.5f, 11, 21.5f, 20, 10.5f, 12, 10.5f}, true); break;
        case Icon::Pad:
            rect(2.5f, 7, 21.5f, 18, 4.5f);
            line(5.5f, 12.5f, 10.5f, 12.5f); line(8, 10, 8, 15);
            dot(15.5f, 11, 1.3f); dot(18, 13.8f, 1.3f);
            break;
        case Icon::Chevron: poly({9, 5, 16, 12, 9, 19}); break;
        case Icon::Back: poly({15, 5, 8, 12, 15, 19}); break;
        case Icon::Export: poly({8, 8, 12, 4, 16, 8}); line(12, 4, 12, 15); poly({4, 14, 4, 20, 20, 20, 20, 14}); break;
        case Icon::Import: poly({8, 11, 12, 15, 16, 11}); line(12, 4, 12, 15); poly({4, 14, 4, 20, 20, 20, 20, 14}); break;
        case Icon::Refresh:
            dl->PathArcTo(P(12, 12), 8 * u, IM_PI * -0.35f, IM_PI * 1.35f, 24); dl->PathStroke(col, 0, t);
            poly({15.5f, 2.5f, 19, 5.2f, 15.8f, 8.4f});
            break;
        case Icon::Graph: poly({3, 3, 3, 21, 21, 21}); poly({7, 15, 11, 10, 14, 13, 20, 6}); break;
        case Icon::Memory:
            rect(5, 5, 19, 19, 2); rect(9, 9, 15, 15, 1);
            for (float x : {9.f, 15.f}) { line(x, 2, x, 5); line(x, 19, x, 22); }
            for (float y : {9.f, 15.f}) { line(2, y, 5, y); line(19, y, 22, y); }
            break;
        case Icon::Pointer: circle(6, 6, 2.5f); line(8, 8, 16, 16); poly({11, 17, 17, 17, 17, 11}); break;
        case Icon::Thread: poly({4, 6, 20, 6}); poly({4, 12, 14, 12}); poly({4, 18, 17, 18}); dot(20, 12, 1.6f); break;
        case Icon::Breakpoint: dl->AddCircleFilled(P(12, 12), 6.5f * u, col); break;
        case Icon::Help:
            circle(12, 12, 9.5f);
            dl->PathArcTo(P(12, 9.3f), 2.8f * u, IM_PI * 1.0f, IM_PI * 2.4f, 12); dl->PathStroke(col, 0, t);
            line(12.6f, 12, 12, 13.8f); dot(12, 17, 1.2f);
            break;
        case Icon::Text: line(5, 6, 19, 6); line(12, 6, 12, 19); line(9, 19, 15, 19); break;
        case Icon::Number: line(9, 4, 7, 20); line(17, 4, 15, 20); line(4, 9, 20, 9); line(4, 15, 20, 15); break;
        case Icon::Decimal:
            dot(5, 18, 1.6f);
            dl->AddEllipse(P(12, 12), ImVec2(3.2f * u, 7 * u), col, 0.f, 0, t);
            line(19, 5, 19, 19); poly({16.5f, 7, 19, 5});
            break;
        case Icon::Question:
            dl->PathArcTo(P(12, 8.5f), 4 * u, IM_PI * 1.0f, IM_PI * 2.4f, 14); dl->PathStroke(col, 0, t);
            line(13, 12.2f, 12, 14.5f); dot(12, 19, 1.4f);
            break;
        case Icon::Dots: dot(5, 12, 1.8f); dot(12, 12, 1.8f); dot(19, 12, 1.8f); break;
        case Icon::Star: poly({12, 3, 14.6f, 9, 21, 9.6f, 16.2f, 13.8f, 17.6f, 20.4f, 12, 17, 6.4f, 20.4f, 7.8f, 13.8f, 3, 9.6f, 9.4f, 9}, true); break;
        case Icon::Map: poly({3, 6, 9, 3.5f, 15, 6, 21, 3.5f, 21, 18, 15, 20.5f, 9, 18, 3, 20.5f}, true); line(9, 3.5f, 9, 18); line(15, 6, 15, 20.5f); break;
        case Icon::Camera: rect(3, 7, 21, 19, 2.5f); poly({8, 7, 9.5f, 4.5f, 14.5f, 4.5f, 16, 7}); circle(12, 13, 3.5f); break;
        case Icon::Speed:
            dl->PathArcTo(P(12, 15), 9 * u, IM_PI * 1.0f, IM_PI * 2.0f, 20); dl->PathStroke(col, 0, t);
            line(12, 15, 16.5f, 9.5f); dot(12, 15, 1.6f);
            break;
        case Icon::Tag: poly({3, 3, 12.5f, 3, 21, 11.5f, 11.5f, 21, 3, 12.5f}, true); dot(8, 8, 1.6f); break;
        case Icon::Layers: poly({12, 3, 21, 8, 12, 13, 3, 8}, true); poly({3, 12.5f, 12, 17.5f, 21, 12.5f}); poly({3, 16.5f, 12, 21.5f, 21, 16.5f}); break;
        case Icon::Popout: poly({14, 4, 20, 4, 20, 10}); line(20, 4, 11, 13); poly({17, 14, 17, 20, 4, 20, 4, 7, 10, 7}); break;
        case Icon::DockLeft: rect(3, 4, 21, 20, 2.5f); dl->AddRectFilled(P(5, 6), P(10.5f, 18), col, u); break;
        case Icon::DockCentre: rect(3, 4, 21, 20, 2.5f); dl->AddRectFilled(P(8, 6), P(16, 18), col, u); break;
        case Icon::DockRight: rect(3, 4, 21, 20, 2.5f); dl->AddRectFilled(P(13.5f, 6), P(19, 18), col, u); break;
        case Icon::Pin:
            line(12, 16, 12, 22);
            poly({8, 3, 16, 3, 15, 9, 18.5f, 12.5f, 18.5f, 16, 5.5f, 16, 5.5f, 12.5f, 9, 9}, true);
            break;
        default: break;
    }
}

void icon_item(Icon i, float size, ImU32 col) {
    const float sz = size > 0.f ? size : ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(sz, sz));
    icon(ImGui::GetWindowDrawList(), i, ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f), sz, col ? col : ImGui::GetColorU32(ImGuiCol_Text));
}

// ---- widgets -------------------------------------------------------------------------------------------------------------------------
bool button(const char* label, Icon ic, Btn kind, float width, float height) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    const bool strong = kind == Btn::Primary || kind == Btn::Danger;
    if (strong) push(Type::Strong);
    const ImVec2 ts = ImGui::CalcTextSize(label, end, false);
    const float h = std::round(px(height > 0.f ? height : kControlH));
    const float isz = std::round(ImGui::GetFontSize() * 1.15f);
    const float padx = sp(S4), gap = sp(S2);
    float wd = 2.f * padx + ts.x + (ic != Icon::None ? isz + (ts.x > 0.f ? gap : 0.f) : 0.f);
    if (ic != Icon::None && ts.x == 0.f) wd = h;
    if (width > 0.f) wd = std::max(wd, px(width));
    else if (width < 0.f) wd = std::max(wd, ImGui::GetContentRegionAvail().x);
    const ImVec2 pos = w->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + wd, pos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) { if (strong) pop(); return false; }
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const Palette& p = pal();
    ImU32 bg = p.surface2, fg = p.text, border = p.line;
    switch (kind) {
        case Btn::Primary: bg = held ? p.accent_press : hovered ? p.accent_hover : p.accent; fg = p.on_accent; border = 0; break;
        case Btn::Danger: bg = held ? shade(p.danger_solid, 0.9f) : hovered ? shade(p.danger_solid, 1.1f) : p.danger_solid; fg = p.on_danger; border = 0; break;
        case Btn::Quiet: bg = held ? p.surface3 : hovered ? p.surface2 : 0; fg = hovered ? p.text : p.text2; border = 0; break;
        default: bg = held ? p.surface3 : hovered ? p.surface3 : p.surface2; break;
    }
    const float r = h * 0.5f;
    ImDrawList* dl = w->DrawList;
    if (bg) dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(bg), r);
    if (border) dl->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(border), r, 0, 1.f);
    ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, r);
    const float content = (ic != Icon::None ? isz + (ts.x > 0.f ? gap : 0.f) : 0.f) + ts.x;
    float x = bb.Min.x + (wd - content) * 0.5f;
    const float cy = bb.Min.y + h * 0.5f;
    if (ic != Icon::None) {
        icon(dl, ic, ImVec2(x + isz * 0.5f, cy), isz, ImGui::GetColorU32(fg));
        x += isz + gap;
    }
    if (ts.x > 0.f) dl->AddText(ImVec2(x, cy - ts.y * 0.5f), ImGui::GetColorU32(fg), label, end);
    if (strong) pop();
    return pressed;
}

bool icon_button(const char* id, Icon ic, const char* tooltip, Btn kind, float size) {
    ImGui::PushID(id);
    const bool r = button("", ic, kind, 0.f, size > 0.f ? size : kControlH);
    ImGui::PopID();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return r;
}

bool toggle(const char* id_str, bool* v) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(id_str);
    const float tw = std::round(px(44.f)), th = std::round(px(26.f));
    const ImVec2 pos = w->DC.CursorPos;
    // vertically centred on a frame-height line, so it sits level with the text beside it
    const float line_h = std::max(th, ImGui::GetFrameHeight());
    const ImRect bb(ImVec2(pos.x, pos.y + (line_h - th) * 0.5f), ImVec2(pos.x + tw, pos.y + (line_h - th) * 0.5f + th));
    ImGui::ItemSize(ImRect(pos, ImVec2(pos.x + tw, pos.y + line_h)));
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(id);
    }
    const Palette& p = pal();
    ImDrawList* dl = w->DrawList;
    const ImU32 track = *v ? (hovered ? p.accent_hover : p.accent) : (hovered ? p.line_strong : p.control_off);
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(track), th * 0.5f);
    const float kr = std::round(px(10.f)), inset = std::round(px(3.f));
    const float kx = *v ? bb.Max.x - inset - kr : bb.Min.x + inset + kr;
    dl->AddCircleFilled(ImVec2(kx, bb.Min.y + th * 0.5f + px(0.5f)), kr + px(0.5f), ImGui::GetColorU32(p.shadow));
    dl->AddCircleFilled(ImVec2(kx, bb.Min.y + th * 0.5f), kr, ImGui::GetColorU32(rgb(255, 255, 255)));
    ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, th * 0.5f);
    return pressed;
}

bool choice_card(const char* id_str, Icon ic, const char* title, const char* body, bool selected, ImVec2 size) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(id_str);
    const ImVec2 pos = w->DC.CursorPos;
    const ImVec2 sz(size.x > 0.f ? size.x : ImGui::GetContentRegionAvail().x, size.y > 0.f ? size.y : px(84.f));
    const ImRect bb(pos, ImVec2(pos.x + sz.x, pos.y + sz.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const Palette& p = pal();
    ImDrawList* dl = w->DrawList;
    const float r = px(kRadiusMd);
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(selected ? p.accent_tint : hovered ? p.surface2 : p.card), r);
    dl->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(selected ? p.accent : p.line), r, 0, selected ? px(2.f) : 1.f);
    const float pad = sp(S4);
    const float isz = px(22.f), disc = px(40.f);
    const ImVec2 ic_c(bb.Min.x + pad + disc * 0.5f, bb.Min.y + sz.y * 0.5f);
    dl->AddCircleFilled(ic_c, disc * 0.5f, ImGui::GetColorU32(selected ? with_alpha(p.accent, 0.22f) : p.surface2));
    icon(dl, ic, ic_c, isz, ImGui::GetColorU32(selected ? p.accent : p.text2));
    const float tx = ic_c.x + disc * 0.5f + sp(S3);
    const float wrap = bb.Max.x - pad - tx;
    push(Type::Strong);
    const float th = ImGui::GetFontSize();
    const ImVec2 tsz = ImGui::CalcTextSize(title, nullptr, false, wrap);
    pop();
    push(Type::Small);
    const ImVec2 bsz = body && *body ? ImGui::CalcTextSize(body, nullptr, false, wrap) : ImVec2(0, 0);
    pop();
    const float gap = body && *body ? sp(S1) : 0.f;
    float ty = bb.Min.y + (sz.y - (tsz.y + gap + bsz.y)) * 0.5f;
    push(Type::Strong);
    dl->AddText(nullptr, 0.f, ImVec2(tx, ty), ImGui::GetColorU32(p.text), title, nullptr, wrap);
    pop();
    (void)th;
    if (body && *body) {
        push(Type::Small);
        dl->AddText(nullptr, 0.f, ImVec2(tx, ty + tsz.y + gap), ImGui::GetColorU32(p.text2), body, nullptr, wrap);
        pop();
    }
    if (selected) icon(dl, Icon::Check, ImVec2(bb.Max.x - pad, bb.Min.y + pad), px(16.f), ImGui::GetColorU32(p.accent));
    ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, r);
    return pressed;
}

void chip(const char* label, ImU32 bg, ImU32 fg, Icon ic) {
    push(Type::Small);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float padx = sp(S2) + px(2.f), h = std::round(ts.y + px(8.f));
    const float isz = ic != Icon::None ? std::round(ts.y) : 0.f;
    const float wd = ts.x + 2.f * padx + (isz > 0.f ? isz + sp(S1) : 0.f);
    // vertically centred on the line it sits on
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float line_h = std::max(h, ImGui::GetFrameHeight());
    pos.y += (line_h - h) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + wd, pos.y + h), ImGui::GetColorU32(bg), h * 0.5f);
    float x = pos.x + padx;
    if (isz > 0.f) {
        icon(dl, ic, ImVec2(x + isz * 0.5f, pos.y + h * 0.5f), isz, ImGui::GetColorU32(fg));
        x += isz + sp(S1);
    }
    dl->AddText(ImVec2(x, pos.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(fg), label);
    ImGui::Dummy(ImVec2(wd, line_h));
    pop();
}

void section(const char* title, const char* sub) {
    push(Type::Title);
    ImGui::TextUnformatted(title);
    pop();
    if (sub && *sub) {
        push(Type::Body);
        ImGui::PushStyleColor(ImGuiCol_Text, pal().text2);
        ImGui::TextWrapped("%s", sub);
        ImGui::PopStyleColor();
        pop();
    }
    ImGui::Dummy(ImVec2(0, sp(S1)));
}

void empty_state(Icon ic, const char* title, const char* body, float height) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = height > 0.f ? height : std::max(avail.y, px(220.f));
    const float wrap = std::min(avail.x - sp(S6), px(460.f));
    push(Type::Title);
    const ImVec2 tsz = ImGui::CalcTextSize(title, nullptr, false, wrap);
    pop();
    push(Type::Body);
    const ImVec2 bsz = body ? ImGui::CalcTextSize(body, nullptr, false, wrap) : ImVec2(0, 0);
    pop();
    const float disc = px(72.f);
    const float total = disc + sp(S4) + tsz.y + sp(S2) + bsz.y;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float top = o.y + std::max(0.f, (h - total) * 0.5f);
    const float cx = o.x + avail.x * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = pal();
    dl->AddCircleFilled(ImVec2(cx, top + disc * 0.5f), disc * 0.5f, ImGui::GetColorU32(p.surface2));
    icon(dl, ic, ImVec2(cx, top + disc * 0.5f), px(32.f), ImGui::GetColorU32(p.text2));
    float y = top + disc + sp(S4);
    push(Type::Title);
    {   // centre each wrapped line
        const char* s = title;
        const char* e = title + std::strlen(title);
        while (s < e) {
            const char* le = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), s, e, wrap);
            if (le == s) le = s + 1;
            const ImVec2 lsz = ImGui::CalcTextSize(s, le);
            dl->AddText(ImVec2(cx - lsz.x * 0.5f, y), ImGui::GetColorU32(p.text), s, le);
            y += ImGui::GetTextLineHeight();
            s = le;
            while (s < e && *s == ' ') ++s;
        }
    }
    pop();
    y += sp(S2);
    if (body) {
        push(Type::Body);
        const char* s = body;
        const char* e = body + std::strlen(body);
        while (s < e) {
            const char* le = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), s, e, wrap);
            if (le == s) le = s + 1;
            const char* nl = static_cast<const char*>(std::memchr(s, '\n', static_cast<size_t>(le - s)));
            if (nl) le = nl;
            const ImVec2 lsz = ImGui::CalcTextSize(s, le);
            dl->AddText(ImVec2(cx - lsz.x * 0.5f, y), ImGui::GetColorU32(p.text2), s, le);
            y += ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 0.5f;
            s = le;
            while (s < e && (*s == ' ' || *s == '\n')) ++s;
        }
        pop();
    }
    ImGui::Dummy(ImVec2(avail.x, std::max(h, y - o.y)));
}

void text(Type t, ImU32 col, const char* fmt, ...) {
    push(t);
    ImGui::PushStyleColor(ImGuiCol_Text, col ? col : pal().text);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
    ImGui::PopStyleColor();
    pop();
}
void text_wrapped(Type t, ImU32 col, const char* fmt, ...) {
    push(t);
    ImGui::PushStyleColor(ImGuiCol_Text, col ? col : pal().text);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextWrappedV(fmt, ap);
    va_end(ap);
    ImGui::PopStyleColor();
    pop();
}

namespace {
struct CapGeom { float cap_w, cap_h, label_w; };
CapGeom cap_geom(const char* key, const char* label, bool pad) {
    CapGeom g{};
    push(Type::Small);
    const float kh = std::round(px(22.f));
    push(Type::Strong);
    const ImVec2 ks = ImGui::CalcTextSize(key);
    pop();
    g.cap_h = kh;
    g.cap_w = pad ? std::max(kh, ks.x + px(12.f)) : std::max(kh, ks.x + px(14.f));
    g.label_w = ImGui::CalcTextSize(label).x;
    pop();
    return g;
}
}  // namespace

float hint_width(const char* key, const char* label, bool pad) {
    const CapGeom g = cap_geom(key, label, pad);
    return g.cap_w + sp(S2) + g.label_w;
}

void hint(const char* key, const char* label, bool pad) {
    const CapGeom g = cap_geom(key, label, pad);
    const Palette& p = pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float line_h = ImGui::GetFrameHeight();
    const float y = pos.y + (line_h - g.cap_h) * 0.5f;
    if (pad) dl->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + g.cap_w, y + g.cap_h), ImGui::GetColorU32(p.cap), g.cap_h * 0.5f);
    else {
        dl->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + g.cap_w, y + g.cap_h), ImGui::GetColorU32(p.surface3), px(5.f));
        dl->AddRect(ImVec2(pos.x, y), ImVec2(pos.x + g.cap_w, y + g.cap_h), ImGui::GetColorU32(p.line_strong), px(5.f));
    }
    push(Type::Small);
    push(Type::Strong);
    ImGui::PushFont(nullptr, size_of(Type::Small) - 1.f);
    const ImVec2 ks = ImGui::CalcTextSize(key);
    dl->AddText(ImVec2(pos.x + (g.cap_w - ks.x) * 0.5f, y + (g.cap_h - ks.y) * 0.5f), ImGui::GetColorU32(pad ? p.on_cap : p.text), key);
    ImGui::PopFont();
    pop();
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + g.cap_w + sp(S2), pos.y + (line_h - ls.y) * 0.5f), ImGui::GetColorU32(p.text2), label);
    pop();
    ImGui::Dummy(ImVec2(g.cap_w + sp(S2) + g.label_w, line_h));
}

void stat(const char* value, const char* caption, ImU32 col) {
    push(Type::Display);
    ImGui::PushStyleColor(ImGuiCol_Text, col ? col : pal().text);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    pop();
    if (caption && *caption) {
        push(Type::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, pal().text2);
        ImGui::TextUnformatted(caption);
        ImGui::PopStyleColor();
        pop();
    }
}

bool key(const char* id_str, const char* label, Icon ic, bool accent, bool on, ImVec2 size) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(id_str);
    const ImVec2 pos = w->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const Palette& p = pal();
    const bool focused = ImGui::GetFocusID() == id && ImGui::GetCurrentContext()->NavCursorVisible;
    ImU32 bg = accent ? (held ? p.accent_press : hovered || focused ? p.accent_hover : p.accent) : on ? p.accent_tint
             : held ? p.surface3 : hovered || focused ? with_alpha(g_theme.surface, p.dark ? 0.16f : 0.13f) : p.surface2;
    const ImU32 fg = accent ? p.on_accent : on ? p.accent : p.text;
    ImDrawList* dl = w->DrawList;
    const float r = px(kRadiusSm);
    // a key cap: a darker lip under the face, then the face (pressed: the face sits down on the lip)
    const float lip = held ? 0.f : std::round(px(2.f));
    dl->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + lip), bb.Max, ImGui::GetColorU32(p.shadow), r);
    dl->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + (held ? lip : 0.f)), ImVec2(bb.Max.x, bb.Max.y - lip), ImGui::GetColorU32(bg), r);
    const ImVec2 c((bb.Min.x + bb.Max.x) * 0.5f, (bb.Min.y + bb.Max.y - lip) * 0.5f);
    if (ic != Icon::None) icon(dl, ic, c, std::round(ImGui::GetFontSize() * 1.2f), ImGui::GetColorU32(fg));
    if (label && *label) {
        const char* end = ImGui::FindRenderedTextEnd(label);
        const ImVec2 ts = ImGui::CalcTextSize(label, end);
        const float x = ic != Icon::None ? c.x + ImGui::GetFontSize() * 0.9f : c.x - ts.x * 0.5f;
        dl->AddText(ImVec2(x, c.y - ts.y * 0.5f), ImGui::GetColorU32(fg), label, end);
    }
    ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, r);
    return pressed;
}

}  // namespace nxrt::cheats::ui::theme
