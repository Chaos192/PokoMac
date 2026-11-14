// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// overlay_screens.h: the overlay's own screens in the theme.
#include "overlay_screens.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <utility>

#include "cheats_theme.h"

namespace nxrt::play::screens {
namespace th = cheats::ui::theme;
namespace {

// A row of button hints (key cap + what it does), wrapped to the width left.
void hints(const std::vector<std::pair<const char*, std::string>>& hs, bool pad) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = th::sp(th::S4);
    float x = 0.f;
    for (size_t i = 0; i < hs.size(); ++i) {
        const float w = th::hint_width(hs[i].first, hs[i].second.c_str(), pad);
        if (i && x + gap + w <= avail) {
            ImGui::SameLine(0.f, gap);
            x += gap + w;
        } else {
            x = w;
        }
        th::hint(hs[i].first, hs[i].second.c_str(), pad);
    }
}

// "Type" or "Type (right)": the face buttons by printed label, or where a Switch pad has them.
std::string face(const char* what, const char* where, bool by_label) { return by_label ? std::string(what) : std::string(what) + " (" + where + ")"; }

}  // namespace

// ---- the software keyboard --------------------------------------------------------------------------------------------------------
void keyboard(const KeyboardView& v, KeyboardResult* r) {
    const th::Palette& p = th::pal();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float w = std::max(th::px(320.f), std::min(th::px(760.f), disp.x - th::sp(th::S7)));
    if (th::begin_dialog("###nxrt_swkbd", v.keys ? (v.title.empty() ? "Keyboard" : v.title.c_str()) : nullptr, th::Icon::Keyboard, w, v.steam)) {
        if (!v.header.empty()) th::text_wrapped(th::Type::Body, p.text, "%s", v.header.c_str());
        if (!v.sub.empty()) th::text_wrapped(th::Type::Small, p.text2, "%s", v.sub.c_str());
        if (!v.header.empty() || !v.sub.empty()) ImGui::Dummy(ImVec2(0.f, th::sp(th::S1)));

        // the text field: drawn here, not an ImGui InputText, so the keyboard, the pad and the mouse all edit the same text and caret
        ImFont* font = th::font_of(th::Type::Body);
        const float big = std::round(th::size_of(th::Type::Title) * th::k());
        const float fh = std::round(big * 2.1f), pad = th::sp(th::S4);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float fw = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(fw, fh));
        const ImVec2 p1(p0.x + fw, p0.y + fh);
        if (r) {
            r->field_placed = true;
            r->field[0] = p0.x; r->field[1] = p0.y; r->field[2] = fw; r->field[3] = fh;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rad = th::px(th::kRadiusSm);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(p.sunken), rad);
        dl->AddRect(p0, p1, ImGui::GetColorU32(p.accent), rad, 0, std::max(1.5f, th::px(2.f)));   // the field has the focus
        const float cx = font->CalcTextSizeA(big, FLT_MAX, 0.0f, v.before_caret.c_str()).x;
        const float scroll = std::max(0.f, cx - (fw - 2.f * pad) + 2.f);   // keep the caret in view
        const float ty = p0.y + (fh - big) * 0.5f;
        dl->PushClipRect(ImVec2(p0.x + pad * 0.5f, p0.y), ImVec2(p1.x - pad * 0.5f, p1.y), true);
        if (v.shown.empty() && !v.guide.empty()) dl->AddText(font, big, ImVec2(p0.x + pad, ty), ImGui::GetColorU32(p.text3), v.guide.c_str());
        else dl->AddText(font, big, ImVec2(p0.x + pad - scroll, ty), ImGui::GetColorU32(p.text), v.shown.c_str());
        if (v.caret_on) {
            const float x = std::round(p0.x + pad - scroll + cx);
            dl->AddRectFilled(ImVec2(x, ty - big * 0.08f), ImVec2(x + std::max(1.5f, th::px(2.f)), ty + big * 1.08f), ImGui::GetColorU32(p.accent));
        }
        dl->PopClipRect();
        if (!v.count.empty()) {
            th::push(th::Type::Small);
            const float cw = ImGui::CalcTextSize(v.count.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fw - cw);
            ImGui::PushStyleColor(ImGuiCol_Text, p.text3);
            ImGui::TextUnformatted(v.count.c_str());
            ImGui::PopStyleColor();
            th::pop();
        }
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S1)));

        // the keys (the theme's key caps); the pad's key has the accent ring, the mouse clicks any of them
        if (v.keys) {
            const float gap = th::sp(th::S2);
            const int units = 10;
            const float unit = (ImGui::GetContentRegionAvail().x - gap * (units - 1)) / units;
            const float kh = std::round(th::px(th::kControlLg));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
            for (int row = 0; row < static_cast<int>(v.rows.size()); ++row) {
                for (int i = 0; i < static_cast<int>(v.rows[row].size()); ++i) {
                    const KeyboardKey& key = v.rows[row][i];
                    if (i) ImGui::SameLine();
                    ImGui::PushID(row * 32 + i);
                    ImGui::BeginDisabled(key.disabled);
                    const ImVec2 sz(unit * key.w + gap * (key.w - 1), kh);
                    if (th::key("##k", key.label.c_str(), th::Icon::None, key.accent, key.on, sz) && r) {
                        r->row = row;
                        r->key = i;
                    }
                    ImGui::EndDisabled();
                    if (row == v.sel_row && i == v.sel_key) {
                        const float o = th::px(3.f);
                        ImGui::GetWindowDrawList()->AddRect(ImVec2(ImGui::GetItemRectMin().x - o, ImGui::GetItemRectMin().y - o),
                                                            ImVec2(ImGui::GetItemRectMax().x + o, ImGui::GetItemRectMax().y + o),
                                                            ImGui::GetColorU32(p.accent), th::px(th::kRadiusSm) + o, 0, std::max(2.f, th::px(2.5f)));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S2)));
        if (!v.keys) {
            th::text_wrapped(th::Type::Small, p.text2, "Type on Steam's keyboard, Enter to confirm. Steam + X brings it back; any button shows the on-screen keys.");
        } else if (v.pad) {
            hints({{"A", face("Type", "right", v.by_label)}, {"B", face("Delete", "bottom", v.by_label)}, {"Y", face("Space", "left", v.by_label)},
                   {"X", face("Shift", "top", v.by_label)}, {"L", "Left"}, {"R", "Right"}, {"+", "OK"}, {"-", "Cancel"}},
                  true);
        } else {
            hints({{"Enter", "OK"}, {"Esc", "Cancel"}, {"Ctrl+V", "Paste"}}, false);
            if (v.steam) hints({{"Steam+X", "Steam keyboard"}}, false);
        }
    }
    th::end_dialog();
}

// ---- prompts --------------------------------------------------------------------------------------------------------------------------
QuitChoice quit_prompt(bool closing, bool pad, bool by_label) {
    const th::Palette& p = th::pal();
    QuitChoice c = QuitChoice::None;
    if (th::begin_dialog("###nxrt_quit", "Close the game?", th::Icon::Power)) {
        th::text_wrapped(th::Type::Body, p.text2, "%s", closing ? "Closing ..." : "Progress since the game's last save is lost.");
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S3)));
        const float gap = th::sp(th::S2);
        const float bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f / th::k();
        ImGui::BeginDisabled(closing);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        if (th::button("Quit", th::Icon::Power, th::Btn::Danger, bw, th::kControlLg)) c = QuitChoice::Quit;
        ImGui::SameLine();
        if (th::button("Cancel", th::Icon::None, th::Btn::Secondary, bw, th::kControlLg)) c = QuitChoice::Cancel;
        if (th::button("Settings", th::Icon::Gear, th::Btn::Quiet, -1.f, th::kControlH)) c = QuitChoice::Settings;
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S1)));
        if (pad) hints({{"A", face("Quit", "right", by_label)}, {"B", face("Cancel", "bottom", by_label)}, {"X", face("Settings", "top", by_label)}}, true);
        else hints({{"Enter", "Quit"}, {"Esc", "Cancel"}}, false);
    }
    th::end_dialog();
    return c;
}

bool ctrl_prompt(int lo, int hi, int players, bool pad, bool by_label) {
    const th::Palette& p = th::pal();
    bool cancel = false;
    const float w = std::max(th::px(300.f), std::min(th::px(500.f), ImGui::GetIO().DisplaySize.x - th::sp(th::S7)));
    if (th::begin_dialog("###nxrt_ctrl", "Connect controllers", th::Icon::Pad, w)) {
        lo = std::max(1, lo);
        if (hi > lo) th::text_wrapped(th::Type::Strong, p.text, "This game wants %d to %d players: %d connected.", lo, hi, players);
        else th::text_wrapped(th::Type::Strong, p.text, "This game wants %d players: %d connected.", lo, players);
        th::text_wrapped(th::Type::Body, p.text2, "Connect a controller for each player. Every controller is a player of its own (Settings > Controls > Players).");
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S3)));
        if (th::button("Cancel", th::Icon::None, th::Btn::Secondary, -1.f, th::kControlLg)) cancel = true;
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S1)));
        if (pad) hints({{"B", face("Cancel", "bottom", by_label)}}, true);
        else hints({{"Esc", "Cancel"}}, false);
    }
    th::end_dialog();
    return cancel;
}

// ---- notices --------------------------------------------------------------------------------------------------------------------------
void fps_counter(ImDrawList* dl, const std::string* lines, int n, int corner, ImVec2 frame) {
    th::Notice nt;
    nt.n = std::min(n, 4);
    for (int i = 0; i < nt.n; ++i) nt.lines[i] = lines[i].c_str();
    nt.size = std::clamp(std::round(frame.y / 48.f), 13.f, 36.f);
    nt.opacity = 0.88f;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    nt.right_align = right;
    const ImVec2 sz = th::notice_size(nt);
    const float m = std::round(nt.size * 0.8f);
    th::notice(dl, nt, ImVec2(right ? frame.x - m - sz.x : m, bottom ? frame.y - m - sz.y : m));
}

void shader_note(ImDrawList* dl, int pending, ImVec2 frame) {
    char text[96];
    if (pending > 0) std::snprintf(text, sizeof text, "Preparing shaders (%d left)", pending);
    else std::snprintf(text, sizeof text, "Preparing shaders (done)");
    th::Notice nt;
    nt.n = 1;
    nt.lines[0] = text;
    nt.icon = th::Icon::Refresh;
    nt.size = std::clamp(std::round(frame.y / 60.f), 12.f, 28.f);
    nt.opacity = 0.86f;
    const ImVec2 sz = th::notice_size(nt);
    th::notice(dl, nt, ImVec2(std::round((frame.x - sz.x) * 0.5f), frame.y - std::round(nt.size * 0.8f) - sz.y));
}

void save_notice(ImDrawList* dl, const char* reason, ImVec2 frame) {
    const th::Palette& p = th::pal();
    char text[160];
    std::snprintf(text, sizeof text, "%s. The last good save is kept.", reason && *reason ? reason : "unknown reason");
    if (text[0] >= 'a' && text[0] <= 'z') text[0] = static_cast<char>(text[0] - 'a' + 'A');   // the reasons are written for the log
    th::Notice nt;
    nt.n = 2;
    nt.lines[0] = "Save failed";
    nt.lines[1] = text;
    nt.icon = th::Icon::Warn;
    nt.icon_col = p.danger;
    nt.stripe = p.danger_solid;
    nt.size = std::clamp(std::round(frame.y / 45.f), 14.f, 36.f);
    nt.opacity = 0.96f;
    const ImVec2 sz = th::notice_size(nt);
    th::notice(dl, nt, ImVec2(std::round((frame.x - sz.x) * 0.5f), nt.size));
}

void touch_button(ImDrawList* dl, ImVec4 r, bool open) {
    const th::Palette& p = th::pal();
    const ImVec2 p0(r.x, r.y), p1(r.x + r.z, r.y + r.w);
    const float rad = std::min(r.w * 0.5f, th::px(th::kRadiusMd));
    dl->AddRectFilled(p0, p1, th::with_alpha(p.solid, open ? 0.92f : 0.8f), rad);
    dl->AddRect(p0, p1, th::with_alpha(p.line_strong, open ? 1.f : 0.8f), rad, 0, 1.f);
    const char* label = open ? "Close" : "Menu";
    ImFont* f = th::font_of(th::Type::Strong);
    const float size = std::round(th::size_of(th::Type::Small) * th::k());
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.f, label);
    dl->AddText(f, size, ImVec2(std::round(p0.x + (r.z - ts.x) * 0.5f), std::round(p0.y + (r.w - ts.y) * 0.5f)), th::with_alpha(p.text, open ? 1.f : 0.9f), label);
}

}  // namespace nxrt::play::screens
