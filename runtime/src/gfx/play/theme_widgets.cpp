// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// cheats_theme.h "ImGui's stock widgets, drawn in the theme" and "windows": the controls the overlay's screens use in place of ImGui's own,
// drawn as the launcher draws its controls (launcher/src/components.css: .slider, .select, progress, the switch's knob). Each keeps the
// ImGui widget's behaviour (ItemAdd / ButtonBehavior / SliderBehavior, so mouse, keyboard and pad navigation and the focus ring work as
// before); only the drawing is the theme's.
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "cheats_theme.h"

namespace nxrt::cheats::ui::theme {
namespace {

ImU32 col(ImU32 c) { return ImGui::GetColorU32(c); }   // with the style's alpha (a disabled control fades)

// The box of a checkbox or radio: a control-sized square on the line, the label after it.
struct BoxItem {
    ImRect box, total;
    bool hovered = false, held = false, pressed = false;
    ImGuiID id = 0;
    bool ok = false;
};
BoxItem box_item(const char* label, float size_px) {
    BoxItem b;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return b;
    b.id = w->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end, false);
    const float box = std::round(px(size_px));
    const float line_h = std::max(ImGui::GetFrameHeight(), box);
    const ImVec2 pos = w->DC.CursorPos;
    b.box = ImRect(ImVec2(pos.x, pos.y + std::round((line_h - box) * 0.5f)), ImVec2(pos.x + box, pos.y + std::round((line_h - box) * 0.5f) + box));
    const float lw = ts.x > 0.f ? ImGui::GetStyle().ItemInnerSpacing.x + ts.x : 0.f;
    b.total = ImRect(pos, ImVec2(pos.x + box + lw, pos.y + line_h));
    ImGui::ItemSize(b.total);
    if (!ImGui::ItemAdd(b.total, b.id)) return b;
    b.ok = true;
    b.pressed = ImGui::ButtonBehavior(b.total, b.id, &b.hovered, &b.held);
    if (ts.x > 0.f)
        w->DrawList->AddText(ImVec2(b.box.Max.x + ImGui::GetStyle().ItemInnerSpacing.x, pos.y + (line_h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                             label, end);
    return b;
}

void tooltip_v(const char* fmt, va_list ap) {
    const Palette& p = pal();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(S3), sp(S2)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, p.solid);
    ImGui::PushStyleColor(ImGuiCol_Border, p.line_strong);
    if (ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None)) {
        push(Type::Small);
        ImGui::PushTextWrapPos(px(380.f));
        ImGui::TextV(fmt, ap);
        ImGui::PopTextWrapPos();
        pop();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

}  // namespace

// ---- checkbox, radio ------------------------------------------------------------------------------------------------------------------
bool checkbox(const char* label, bool* v) {
    BoxItem b = box_item(label, 20.f);
    if (!b.ok) return false;
    if (b.pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(b.id);
    }
    const Palette& p = pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = px(kRadiusXs);
    if (*v) {
        dl->AddRectFilled(b.box.Min, b.box.Max, col(b.held ? p.accent_press : b.hovered ? p.accent_hover : p.accent), r);
        icon(dl, Icon::Check, b.box.GetCenter(), b.box.GetWidth() * 0.82f, col(p.on_accent));
    } else {
        dl->AddRectFilled(b.box.Min, b.box.Max, col(b.hovered ? p.sunken_hover : p.sunken), r);
        dl->AddRect(b.box.Min, b.box.Max, col(b.hovered ? p.text3 : p.line_strong), r, 0, std::max(1.f, px(1.5f)));
    }
    ImGui::RenderNavCursor(b.total, b.id, ImGuiNavRenderCursorFlags_None, r);
    return b.pressed;
}

bool radio(const char* label, bool active) {
    BoxItem b = box_item(label, 20.f);
    if (!b.ok) return false;
    if (b.pressed) ImGui::MarkItemEdited(b.id);
    const Palette& p = pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = b.box.GetCenter();
    const float rr = b.box.GetWidth() * 0.5f;
    if (active) {
        dl->AddCircleFilled(c, rr, col(b.hovered ? p.accent_hover : p.accent));
        dl->AddCircleFilled(c, rr * 0.42f, col(p.on_accent));
    } else {
        dl->AddCircleFilled(c, rr, col(b.hovered ? p.sunken_hover : p.sunken));
        dl->AddCircle(c, rr - 0.5f, col(b.hovered ? p.text3 : p.line_strong), 0, std::max(1.f, px(1.5f)));
    }
    ImGui::RenderNavCursor(b.total, b.id, ImGuiNavRenderCursorFlags_None, rr);
    return b.pressed;
}

bool radio(const char* label, int* v, int value) {
    const bool pressed = radio(label, *v == value);
    if (pressed) *v = value;
    return pressed;
}

// ---- slider -----------------------------------------------------------------------------------------------------------------------------
namespace {
bool slider_scalar(const char* label, ImGuiDataType type, void* v, const void* lo, const void* hi, const char* format, ImGuiSliderFlags flags) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = w->GetID(label);
    const float width = ImGui::CalcItemWidth();
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, label_end, false);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = w->DC.CursorPos;
    const ImRect frame_bb(pos, ImVec2(pos.x + width, pos.y + h));
    const ImRect total_bb(pos, ImVec2(frame_bb.Max.x + (label_size.x > 0.f ? style.ItemInnerSpacing.x + label_size.x : 0.f), frame_bb.Max.y));
    const bool input_ok = (flags & ImGuiSliderFlags_NoInput) == 0;
    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id, &frame_bb, input_ok ? ImGuiItemFlags_Inputable : 0)) return false;
    if (!format) format = ImGui::DataTypeGetInfo(type)->PrintFmt;

    // as ImGui::SliderScalar: Ctrl+click (or the pad's "input" activation) types a value; a click or the pad's A grabs the knob
    const bool hovered = ImGui::ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
    bool typing = input_ok && ImGui::TempInputIsActive(id);
    if (!typing) {
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
        const bool make_active = clicked || g.NavActivateId == id;
        if (make_active && clicked) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
        if (make_active && input_ok && ((clicked && g.IO.KeyCtrl) || (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput))))
            typing = true;
        if (make_active) std::memcpy(&g.ActiveIdValueOnActivation, v, ImGui::DataTypeGetInfo(type)->Size);
        if (make_active && !typing) {
            ImGui::SetActiveID(id, w);
            ImGui::SetFocusID(id, w);
            ImGui::FocusWindow(w);
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }
    if (typing) {
        const bool clamp = (flags & ImGuiSliderFlags_ClampOnInput) != 0;
        return ImGui::TempInputScalar(frame_bb, id, label, type, v, format, clamp ? lo : nullptr, clamp ? hi : nullptr);
    }

    // the value, in the column after the track (Small type, the secondary colour, figures tabular)
    const Palette& p = pal();
    char value[64];
    const char* value_end = value + ImGui::DataTypeFormatString(value, IM_COUNTOF(value), type, v, format);
    push(Type::Small);
    const float vw = std::max(px(44.f), ImGui::CalcTextSize(value, value_end).x);
    const float vh = ImGui::GetFontSize();
    pop();
    const float gap = sp(S3);
    const ImRect slide_bb(frame_bb.Min, ImVec2(std::max(frame_bb.Min.x + h, frame_bb.Max.x - vw - gap), frame_bb.Max.y));
    ImRect grab_bb;
    const bool changed = ImGui::SliderBehavior(slide_bb, id, type, v, lo, hi, format, flags, &grab_bb);
    if (changed) ImGui::MarkItemEdited(id);
    // SliderBehavior maps the mouse along [Min + 2 + grab / 2, Max - 2 - grab / 2]: the track spans the same, the knob sits on the grab
    const float grab = grab_bb.Max.x > grab_bb.Min.x ? grab_bb.GetWidth() : style.GrabMinSize;
    const float x0 = slide_bb.Min.x + 2.f + grab * 0.5f, x1 = slide_bb.Max.x - 2.f - grab * 0.5f;
    const float kx = grab_bb.Max.x > grab_bb.Min.x ? grab_bb.GetCenter().x : x0;
    const float cy = frame_bb.GetCenter().y;
    const float th = std::max(2.f, std::round(px(4.f))), kr = std::round(px(9.f));
    ImDrawList* dl = w->DrawList;
    const bool active = g.ActiveId == id;
    dl->AddRectFilled(ImVec2(x0 - th * 0.5f, cy - th * 0.5f), ImVec2(x1 + th * 0.5f, cy + th * 0.5f), col(hovered ? p.line_strong : p.control_off), th);
    dl->AddRectFilled(ImVec2(x0 - th * 0.5f, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), col(active || hovered ? p.accent_hover : p.accent), th);
    const float kk = active ? kr * 1.12f : kr;   // the launcher's thumb grows a little while held
    dl->AddCircleFilled(ImVec2(kx, cy + px(1.f)), kk + px(1.f), col(p.shadow));
    dl->AddCircleFilled(ImVec2(kx, cy), kk, col(IM_COL32(255, 255, 255, 255)));
    dl->AddCircle(ImVec2(kx, cy), kk, col(p.line), 0, 1.f);
    ImGui::RenderNavCursor(ImRect(ImVec2(kx - kk, cy - kk), ImVec2(kx + kk, cy + kk)), id, ImGuiNavRenderCursorFlags_None, kk);
    push(Type::Small);
    dl->AddText(ImVec2(frame_bb.Max.x - ImGui::CalcTextSize(value, value_end).x, cy - vh * 0.5f), col(active ? p.text : p.text2), value, value_end);
    pop();
    if (label_size.x > 0.f)
        dl->AddText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + (h - label_size.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label,
                    label_end);
    return changed;
}
}  // namespace

bool slider_int(const char* label, int* v, int lo, int hi, const char* format, ImGuiSliderFlags flags) {
    return slider_scalar(label, ImGuiDataType_S32, v, &lo, &hi, format, flags);
}
bool slider_float(const char* label, float* v, float lo, float hi, const char* format, ImGuiSliderFlags flags) {
    return slider_scalar(label, ImGuiDataType_Float, v, &lo, &hi, format, flags);
}

// ---- select (combo) ---------------------------------------------------------------------------------------------------------------------
bool begin_combo(const char* label, const char* preview, ImGuiComboFlags flags) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const Palette& p = pal();
    // the field's rectangle as BeginCombo lays it out (the chevron and the preview are drawn over it after)
    const ImVec2 pos = w->DC.CursorPos;
    const float h = ImGui::GetFrameHeight();
    const float width = (flags & ImGuiComboFlags_NoPreview) ? h : ImGui::CalcItemWidth();
    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + h));
    ImDrawList* dl = w->DrawList;
    const ImGuiID id = w->GetID(label);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.surface2);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, p.surface3);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, p.surface3);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, p.solid);
    ImGui::PushStyleColor(ImGuiCol_Border, p.line_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(S1) + px(2.f), sp(S1) + px(2.f)));
    const bool open = ImGui::BeginCombo(label, nullptr, (flags | ImGuiComboFlags_NoArrowButton) & ~ImGuiComboFlags_NoPreview);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    const float isz = std::round(ImGui::GetFontSize() * 0.95f);
    const float cx = bb.Max.x - style.FramePadding.x - isz * 0.5f;
    if (preview && !(flags & ImGuiComboFlags_NoPreview)) {
        // into the field's own list (with the list open the current window is the popup), clipped short of the chevron
        dl->PushClipRect(bb.Min, ImVec2(cx - isz * 0.5f - sp(S1), bb.Max.y), true);
        dl->AddText(ImVec2(bb.Min.x + style.FramePadding.x, bb.Min.y + style.FramePadding.y), col(p.text), preview);
        dl->PopClipRect();
    }
    const bool hot = open || ImGui::GetHoveredID() == id;
    icon(dl, Icon::ChevronDown, ImVec2(cx, bb.GetCenter().y), isz, col(hot ? p.text : p.text2));
    return open;
}

void end_combo() { ImGui::EndCombo(); }

bool combo(const char* label, int* cur, const char* const items[], int n) {
    const char* preview = *cur >= 0 && *cur < n ? items[*cur] : "";
    if (!begin_combo(label, preview)) return false;
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const bool sel = i == *cur;
        if (ImGui::Selectable(items[i], sel) && !sel) {
            *cur = i;
            changed = true;
        }
        if (sel) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    end_combo();
    if (changed) ImGui::MarkItemEdited(ImGui::GetCurrentContext()->LastItemData.ID);
    return changed;
}

bool combo(const char* label, int* cur, const char* items_zero_separated) {
    const char* items[64];
    int n = 0;
    for (const char* s = items_zero_separated; *s && n < 64; s += std::strlen(s) + 1) items[n++] = s;
    return combo(label, cur, items, n);
}

// ---- number field ---------------------------------------------------------------------------------------------------------------------
bool input_int(const char* label, int* v, int step, int step_fast, ImGuiInputTextFlags flags) {
    if (step <= 0) return ImGui::InputScalar(label, ImGuiDataType_S32, v, nullptr, nullptr, "%d", flags);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const char* label_end = ImGui::FindRenderedTextEnd(label);
    const float h = ImGui::GetFrameHeight();
    const float gap = sp(S1);
    const float width = ImGui::CalcItemWidth();
    bool changed = false;
    ImGui::BeginGroup();
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(std::max(1.f, width - 2.f * (h + gap)));
    if (ImGui::InputScalar("", ImGuiDataType_S32, v, nullptr, nullptr, "%d", flags)) changed = true;
    const bool fast = ImGui::GetIO().KeyCtrl && step_fast > 0;
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    ImGui::SameLine(0.f, gap);
    if (button("##minus", Icon::Minus, Btn::Secondary, h / k(), h / k())) {
        *v -= fast ? step_fast : step;
        changed = true;
    }
    ImGui::SameLine(0.f, gap);
    if (button("##plus", Icon::Plus, Btn::Secondary, h / k(), h / k())) {
        *v += fast ? step_fast : step;
        changed = true;
    }
    ImGui::PopItemFlag();
    if (label != label_end) {
        ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label, label_end);
    }
    ImGui::PopID();
    ImGui::EndGroup();
    if (changed) ImGui::MarkItemEdited(ImGui::GetCurrentContext()->LastItemData.ID);
    return changed;
}

// ---- disclosure (tree node, collapsing header) ---------------------------------------------------------------------------------------
namespace {
bool disclosure(const char* label, ImGuiTreeNodeFlags flags, bool header) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    bool open = ImGui::TreeNodeUpdateNextOpen(id, flags);
    if (header) push(Type::Strong);
    const ImVec2 ts = ImGui::CalcTextSize(label, end, false);
    const float h = header ? ImGui::GetFrameHeight() : std::max(ImGui::GetTextLineHeight() + sp(S1) * 2.f, ImGui::GetFrameHeight() * 0.85f);
    const float isz = std::round(ImGui::GetFontSize() * 0.9f);
    const ImVec2 pos = w->DC.CursorPos;
    const float wd = header || (flags & (ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_SpanFullWidth))
                         ? std::max(ImGui::GetContentRegionAvail().x, isz + sp(S2) + ts.x)
                         : isz + sp(S2) + ts.x + sp(S3);
    const ImRect bb(pos, ImVec2(pos.x + wd, pos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) {
        if (header) pop();
        if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen) && !header) ImGui::TreePushOverrideID(id);
        return open;
    }
    bool hov = false, held = false;
    if (ImGui::ButtonBehavior(bb, id, &hov, &held)) {
        open = !open;
        ImGui::TreeNodeSetOpen(id, open);
    }
    // the pad and the arrows: right opens, left closes (as ImGui's tree nodes)
    ImGuiContext& g = *GImGui;
    if (g.NavId == id && g.NavMoveDir == ImGuiDir_Left && open) {
        open = false;
        ImGui::TreeNodeSetOpen(id, false);
        ImGui::NavMoveRequestCancel();
    }
    if (g.NavId == id && g.NavMoveDir == ImGuiDir_Right && !open) {
        open = true;
        ImGui::TreeNodeSetOpen(id, true);
        ImGui::NavMoveRequestCancel();
    }
    const Palette& p = pal();
    ImDrawList* dl = w->DrawList;
    const float r = px(kRadiusSm);
    if (header) dl->AddRectFilled(bb.Min, bb.Max, col(held ? p.surface3 : hov ? p.surface2 : p.card), r);
    else if (hov) dl->AddRectFilled(bb.Min, bb.Max, col(held ? p.surface2 : p.surface1), r);
    const float x = bb.Min.x + (header ? sp(S3) : sp(S1));
    icon(dl, open ? Icon::ChevronDown : Icon::Chevron, ImVec2(x + isz * 0.5f, bb.GetCenter().y), isz, col(hov ? p.text : p.text2));
    dl->AddText(ImVec2(x + isz + sp(S2), bb.Min.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label, end);
    ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, r);
    if (header) pop();
    if (open && !header && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) ImGui::TreePushOverrideID(id);
    return open;
}
}  // namespace

bool tree_node(const char* label, ImGuiTreeNodeFlags flags) { return disclosure(label, flags, false); }
bool collapsing_header(const char* label, ImGuiTreeNodeFlags flags) { return disclosure(label, flags | ImGuiTreeNodeFlags_NoTreePushOnOpen, true); }

// ---- progress -------------------------------------------------------------------------------------------------------------------------
void progress(float fraction, ImVec2 size, const char* overlay) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return;
    const Palette& p = pal();
    const float line_h = ImGui::GetFrameHeight();
    const ImVec2 pos = w->DC.CursorPos;
    const float total = size.x > 0.f ? size.x : std::max(px(40.f), ImGui::GetContentRegionAvail().x + (size.x < 0.f ? size.x + 1.f : 0.f));
    const float hh = size.y > 0.f ? size.y : line_h;
    push(Type::Small);
    const float tw = overlay && *overlay ? ImGui::CalcTextSize(overlay).x + sp(S3) : 0.f;
    const float th = std::max(3.f, std::round(px(6.f)));
    const ImRect bb(pos, ImVec2(pos.x + total, pos.y + hh));
    ImGui::ItemSize(bb);
    if (ImGui::ItemAdd(bb, 0)) {
        ImDrawList* dl = w->DrawList;
        const float cy = bb.GetCenter().y;
        const float x0 = bb.Min.x, x1 = std::max(x0 + th, bb.Max.x - tw);
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), col(p.surface2), th);
        if (fraction < 0.f) {   // indeterminate: a segment sweeping along
            const float t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.8, 1.0));
            const float seg = (x1 - x0) * 0.3f;
            const float a = x0 + (x1 - x0 + seg) * t - seg;
            dl->PushClipRect(ImVec2(x0, cy - th), ImVec2(x1, cy + th), true);
            dl->AddRectFilled(ImVec2(a, cy - th * 0.5f), ImVec2(a + seg, cy + th * 0.5f), col(p.accent), th);
            dl->PopClipRect();
        } else if (fraction > 0.f) {
            const float f = std::min(1.f, fraction);
            dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(std::max(x0 + th, x0 + (x1 - x0) * f), cy + th * 0.5f), col(p.accent), th);
        }
        if (tw > 0.f) {
            const ImVec2 ts = ImGui::CalcTextSize(overlay);
            dl->AddText(ImVec2(bb.Max.x - ts.x, cy - ts.y * 0.5f), col(p.text2), overlay);
        }
    }
    pop();
}

// ---- colour ---------------------------------------------------------------------------------------------------------------------------
bool colour_swatch(const char* id_str, ImU32 c, float size, bool selected) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID id = w->GetID(id_str);
    const float d = std::round(size > 0.f ? size : ImGui::GetFrameHeight());
    const ImVec2 pos = w->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + d, pos.y + d));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hov = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
    const Palette& p = pal();
    ImDrawList* dl = w->DrawList;
    const ImVec2 cc = bb.GetCenter();
    const float r = d * 0.5f;
    if (selected) dl->AddCircle(cc, r - 1.f, col(p.text), 0, std::max(1.5f, px(2.f)));
    dl->AddCircleFilled(cc, selected ? r - px(4.f) : (hov ? r : r - px(1.f)), col(c | IM_COL32_A_MASK));
    dl->AddCircle(cc, selected ? r - px(4.f) : r - px(1.f), col(p.line_strong), 0, 1.f);
    ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, r);
    return pressed;
}

bool colour_edit(const char* label, float rgb[3]) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    auto to8 = [](float f) { return static_cast<int>(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
    const ImU32 c = IM_COL32(to8(rgb[0]), to8(rgb[1]), to8(rgb[2]), 255);
    bool changed = false;
    ImGui::BeginGroup();
    ImGui::PushID(label);
    colour_swatch("##swatch", c);
    ImGui::SameLine(0.f, sp(S2));
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", to8(rgb[0]), to8(rgb[1]), to8(rgb[2]));
    ImGui::SetNextItemWidth(std::max(px(96.f), ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - sp(S2)));
    push(Type::Mono);
    if (ImGui::InputText("##hex", buf, sizeof buf, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsNoBlank)) {
        ImU32 parsed = 0;
        const size_t n = std::strlen(buf);
        if ((n == 7 || n == 4) && parse_colour(buf, &parsed)) {
            rgb[0] = ((parsed >> IM_COL32_R_SHIFT) & 0xFF) / 255.f;
            rgb[1] = ((parsed >> IM_COL32_G_SHIFT) & 0xFF) / 255.f;
            rgb[2] = ((parsed >> IM_COL32_B_SHIFT) & 0xFF) / 255.f;
            changed = true;
        }
    }
    pop();
    if (label != end) {
        ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label, end);
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}

// ---- graph ----------------------------------------------------------------------------------------------------------------------------
void plot_lines(const char* label, const float* values, int n, int offset, const char* overlay, float lo, float hi, ImVec2 size) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems || n <= 0) return;
    const ImGuiID id = w->GetID(label);
    const Palette& p = pal();
    const float wd = size.x > 0.f ? size.x : size.x < 0.f ? std::max(px(40.f), ImGui::GetContentRegionAvail().x + size.x) : ImGui::CalcItemWidth();
    const float hh = size.y > 0.f ? size.y : px(48.f);
    const ImVec2 pos = w->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + wd, pos.y + hh));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return;
    if (lo == FLT_MAX || hi == FLT_MAX) {
        float mn = FLT_MAX, mx = -FLT_MAX;
        for (int i = 0; i < n; ++i) { mn = std::min(mn, values[i]); mx = std::max(mx, values[i]); }
        if (lo == FLT_MAX) lo = mn;
        if (hi == FLT_MAX) hi = mx;
    }
    if (hi <= lo) hi = lo + 1.f;
    ImDrawList* dl = w->DrawList;
    const float r = px(kRadiusSm);
    dl->AddRectFilled(bb.Min, bb.Max, col(p.sunken), r);
    const float pad = sp(S1) + px(2.f);
    const ImRect in(ImVec2(bb.Min.x + pad, bb.Min.y + pad), ImVec2(bb.Max.x - pad, bb.Max.y - pad));
    ImVec2 pts[512];
    const int m = std::min(n, 512);
    for (int i = 0; i < m; ++i) {
        const float v = values[(offset + n - m + i) % n];
        const float t = std::clamp((v - lo) / (hi - lo), 0.f, 1.f);
        pts[i] = ImVec2(in.Min.x + in.GetWidth() * (m > 1 ? static_cast<float>(i) / (m - 1) : 0.f), in.Max.y - in.GetHeight() * t);
    }
    // the area under the line, faint, then the line in the accent
    for (int i = 0; i + 1 < m; ++i) {
        dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, in.Max.y), ImVec2(pts[i].x, in.Max.y), col(p.accent_tint));
    }
    dl->AddPolyline(pts, m, col(p.accent), ImDrawFlags_None, std::max(1.f, px(1.5f)));
    if (overlay && *overlay) {
        push(Type::Small);
        dl->AddText(ImVec2(in.Min.x + px(2.f), in.Min.y), col(p.text2), overlay);
        pop();
    }
}

// ---- tooltips -------------------------------------------------------------------------------------------------------------------------
void tooltip(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tooltip_v(fmt, ap);
    va_end(ap);
}

void item_tooltip(const char* fmt, ...) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    va_list ap;
    va_start(ap, fmt);
    tooltip_v(fmt, ap);
    va_end(ap);
}

// ---- windows --------------------------------------------------------------------------------------------------------------------------
bool begin_window(const char* title, bool* open, ImGuiWindowFlags flags) {
    const Palette& p = pal();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.solid);
    ImGui::PushStyleColor(ImGuiCol_Border, p.line_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(kRadiusLg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(S5), sp(S4)));
    const bool shown = ImGui::Begin(title, open, flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown || (flags & ImGuiWindowFlags_NoTitleBar)) return shown;
    // the header: the title (the text before "##" / "###"), a close button when the window can be closed
    const char* end = ImGui::FindRenderedTextEnd(title);
    const float x0 = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    push(Type::Strong);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::TextUnformatted(title, end);
    ImGui::PopStyleColor();
    pop();
    if (open) {
        const float bs = kControlSm;
        ImGui::SameLine(x0 + avail - px(bs));
        if (icon_button("##close", Icon::Close, "Close", Btn::Quiet, bs)) *open = false;
    }
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + avail, a.y), col(p.line), 1.f);
    ImGui::Dummy(ImVec2(0.f, sp(S1)));
    return shown;
}

bool begin_dialog(const char* id, const char* title, Icon ic, float width, bool top) {
    const Palette& p = pal();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, col(p.modal_dim));
    const float w = width > 0.f ? width : std::max(px(280.f), std::min(px(460.f), disp.x - sp(S7)));
    if (top) ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, std::round(disp.y * 0.04f)), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    else ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.solid);
    ImGui::PushStyleColor(ImGuiCol_Border, p.line_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(kRadiusLg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(S6), sp(S5)));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    const bool shown = ImGui::Begin(id, nullptr, wf);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) return false;
    if (title && *title) {
        // the header: the icon in a soft accent disc, the title in Title type
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        push(Type::Title);
        const float th = ImGui::GetFontSize();
        const float disc = ic != Icon::None ? std::round(th * 1.9f) : 0.f;
        if (disc > 0.f) {
            const ImVec2 c(o.x + disc * 0.5f, o.y + disc * 0.5f);
            dl->AddCircleFilled(c, disc * 0.5f, col(p.accent_tint));
            icon(dl, ic, c, std::round(th * 1.05f), col(p.accent));
        }
        const float tx = disc > 0.f ? disc + sp(S3) : 0.f;
        const float hrow = std::max(disc, th);
        ImGui::SetCursorScreenPos(ImVec2(o.x + tx, o.y + (hrow - th) * 0.5f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(title);
        ImGui::PopTextWrapPos();
        pop();
        ImGui::SetCursorScreenPos(ImVec2(o.x, std::max(ImGui::GetCursorScreenPos().y, o.y + hrow)));
        ImGui::Dummy(ImVec2(0.f, sp(S3)));
    }
    return true;
}

void end_dialog() { ImGui::End(); }

// ---- notices on the foreground list ---------------------------------------------------------------------------------------------------
namespace {
struct NoticeGeom { float pad, gap, stripe, isz, size, w, h, line_h[4], line_w[4]; };
NoticeGeom notice_geom(const Notice& n) {
    NoticeGeom g{};
    g.size = n.size > 0.f ? n.size : size_of(Type::Body) * k();
    g.pad = std::round(g.size * 0.55f);
    g.gap = std::round(g.size * 0.2f);
    g.stripe = n.stripe ? std::max(3.f, std::round(g.size * 0.22f)) : 0.f;
    g.isz = n.icon != Icon::None ? std::round(g.size * 1.15f) : 0.f;
    float tw = 0.f, th = 0.f;
    for (int i = 0; i < n.n && i < 4; ++i) {
        ImFont* f = font_of(i == 0 ? Type::Strong : Type::Body);
        const float sz = i == 0 ? g.size : std::round(g.size * 0.86f);
        const ImVec2 s = f->CalcTextSizeA(sz, FLT_MAX, 0.f, n.lines[i] ? n.lines[i] : "");
        g.line_w[i] = s.x;
        g.line_h[i] = s.y;
        tw = std::max(tw, s.x);
        th += s.y + (i ? g.gap : 0.f);
    }
    g.w = g.stripe + g.pad * 2.f + (g.isz > 0.f ? g.isz + g.pad * 0.75f : 0.f) + tw;
    g.h = g.pad * 2.f + std::max(th, g.isz);
    return g;
}
}  // namespace

ImVec2 notice_size(const Notice& n) {
    const NoticeGeom g = notice_geom(n);
    return ImVec2(g.w, g.h);
}

void notice(ImDrawList* dl, const Notice& n, ImVec2 pos) {
    const NoticeGeom g = notice_geom(n);
    const Palette& p = pal();
    const float r = std::round(g.size * 0.6f);
    const ImVec2 b(pos.x + g.w, pos.y + g.h);
    // a soft shadow, the dialog colour, a hairline; a stripe in the notice's colour on the left
    dl->AddRectFilled(ImVec2(pos.x, pos.y + g.size * 0.12f), ImVec2(b.x, b.y + g.size * 0.12f), with_alpha(IM_COL32(0, 0, 0, 255), 0.22f * n.opacity), r);
    dl->AddRectFilled(pos, b, with_alpha(p.solid, n.opacity), r);
    dl->AddRect(pos, b, with_alpha(p.line_strong, std::min(1.f, n.opacity)), r, 0, 1.f);
    if (g.stripe > 0.f) {   // an accent bar inset on the left, in the notice's colour
        const float inset = std::round(g.pad * 0.5f);
        dl->AddRectFilled(ImVec2(pos.x + inset, pos.y + inset), ImVec2(pos.x + inset + g.stripe, b.y - inset), n.stripe, g.stripe * 0.5f);
    }
    float x = pos.x + g.stripe + g.pad;
    if (g.isz > 0.f) {
        icon(dl, n.icon, ImVec2(x + g.isz * 0.5f, pos.y + g.h * 0.5f), g.isz, n.icon_col ? n.icon_col : p.accent);
        x += g.isz + g.pad * 0.75f;
    }
    float tw = 0.f;
    for (int i = 0; i < n.n && i < 4; ++i) tw = std::max(tw, g.line_w[i]);
    float th = 0.f;
    for (int i = 0; i < n.n && i < 4; ++i) th += g.line_h[i] + (i ? g.gap : 0.f);
    float y = pos.y + (g.h - th) * 0.5f;
    for (int i = 0; i < n.n && i < 4; ++i) {
        ImFont* f = font_of(i == 0 ? Type::Strong : Type::Body);
        const float sz = i == 0 ? g.size : std::round(g.size * 0.86f);
        const float lx = n.right_align ? x + tw - g.line_w[i] : x;
        dl->AddText(f, sz, ImVec2(lx, y), i == 0 ? p.text : p.text2, n.lines[i] ? n.lines[i] : "");
        y += g.line_h[i] + g.gap;
    }
}

}  // namespace nxrt::cheats::ui::theme
