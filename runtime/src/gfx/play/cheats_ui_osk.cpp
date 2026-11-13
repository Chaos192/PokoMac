// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's on-screen keyboard and hex / number keypad (cheats_ui_internal.h field / editor): every text field of the overlay
// can be filled with a controller alone. With a pad in use, activating a field (A) or pressing Y on it opens the keyboard for that field;
// the D-pad moves between keys, A types, B deletes, + confirms, - switches case. With a mouse or a keyboard the field is a normal text box.
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include "cheats_ui_internal.h"
#include "nxrt/input_source.h"

namespace nxrt::cheats::ui {
namespace {

struct Osk {
    bool open = false;
    bool focus = false;
    Keys keys = Keys::Text;
    ImGuiID target = 0;               // the field's id
    char* buf = nullptr;              // a char buffer field ...
    size_t n = 0;
    std::string* str = nullptr;       // ... or a std::string one
    std::string text;                 // what is being typed
    std::string title;
    bool shift = false;
    ImGuiID confirmed = 0;            // a field whose keyboard was confirmed (read once by that field)
};
Osk g_osk;

void osk_open_for(ImGuiID id, char* buf, size_t n, std::string* str, Keys keys, const char* title) {
    Osk& o = g_osk;
    o.open = true;
    o.focus = true;
    o.keys = keys;
    o.target = id;
    o.buf = buf;
    o.n = n;
    o.str = str;
    o.text = buf ? std::string(buf) : (str ? *str : std::string());
    o.title = title && *title ? title : "Enter text";
    o.shift = false;
}

int resize_cb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}

// Whether the field just drawn should open the keyboard: the pad activated it, or Y was pressed on it.
bool wants_osk() {
    if (!pad_mode()) return false;
    return ImGui::IsItemActivated() || (ImGui::IsItemFocused() && pad_pressed(hid::btn(hid::kBtnY)));
}
}  // namespace

bool osk_open() { return g_osk.open; }
void osk_show(std::string* s, Keys keys, const char* title) {
    if (!s) { g_osk.open = false; return; }      // null: closes it
    osk_open_for(0, nullptr, 0, s, keys, title);
}

bool field(const char* id, char* buf, size_t n, Keys keys, const char* hint, float width) {
    if (width != 0.f) ImGui::SetNextItemWidth(width * theme::k());   // widths are in UI units: they grow with the UI scale
    ImGuiInputTextFlags fl = ImGuiInputTextFlags_EnterReturnsTrue;
    if (keys == Keys::Hex) fl |= ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase;
    const bool entered = ImGui::InputTextWithHint(id, hint ? hint : "", buf, n, fl);
    const ImGuiID gid = ImGui::GetItemID();
    if (wants_osk()) {
        ImGui::ClearActiveID();                 // the keyboard edits the text, not the box
        osk_open_for(gid, buf, n, nullptr, keys, hint);
    }
    if (g_osk.confirmed == gid) {
        g_osk.confirmed = 0;
        return true;
    }
    return entered;
}

bool field(const char* id, std::string* s, Keys keys, const char* hint, float width) {
    if (width != 0.f) ImGui::SetNextItemWidth(width * theme::k());   // widths are in UI units: they grow with the UI scale
    ImGuiInputTextFlags fl = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize;
    if (keys == Keys::Hex) fl |= ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase;
    if (s->capacity() < 16) s->reserve(16);
    const bool entered = ImGui::InputTextWithHint(id, hint ? hint : "", s->data(), s->capacity() + 1, fl, &resize_cb, s);
    const ImGuiID gid = ImGui::GetItemID();
    if (wants_osk()) {
        ImGui::ClearActiveID();
        osk_open_for(gid, nullptr, 0, s, keys, hint);
    }
    if (g_osk.confirmed == gid) {
        g_osk.confirmed = 0;
        return true;
    }
    return entered;
}

bool editor(const char* id, std::string* s, ImVec2 size) {
    if (s->capacity() < 64) s->reserve(64);
    const bool changed = ImGui::InputTextMultiline(id, s->data(), s->capacity() + 1, size, ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                                                   &resize_cb, s);
    const ImGuiID gid = ImGui::GetItemID();
    if (wants_osk()) {
        ImGui::ClearActiveID();
        osk_open_for(gid, nullptr, 0, s, Keys::Text, "Edit the script");
    }
    if (g_osk.confirmed == gid) {
        g_osk.confirmed = 0;
        return true;
    }
    return changed;
}

void osk_draw() {
    Osk& o = g_osk;
    if (!o.open) return;
    namespace th = theme;
    const th::Palette& p = th::pal();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // a sheet at the foot of the screen, over everything, as a console's keyboard: the text being typed, big, then the keys
    const bool text = o.keys == Keys::Text;
    const float kw = std::round(th::px(text ? 54.f : 88.f)), kh = std::round(th::px(text ? 48.f : 56.f)), gap = std::round(th::px(6.f));
    const int cols = text ? 12 : (o.keys == Keys::Hex ? 6 : 3);
    const float pad = th::sp(th::S6);
    const float keys_w = cols * kw + (cols - 1) * gap;
    const float sheet_w = std::min(vp->WorkSize.x - th::sp(th::S4) * 2.f, std::max(keys_w, th::px(560.f)) + 2.f * pad);
    const float indent = std::max(0.f, (sheet_w - 2.f * pad - keys_w) * 0.5f);   // the keys centred in a wider sheet
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - th::sp(th::S4)), ImGuiCond_Always,
                            ImVec2(0.5f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(sheet_w, 0.f));
    if (o.focus) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.solid);
    ImGui::PushStyleColor(ImGuiCol_Border, p.line_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, th::sp(th::S5)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, th::px(th::kRadiusLg));
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    const bool shown = ImGui::Begin("###cheats_osk", nullptr, fl);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) {
        ImGui::End();
        return;
    }
    // what is being typed: the field's name, then the text with a caret in the accent
    th::text(th::Type::Small, p.text2, "%s", o.title.c_str());
    {
        const bool multi = o.text.find('\n') != std::string::npos;
        th::push(o.keys == Keys::Hex ? th::Type::Mono : th::Type::Title);
        const float lh = ImGui::GetTextLineHeight();
        const float h = std::round(multi ? lh * 4.f + th::sp(th::S4) : lh + th::sp(th::S5));
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), ImGui::GetColorU32(p.sunken), th::px(th::kRadiusSm));
        dl->AddRect(a, ImVec2(a.x + w, a.y + h), ImGui::GetColorU32(p.accent), th::px(th::kRadiusSm), 0, th::px(2.f));
        // the end of the text stays in view (a long script scrolls)
        const float inner = w - th::sp(th::S4) * 2.f;
        std::string shown_text = o.text;
        if (!multi)
            while (!shown_text.empty() && ImGui::CalcTextSize(shown_text.c_str()).x > inner - th::px(4.f)) shown_text.erase(shown_text.begin());
        else if (std::count(shown_text.begin(), shown_text.end(), '\n') > 3) {
            size_t cut = shown_text.size();
            for (int n = 0; n < 4 && cut != std::string::npos; ++n) cut = shown_text.rfind('\n', cut - 1);
            if (cut != std::string::npos) shown_text = shown_text.substr(cut + 1);
        }
        const ImVec2 tp(a.x + th::sp(th::S4), a.y + (multi ? th::sp(th::S2) : (h - lh) * 0.5f));
        dl->PushClipRect(a, ImVec2(a.x + w, a.y + h), true);
        dl->AddText(tp, ImGui::GetColorU32(p.text), shown_text.c_str());
        // the caret after the last character, blinking
        const size_t nl = shown_text.rfind('\n');
        const std::string last = nl == std::string::npos ? shown_text : shown_text.substr(nl + 1);
        const float cx = tp.x + ImGui::CalcTextSize(last.c_str()).x + th::px(1.f);
        const float cy = tp.y + static_cast<float>(std::count(shown_text.begin(), shown_text.end(), '\n')) * lh;
        if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + th::px(2.f), cy + lh), ImGui::GetColorU32(p.accent));
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(w, h));
        th::pop();
    }
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));

    bool ok = false, cancel = false;
    auto act = [&](const char* ins) {
        if (!std::strcmp(ins, "\b")) { if (!o.text.empty()) o.text.pop_back(); }
        else if (!std::strcmp(ins, "\x7f")) o.text.clear();
        else if (!std::strcmp(ins, "\x01")) o.shift = !o.shift;
        else if (!std::strcmp(ins, "\x02")) ok = true;
        else if (!std::strcmp(ins, "\x03")) cancel = true;
        else o.text += ins;
    };
    // one key; `first` takes the pad's focus when the keyboard opens
    auto key = [&](const char* id, const char* label, th::Icon ic, const char* ins, float w, bool accent = false, bool on = false, bool first = false) {
        if (th::key(id, label, ic, accent, on, ImVec2(w, kh))) act(ins);
        if (o.focus && first) {
            ImGui::SetKeyboardFocusHere(-1);
            o.focus = false;
        }
    };
    th::push(th::Type::Strong);
    ImGui::PushFont(nullptr, th::size_of(th::Type::Title) - 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    if (indent > 0.f) ImGui::Indent(indent);
    if (text) {
        static const char* rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'#", "zxcvbnm,./+*", "_:?!()<>\"&|%"};
        for (int ri = 0; ri < 5; ++ri) {
            for (int j = 0; rows[ri][j]; ++j) {
                const char c = rows[ri][j];
                char s[2] = {o.shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c, 0};
                if (j) ImGui::SameLine();
                ImGui::PushID(ri * 32 + j);
                key("##k", s, th::Icon::None, s, kw, false, false, c == 'q');
                ImGui::PopID();
            }
        }
        // the bottom row: shift, space, new line, delete, OK
        const float unit = kw + gap;
        key("##shift", "", th::Icon::Up, "\x01", unit * 2.f - gap, false, o.shift);
        ImGui::SameLine();
        key("##space", "space", th::Icon::None, " ", unit * 4.f - gap);
        ImGui::SameLine();
        key("##nl", "new line", th::Icon::None, "\n", unit * 2.f - gap);
        ImGui::SameLine();
        key("##del", "", th::Icon::Back, "\b", unit * 2.f - gap);
        ImGui::SameLine();
        key("##ok", "OK", th::Icon::Check, "\x02", unit * 2.f - gap, true);
    } else {
        static const char* hex_rows[] = {"789ABC", "456DEF", "123+-x", "0"};
        static const char* num_rows[] = {"789", "456", "123", "0.-"};
        const char** rows = o.keys == Keys::Hex ? hex_rows : num_rows;
        for (int i = 0; i < 4; ++i) {
            for (const char* c = rows[i]; *c; ++c) {
                char s[2] = {*c, 0};
                if (c != rows[i]) ImGui::SameLine();
                ImGui::PushID(i * 16 + static_cast<int>(c - rows[i]));
                key("##k", s, th::Icon::None, s, kw, false, false, *c == '1');
                ImGui::PopID();
            }
        }
        const float unit = kw + gap;
        const float half = (cols * unit - gap - gap) * 0.5f;
        key("##del", "", th::Icon::Back, "\b", half);
        ImGui::SameLine();
        key("##ok", "OK", th::Icon::Check, "\x02", half, true);
    }
    if (indent > 0.f) ImGui::Unindent(indent);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    th::pop();
    // the hints: what the pad's buttons do here
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
    if (pad_mode()) {
        th::hint("A", "Type", true);
        ImGui::SameLine(0, th::sp(th::S4));
        th::hint("B", "Delete", true);
        if (text) { ImGui::SameLine(0, th::sp(th::S4)); th::hint("-", "Shift", true); }
        ImGui::SameLine(0, th::sp(th::S4));
        th::hint("+", "OK", true);
    } else {
        th::hint("Enter", "OK", false);
        ImGui::SameLine(0, th::sp(th::S4));
        th::hint("Esc", "Cancel", false);
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - th::px(220.f)));
    if (th::button("Clear", th::Icon::Trash, th::Btn::Quiet, 0.f, th::kControlSm)) act("\x7f");
    ImGui::SameLine(0, th::sp(th::S2));
    if (th::button("Cancel", th::Icon::Close, th::Btn::Quiet, 0.f, th::kControlSm)) cancel = true;
    // the pad's shortcuts on the keyboard
    if (pad_pressed(hid::btn(hid::kBtnB))) { if (!o.text.empty()) o.text.pop_back(); }
    if (pad_pressed(hid::btn(hid::kBtnPlus))) ok = true;
    if (pad_pressed(hid::btn(hid::kBtnMinus))) o.shift = !o.shift;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) ok = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
    ImGui::End();
    if (ok) {
        if (o.buf && o.n) {
            std::strncpy(o.buf, o.text.c_str(), o.n - 1);
            o.buf[o.n - 1] = 0;
        } else if (o.str) {
            *o.str = o.text;
        }
        o.confirmed = o.target;
        o.open = false;
    } else if (cancel) {
        o.open = false;
    }
}

}  // namespace nxrt::cheats::ui
