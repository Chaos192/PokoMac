// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's themes and look settings (cheats_theme.h; docs/CHEATS.md "Themes"): a theme file is JSON (format 1) that sets the
// whole palette (panel, dialogs, surfaces, wells, text, lines, accent, ok / warn / danger, the backdrop), the corner radius, the panel's
// style (solid or glass), opacity, blur and tint, and optionally a font. The built-in themes (runtime/assets/cheats/themes: the launcher's
// five tones and a glass one) and the look settings' defaults and ranges (runtime/assets/cheats/look.json, which the launcher imports too)
// are compiled in. A file that does not read, or holds a wrong value, gives the default theme and the reason (logged once by the caller).
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../cheats/json_lite.h"
#include "cheats_theme.h"

namespace nxrt::cheats::ui::theme {
namespace {
namespace fs = std::filesystem;
using cheats::json::JVal;

#if defined(NXRT_HAVE_CHEATS_THEMES)
// kBuiltinThemes[] {name, json} and kLookSpec: generated from runtime/assets/cheats by cmake/gfx.cmake
#include "cheats_themes.inc"
#endif

// a path as UTF-8 in a std::string (C++20's u8string() is a std::u8string)
std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Sunrise, as runtime/assets/cheats/themes/sunrise.json has it: the fallback when even the built-in list is missing.
ThemeSpec sunrise_fallback() {
    ThemeSpec t;
    t.panel = IM_COL32(26, 20, 15, 255);
    t.dialog = IM_COL32(36, 28, 21, 255);
    t.surface = IM_COL32(255, 238, 220, 255);
    t.well = IM_COL32(0, 0, 0, 66);
    t.text = IM_COL32(248, 243, 237, 255);
    t.on_accent = IM_COL32(42, 23, 0, 255);
    t.ok = IM_COL32(159, 216, 255, 255);
    t.warn = IM_COL32(255, 179, 92, 255);
    t.danger = IM_COL32(255, 122, 107, 255);
    t.danger_solid = IM_COL32(216, 67, 76, 255);
    t.scrim = IM_COL32(10, 6, 3, 140);
    return t;
}

float rel_lum(ImU32 c) {
    auto ch = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const ImVec4 f = ImGui::ColorConvertU32ToFloat4(c);
    return 0.2126f * ch(f.x) + 0.7152f * ch(f.y) + 0.0722f * ch(f.z);
}
float contrast(ImU32 a, ImU32 b) {
    const float la = rel_lum(a), lb = rel_lum(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

// Reads a theme over `base` (its missing values stay base's). False with *err on the first wrong value.
bool read_theme(const std::string& text, const fs::path& folder, ThemeSpec* t, std::string* err) {
    JVal root;
    if (text.size() > (64u << 10)) { *err = "the file is bigger than 64 KB"; return false; }
    if (!cheats::json::parse_root(text, &root) || root.t != JVal::Obj) { *err = "it is not a JSON object"; return false; }
    auto num = [&](const char* key, float lo, float hi, float* out, float scale) -> bool {
        const JVal* v = root.get(key);
        if (!v) return true;
        if (v->t != JVal::Num || !(v->num >= lo && v->num <= hi)) {
            char b[160];
            std::snprintf(b, sizeof b, "\"%s\" must be a number from %g to %g", key, lo, hi);
            *err = b;
            return false;
        }
        *out = static_cast<float>(v->num) * scale;
        return true;
    };
    if (const JVal* f = root.get("format"); f && !(f->t == JVal::Num && f->num == 1)) { *err = "\"format\" must be 1"; return false; }
    for (const auto& kv : root.obj) {
        static const char* known[] = {"format", "name", "author", "description", "version", "style", "opacity", "blur", "tint", "radius",
                                      "accent", "colors", "colours", "font", "font_size"};
        if (std::none_of(std::begin(known), std::end(known), [&](const char* k) { return kv.first == k; })) {
            *err = "unknown key \"" + kv.first + "\"";
            return false;
        }
    }
    if (const JVal* v = root.get("name")) {
        if (v->t != JVal::Str || v->str.empty() || v->str.size() > 64) { *err = "\"name\" must be a text of 1 to 64 characters"; return false; }
        t->name = v->str;
    }
    if (const JVal* v = root.get("style")) {
        const std::string st = v->t == JVal::Str ? lower(v->str) : std::string();
        if (st != "solid" && st != "glass") { *err = "\"style\" must be \"solid\" or \"glass\""; return false; }
        t->glass = st == "glass";
    }
    const LookSpec& sp = look_spec();
    if (!num("opacity", static_cast<float>(sp.opacity.min), static_cast<float>(sp.opacity.max), &t->opacity, 0.01f)) return false;
    if (!num("blur", 0.f, 100.f, &t->blur, 0.01f)) return false;
    if (!num("tint", 0.f, 100.f, &t->tint, 0.01f)) return false;
    if (!num("radius", 0.f, 40.f, &t->radius, 1.f)) return false;
    if (!num("font_size", 12.f, 22.f, &t->font_size, 1.f)) return false;
    if (const JVal* v = root.get("accent")) {
        if (v->t != JVal::Str || !parse_colour(v->str, &t->accent)) { *err = "\"accent\" is not a colour (like \"#ffc23d\")"; return false; }
        t->accent |= IM_COL32_A_MASK;
    }
    const JVal* cols = root.get("colors");
    if (!cols) cols = root.get("colours");
    if (cols) {
        if (cols->t != JVal::Obj) { *err = "\"colors\" must be an object of colours"; return false; }
        struct Slot { const char* key; ImU32* at; };
        const Slot slots[] = {{"panel", &t->panel}, {"dialog", &t->dialog}, {"surface", &t->surface}, {"card", &t->card}, {"well", &t->well},
                              {"text", &t->text}, {"text2", &t->text2}, {"text3", &t->text3}, {"line", &t->line},
                              {"line_strong", &t->line_strong}, {"on_accent", &t->on_accent}, {"ok", &t->ok}, {"warn", &t->warn},
                              {"danger", &t->danger}, {"danger_solid", &t->danger_solid}, {"scrim", &t->scrim}};
        for (const auto& kv : cols->obj) {
            const Slot* s = nullptr;
            for (const Slot& x : slots)
                if (kv.first == x.key) s = &x;
            if (!s) { *err = "unknown colour \"" + kv.first + "\""; return false; }
            ImU32 c = 0;
            const bool ok = kv.second.t == JVal::Str ? parse_colour(kv.second.str, &c) : false;
            if (!ok) { *err = "colour \"" + kv.first + "\" is not a colour (like \"#1a140f\" or \"#1a140fcc\")"; return false; }
            *s->at = c ? c : 1;     // fully transparent black stays "given" (0 means: derive it)
        }
    }
    if (const JVal* v = root.get("font")) {
        if (v->t != JVal::Str) { *err = "\"font\" must be the name of a font file next to the theme"; return false; }
        if (!v->str.empty()) {
            fs::path f = fs::u8path(v->str);
            if (f.is_relative()) f = folder / f;
            std::error_code ec;
            const std::string ext = lower(u8(f.extension()));
            if (ext != ".ttf" && ext != ".otf") { *err = "\"font\" must be a .ttf or .otf file"; return false; }
            if (!fs::is_regular_file(f, ec)) { *err = "the font " + u8(f) + " is not there"; return false; }
            t->font = u8(f);
        }
    }
    // readable: the text on the panel (as opaque) at least 4.5:1 (WCAG AA), the accent's text on the accent at least 3:1
    const float ct = contrast(t->text | IM_COL32_A_MASK, t->panel | IM_COL32_A_MASK);
    if (ct < 4.5f) {
        char b[160];
        std::snprintf(b, sizeof b, "the text is hard to read on the panel (contrast %.1f:1, it needs 4.5:1)", ct);
        *err = b;
        return false;
    }
    if (t->on_accent && contrast(t->on_accent | IM_COL32_A_MASK, t->accent) < 3.f) {
        *err = "\"on_accent\" is hard to read on the accent (it needs a contrast of 3:1)";
        return false;
    }
    return true;
}

ThemeSpec builtin(const std::string& name, bool* found) {
    *found = false;
    ThemeSpec t = sunrise_fallback();
#if defined(NXRT_HAVE_CHEATS_THEMES)
    for (const auto& b : kBuiltinThemes)
        if (name == b.name) {
            std::string err;
            ThemeSpec r = sunrise_fallback();
            if (read_theme(reinterpret_cast<const char*>(b.json), fs::path(), &r, &err)) {
                r.source = "builtin:" + name;
                *found = true;
                return r;
            }
        }
#endif
    if (name == "sunrise") *found = true;
    return t;
}

}  // namespace

bool parse_colour(const std::string& text, ImU32* out) {
    std::string s = lower(trim(text));
    int r = 0, g = 0, b = 0, a = 255;
    if (!s.empty() && s[0] == '#') {
        const std::string h = s.substr(1);
        if (h.empty() || h.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
        const unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
        if (h.size() == 3) { r = ((v >> 8) & 0xF) * 17; g = ((v >> 4) & 0xF) * 17; b = (v & 0xF) * 17; }
        else if (h.size() == 6) { r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF; }
        else if (h.size() == 8) { r = (v >> 24) & 0xFF; g = (v >> 16) & 0xFF; b = (v >> 8) & 0xFF; a = v & 0xFF; }
        else return false;
    } else {
        // rgb(r, g, b) / rgba(r, g, b, a) / r, g, b (a: 0..1)
        if (s.rfind("rgba(", 0) == 0 || s.rfind("rgb(", 0) == 0) {
            const size_t open = s.find('('), close = s.rfind(')');
            if (close == std::string::npos || close < open) return false;
            s = s.substr(open + 1, close - open - 1);
        }
        double v[4] = {0, 0, 0, 1};
        int n = 0;
        const char* p = s.c_str();
        while (*p && n < 4) {
            char* end = nullptr;
            v[n] = std::strtod(p, &end);
            if (end == p) return false;
            ++n;
            p = end;
            while (*p == ' ') ++p;
            if (*p == ',') ++p;
            while (*p == ' ') ++p;
        }
        if (*p || n < 3) return false;
        for (int i = 0; i < 3; ++i)
            if (v[i] < 0 || v[i] > 255) return false;
        if (v[3] < 0 || v[3] > 1) return false;
        r = static_cast<int>(v[0] + 0.5);
        g = static_cast<int>(v[1] + 0.5);
        b = static_cast<int>(v[2] + 0.5);
        a = static_cast<int>(v[3] * 255.0 + 0.5);
    }
    *out = IM_COL32(r, g, b, a);
    return true;
}

const LookSpec& look_spec() {
    static const LookSpec spec = [] {
        LookSpec s;
#if defined(NXRT_HAVE_CHEATS_THEMES)
        JVal root;
        if (cheats::json::parse_root(reinterpret_cast<const char*>(kLookSpec), &root) && root.t == JVal::Obj) {
            auto range = [&](const char* key, Range* r) {
                const JVal* o = root.get(key);
                if (!o) return;
                if (const JVal* d = o->get("default")) r->def = d->t == JVal::Num ? static_cast<int>(d->num) : -1;
                if (const JVal* v = o->get("min")) r->min = v->as_int(r->min);
                if (const JVal* v = o->get("max")) r->max = v->as_int(r->max);
            };
            range("opacity", &s.opacity);
            range("blur", &s.blur);
            range("tint", &s.tint);
            range("scale", &s.scale);
            range("hud_opacity", &s.hud_opacity);
            auto str = [&](const char* key, std::string* out) {
                if (const JVal* o = root.get(key))
                    if (const JVal* d = o->get("default"); d && d->t == JVal::Str) *out = d->str;
            };
            str("theme", &s.theme);
            str("accent", &s.accent);
            str("dock", &s.dock);
            if (const JVal* o = root.get("animations"))
                if (const JVal* d = o->get("default")) s.animations = d->as_bool(true);
        }
#endif
        return s;
    }();
    return spec;
}

ThemeSpec load_theme(const std::string& name_or_path, std::string* err) {
    const std::string want = trim(name_or_path).empty() ? look_spec().theme : trim(name_or_path);
    bool found = false;
    ThemeSpec t = builtin(lower(want), &found);
    if (found) return t;
    // a file: a path (".json"), or a bare name that is not built in
    const fs::path path = fs::u8path(want);
    std::error_code ec;
    std::string why;
    if (lower(u8(path.extension())) != ".json") why = "it is not a built-in theme (" + want + ") nor a .json file";
    else if (!fs::is_regular_file(path, ec)) why = "the file " + want + " is not there";
    else {
        std::ifstream f(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ThemeSpec r = builtin(look_spec().theme, &found);     // what the file leaves out is the default theme's
        r.name = u8(path.stem());
        if (read_theme(text, path.parent_path(), &r, &why)) {
            r.source = u8(path);
            return r;
        }
        why = u8(path.filename()) + ": " + why;
    }
    if (err) *err = why;
    return builtin(look_spec().theme, &found);
}

}  // namespace nxrt::cheats::ui::theme
