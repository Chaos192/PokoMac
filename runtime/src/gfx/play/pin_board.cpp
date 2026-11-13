// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Pins: the shared engine of the menus' pinned readouts (pin_board.h).
#include "pin_board.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "cheats_theme.h"
#include "play_types.h"

namespace nxrt::play::pins {

namespace th = nxrt::cheats::ui::theme;

namespace {
std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t b = 0;
    for (;;) {
        const size_t e = s.find(sep, b);
        out.push_back(trim_copy(s.substr(b, e == std::string::npos ? std::string::npos : e - b)));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}
bool id_ok(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    return true;
}
bool int_of(const std::string& s, int lo, int hi, int* out) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || !end || *end) return false;
    *out = static_cast<int>(std::clamp<long>(v, lo, hi));
    return true;
}
// The name keeps what reads in the text form: no '|' or ';', no line breaks, at most 40 characters.
std::string clean_name(const std::string& n) {
    std::string o;
    for (char c : n)
        if (c != '|' && c != ';' && c != '\n' && c != '\r') o += c;
    o = trim_copy(o);
    if (o.size() > 40) o.resize(40);
    return o;
}
}  // namespace

const char* layout_name(Layout l) { return l == Layout::Horizontal ? "horizontal" : l == Layout::Compact ? "compact" : "vertical"; }
bool parse_layout(const std::string& s, Layout* out) {
    std::string l = trim_copy(s);
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "vertical" || l == "column") *out = Layout::Vertical;
    else if (l == "horizontal" || l == "row") *out = Layout::Horizontal;
    else if (l == "compact" || l == "line") *out = Layout::Compact;
    else return false;
    return true;
}

bool Group::operator==(const Group& o) const {
    return id == o.id && name == o.name && items == o.items && x == o.x && y == o.y && size == o.size && opacity == o.opacity && layout == o.layout &&
           shown == o.shown && key == o.key && pad == o.pad;
}

Group* Board::find(const std::string& id) {
    for (Group& g : groups)
        if (g.id == id) return &g;
    return nullptr;
}

bool parse_group(const std::string& text, Group* out, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    const std::string t = trim_copy(text);
    const size_t colon = t.find(':');
    if (colon == std::string::npos) return fail("'" + t + "': no 'id:' at the start");
    Group g;
    g.id = trim_copy(t.substr(0, colon));
    if (!id_ok(g.id)) return fail("'" + g.id + "': not a group id (letters, digits, _ - .)");
    const std::vector<std::string> fields = split(t.substr(colon + 1), ';');
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string& f = fields[i];
        if (i == 0) {   // the items
            if (f.empty() || f == "none") continue;
            for (const std::string& it : split(f, '+')) {
                if (!id_ok(it)) return fail(g.id + ": '" + it + "' is not an item");
                g.items.push_back(it);
            }
            continue;
        }
        if (f.empty()) continue;
        const size_t eq = f.find('=');
        if (eq == std::string::npos) return fail(g.id + ": '" + f + "' has no '='");
        const std::string k = trim_copy(f.substr(0, eq)), v = trim_copy(f.substr(eq + 1));
        if (k == "name") g.name = clean_name(v);
        else if (k == "at") {
            const std::vector<std::string> xy = split(v, ',');
            int x = 0, y = 0;
            if (xy.size() != 2 || !int_of(xy[0], -1, 100000, &x) || !int_of(xy[1], -1, 100000, &y)) return fail(g.id + ": at=" + v + " is not x,y");
            g.x = static_cast<float>(x);
            g.y = static_cast<float>(y);
        } else if (k == "size") {
            if (!int_of(v, 50, 200, &g.size)) return fail(g.id + ": size=" + v);
        } else if (k == "opacity") {
            if (!int_of(v, 20, 100, &g.opacity)) return fail(g.id + ": opacity=" + v);
        } else if (k == "layout") {
            if (!parse_layout(v, &g.layout)) return fail(g.id + ": layout=" + v + " (vertical, horizontal or compact)");
        } else if (k == "key") {
            if (!parse_key_chord(v, &g.key)) return fail(g.id + ": key=" + v + " is not a key");
        } else if (k == "pad") {
            if (!parse_pad_combo(v, &g.pad)) return fail(g.id + ": pad=" + v + " is not a controller chord");
        } else if (k == "shown") {
            g.shown = v == "1" || v == "true" || v == "yes" || v == "on";
        } else {
            return fail(g.id + ": unknown field '" + k + "'");
        }
    }
    *out = g;
    return true;
}

std::string format_group(const Group& g) {
    std::string o = g.id + ":";
    for (size_t i = 0; i < g.items.size(); ++i) o += (i ? "+" : "") + g.items[i];
    if (g.items.empty()) o += "none";
    if (!g.name.empty()) o += ";name=" + clean_name(g.name);
    if (g.x >= 0.f && g.y >= 0.f) {
        char b[48];
        std::snprintf(b, sizeof b, ";at=%d,%d", static_cast<int>(std::lround(g.x)), static_cast<int>(std::lround(g.y)));
        o += b;
    }
    o += ";size=" + std::to_string(g.size) + ";opacity=" + std::to_string(g.opacity) + ";layout=" + layout_name(g.layout);
    o += ";key=" + key_chord_text(g.key) + ";pad=" + pad_combo_text(g.pad) + ";shown=" + (g.shown ? "1" : "0");
    return o;
}

bool parse_groups(const std::string& text, std::vector<Group>* out, std::string* err) {
    std::vector<Group> gs;
    const std::string t = trim_copy(text);
    if (!t.empty() && t != "none") {
        for (const std::string& part : split(t, '|')) {
            if (part.empty()) continue;
            Group g;
            if (!parse_group(part, &g, err)) return false;
            for (const Group& o : gs)
                if (o.id == g.id) {
                    if (err) *err = "two groups are called '" + g.id + "'";
                    return false;
                }
            gs.push_back(std::move(g));
        }
    }
    *out = std::move(gs);
    return true;
}

std::string format_groups(const std::vector<Group>& gs) {
    std::string o;
    for (size_t i = 0; i < gs.size(); ++i) o += (i ? "|" : "") + format_group(gs[i]);
    return o;
}

std::string new_group_id(const Board& b) {
    for (int n = 1;; ++n) {
        const std::string id = n == 1 ? "g" : "g" + std::to_string(n);
        bool used = false;
        for (const Group& g : b.groups) used = used || g.id == id;
        if (!used) return id;
    }
}

std::string window_name(const char* scope, const Group& g) { return std::string("##pin_") + scope + "_" + g.id; }

// ---- drawing ----------------------------------------------------------------------------------------------------------------------------

namespace {
ImU32 tone_colour(int tone, float a) {
    const th::Palette& p = th::pal();
    ImU32 c = p.text;
    switch (tone) {
        case 1: c = p.accent; break;
        case 2: c = p.warn; break;
        case 3: c = p.ok; break;
        case 4: c = p.text3; break;
        default: break;
    }
    return th::with_alpha(c | IM_COL32_A_MASK, a * (((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.f));
}

// A sparkline of a ring of values in [x0, x0 + w] x [y0, y0 + h]: the line in the accent, a faint fill under it.
void sparkline(ImDrawList* dl, const Line& l, ImVec2 p0, float w, float h, float alpha) {
    if (!l.graph || l.graph_n < 2) return;
    float mx = l.graph_max;
    if (mx <= 0.f)
        for (int i = 0; i < l.graph_n; ++i) mx = std::max(mx, l.graph[i]);
    if (mx <= 0.f) mx = 1.f;
    const th::Palette& p = th::pal();
    ImVec2 pts[256];
    const int n = std::min(l.graph_n, 256);
    const int start = l.graph_n - n;
    for (int i = 0; i < n; ++i) {
        const float v = l.graph[(l.graph_head + start + i) % l.graph_n];
        pts[i] = ImVec2(p0.x + w * static_cast<float>(i) / static_cast<float>(n - 1), p0.y + h - h * std::clamp(v / mx, 0.f, 1.f));
    }
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), th::with_alpha(p.surface2 | IM_COL32_A_MASK, 0.10f * alpha), th::px(3.f));
    dl->AddPolyline(pts, n, th::with_alpha(p.accent | IM_COL32_A_MASK, alpha), ImDrawFlags_None, std::max(1.f, th::px(1.5f)));
}
}  // namespace

void draw(Board& b, const Items& items, bool interactive, const char* scope, bool* moved) {
    if (!b.all_shown) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float k = th::k();
    const th::Palette& p = th::pal();
    float stack_y = th::sp(th::S4);   // where an unplaced group goes: under the ones before it
    std::vector<Line> lines;
    for (Group& g : b.groups) {
        if (!g.shown || g.items.empty()) continue;
        lines.clear();
        for (const std::string& it : g.items) items(it, lines);
        if (lines.empty()) continue;
        const float sc = std::clamp(g.size, 50, 200) / 100.f;
        const float a = std::clamp(g.opacity, 20, 100) / 100.f;
        auto s = [&](float v) { return std::round(th::px(v) * sc); };
        const ImVec2 at = g.x >= 0.f ? ImVec2(vp->WorkPos.x + g.x * k, vp->WorkPos.y + g.y * k) : ImVec2(vp->WorkPos.x + th::sp(th::S4), vp->WorkPos.y + stack_y);
        ImGui::SetNextWindowPos(at, interactive ? ImGuiCond_Appearing : ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(1.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, th::with_alpha(th::theme().panel | IM_COL32_A_MASK, a));
        ImGui::PushStyleColor(ImGuiCol_Border, th::with_alpha(p.line_strong | IM_COL32_A_MASK, 0.5f * a));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s(10.f), s(7.f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, s(th::kRadiusSm));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!interactive) fl |= ImGuiWindowFlags_NoInputs;
        const std::string wname = window_name(scope, g);
        if (ImGui::Begin(wname.c_str(), nullptr, fl)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float gw = s(132.f), gh = s(26.f);
            th::push(th::Type::Small, sc);
            const float fs = ImGui::GetFontSize();
            if (!g.name.empty() && g.layout != Layout::Compact) {
                ImGui::PushStyleColor(ImGuiCol_Text, tone_colour(4, a));
                ImGui::TextUnformatted(g.name.c_str());
                ImGui::PopStyleColor();
            }
            if (g.layout == Layout::Vertical) {
                // two columns: the labels, the values (right aligned); a graph line spans both
                float lw = 0.f, vw = 0.f;
                th::push(th::Type::Strong, sc);
                for (const Line& l : lines) vw = std::max(vw, ImGui::CalcTextSize(l.value.c_str()).x);
                th::pop();
                for (const Line& l : lines) lw = std::max(lw, ImGui::CalcTextSize(l.label.c_str()).x);
                const float gap = s(14.f);
                bool marks = false;
                for (const Line& l : lines) marks = marks || l.icon >= 0;
                const float mark_w = marks ? fs + s(6.f) : 0.f;
                const float row_w = std::max(mark_w + lw + gap + vw, gw);
                for (const Line& l : lines) {
                    const ImVec2 c = ImGui::GetCursorScreenPos();
                    const ImVec2 mc(c.x + fs * 0.45f, c.y + fs * 0.62f);
                    if (l.icon == 0) dl->AddCircleFilled(mc, fs * 0.28f, tone_colour(l.tone == 0 ? 4 : l.tone, a));
                    else if (l.icon > 0) th::icon(dl, static_cast<th::Icon>(l.icon), mc, fs * 0.9f, tone_colour(l.tone == 0 ? 4 : l.tone, a));
                    if (!l.label.empty()) dl->AddText(ImVec2(c.x + mark_w, c.y + s(2.f)), tone_colour(4, a), l.label.c_str());
                    th::push(th::Type::Strong, sc);
                    const float tw = ImGui::CalcTextSize(l.value.c_str()).x;
                    dl->AddText(ImVec2(c.x + row_w - tw, c.y), tone_colour(l.tone, a), l.value.c_str());
                    const float h = ImGui::GetFontSize();
                    th::pop();
                    ImGui::Dummy(ImVec2(row_w, h));
                    if (l.graph) {
                        const ImVec2 g0 = ImGui::GetCursorScreenPos();
                        sparkline(dl, l, g0, row_w, gh, a);
                        ImGui::Dummy(ImVec2(row_w, gh));
                    }
                }
            } else if (g.layout == Layout::Horizontal) {
                // cells side by side: the value over its label
                bool first = true;
                for (const Line& l : lines) {
                    if (!first) ImGui::SameLine(0.f, s(16.f));
                    first = false;
                    ImGui::BeginGroup();
                    th::push(th::Type::Strong, sc);
                    const float vw = ImGui::CalcTextSize(l.value.c_str()).x;
                    th::pop();
                    const float lw = ImGui::CalcTextSize(l.label.c_str()).x;
                    const float cw = std::max({vw, lw, l.graph ? gw * 0.75f : 0.f});
                    const ImVec2 c = ImGui::GetCursorScreenPos();
                    th::push(th::Type::Strong, sc);
                    const float vh = ImGui::GetFontSize();
                    dl->AddText(c, tone_colour(l.tone, a), l.value.c_str());
                    th::pop();
                    float y = c.y + vh + s(1.f);
                    if (l.graph) {
                        sparkline(dl, l, ImVec2(c.x, y), cw, gh * 0.7f, a);
                        y += gh * 0.7f + s(2.f);
                    }
                    dl->AddText(ImVec2(c.x, y), tone_colour(4, a), l.label.c_str());
                    ImGui::Dummy(ImVec2(cw, y + fs - c.y));
                    ImGui::EndGroup();
                }
            } else {
                // compact: one line, "value label · value label"
                const ImVec2 c = ImGui::GetCursorScreenPos();
                float x = c.x;
                const float dot = s(10.f);
                bool first = true;
                for (const Line& l : lines) {
                    if (!first) {
                        dl->AddCircleFilled(ImVec2(x + dot * 0.5f, c.y + fs * 0.55f), s(1.6f), tone_colour(4, a));
                        x += dot;
                    }
                    first = false;
                    th::push(th::Type::Strong, sc);
                    dl->AddText(ImVec2(x, c.y), tone_colour(l.tone, a), l.value.c_str());
                    x += ImGui::CalcTextSize(l.value.c_str()).x + s(4.f);
                    th::pop();
                    if (!l.label.empty()) {
                        dl->AddText(ImVec2(x, c.y + s(1.f)), tone_colour(4, a), l.label.c_str());
                        x += ImGui::CalcTextSize(l.label.c_str()).x + s(2.f);
                    }
                }
                ImGui::Dummy(ImVec2(x - c.x, fs + s(2.f)));
            }
            th::pop();
            ImGuiWindow* w = ImGui::GetCurrentWindow();
            if (interactive) {
                if (ImGui::IsWindowHovered()) ImGui::BringWindowToDisplayFront(w);   // the part out from under the panel lifts it to be dragged
                // dragged to a new place: kept in UI units (the same place at any resolution)
                const float nx = std::round((w->Pos.x - vp->WorkPos.x) / k), ny = std::round((w->Pos.y - vp->WorkPos.y) / k);
                if ((nx != g.x || ny != g.y) && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (g.x >= 0.f || w->Pos.x != at.x || w->Pos.y != at.y)) {
                    g.x = std::max(0.f, nx);
                    g.y = std::max(0.f, ny);
                    if (moved) *moved = true;
                }
            }
            if (g.x < 0.f) stack_y += w->Size.y + th::sp(th::S2);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
}

}  // namespace nxrt::play::pins
