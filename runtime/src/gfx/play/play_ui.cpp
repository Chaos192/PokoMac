// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings: the F1 menu of the overlay. The contract is play_ui.h. The menu edits a working copy of the settings: changes are
// handed to the host as they are made (Apply and save does it at once), Revert and Factory defaults replace the copy, and nothing else
// reaches the game. Its look is the overlay's theme (cheats_theme.h); its rows and cards are play_ui_widgets.h.
#include "play_ui.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "cheats_theme.h"
#include "cheats_ui.h"
#include "hotkeys.h"
#include "nxrt/audio_sink.h"
#include "nxrt/game.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "perf_pins.h"
#include "pin_board.h"
#include "play_game.h"
#include "play_ui_widgets.h"

namespace nxrt::cheats::ui {
ImTextureData* game_icon();   // cheats_icon.cpp: the title folder's icon, also the F1 menu's
}

namespace nxrt::play {

namespace th = nxrt::cheats::ui::theme;

namespace widgets {
const std::string& take_tip();
}

namespace {

namespace hid = nxrt::hid;


constexpr int kWindowModeCount = 3;
constexpr const char* kWindowModeNames[kWindowModeCount] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
constexpr const char* kWindowModeKeys[kWindowModeCount] = {"windowed", "borderless", "exclusive"};
constexpr int kSizePresets[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr int kSizePresetCount = static_cast<int>(sizeof kSizePresets / sizeof kSizePresets[0]);
constexpr int kFpsChoices[] = {-1, 0, 30, 40, 45, 60, 72, 90, 120, 144, 165, 240, 360};   // -1 = the refresh rate
// The aspect ratio combo: Auto stores 0, the presets store their ratio, Custom shows a slider (index kAspectCustom, no value of its own).
constexpr const char* kAspectNames[] = {"Auto", "16:9", "16:10", "21:9", "32:9", "Custom"};
constexpr double kAspectValues[] = {0.0, 16.0 / 9.0, 1.6, 21.0 / 9.0, 32.0 / 9.0};
constexpr int kAspectCustom = 5;
constexpr int kAspectNameCount = static_cast<int>(sizeof kAspectNames / sizeof kAspectNames[0]);
constexpr const char* kAnisoNames[] = {"Game default", "2x", "4x", "8x", "16x"};
constexpr int kAnisoValues[] = {0, 2, 4, 8, 16};
constexpr int kAnisoCount = static_cast<int>(sizeof kAnisoValues / sizeof kAnisoValues[0]);
constexpr const char* kAstcValues[] = {"fast", "high", "uncompressed"};
constexpr const char* kAstcNames[] = {"Fast", "High (sharper, built in the background)", "Uncompressed (lots of video memory)"};
constexpr int kAstcCount = static_cast<int>(sizeof kAstcValues / sizeof kAstcValues[0]);
constexpr Trig kTrigChoices[] = {Trig::Press, Trig::Tap, Trig::Hold, Trig::Toggle, Trig::DoubleTap};
// The Button prompts combo on the Controller tab, in PromptStyle order.
constexpr const char* kPromptLabels[kPromptStyleCount] = {"Switch (the game's own)", "Xbox", "PlayStation", "Steam Controller", "Steam Deck", "Keyboard and mouse",
                                                          "Auto (follows the last used device)"};

struct SwitchButton {
    const char* name;
    hid::NpadButtonBit bit;
};
constexpr SwitchButton kSwitchButtons[] = {
    {"A", hid::kBtnA},         {"B", hid::kBtnB},         {"X", hid::kBtnX},         {"Y", hid::kBtnY},
    {"L", hid::kBtnL},         {"R", hid::kBtnR},         {"ZL", hid::kBtnZL},       {"ZR", hid::kBtnZR},
    {"PLUS", hid::kBtnPlus},   {"MINUS", hid::kBtnMinus}, {"UP", hid::kBtnUp},       {"DOWN", hid::kBtnDown},
    {"LEFT", hid::kBtnLeft},   {"RIGHT", hid::kBtnRight}, {"LSTICK", hid::kBtnStickL}, {"RSTICK", hid::kBtnStickR},
};
constexpr int kSwitchButtonCount = static_cast<int>(sizeof kSwitchButtons / sizeof kSwitchButtons[0]);
static_assert(kSwitchButtonCount == 16, "the Switch pad has 16 buttons in the status view");

// The working copy and the editing state of the page. One page per process.
struct Page {
    bool loaded = false;
    bool dirty = false;                    // the working copy differs from the settings in force (set by every change)
    Settings edit;
    Ctx ctx = Ctx::Play;               // the context shown in the Bindings tab
    int rebind = -1;                       // index into edit.bindings of the row waiting for an input; -1 = none
    bool link_pitch = true;                // Mouse tab: moving the horizontal sensitivity moves the vertical one too (local, not saved)
    bool out_tab_open = false;             // the Switch layout tab was open last frame; its fields are refilled when it opens
    char out_text[kActCount][128] = {};    // the text of each output field
    std::string out_err[kActCount];        // parse error of each field; empty when its text is valid
    bool aspect_custom = false;            // Display tab: the Custom aspect entry is picked (local, not saved)
    bool fps_capture = false;              // Display tab: the frame rate counter's hotkey waits for a key (local, not saved)
    bool quit_capture = false;             // Keyboard tab: the quit key waits for a key (local, not saved)
    std::string notice;                    // a line under the buttons: a refused or conflicting binding (cleared by the next rebind)
    bool notice_warn = false;              // the notice is a warning (drawn in orange), not information
    std::vector<std::string> monitors;     // Display page: "1: name (w x h @ hz)", refreshed every couple of seconds while the page is open
    double monitors_at = -1e9;
    std::vector<std::string> audio_devices;   // Audio page: output device names, read when the page opens and by its Refresh button
    bool audio_loaded = false;
    // the menu (draw_menu)
    int page = 0;                          // MenuPage
    bool page_changed = false;             // scroll the page to the top
    int controls_part = 0;                 // the Controls page's part: mouse, bindings, controller, Switch layout, status
    int pin_capture = -1;                  // the pin group whose toggle key waits for a key (-1: none)
    int dock = 1;                          // 0 left, 1 centre, 2 right
    bool focus_rail = true;                // put the controller's focus on the page's entry of the rail (when the menu opens)
    double shown_at = 0.0, synced_at = 0.0;
    int last_frame = -10;                  // ImGui frame of the last draw: a gap means the menu was opened again
    float scroll_to = -1.f;                // tests: scroll the page to this many UI units down (menu_scroll)
};

void mark(Page& p) { p.dirty = true; }

const char* yes_no(bool b) { return b ? "yes" : "no"; }
const char* cstr(const char* s) { return s ? s : ""; }
const char* act_label(Act a) { return a == Act::None ? "none (input off)" : act_info(a).name; }

// Stops our rebind (a binding row or the quit key). The host's capture is cancelled only when it is ours.
void stop_rebind(UiHost& host, Page& p) {
    if ((p.rebind >= 0 || p.quit_capture || p.pin_capture >= 0) && host.capturing()) host.cancel_capture();
    p.rebind = -1;
    p.quit_capture = false;
    p.pin_capture = -1;
}

// Ends the frame rate counter's hotkey capture. The host's capture is cancelled only when it is ours.
void stop_fps_capture(UiHost& host, Page& p) {
    if (p.fps_capture && host.capturing()) host.cancel_capture();
    p.fps_capture = false;
}

// The bindings of the working copy that share row `row`'s input in the same context and trigger: pressing that input would do both.
std::vector<int> conflicts_of(const Page& p, int row) {
    std::vector<int> out;
    const Binding& a = p.edit.bindings[static_cast<std::size_t>(row)];
    if (a.in.kind == InKind::None) return out;
    for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
        const Binding& b = p.edit.bindings[i];
        if (static_cast<int>(i) != row && b.ctx == a.ctx && b.in == a.in && b.trig == a.trig) out.push_back(static_cast<int>(i));
    }
    return out;
}

void set_notice(Page& p, const std::string& text, bool warn) {
    p.notice = text;
    p.notice_warn = warn;
}

// Takes the captured input, if the host has one. Results are taken every frame, so a stale one never reaches a later rebind.
// Only one capture runs at a time (the binding row or the frame rate hotkey), so a result goes to whichever one is waiting.
// The key that opens the menu cannot be bound (and the frame rate hotkey cannot be bound to an action): the page says so and keeps waiting.
void poll_capture(UiHost& host, Page& p) {
    InputId id;
    if (host.capture_result(&id)) {
        const std::string name = input_name(id);
        const bool reserved = host.reserved_key(id);
        if (p.pin_capture >= 0) {
            // A pin's toggle key: keyboard keys only, never one the menu, the counter, the quit key or the game's bindings use.
            const Settings cur = host.settings();
            bool bound = false;
            for (const Binding& b : p.edit.bindings) bound = bound || b.in == id;
            const bool taken = id.kind == InKind::Key && (id == cur.fps.key || id == p.edit.quit.key);
            if (reserved || taken || bound) {
                set_notice(p, name + (reserved ? " is one of the overlay's own keys" : taken ? " is the counter's or the quit key" : " is bound in the game") +
                                  " and cannot toggle a pin. Press another key, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            const int gi = p.pin_capture;
            p.pin_capture = -1;
            if (id.kind == InKind::Key && gi < static_cast<int>(p.edit.hud.groups.size())) {
                p.edit.hud.groups[static_cast<std::size_t>(gi)].key = {id.code, 0};
                p.edit.hud.in_file = true;
                mark(p);
                set_notice(p, name + " shows or hides the pin.", false);
            }
            return;
        }
        if (p.fps_capture) {
            // The frame rate hotkey takes keyboard keys only; a mouse button or a wheel notch is dropped and the capture ends.
            if (reserved) {
                set_notice(p, name + " opens this menu and cannot be the frame rate hotkey. Press another key, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.fps_capture = false;
            if (id.kind == InKind::Key) {
                Settings cur = host.settings();
                cur.fps.key = id;
                host.apply(cur);   // live: not part of the working copy
            }
        } else if (p.quit_capture) {
            // The quit key: keyboard keys only (a mouse button or a wheel notch is dropped and the capture ends), part of the working copy.
            const Settings cur = host.settings();
            if (reserved || (id.kind == InKind::Key && id == cur.fps.key)) {
                set_notice(p, name + (reserved ? " opens this menu" : " is the frame rate counter hotkey") + " and cannot be the quit key. Press another key, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.quit_capture = false;
            if (id.kind == InKind::Key) {
                p.edit.quit.key = id;
                mark(p);
                bool used = false;
                for (const Binding& b : p.edit.bindings) used = used || b.in == id;
                if (used) set_notice(p, name + " is the quit key now; its bindings no longer reach the game.", true);
                else set_notice(p, name + " is the quit key.", false);
            }
        } else if (p.rebind >= 0 && p.rebind < static_cast<int>(p.edit.bindings.size())) {
            const Settings cur = host.settings();
            if (reserved || (id.kind == InKind::Key && (id == cur.fps.key || id == p.edit.quit.key))) {
                set_notice(p, name + (reserved ? " opens this menu" : id == cur.fps.key ? " is the frame rate counter hotkey" : " is the quit key") +
                                  " and cannot be bound. Press another input, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.edit.bindings[static_cast<std::size_t>(p.rebind)].in = id;
            mark(p);
            const std::vector<int> other = conflicts_of(p, p.rebind);
            if (other.empty()) {
                set_notice(p, name + " bound.", false);
            } else {
                const Binding& o = p.edit.bindings[static_cast<std::size_t>(other[0])];
                set_notice(p, name + " is also bound to " + act_label(o.act) + " (" + ctx_name(o.ctx) + "): both happen when it is pressed.", true);
            }
        }
        p.rebind = -1;
    } else if ((p.rebind >= 0 || p.fps_capture || p.quit_capture || p.pin_capture >= 0) && !host.capturing()) {
        p.rebind = -1;   // Escape, or the host ended the capture without a result
        p.fps_capture = false;
        p.quit_capture = false;
        p.pin_capture = -1;
        set_notice(p, "Rebinding cancelled.", false);
    }
}

// Fills the Switch layout fields from the working copy. Also clears their errors.
void refresh_out_text(Page& p) {
    for (int a = 1; a < kActCount; ++a) {   // Act::None has no row
        const std::string text = output_text(p.edit.out[a]);
        std::snprintf(p.out_text[a], sizeof p.out_text[a], "%s", text.c_str());
        p.out_err[a].clear();
    }
}

// Changes made by the page that must hold before the copy is handed to the host: no bindings without an input, values in range.
void sanitize(Settings& s) {
    std::vector<Binding>& v = s.bindings;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Binding& b) { return b.in.kind == InKind::None; }), v.end());
    for (Binding& b : v) b.ms = std::clamp(b.ms, 0, 5000);
    s.display.width = std::clamp(s.display.width, 320, 16384);
    s.display.height = std::clamp(s.display.height, 320, 16384);
    // A fixed render size is 0 (follow the window) or 320..16384 per side; the settings file rejects anything else.
    s.display.render_width = s.display.render_width <= 0 ? 0 : std::clamp(s.display.render_width, 320, 16384);
    s.display.render_height = s.display.render_height <= 0 ? 0 : std::clamp(s.display.render_height, 320, 16384);
    // Extras: peak and paper white are 0 (auto) or in range; the upscale mode is kept as it is, even for the modes not yet offered.
    extras::HdrSettings& h = s.extras.hdr;
    h.peak_nits = h.peak_nits <= 0 ? 0 : std::clamp(h.peak_nits, 200.0, 10000.0);
    h.paper_white_nits = h.paper_white_nits <= 0 ? 0 : std::clamp(h.paper_white_nits, 80.0, 500.0);
    h.highlight_boost = std::clamp(h.highlight_boost, 0.0, 2.0);
    s.extras.upscale.sharpness = std::clamp(s.extras.upscale.sharpness, 0.0, 1.0);
    s.extras.picture = extras::picture_clamped(s.extras.picture);
    s.audio.volume = std::clamp(s.audio.volume, 0.0, 1.0);
    s.display.monitor = std::clamp(s.display.monitor, 0, 32);
    if (s.audio.device.empty()) s.audio.device = "default";
}

// The frame rate counter is live (the hotkey changes it in the game), so the working copy always takes it from the host, never from a
// stale copy: Apply and save, Revert and Factory defaults leave the counter as it is.
void apply_now(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit.fps = host.settings().fps;
    sanitize(p.edit);
    host.apply(p.edit);
    p.dirty = false;
}

void revert_to_saved(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.settings();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);   // the colour sliders preview live: back to the saved values
    refresh_out_text(p);
    p.dirty = false;
}

void load_factory(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.factory();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);
    refresh_out_text(p);
    mark(p);
}

}  // namespace

// ---- rows and cards (play_ui_widgets.h) ------------------------------------------------------------------------------------------------

namespace widgets {
namespace {
bool g_card_open = false;
float g_control_x = 0.f;
std::string g_row_id;
std::string g_tip;          // the description of the focused / hovered row this frame (the footer shows it)
std::string g_tip_shown;    // last frame's, drawn by the footer
}  // namespace

ImVec4 col_warn() { return th::v4(th::pal().warn); }
ImVec4 col_danger() { return th::v4(th::pal().danger); }
ImVec4 col_ok() { return th::v4(th::pal().ok); }
ImVec4 col_accent() { return th::v4(th::pal().accent); }
ImVec4 col_off() { return th::v4(th::pal().control_off); }
ImVec4 col_warn_tint() { return th::v4(th::pal().warn_tint); }

void end_card() {
    if (!g_card_open) return;
    g_card_open = false;
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.f, th::sp(th::S3)));
}

void card(const char* title) {
    end_card();
    const th::Palette& p = th::pal();
    if (title && *title && ImGui::FindRenderedTextEnd(title) != title) {
        th::push(th::Type::Strong);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
        ImGui::TextUnformatted(title, ImGui::FindRenderedTextEnd(title));
        ImGui::PopStyleColor();
        th::pop();
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleColor(ImGuiCol_Border, p.line);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, th::px(th::kRadiusMd));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(th::sp(th::S4), th::sp(th::S3)));
    ImGui::BeginChild(title && *title ? title : "##card", ImVec2(0.f, 0.f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    g_card_open = true;
}

const char* row(const char* label) {
    const char* hash = std::strstr(label, "##");
    const size_t vis = hash ? static_cast<size_t>(hash - label) : std::strlen(label);
    if (vis == 0) return label;
    g_row_id = std::string("##") + (hash ? hash + 2 : label);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorPosX();
    const float lw = std::round(std::clamp(avail * 0.46f, std::min(avail * 0.5f, th::px(150.f)), th::px(360.f)));
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x0 + lw - th::sp(th::S3));
    ImGui::TextUnformatted(label, label + vis);
    ImGui::PopTextWrapPos();
    ImGui::SameLine(x0 + lw);
    g_control_x = x0 + lw;
    ImGui::SetNextItemWidth(std::max(th::px(80.f), avail - lw));
    return g_row_id.c_str();
}

float control_x() { return g_control_x; }

bool check(const char* label, bool* v) {
    const char* id = row(label);
    if (id == label) return th::toggle(label, v);   // no text: a bare switch
    return th::toggle(id, v);
}

bool combo(const char* label, const char* preview, ImGuiComboFlags flags) { return ImGui::BeginCombo(row(label), preview, flags); }
bool slider_int(const char* label, int* v, int lo, int hi, const char* format, ImGuiSliderFlags flags) {
    return ImGui::SliderInt(row(label), v, lo, hi, format, flags);
}
bool slider_float(const char* label, float* v, float lo, float hi, const char* format, ImGuiSliderFlags flags) {
    return ImGui::SliderFloat(row(label), v, lo, hi, format, flags);
}
bool input_int(const char* label, int* v, int step, int step_fast, ImGuiInputTextFlags flags) {
    return ImGui::InputInt(row(label), v, step, step_fast, flags);
}

bool segmented(const char* label, const char* const* names, int n, int* cur) {
    const char* id = row(label);
    ImGui::PushID(id);
    const th::Palette& p = th::pal();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = std::min(h * 0.5f, ImGui::GetStyle().FrameRounding + th::px(2.f));
    dl->AddRectFilled(pos, ImVec2(pos.x + avail, pos.y + h), ImGui::GetColorU32(p.sunken), r);
    bool changed = false;
    const float w = avail / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const ImRect bb(ImVec2(pos.x + w * static_cast<float>(i), pos.y), ImVec2(pos.x + w * static_cast<float>(i + 1), pos.y + h));
        ImGui::SetCursorScreenPos(bb.Min);
        const ImGuiID iid = ImGui::GetID("##seg");
        ImGui::ItemSize(bb);
        if (ImGui::ItemAdd(bb, iid)) {
            bool hov = false, held = false;
            if (ImGui::ButtonBehavior(bb, iid, &hov, &held) && *cur != i) {
                *cur = i;
                changed = true;
            }
            const bool sel = *cur == i;
            const ImVec2 a(bb.Min.x + th::px(2.f), bb.Min.y + th::px(2.f)), b(bb.Max.x - th::px(2.f), bb.Max.y - th::px(2.f));
            if (sel) dl->AddRectFilled(a, b, ImGui::GetColorU32(p.accent), std::max(0.f, r - th::px(2.f)));
            else if (hov) dl->AddRectFilled(a, b, ImGui::GetColorU32(p.surface2), std::max(0.f, r - th::px(2.f)));
            const ImVec2 ts = ImGui::CalcTextSize(names[i]);
            dl->PushClipRect(a, b, true);
            dl->AddText(ImVec2(bb.Min.x + std::max(th::px(4.f), (w - ts.x) * 0.5f), bb.Min.y + (h - ts.y) * 0.5f),
                        ImGui::GetColorU32(sel ? p.on_accent : hov ? p.text : p.text2), names[i]);
            dl->PopClipRect();
            ImGui::RenderNavCursor(bb, iid, ImGuiNavRenderCursorFlags_None, r);
        }
        ImGui::PopID();
        if (i + 1 < n) ImGui::SameLine(0.f, 0.f);
    }
    ImGui::PopID();
    return changed;
}

void tip(const char* text) {
    if (!text) return;
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) g_tip = text;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetItemTooltip("%s", text);
}

void restart_badge() {
    // a note under the control; the last item stays the control's, so a tip() after it still describes the control
    const ImGuiLastItemData last = GImGui->LastItemData;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), g_control_x));
    th::push(th::Type::Small);
    ImGui::PushStyleColor(ImGuiCol_Text, th::pal().warn);
    ImGui::TextUnformatted("Applies at the next start");
    ImGui::PopStyleColor();
    th::pop();
    GImGui->LastItemData = last;
}

void note(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, th::pal().text2);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextV(fmt, a);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(a);
}

void warn(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, th::pal().warn);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextV(fmt, a);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(a);
}

// The footer's description: what tip() saw this frame (the footer is drawn after the page).
const std::string& take_tip() {
    g_tip_shown = g_tip;
    g_tip.clear();
    return g_tip_shown;
}
}  // namespace widgets

namespace {
using namespace widgets;

// A slider on a double; true when the user changed it.
bool slider(const char* label, double* v, double lo, double hi, const char* format, ImGuiSliderFlags flags = 0) {
    float f = static_cast<float>(*v);
    if (!slider_float(label, &f, static_cast<float>(lo), static_cast<float>(hi), format, flags)) return false;
    *v = f;
    return true;
}

// An int field. The value is clamped when the field is left, not while typing (typing "1280" must not turn into "320" on the way).
bool int_field(const char* label, int* v, int lo, int hi, int step, int step_fast) {
    bool changed = input_int(label, v, step, step_fast);
    if (ImGui::IsItemDeactivated()) {
        const int c = std::clamp(*v, lo, hi);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// One side of the fixed render size: 0 (follow the window) or 320..16384. A value in between is clamped to 320 when the field is left.
bool render_size_field(const char* label, int* v) {
    bool changed = ImGui::InputInt(label, v, 0, 0);
    if (ImGui::IsItemDeactivated()) {
        const int c = *v <= 0 ? 0 : std::clamp(*v, 320, 16384);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// Sizes are in units of the 16 px reference font, so the page follows the overlay's UI scale (overlay.cpp).
float px(float v) { return v * ImGui::GetFontSize() / 16.0f; }

// ---- Mouse --------------------------------------------------------------------------------------------------------------------------

void sensitivity_help(double rad) {
    if (rad <= 0) {
        ImGui::TextDisabled("= %.6f rad per count", rad);
        return;
    }
    ImGui::TextDisabled("= %.6f rad per count, %.3f degrees per count, 360 degrees in %.0f counts", rad, rad * 180.0 / kPi, 2.0 * kPi / rad);
}

// The game's own entries of the Mouse tab (play_game.h GamePlay::mouse_tab): where = 0 after the sensitivity, 1 after the invert boxes, 2 at the end.
void game_mouse_section(int where, Page& p) {
    const GamePlay* gp = game_play();
    if (gp && gp->mouse_tab && gp->mouse_tab(where, p.edit)) mark(p);
}

void mouse_tab(Page& p) {
    MouseSettings& m = p.edit.mouse;
    double src = source_sens(m.yaw_rad);
    if (slider("Horizontal sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.yaw_rad = rad_per_count(src);
        if (p.link_pitch) m.pitch_rad = m.yaw_rad;
        mark(p);
    }
    sensitivity_help(m.yaw_rad);

    src = source_sens(m.pitch_rad);
    if (slider("Vertical sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.pitch_rad = rad_per_count(src);
        mark(p);
    }
    sensitivity_help(m.pitch_rad);
    check("Link vertical to horizontal", &p.link_pitch);
    game_mouse_section(0, p);

    ImGui::Spacing();
    if (check("Invert vertical", &m.invert_y)) mark(p);
    if (check("Invert horizontal", &m.invert_x)) mark(p);
    game_mouse_section(1, p);
    if (check("Mouse pointer in menus (cursor, hover, click)", &m.menu_cursor)) mark(p);
    game_mouse_section(2, p);
}

// ---- Controller ---------------------------------------------------------------------------------------------------------------------

// The 22 controller buttons, in PadSrc order.
constexpr const char* kPadSrcLabels[kPadSrcCount] = {
    "A / Cross (bottom)", "B / Circle (right)", "X / Square (left)", "Y / Triangle (top)",
    "LB / L1",            "RB / R1",            "LT / L2",            "RT / R2",
    "Menu / Options",     "View / Create",      "Left stick click",   "Right stick click",
    "D-pad up",           "D-pad down",         "D-pad left",         "D-pad right",
    "Right grip R4 (upper)", "Left grip L4 (upper)", "Right grip R5 (lower)", "Left grip L5 (lower)",
    "Touchpad / left trackpad click", "Right trackpad click",
};
static_assert(sizeof kPadSrcLabels / sizeof kPadSrcLabels[0] == 22, "one label per controller button");

// The choices of a controller button: Default, Nothing, then the single Switch buttons in kSwitchButtons order.
constexpr const char* kPadChoiceNames[] = {
    "Default", "Nothing",
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick click", "Right stick click",
};
constexpr int kPadChoiceCount = static_cast<int>(sizeof kPadChoiceNames / sizeof kPadChoiceNames[0]);
static_assert(kPadChoiceCount == 2 + kSwitchButtonCount, "one choice per Switch button, plus Default and Nothing");
constexpr const char* kPadCustomLabel = "Custom (edit in controls.ini)";

// The value a choice stores.
uint64_t pad_choice_value(int c) {
    if (c == 0) return kPadDefault;
    if (c == 1) return 0;
    return static_cast<uint64_t>(hid::btn(kSwitchButtons[c - 2].bit));
}

// The choice a stored value shows as, or -1 when no choice holds it (a mask such as two buttons, set in the file).
int pad_choice_of(uint64_t v) {
    for (int c = 0; c < kPadChoiceCount; ++c)
        if (pad_choice_value(c) == v) return c;
    return -1;
}

void pad_buttons_group(Page& p) {
    card("Pad buttons");
    ImGui::TextWrapped("What each controller button presses on the Switch pad. Default keeps the built-in layout.");
    if (ImGui::BeginTable("pad_buttons", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Controller button");
        ImGui::TableSetupColumn("Presses on the Switch pad");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kPadSrcCount; ++i) {
            uint64_t& v = p.edit.pad.remap[i];
            const int sel = pad_choice_of(v);
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPadSrcLabels[i]);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (combo("##remap", sel >= 0 ? kPadChoiceNames[sel] : kPadCustomLabel)) {
                if (sel < 0) {   // a mask no choice holds: shown as the current entry, not offered
                    ImGui::BeginDisabled();
                    ImGui::Selectable(kPadCustomLabel, true);
                    ImGui::EndDisabled();
                }
                for (int c = 0; c < kPadChoiceCount; ++c) {
                    if (ImGui::Selectable(kPadChoiceNames[c], c == sel) && c != sel) {
                        v = pad_choice_value(c);
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset pad buttons")) {
        bool changed = false;
        for (uint64_t& v : p.edit.pad.remap) {
            if (v != kPadDefault) {
                v = kPadDefault;
                changed = true;
            }
        }
        if (changed) mark(p);
    }
}

// The quit combination ([quit] pad): two controller buttons held together open the "Close the game?" prompt; both none = off. A file may
// name more buttons: the combos then show the first two, and changing either keeps only what they show.
void quit_pad_group(Page& p) {
    card("Quit shortcut");
    uint32_t& bits = p.edit.quit.pad;
    int sel[2] = {-1, -1}, n = 0;
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (!(bits & pad_src_bit(static_cast<PadSrc>(i)))) continue;
        if (n < 2) sel[n] = i;
        ++n;
    }
    for (int k = 0; k < 2; ++k) {
        ImGui::PushID(k);
        if (combo(k == 0 ? "Quit: button 1" : "Quit: button 2", sel[k] < 0 ? "none" : kPadSrcLabels[sel[k]])) {
            for (int c = -1; c < kPadSrcCount; ++c) {
                if (ImGui::Selectable(c < 0 ? "none" : kPadSrcLabels[c], c == sel[k]) && c != sel[k]) {
                    int next[2] = {sel[0], sel[1]};
                    next[k] = c;
                    bits = 0;
                    for (int v : next)
                        if (v >= 0) bits |= pad_src_bit(static_cast<PadSrc>(v));
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    tip("Holding these controller buttons together asks \"Close the game?\" (A quits, B cancels). They are the physical buttons, whatever they "
        "press on the Switch pad. Both none turns the shortcut off.");
    if (n > 2) ImGui::TextColored(col_warn(), "controls.ini names %d buttons (%s); changing one here keeps only the two shown.", n, pad_combo_text(bits).c_str());
}

// The button prompt style: which icons the game shows (Auto follows the last device used). Shared by the keyboard and the pad.
void prompts_row(const LiveStatus& live, Page& p) {
    PromptSettings& prompts = p.edit.prompts;
    const int prompt_sel = static_cast<int>(prompts.style);
    if (combo("Button prompts", kPromptLabels[prompt_sel])) {
        for (int i = 0; i < kPromptStyleCount; ++i) {
            if (ImGui::Selectable(kPromptLabels[i], i == prompt_sel) && i != prompt_sel) {
                prompts.style = static_cast<PromptStyle>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("The button icons in the game's prompts: the game's own Switch icons, Xbox, PlayStation, Steam Controller, Steam Deck, keyboard and mouse, or Auto to follow the device you used last.");
    if (prompts.style == PromptStyle::Auto) {
        ImGui::SetCursorPosX(control_x());
        ImGui::TextDisabled("Showing %s now", cstr(live.prompt_now));
    }
}

void pad_tab(const LiveStatus& live, Page& p) {   // the Gamepad tab of the Controls page
    PadSettings& pad = p.edit.pad;
    static const char* const kLayouts[] = {"Switch (right = A)", "Xbox (A = A)"};
    int by_label = pad.label_abxy ? 1 : 0;
    if (segmented("Face buttons##pad_layout", kLayouts, 2, &by_label)) {
        pad.label_abxy = by_label == 1;
        mark(p);
    }
    tip("Switch: face buttons by position, as on a Switch pad: the right button confirms (Switch A), the bottom one goes back (Switch B). "
        "Xbox: by the letter printed on them: the pad's A is Switch A wherever it sits (on an Xbox pad, the bottom button confirms).");

    card("Sticks and triggers");
    if (slider("Left stick dead zone", &pad.stick_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    tip("The stick must move this far from the centre before it counts (a fraction of its travel). Applies before the game's own dead zone.");
    if (slider("Right stick dead zone", &pad.right_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    tip("The same for the right stick (looking).");
    if (slider("Left stick max", &pad.stick_max, 0.5, 1.0, "%.2f")) mark(p);
    tip("How far the left stick must be pushed to count as fully pushed (a fraction of its travel). Lower it if your stick never reaches full speed (for example it tops out around 0.95). 1.00 turns it off.");
    if (slider("Right stick max", &pad.right_stick_max, 0.5, 1.0, "%.2f")) mark(p);
    tip("The same for the right stick (looking): lower it if the camera never turns at full speed.");
    if (check("Invert look (right stick up looks down)", &pad.invert_y)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Applies in play only; adds to the game's own invert option.");
    if (slider("Look sensitivity", &pad.look_sens, 0.25, 3.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Multiplies the right stick while playing. Below 1 the top turning speed is lower; above 1 you reach it with less stick.");
    if (slider("Trigger threshold", &pad.trigger_threshold, 0.05, 1.0, "%.2f")) mark(p);
    if (check("Gyro (six-axis sensor)", &pad.gyro)) mark(p);
    if (slider("Gyro scale", &pad.gyro_scale, 0.1, 4.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (check("Rumble", &pad.rumble)) mark(p);
    if (slider("Rumble scale", &pad.rumble_scale, 0.0, 2.0, "%.2f")) mark(p);

    pad_buttons_group(p);
    quit_pad_group(p);

    card("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro available: %s    Rumble available: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Display ------------------------------------------------------------------------------------------------------------------------

int window_mode_index(const std::string& mode) {
    for (int i = 0; i < kWindowModeCount; ++i)
        if (mode == kWindowModeKeys[i]) return i;
    return 0;
}

// The combo entry for an aspect value. Auto wins for 0; Custom is shown when picked or when the value is no preset.
int aspect_index(double aspect, bool custom) {
    if (aspect <= 0.0) return 0;
    if (custom) return kAspectCustom;
    for (int i = 1; i < kAspectCustom; ++i)
        if (std::fabs(aspect - kAspectValues[i]) < 1e-6) return i;
    return kAspectCustom;
}

void fps_label(int fps, char* buf, std::size_t n) {
    if (fps < 0) std::snprintf(buf, n, "Refresh rate");
    else if (fps == 0) std::snprintf(buf, n, "Unlimited");
    else std::snprintf(buf, n, "%d fps", fps);
}

// The frame rate counter. Its controls act on the host at once, not on the working copy: the game's hotkey flips `show` too, so the
// page reads the host's value every frame and never writes a stale copy. These controls do not mark the page as changed.
constexpr const char* kCornerNames[] = {"Top left", "Top right", "Bottom left", "Bottom right"};   // in Corner order
constexpr int kCornerCount = static_cast<int>(sizeof kCornerNames / sizeof kCornerNames[0]);
constexpr const char* kFpsDetailNames[] = {"Frame rate", "Frame rate and frame time", "Plus 1% low and worst frame"};
constexpr int kFpsDetailCount = static_cast<int>(sizeof kFpsDetailNames / sizeof kFpsDetailNames[0]);

void set_fps_live(UiHost& host, const FpsSettings& f) {
    Settings cur = host.settings();
    cur.fps = f;
    host.apply(cur);
}

void fps_group(UiHost& host, Page& p) {
    const FpsSettings fps = host.settings().fps;
    card("Frame rate counter");

    bool show = fps.show;
    if (check("Show frame rate counter", &show)) {
        FpsSettings f = fps;
        f.show = show;
        set_fps_live(host, f);
    }

    const int corner = static_cast<int>(fps.corner);
    if (combo("Corner##fps_corner", kCornerNames[corner])) {
        for (int i = 0; i < kCornerCount; ++i) {
            if (ImGui::Selectable(kCornerNames[i], i == corner) && i != corner) {
                FpsSettings f = fps;
                f.corner = static_cast<Corner>(i);
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    const int detail = std::clamp(fps.detail, 0, kFpsDetailCount - 1);
    if (combo("Detail##fps_detail", kFpsDetailNames[detail])) {
        for (int i = 0; i < kFpsDetailCount; ++i) {
            if (ImGui::Selectable(kFpsDetailNames[i], i == detail) && i != detail) {
                FpsSettings f = fps;
                f.detail = i;
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    bool shots = fps.in_screenshots;
    if (check("Also draw it in screenshots", &shots)) {
        FpsSettings f = fps;
        f.in_screenshots = shots;
        set_fps_live(host, f);
    }

    const std::string key = input_name(fps.key);
    row("Hotkey##fps_key_row");
    const std::string label = (p.fps_capture ? std::string("Press a key...") : (key.empty() ? std::string("?") : key) + "   Change") + "##fps_key";
    if (ImGui::Button(label.c_str())) {
        stop_rebind(host, p);
        p.fps_capture = true;
        host.begin_capture();
    }
    tip("A keyboard key that shows or hides the counter in the game. It never reaches the game.");
}

// ---- Display page groups (HDR, upscaling, low latency) ----------------------------------------------------------------------------------

// HDR output, upscaling and low latency (nxrt/extras.h). Items that apply at the next start say so in their labels, as the Display tab does.
constexpr const char* kHdrModeNames[] = {"Off", "Auto", "On", "Native"};
constexpr const char* kHdrFormatNames[] = {"Auto", "HDR10", "scRGB"};
constexpr const char* kLatencyModeNames[] = {"Off", "Auto", "On", "Boost"};
constexpr const char* kUpscaleNames[] = {"Off", "FSR 1 (spatial)", "FSR 3", "DLSS", "XeSS", "NIS (spatial)", "DLAA", "Nearest (sharp pixels)",
                                         "MetalFX (temporal)", "MetalFX AA (native)"};   // indexed by extras::UpscaleMode
// The order the list shows them in. ponytail: NIS, FSR 3, DLSS, DLAA and XeSS are off until reworked (extras::upscale_mode_enabled, #232).
// MetalFX exists on macOS only (platform/mac/gfx/temporal_metalfx.mm), where it is the default.
#if defined(__APPLE__)
constexpr extras::UpscaleMode kUpscaleOrder[] = {extras::UpscaleMode::Off, extras::UpscaleMode::MetalFx, extras::UpscaleMode::MetalFxAa, extras::UpscaleMode::Fsr1,
                                                 extras::UpscaleMode::Nearest};
#else
constexpr extras::UpscaleMode kUpscaleOrder[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Nearest};
#endif
constexpr const char* kUpscaleQualityNames[] = {"Native (AA only)", "Quality", "Balanced", "Performance", "Ultra performance", "Custom (scene resolution)"};

// Off and the spatial upscalers (FSR 1, NIS) always work. The temporal ones (src/gfx/extras/temporal.h) get depth, camera motion vectors made by
// the runtime and the game's projection jitter: FSR 3 when the FidelityFX DLLs are found, DLSS / DLAA when the Streamline DLLs and nvngx_dlss.dll are found (the GPU
// check runs when the renderer starts them). XeSS is not wired. A file that holds an unavailable one shows it as selected (the game runs as Off).
bool upscale_available(extras::UpscaleMode m) {
    switch (m) {
        case extras::UpscaleMode::Off:
        case extras::UpscaleMode::Fsr1:
        case extras::UpscaleMode::Nearest: return true;
#if defined(__APPLE__)
        case extras::UpscaleMode::MetalFx:
        case extras::UpscaleMode::MetalFxAa: return extras::temporal_metalfx_available();
#endif
        default: return false;   // switched off for now (#232)
    }
}
bool is_metalfx(extras::UpscaleMode m) { return m == extras::UpscaleMode::MetalFx || m == extras::UpscaleMode::MetalFxAa; }
const char* temporal_why(extras::UpscaleMode m) {
#if defined(__APPLE__)
    if (is_metalfx(m)) {
        const char* why = extras::temporal_metalfx_why();
        return why ? why : "This Mac's GPU has no MetalFX temporal scaler.";
    }
#endif
    if (m == extras::UpscaleMode::Fsr3)
        return "Needs amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (AMD FidelityFX SDK v2.4.0) in upscalers\\fsr3 next to the "
               "executable, or a path in [upscale] fsr_dll.";
    if (m == extras::UpscaleMode::Dlss || m == extras::UpscaleMode::Dlaa)
        return "Needs an NVIDIA RTX GPU and the Streamline DLLs (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) in upscalers\\streamline "
               "next to the executable, or their folder in [upscale] dlss_dll. The launcher's Game files page downloads nvngx_dlss.dll from NVIDIA.";
    return "Switched off for now: it will come back once it is reworked.";
}

// A nits field where 0 means "auto" (shown as "auto"). Other values are clamped into lo..hi when the field is left, so the slider can be
// dragged through the gap below lo while it is held.
bool auto_nits_slider(const char* label, double* v, int lo, int hi) {
    int x = static_cast<int>(std::lround(*v));
    bool changed = slider_int(label, &x, 0, hi, x <= 0 ? "auto" : "%d nits");
    if (ImGui::IsItemDeactivated()) {
        const int c = x <= 0 ? 0 : std::clamp(x, lo, hi);
        changed = changed || c != x;
        x = c;
    }
    if (changed) *v = x;
    return changed;
}

void hdr_group(Page& p) {
    extras::HdrSettings& h = p.edit.extras.hdr;
    card("HDR output");

    const int mode = static_cast<int>(h.mode);
    const bool mode_open = combo("Mode##hdr_mode", kHdrModeNames[mode]);
    tip("Off keeps the SDR picture, Auto uses HDR when the display and Windows offer it, On runs the HDR pipeline even without an HDR display. "
        "Native is On with the levels the game declares for an HDR TV on Switch 2.");
    if (mode_open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kHdrModeNames[i], i == mode) && i != mode) {
                h.mode = static_cast<extras::HdrMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    const int format = static_cast<int>(h.format);
    const bool format_open = combo("Format##hdr_format", kHdrFormatNames[format]);
    tip("The signal sent to the display: HDR10, or scRGB; Auto picks HDR10 when the display offers it and scRGB otherwise.");
    if (format_open) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kHdrFormatNames[i], i == format) && i != format) {
                h.format = static_cast<extras::HdrFormat>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    if (auto_nits_slider("Peak brightness##hdr_peak", &h.peak_nits, 200, 10000)) mark(p);
    tip("How bright the brightest highlights get, in nits; auto reads the peak brightness the display reports.");
    if (auto_nits_slider("Paper white##hdr_paper", &h.paper_white_nits, 80, 500)) mark(p);
    tip("The brightness of ordinary white in the game, in nits; auto follows Windows' SDR content brightness when HDR is on there, else 203.");
    if (slider("Highlight boost", &h.highlight_boost, 0.0, 2.0, "%.2f")) mark(p);
    tip("0 keeps the game's SDR look up to its own white, 1 lets highlights reach the display's peak, 2 pushes them further.");

    const extras::HdrState hs = extras::hdr_state();
    ImGui::Text("HDR scene: %s, swapchain: %s, paper white %.0f nits, peak %.0f nits, peak ratio %.2f, display reports HDR: %s",
                hs.scene_hdr ? "on" : "off", extras::output_kind_name(hs.output), static_cast<double>(hs.paper_white_nits),
                static_cast<double>(hs.peak_nits), static_cast<double>(hs.peak_ratio), yes_no(hs.display_hdr));
}

void upscale_group(Page& p) {
    extras::UpscaleSettings& u = p.edit.extras.upscale;
    card("Upscaling");

    // Only the modes that work are offered. A file that holds another one shows it as the current entry, marked, until the player picks.
    const int mode = static_cast<int>(u.mode);
    std::string preview = kUpscaleNames[mode];
    if (!upscale_available(u.mode)) preview += " (not available)";
    const bool open = combo("Upscaler##upscale_mode", preview.c_str());
#if defined(__APPLE__)
    tip("How the game's 3D scene is anti-aliased and enlarged. MetalFX (Apple): temporal anti-aliasing and upscaling from the quality preset's scene size, "
        "the sharpest and steadiest picture. MetalFX AA: the same at the full scene size, anti-aliasing only. FSR 1 (AMD): filtered and sharpened, "
        "no anti-aliasing. Off: smooth (bilinear). Nearest: every pixel kept as a hard block, no blur.");
#else
    tip("How the game's picture is enlarged from a lower render or scene resolution. Off: smooth (bilinear). FSR 1 (AMD): filtered and sharpened, any GPU. "
        "Nearest: every pixel kept as a hard block, no blur. FSR 3, DLSS and NIS are switched off until they are reworked.");
#endif
    if (open) {
        for (extras::UpscaleMode m : kUpscaleOrder) {
            const int i = static_cast<int>(m);
            const bool ok = upscale_available(m);
            if (!ok) ImGui::BeginDisabled();
            if (ImGui::Selectable(kUpscaleNames[i], i == mode) && i != mode && ok) {
                u.mode = m;
                mark(p);
            }
            if (!ok) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", temporal_why(m));
            }
        }
        ImGui::EndCombo();
    }
    if ((u.mode == extras::UpscaleMode::Dlss || u.mode == extras::UpscaleMode::Dlaa) && !extras::temporal_dlss_started()) restart_badge();
    if (is_metalfx(u.mode) && extras::upscale_fell_back())   // MetalFX could not start: FSR 1 runs instead (temporal.cpp hand_over)
        ImGui::TextDisabled("MetalFX is not running (%s); FSR 1 upscales instead.", temporal_why(u.mode));

    if (u.mode == extras::UpscaleMode::Fsr3 || u.mode == extras::UpscaleMode::Dlss || u.mode == extras::UpscaleMode::MetalFx) {
        const int q = static_cast<int>(u.quality);
        if (combo("Quality##upscale_quality", kUpscaleQualityNames[q])) {
            for (int i = 0; i < 6; ++i) {
                if (ImGui::Selectable(kUpscaleQualityNames[i], i == q) && i != q) {
                    u.quality = static_cast<extras::UpscaleQuality>(i);
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The scene's render size per side: Native 100%, Quality 67%, Balanced 59%, Performance 50%, Ultra performance 33%, Custom the scene resolution "
            "below. Takes effect where the game renders its scene below the output size; otherwise the upscaler runs as native anti-aliasing.");
    }

    if (slider("Sharpness", &u.sharpness, 0.0, 1.0, "%.2f")) mark(p);
    tip("How much FSR 1, NIS or FSR 3 sharpens the upscaled picture; 1 is the sharpest (DLSS has no sharpening).");
    if (slider_int("Scene resolution##upscale_scene", &u.scene_scale, 25, 100, "%d%%")) mark(p);
    tip("Renders the 3D scene at this percent of the output size per side; the game scales it up and keeps the HUD and the post effects at full size. 100 turns it off. "
        "FSR 3, DLSS and MetalFX use their quality preset's percent instead, unless it is Custom. Applies live.");
    {   // what is in force now (#116): the percent the game module applies and who enlarges the scene
        const int pct = extras::upscale_scene_scale();
        float fx = 1.0f, fy = 1.0f;
        extras::scene_fraction(&fx, &fy);
        const extras::UpscaleMode m = extras::upscale_mode();
        if (pct >= 100) {
            ImGui::TextDisabled("Scene: full size%s", extras::upscale_temporal() ? " (the temporal upscaler runs as anti-aliasing)" : "");
        } else if (fx >= 1.0f && fy >= 1.0f) {
            ImGui::TextDisabled("Scene: %d%% per side, stretched by the game (dynamic resolution or split screen on, or not in the game yet)", pct);
        } else {
            const char* who = m == extras::UpscaleMode::Fsr1 ? "upscaled with FSR 1" : m == extras::UpscaleMode::Nearest ? "enlarged with nearest neighbour" : m == extras::UpscaleMode::Nis ? "upscaled with NIS"
                            : m == extras::UpscaleMode::Fsr3 ? "upscaled with FSR 3" : m == extras::UpscaleMode::Dlss ? "upscaled with DLSS"
                            : m == extras::UpscaleMode::MetalFx ? "upscaled with MetalFX"
                            : "stretched by the game (pick an upscaler to make it sharper)";
            ImGui::TextDisabled("Scene: %d%% per side, %s", pct, who);
        }
    }
    ImGui::TextDisabled("Or render everything below the window size with the render scale above; the upscaler then fills the rest.");
}

void latency_group(Page& p) {
    extras::LatencySettings& l = p.edit.extras.latency;
    card("Low latency");   // NVIDIA Reflex or AMD Anti-Lag 2 (nxrt/lowlat.h)

    const int mode = static_cast<int>(l.mode);
    const bool open = combo("Mode##latency_mode", kLatencyModeNames[mode]);
    tip("Lowers the delay from input to picture with NVIDIA Reflex or AMD Anti-Lag 2, whichever the GPU has; Boost asks Reflex for its boost mode.");
    if (open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kLatencyModeNames[i], i == mode) && i != mode) {
                l.mode = static_cast<extras::LatencyMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();

    if (slider_int("Frames ahead##latency_ahead", &l.frames_ahead, -1, 3, l.frames_ahead < 0 ? "no limit" : "%d")) mark(p);
    tip("With the mode on, the game starts simulating a frame only when at most this many earlier frames are still on their way to the screen. 1 keeps the game's frame queue short; 0 is the shortest delay and costs frame rate.");
    restart_badge();
    if (check("Log timings##latency_log", &l.log_timings)) mark(p);
    tip("Writes one line of latency timings to the log every second.");
    restart_badge();

    if (const extras::LowLatency* ll = extras::lowlat()) {
        const extras::LowLatency::Stats st = ll->stats();
        ImGui::Text("%s: enabled %s, input to present %.2f ms, driver input to GPU end %.2f ms, sleep %.2f ms", ll->backend_name(),
                    yes_no(st.enabled), st.input_to_present_ms, st.input_to_gpu_ms, st.sleep_ms_avg);
    } else {
        ImGui::TextDisabled("Not running: the renderer has not started its low-latency part yet, or the mode is off.");
    }
}

// The monitor list ("1: name (1920x1080 @ 144 Hz)"), read again every couple of seconds while the page is open.
void refresh_monitors(Page& p) {
    const double now = ImGui::GetTime();
    if (p.monitors_at >= 0.0 && now - p.monitors_at < 2.0) return;
    p.monitors_at = now;
    p.monitors.clear();
    const int n = platform::display_count();
    for (int i = 1; i <= n; ++i) {
        char buf[192];
        if (platform::display_name(i, buf, sizeof buf)) p.monitors.emplace_back(buf);
    }
}

void window_group(const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    card("Window");
    const int mode = window_mode_index(d.window_mode);
    if (combo("Window mode", kWindowModeNames[mode])) {
        for (int i = 0; i < kWindowModeCount; ++i) {
            if (ImGui::Selectable(kWindowModeNames[i], i == mode) && i != mode) {
                d.window_mode = kWindowModeKeys[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("Borderless fullscreen keeps the desktop resolution and switches instantly. Exclusive fullscreen takes over the display. Alt+Enter flips between windowed and fullscreen in the game.");

    refresh_monitors(p);
    {
        char fallback[48];
        const char* preview = "Automatic (where the window is)";
        if (d.monitor > 0) {
            if (d.monitor <= static_cast<int>(p.monitors.size())) {
                preview = p.monitors[static_cast<std::size_t>(d.monitor - 1)].c_str();
            } else {
                std::snprintf(fallback, sizeof fallback, "%d: not connected", d.monitor);
                preview = fallback;
            }
        }
        if (combo("Monitor", preview)) {
            if (ImGui::Selectable("Automatic (where the window is)", d.monitor <= 0) && d.monitor > 0) {
                d.monitor = 0;
                mark(p);
            }
            for (std::size_t i = 0; i < p.monitors.size(); ++i) {
                const int number = static_cast<int>(i) + 1;
                if (ImGui::Selectable(p.monitors[i].c_str(), d.monitor == number) && d.monitor != number) {
                    d.monitor = number;
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        tip("The display the window moves to. Fullscreen modes use this display.");
    }

    int preset = -1;
    for (int i = 0; i < kSizePresetCount; ++i)
        if (kSizePresets[i][0] == d.width && kSizePresets[i][1] == d.height) preset = i;
    char size_label[48];
    std::snprintf(size_label, sizeof size_label, "%d x %d%s", d.width, d.height, preset < 0 ? " (custom)" : "");
    // Only the windowed mode's size; the picture's own size is under Resolution. Greyed out in the fullscreen modes (#117).
    const bool windowed = window_mode_index(d.window_mode) == window_mode_index("windowed");
    ImGui::BeginDisabled(!windowed);
    if (combo("Windowed size", size_label)) {
        for (int i = 0; i < kSizePresetCount; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "%d x %d", kSizePresets[i][0], kSizePresets[i][1]);
            if (ImGui::Selectable(name, i == preset) && i != preset) {
                d.width = kSizePresets[i][0];
                d.height = kSizePresets[i][1];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", windowed ? "The size of the game window in windowed mode. It is not the size the game renders at: see Resolution below."
                                         : "Only used in windowed mode: the size the window returns to. Fullscreen shows the display's size; the size "
                                           "the game renders at is under Resolution below.");
    if (int_field("Width##win_w", &d.width, 320, 16384, 1, 100)) mark(p);
    if (int_field("Height##win_h", &d.height, 320, 16384, 1, 100)) mark(p);
    if (windowed && live.width > 0 && (live.width != d.width || live.height != d.height)) {
        if (ImGui::Button("Use the current window size")) {
            d.width = live.width;
            d.height = live.height;
            mark(p);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("now %d x %d", live.width, live.height);
    }
    ImGui::EndDisabled();
    if (check("Keep the 16:9 picture, letterbox the rest", &d.keep_aspect)) mark(p);
}

void framerate_group(UiHost& host, const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    card("Frame rate");
    if (check("Vsync", &d.vsync)) mark(p);
    tip("Off: no forced vsync, lowest latency, works well with G-Sync / FreeSync together with a frame limit. On: the display paces the game.");

    char cap[32];
    fps_label(d.fps_cap, cap, sizeof cap);
    if (combo("Frame limit", cap)) {
        for (int fps : kFpsChoices) {
            char name[32];
            fps_label(fps, name, sizeof name);
            if (ImGui::Selectable(name, fps == d.fps_cap) && fps != d.fps_cap) {
                d.fps_cap = fps;
                quality_preset_edited(d, QualityField::FrameLimit);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("Refresh rate follows the display. With vsync on, a limit only counts when it is below the refresh rate.");
    {   // [display] fps_snap: on by default on a detected Steam Deck only (auto); this box writes true / false
        const bool deck = nxrt::host_is_steam_deck();
        bool snap = d.fps_snap == "true" || (d.fps_snap == "auto" && deck);
        ImGui::PushStyleColor(ImGuiCol_Text, col_ok());
        if (check("Cap FPS to 30 for Steam Deck", &snap)) {
            d.fps_snap = snap ? "true" : "false";
            mark(p);
        }
        ImGui::PopStyleColor();
        tip("The Steam Deck's screen refreshes at a fixed 90 Hz (OLED) or 60 Hz (LCD) and cannot follow the game. At 40 fps, frames stay on "
            "screen for uneven times, which feels like stutter; at 30 every frame stays for the same time. Needs vsync on. With a higher limit "
            "it takes the next even step instead (60 becomes 45 at 90 Hz). Off: your frame limit stays exactly as set.");
        ImGui::TextColored(col_ok(), "THIS ACTUALLY MAKES THE GAME SMOOTHER on a Steam Deck.");
        ImGui::TextDisabled(deck ? "On by default on this Steam Deck. Untick it to keep your frame limit exactly as set."
                                 : "Off by default here: this is not a Steam Deck. Tick it only on a screen without G-Sync / FreeSync.");
        if (snap && !d.vsync) ImGui::TextColored(col_warn(), "Turn vsync on for this to work.");
    }
    if (live.snap_k >= 2 && d.fps_cap > 0) {
        ImGui::TextDisabled("%d (runs at %g on this %g Hz screen%s)", d.fps_cap, live.snap_cap, static_cast<double>(live.refresh_hz),
                            d.fps_cap == 40 ? "; set 80 Hz in Steam's Performance menu for 40" : "; pick a refresh rate that is a multiple of it");
    }
    ImGui::BeginDisabled(live.refresh_hz <= 0.0f);
    if (ImGui::Button("Just under the refresh rate (G-Sync / FreeSync)")) {
        // Below 100 Hz suggest_vrr_cap is the refresh rate itself: round it (59.94 -> 60, 89.98 -> 90) so the cap is not a frame short.
        const double cap = nxrt::suggest_vrr_cap(live.refresh_hz);
        d.fps_cap = static_cast<int>(live.refresh_hz < 100.0f ? std::lround(cap) : std::floor(cap));
        quality_preset_edited(d, QualityField::FrameLimit);
        mark(p);
    }
    tip("For G-Sync / FreeSync screens: sets the limit a few fps under the refresh rate (116 at 120 Hz, 138 at 144 Hz) so the screen "
        "keeps following the game instead of tearing or stuttering at the top. Below 100 Hz it sets the refresh rate itself (60, 90): "
        "a lower limit there only makes the game less smooth. On a screen without G-Sync / FreeSync, use vsync instead.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (live.refresh_hz > 0.0f) ImGui::TextDisabled("display %.2f Hz", live.refresh_hz);
    else ImGui::TextDisabled("display refresh rate unknown");

    fps_group(host, p);
}

// The size the game renders at and what happens to it on the way to the screen, live (#117): the picture the NVN back end presented last
// (the window texture's crop at the render scale) against the swapchain size, and the upscaler that fills the difference.
void render_size_line() {
    const gfx::Tuning& tu = gfx::tuning();
    const int sw = tu.present_src_w.load(std::memory_order_relaxed), sh = tu.present_src_h.load(std::memory_order_relaxed);
    const int ow = tu.present_out_w.load(std::memory_order_relaxed), oh = tu.present_out_h.load(std::memory_order_relaxed);
    if (sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0) {
        ImGui::TextDisabled("Rendering: not known yet (no frame presented)");
        return;
    }
    const extras::UpscaleMode m = extras::upscale_mode();
    const char* how = "stretched";
    if (sw > ow || sh > oh) how = "scaled down";
    else if (m == extras::UpscaleMode::Fsr1) how = "upscaled with FSR 1";
    else if (m == extras::UpscaleMode::Nis) how = "upscaled with NIS";
    if (sw == ow && sh == oh) {
        ImGui::Text("Rendering %d x %d, shown at that size (no upscaling)", sw, sh);
    } else {
        ImGui::Text("Rendering %d x %d -> %s to %d x %d", sw, sh, how, ow, oh);
        if (sw < ow && sh <= oh && m != extras::UpscaleMode::Fsr1 && m != extras::UpscaleMode::Nis)
            ImGui::TextDisabled("Below the output size: FSR 1 or NIS (Graphics page) upscale it sharper than the plain stretch.");
    }
    tip("The game's picture size right now (its targets times the render scale) and the size it is shown at (the window or the display).");
}

void resolution_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    card("Resolution");
    render_size_line();
    static const char* const kRenderModes[] = {"Console", "Match window", "Custom"};
    int mode = d.render_scale == kRenderScaleConsole ? 0 : d.render_scale <= 0 ? 1 : 2;
    if (segmented("Render resolution", kRenderModes, 3, &mode)) {
        d.render_scale = mode == 0 ? kRenderScaleConsole : mode == 1 ? 0 : 100;
        quality_preset_edited(d, QualityField::RenderScale);
        mark(p);
    }
    tip("Console renders at the console's 1920 x 1080 and scales it to the window: 60 fps on most Macs and PCs (the default). Match window "
        "renders at the window's size (sharper, for a fast GPU). Custom picks a percentage of 1920 x 1080.");
    restart_badge();
    if (mode == 2) {
        if (slider_int("Render scale", &d.render_scale, 25, 400, "%d%%")) {
            quality_preset_edited(d, QualityField::RenderScale);
            mark(p);
        }
        tip("The game's internal resolution as a percentage of its own size (100% = 1920 x 1080). Above 100% renders more pixels than the console did.");
        restart_badge();
    }
}

// ---- Graphics -----------------------------------------------------------------------------------------------------------------------

// The game's quality preset (kDisplayOptQuality, the presets of play_types.h quality_presets()): picking one writes its values (texture
// LOD bias and anisotropy; a device preset also the render scale and the frame limit); changing one of those afterwards makes it Custom
// (quality_preset_edited).

// What a preset sets, in plain words, from the values the game's hook writes.
std::string quality_preset_summary(const QualityPresetInfo& q, const GamePlay* gp, const DisplaySettings& now) {
    std::string t = q.about;
    if (!gp || !gp->quality_preset) return t;
    DisplaySettings v = now;
    if (!gp->quality_preset(q.id, v)) return t;
    char buf[160];
    t += "\n";
    if (q.sets_frame) {
        if (v.render_scale == kRenderScaleConsole) {
            t += "Render scale: the console's (1920 x 1080). ";
        } else if (v.render_scale <= 0) {
            t += "Render scale: automatic (the window's size). ";
        } else {
            std::snprintf(buf, sizeof buf, "Render scale: %d%% (%d x %d). ", v.render_scale, 1920 * v.render_scale / 100, 1080 * v.render_scale / 100);
            t += buf;
        }
        if (v.fps_cap < 0) {
            t += "Frame limit: the display's refresh rate. ";
        } else if (v.fps_cap == 0) {
            t += "Frame limit: none. ";
        } else {
            std::snprintf(buf, sizeof buf, "Frame limit: %d fps. ", v.fps_cap);
            t += buf;
        }
    }
    if (v.anisotropy > 0) {
        std::snprintf(buf, sizeof buf, "Texture filtering: %dx. ", v.anisotropy);
        t += buf;
    } else {
        t += "Texture filtering: the game's. ";
    }
    t += v.lod_bias < 0 ? "Textures: sharper." : v.lod_bias > 0 ? "Textures: softer." : "Textures: the game's sharpness.";
    if (q.sets_frame) t += "\nThe render scale applies on restart.";
    return t;
}

void quality_preset_combo(Page& p, const GamePlay* gp) {
    DisplaySettings& d = p.edit.display;
    int count = 0;
    const QualityPresetInfo* qs = quality_presets(&count);
    int sel = count - 1;   // custom
    for (int i = 0; i < count; ++i)
        if (d.quality_preset == qs[i].id) sel = i;
    if (combo("Quality preset", qs[sel].name)) {
        for (int i = 0; i < count - 1; ++i) {
            if (ImGui::Selectable(qs[i].name, i == sel) && i != sel && gp->quality_preset && gp->quality_preset(qs[i].id, d)) mark(p);
            if (ImGui::IsItemHovered()) {
                const std::string t = quality_preset_summary(qs[i], gp, d);
                ImGui::SetItemTooltip("%s", t.c_str());
            }
        }
        ImGui::EndCombo();
    }
    tip("Sets several picture settings together. Switch 2 is the console's picture; High and Ultra are sharper and reach farther. The device "
        "presets also pick a render scale and a frame limit for that kind of PC. Changing one of those settings afterwards makes it Custom.");
}

void picture_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    const GamePlay* gp = game_play();
    const unsigned opts = gp ? gp->display_options : 0u;
    card("Picture");

    if (opts & kDisplayOptAspect) {
        const int aspect_sel = aspect_index(d.aspect, p.aspect_custom);
        if (combo("Aspect ratio", kAspectNames[aspect_sel])) {
            for (int i = 0; i < kAspectNameCount; ++i) {
                if (ImGui::Selectable(kAspectNames[i], i == aspect_sel) && i != aspect_sel) {
                    if (i == kAspectCustom) {
                        p.aspect_custom = true;
                        if (d.aspect <= 0.0) d.aspect = kAspectValues[1];   // Custom starts at 16:9
                    } else {
                        p.aspect_custom = false;
                        d.aspect = kAspectValues[i];                       // Auto is index 0, value 0
                    }
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        restart_badge();
        if (aspect_sel == kAspectCustom && slider("Custom aspect ratio", &d.aspect, 1.0, 5.0, "%.2f")) mark(p);
    }

    row("Fixed render size (0 = follow the window)##render_size");
    {
        const float half = std::max(px(60.0f), (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("x").x - 2.f * ImGui::GetStyle().ItemSpacing.x) * 0.5f);
        ImGui::SetNextItemWidth(half);
        if (render_size_field("##render_w", &d.render_width)) mark(p);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        if (render_size_field("##render_h", &d.render_height)) mark(p);
    }
    restart_badge();
    tip("Renders at exactly this size instead of the window size times the render scale (Display page).");

    if (opts & kDisplayOptFov) {
        if (slider("Field of view offset (degrees)", &d.fov_offset, -30.0, 40.0, "%.1f")) mark(p);
        tip("Degrees added to the vertical field of view of every game camera. 0 is the game's own.");
    }
    if (check("Dynamic resolution", &d.dynamic_resolution)) mark(p);
    tip("The game's own dynamic resolution: it lowers the render size when the GPU is late. Off keeps the full size.");

    const bool quality = (opts & kDisplayOptQuality) != 0;
    if (quality) quality_preset_combo(p, gp);
    auto custom = [&](QualityField f) {
        if (quality) quality_preset_edited(d, f);
        mark(p);
    };
    int aniso_sel = 0;
    for (int i = 0; i < kAnisoCount; ++i)
        if (kAnisoValues[i] == d.anisotropy) aniso_sel = i;
    if (combo("Texture anisotropy", kAnisoNames[aniso_sel])) {
        for (int i = 0; i < kAnisoCount; ++i) {
            if (ImGui::Selectable(kAnisoNames[i], i == aniso_sel) && i != aniso_sel) {
                d.anisotropy = kAnisoValues[i];
                custom(QualityField::Anisotropy);
            }
        }
        ImGui::EndCombo();
    }
    tip("Sharper textures at a slant. Game default keeps what the game asks for; the other entries are a minimum on every filtering sampler.");
    if (slider("Texture LOD bias", &d.lod_bias, -2.0, 2.0, "%.2f")) custom(QualityField::LodBias);
    tip("Negative values pick sharper texture levels, positive values softer ones.");
    if (opts & kDisplayOptBloom) {
        if (check("Bloom", &d.bloom)) mark(p);
    }
    if (opts & kDisplayOptChromatic) {
        if (check("Chromatic aberration", &d.chromatic_aberration)) mark(p);
    }
}

void gpu_group(Page& p) {
    DisplaySettings& d = p.edit.display;
    card("GPU and shaders");
    int astc_sel = 1;
    for (int i = 0; i < kAstcCount; ++i)
        if (d.astc_quality == kAstcValues[i]) astc_sel = i;
    if (combo("Texture compression quality", kAstcNames[astc_sel])) {
        for (int i = 0; i < kAstcCount; ++i) {
            if (ImGui::Selectable(kAstcNames[i], i == astc_sel) && i != astc_sel) {
                d.astc_quality = kAstcValues[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("The game's textures are ASTC, which this GPU cannot sample, so they are converted. Fast: the quick BC7 conversion. High: the same at first, then a background thread "
        "re-encodes each texture slowly into the texture cache, so later runs are sharper (more disk space, no frame cost). Uncompressed: RGBA8, sharpest, 4 times the video memory "
        "of BC7. [gfx] astc_quality in the ini wins over this.");
#ifndef __APPLE__   // Apple silicon has unified memory: there is nothing faster to copy to (play.cpp keeps pool_mirror off)
    if (check("Faster GPU memory (uses more video memory)", &d.fast_gpu_memory)) mark(p);
    tip("Keeps device-local copies of the memory the draws read: faster, costs up to 2 GiB of video memory. [nvn] pool_mirror in the ini wins over this.");
    restart_badge();
#endif
    if (check("Fewer GPU pipeline waits", &d.fewer_gpu_waits)) mark(p);
    tip("No full barrier after render passes that only wrote their own targets. [nvn] pass_barriers in the ini wins over this.");
    restart_badge();
    if (check("Prepare shaders ahead (pipeline prewarm)", &d.shader_prewarm)) mark(p);
    tip("Builds the pipelines that earlier runs recorded when the game loads a program, on background threads, so draws do not stall on a first compile. [nvn] prewarm in the ini wins over this.");
    restart_badge();
    if (const int left = gfx::shader_backlog(); left > 0) ImGui::TextDisabled("Preparing shaders: %d left", left);
    else ImGui::TextDisabled("No shaders waiting to be prepared");
}

// Colour controls of the present step (nxrt/extras.h PictureSettings). They show live while dragged; Apply and save keeps them.
void color_group(Page& p) {
    extras::PictureSettings& c = p.edit.extras.picture;
    card("Colour");
    bool changed = false;
    row("Preset##pic_preset");
    if (ImGui::Button("Neutral##pic_neutral")) changed = extras::picture_preset("neutral", &c);
    tip("The game's own picture, untouched.");
    ImGui::SameLine();
    if (ImGui::Button("Vivid##pic_vivid")) changed = extras::picture_preset("vivid", &c);
    tip("A modest boost: contrast 1.10, saturation 1.15, black level -0.03.");
    changed |= slider("Contrast##pic_contrast", &c.contrast, 0.5, 1.5, "%.2f");
    tip("Spreads the tones away from mid grey; 1 is the game's own.");
    changed |= slider("Saturation##pic_saturation", &c.saturation, 0.0, 2.0, "%.2f");
    tip("Colour intensity; 0 is grey, 1 is the game's own.");
    changed |= slider("Black level##pic_black", &c.black, -0.5, 0.25, "%.3f");
    tip("Below 0 pulls the darkest tones down to black (against a hazy, washed-out look); above 0 lifts them. White stays. 0 is the game's own.");
    changed |= slider("Gamma##pic_gamma", &c.gamma, 0.5, 2.0, "%.2f");
    tip("Mid-tone brightness: above 1 brightens, below 1 darkens. Black and white stay. 1 is the game's own.");
    if (changed) {
        extras::set_picture_live(c);
        mark(p);
    }
    ImGui::TextDisabled(c.neutral() ? "Neutral: the picture is presented as the game drew it." : "Applies live; Apply and save keeps it.");
}

void graphics_page(Page& p) {
    hdr_group(p);
    picture_group(p);
    color_group(p);
    gpu_group(p);
}

// ---- Audio --------------------------------------------------------------------------------------------------------------------------

void load_audio_devices(Page& p) {
    p.audio_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) p.audio_devices.push_back(d.name);
    p.audio_loaded = true;
}

void audio_page(Page& p) {
    AudioSettings& a = p.edit.audio;
    card("Volume");
    float pct = static_cast<float>(a.volume * 100.0);
    if (slider_float("Master volume", &pct, 0.0f, 100.0f, "%.0f%%")) {
        a.volume = std::clamp(static_cast<double>(pct), 0.0, 100.0) / 100.0;
        mark(p);
    }
    tip("The volume of everything the game plays, applied as a gain in the audio output. [audio] volume in the ini or NXRT_AUDIO_VOLUME wins at the start.");
    if (check("Mute when the window is not in focus", &a.mute_unfocused)) mark(p);

    card("Output");
    if (!p.audio_loaded) load_audio_devices(p);
    const bool is_default = a.device.empty() || a.device == "default";
    const char* preview = is_default ? "System default" : a.device.c_str();
    if (combo("Output device", preview)) {
        if (ImGui::Selectable("System default", is_default) && !is_default) {
            a.device = "default";
            mark(p);
        }
        for (const std::string& name : p.audio_devices) {
            if (ImGui::Selectable(name.c_str(), !is_default && a.device == name) && a.device != name) {
                a.device = name;
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("Where the sound plays. The game opens its audio device once, so a new choice takes effect at the next start. [audio] device in the ini wins over this.");
    if (ImGui::Button("Refresh the device list")) load_audio_devices(p);
    ImGui::SameLine();
    ImGui::TextDisabled("%d devices", static_cast<int>(p.audio_devices.size()));
    if (!is_default && std::find(p.audio_devices.begin(), p.audio_devices.end(), a.device) == p.audio_devices.end())
        ImGui::TextColored(col_warn(), "'%s' is not connected: the system default is used.", a.device.c_str());
}

// ---- Bindings -----------------------------------------------------------------------------------------------------------------------

std::string input_button_text(const Binding& b, bool waiting) {
    if (waiting) return "press a key...";
    if (b.in.kind == InKind::None) return "<none>";
    const std::string name = input_name(b.in);
    return name.empty() ? "?" : name;
}

// The quit key ([quit] key): opens the "Close the game?" prompt and never reaches the game. Part of the working copy.
void quit_key_group(UiHost& host, Page& p) {
    card("Quit shortcut");
    const std::string key = p.edit.quit.key.kind == InKind::Key ? input_name(p.edit.quit.key) : std::string();
    row("Quit key##quit_key_row");
    const std::string label = (p.quit_capture ? std::string("Press a key...") : (key.empty() ? std::string("none") : key) + "   Change") + "##quit_key";
    if (ImGui::Button(label.c_str())) {
        stop_rebind(host, p);
        stop_fps_capture(host, p);
        set_notice(p, "", false);
        p.quit_capture = true;
        host.begin_capture();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(p.edit.quit.key.kind == InKind::None);
    if (ImGui::Button("Clear##quit_key")) {
        if (p.quit_capture) stop_rebind(host, p);
        p.edit.quit.key = {};
        mark(p);
    }
    ImGui::EndDisabled();
    tip("A keyboard key that asks \"Close the game?\". The key never reaches the game. The gamepad's combination is on the Gamepad tab.");
}

void bindings_tab(UiHost& host, Page& p) {
    if (combo("Context", ctx_name(p.ctx))) {
        for (int c = 0; c < kCtxCount; ++c) {
            const Ctx ctx = static_cast<Ctx>(c);
            if (ImGui::Selectable(ctx_name(ctx), ctx == p.ctx) && ctx != p.ctx) {
                stop_rebind(host, p);
                p.ctx = ctx;
            }
        }
        ImGui::EndCombo();
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    int remove = -1;
    if (ImGui::BeginTable("bindings", 5, kFlags, ImVec2(0.0f, px(320.0f)))) {
        ImGui::TableSetupColumn("Input");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, px(110.0f));
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, px(90.0f));
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, px(32.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
            Binding& b = p.edit.bindings[i];
            if (b.ctx != p.ctx) continue;
            const int row = static_cast<int>(i);
            ImGui::PushID(row);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool waiting = p.rebind == row;
            const std::vector<int> clash = conflicts_of(p, row);
            const std::string label = input_button_text(b, waiting) + "##in";
            if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, col_accent());
            else if (!clash.empty()) ImGui::PushStyleColor(ImGuiCol_Button, col_warn_tint());
            const bool clicked = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f));
            if (waiting || !clash.empty()) ImGui::PopStyleColor();
            if (!clash.empty() && ImGui::IsItemHovered()) {
                std::string who;
                for (int c : clash) who += std::string(who.empty() ? "" : ", ") + act_label(p.edit.bindings[static_cast<std::size_t>(c)].act);
                ImGui::SetItemTooltip("Also bound here to: %s", who.c_str());
            }
            if (clicked) {
                stop_fps_capture(host, p);
                p.quit_capture = false;
                set_notice(p, "", false);
                p.rebind = row;
                host.begin_capture();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (combo("##act", act_label(b.act))) {
                for (int a = 0; a < kActCount; ++a) {
                    const Act act = static_cast<Act>(a);
                    if (ImGui::Selectable(act_label(act), act == b.act) && act != b.act) {
                        b.act = act;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (combo("##trig", trig_name(b.trig))) {
                for (Trig t : kTrigChoices) {
                    if (ImGui::Selectable(trig_name(t), t == b.trig) && t != b.trig) {
                        b.trig = t;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(3);
            const bool timed = b.trig == Trig::Tap || b.trig == Trig::Hold || b.trig == Trig::DoubleTap;
            ImGui::BeginDisabled(!timed);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (int_field("##ms", &b.ms, 0, 5000, 0, 0)) mark(p);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("x")) remove = row;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (remove >= 0) {
        if (p.rebind == remove) stop_rebind(host, p);
        else if (p.rebind > remove) --p.rebind;
        p.edit.bindings.erase(p.edit.bindings.begin() + remove);
        mark(p);
    }

    if (ImGui::Button("Add binding")) {
        Binding b;
        b.in = InputId{};
        b.ctx = p.ctx;
        b.act = Act::Primary;
        b.trig = Trig::Press;
        b.ms = 0;
        p.edit.bindings.push_back(b);
        stop_fps_capture(host, p);
        p.quit_capture = false;
        set_notice(p, "", false);
        p.rebind = static_cast<int>(p.edit.bindings.size()) - 1;
        host.begin_capture();
        mark(p);
    }
    // Inputs used twice in one context with the same trigger (their rows are orange): both actions happen when it is pressed.
    int shown = 0;
    for (std::size_t i = 0; i < p.edit.bindings.size() && shown < 6; ++i) {
        const Binding& b = p.edit.bindings[i];
        if (b.ctx != p.ctx) continue;
        for (int c : conflicts_of(p, static_cast<int>(i))) {
            if (c < static_cast<int>(i)) continue;   // each pair once
            ImGui::TextColored(col_warn(), "%s is bound to %s and to %s", input_name(b.in).c_str(), act_label(b.act),
                               act_label(p.edit.bindings[static_cast<std::size_t>(c)].act));
            if (++shown >= 6) break;
        }
    }
    ImGui::TextWrapped("Click an input and press the key or mouse button (or turn the wheel) to bind it, Escape cancels. "
                       "Bindings of a context win over the 'all' layer for the same input. Action 'none' switches an input off in that context.");
    quit_key_group(host, p);
}

// ---- Switch layout ------------------------------------------------------------------------------------------------------------------

void switch_tab(Page& p) {
    // The cycling actions by the game's names (mpr: beam_next and beam_prev ... beam_1 to beam_4).
    const std::string help = std::string("What each action presses on the Switch pad: buttons A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK "
                                         "and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT, joined by +. ") +
                             act_info(Act::SetANext).name + " and " + act_info(Act::SetAPrev).name + " have no fixed output (they pick " +
                             act_info(Act::SetA1).name + " to " + act_info(Act::SetA4).name + ").";
    ImGui::TextWrapped("%s", help.c_str());
    if (ImGui::BeginTable("switch_out", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
        ImGui::TableSetupColumn("Output");
        ImGui::TableHeadersRow();
        for (int a = 1; a < kActCount; ++a) {
            const bool bad = !p.out_err[a].empty();
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(act_info(static_cast<Act>(a)).name);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bad) {
                ImGui::PushStyleColor(ImGuiCol_Border, col_danger());
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (ImGui::InputText("##out", p.out_text[a], sizeof p.out_text[a])) {
                Output o;
                std::string err;
                if (parse_output(p.out_text[a], &o, &err)) {
                    p.edit.out[a] = o;
                    p.out_err[a].clear();
                    mark(p);
                } else {
                    p.out_err[a] = err;   // the previous valid output stays in the working copy
                }
            }
            if (bad) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            if (!p.out_err[a].empty()) ImGui::TextColored(col_danger(), "%s", p.out_err[a].c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset this table to the defaults")) {
        for (int a = 1; a < kActCount; ++a) {
            Output o;
            if (parse_output(act_info(static_cast<Act>(a)).default_out, &o)) p.edit.out[a] = o;
        }
        refresh_out_text(p);
        mark(p);
    }
}

// ---- Status -------------------------------------------------------------------------------------------------------------------------

// Live view only: nothing here touches the working copy.
void status_tab(const LiveStatus& live) {
    ImGui::Text("Context: %s", ctx_name(live.ctx));
    ImGui::Text("Aim: %s", cstr(live.aim));
    ImGui::Text("Mouse captured: %s", yes_no(live.mouse_captured));
    ImGui::Text("Pitch: %.1f degrees", live.pitch_deg);
    if (live.input_calls == 0) ImGui::TextDisabled("ProcessInput calls: 0 (the game has not reached gameplay yet)");
    else ImGui::Text("ProcessInput calls: %llu", static_cast<unsigned long long>(live.input_calls));

    card("Switch pad output");
    for (int i = 0; i < kSwitchButtonCount; ++i) {
        const bool on = (live.out.buttons & hid::btn(kSwitchButtons[i].bit)) != 0;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? col_accent() : col_off());
        ImGui::Button(kSwitchButtons[i].name, ImVec2(px(56.0f), 0.0f));
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (i % 8 != 7) ImGui::SameLine();
    }
    ImGui::Text("LS (%5.2f, %5.2f)   RS (%5.2f, %5.2f)", live.out.lx, live.out.ly, live.out.rx, live.out.ry);

    card("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro: %s    Rumble: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Controls and Game --------------------------------------------------------------------------------------------------------------

// A row of pills that picks one of several views (the Controls page's parts); true when another one was picked.
bool pills(const char* id, const char* const* names, int n, int* cur) {
    const th::Palette& pal = th::pal();
    ImGui::PushID(id);
    bool changed = false;
    const float h = std::round(th::px(th::kControlSm));
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        th::push(th::Type::Strong);
        const ImVec2 ts = ImGui::CalcTextSize(names[i]);
        const float w = ts.x + 2.f * th::sp(th::S4);
        if (i) {
            ImGui::SameLine(0.f, th::sp(th::S1));
            if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
        }
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
        const ImGuiID iid = ImGui::GetID("##pill");
        ImGui::ItemSize(bb);
        if (ImGui::ItemAdd(bb, iid)) {
            bool hov = false, held = false;
            if (ImGui::ButtonBehavior(bb, iid, &hov, &held) && *cur != i) {
                *cur = i;
                changed = true;
            }
            const bool sel = *cur == i;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (sel || hov) dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(sel ? pal.accent_tint : pal.surface2), h * 0.5f);
            dl->AddText(ImVec2(bb.Min.x + th::sp(th::S4), bb.Min.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(sel ? pal.accent : hov ? pal.text : pal.text2), names[i]);
            ImGui::RenderNavCursor(bb, iid, ImGuiNavRenderCursorFlags_None, h * 0.5f);
        }
        th::pop();
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, th::sp(th::S2)));
    return changed;
}

// The Controls page: the button prompt style on top, then mouse, keyboard bindings, gamepad, the Switch layout and the live status.
void controls_page(UiHost& host, const LiveStatus& live, Page& p) {
    card("##prompts");
    prompts_row(live, p);
    end_card();
    static const char* const kParts[] = {"Mouse", "Keys and buttons", "Controller", "Switch layout", "Status"};
    if (pills("controls_parts", kParts, 5, &p.controls_part)) stop_rebind(host, p);
    switch (p.controls_part) {
        case 0:
            card("Mouse");
            mouse_tab(p);
            break;
        case 1:
            card("Keyboard and mouse bindings");
            bindings_tab(host, p);
            break;
        case 2:
            card("Controller");
            pad_tab(live, p);
            break;
        case 3:
            if (!p.out_tab_open) refresh_out_text(p);
            card("What each action presses");
            switch_tab(p);
            break;
        default:
            card("Live");
            status_tab(live);
            break;
    }
    p.out_tab_open = p.controls_part == 3;
    end_card();
}

// ---- Frame generation -----------------------------------------------------------------------------------------------------------------

constexpr const char* kFgModeNames[] = {"Off", "2x", "3x", "4x", "Display"};
constexpr const char* kFgBackendNames[] = {"Auto", "DLSS frame generation", "FSR 3 frame generation", "MetalFX frame interpolation"};

// ---- the pins (Performance HUD) ---------------------------------------------------------------------------------------------------------

// Controller chords a pin can use (hotkeys.h: a button that presses nothing in the game, held first).
constexpr const char* kPinPadChords[] = {"left_paddle1+right_paddle1", "left_paddle1+dpad_up", "left_paddle1+dpad_down", "right_paddle1+dpad_up",
                                         "right_paddle1+dpad_down", "touchpad+left_shoulder", "touchpad+right_shoulder", "touchpad+dpad_up"};

void hud_edited(Page& p) {
    p.edit.hud.in_file = true;
    mark(p);
}

pins::Group* group_with(perf::HudSettings& h, const std::string& item) {
    for (pins::Group& g : h.groups)
        if (std::find(g.items.begin(), g.items.end(), item) != g.items.end()) return &g;
    return nullptr;
}

// The first of F8 F9 F10 F11 no other toggle uses, for a new group (none when all are taken).
KeyChord free_pin_key(const perf::HudSettings& h, const MenuFrame& f) {
    for (int sc : {SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11}) {
        bool used = h.key.sc == sc;
        for (const pins::Group& g : h.groups) used = used || g.key.sc == sc;
        for (const std::string& k : {f.menu_key, f.cheats_key, f.pins_key}) {
            KeyChord c;
            used = used || (parse_key_chord(k, &c) && c.sc == sc && c.mods == 0);
        }
        if (!used) return {sc, 0};
    }
    return {};
}

void pins_page(UiHost& host, Page& p, const MenuFrame& f) {
    perf::HudSettings& h = p.edit.hud;
    const th::Palette& pal = th::pal();
    card("Pins over the game");
    note("Pinned readouts stay on screen while this menu is closed. They only show: every key, click and button still goes to the game.");
    if (check("Show pins##pins_all", &h.all_shown)) hud_edited(p);
    tip("Hides or shows every pin at once (the hide-all key does the same in the game).");
    row("Hide or show every pin##pins_all_key");
    ImGui::Text("%s", f.pins_key.empty() ? "no key" : f.pins_key.c_str());
    tip("The hide-all key is set in the app's Settings (In-game shortcuts), for every game or for this one.");

    // quick pins: each readout on its own, in the Performance group
    for (int pass = 0; pass < 2; ++pass) {
        card(pass == 0 ? "Performance readouts" : "Settings as readouts");
        for (const perf::ItemInfo& it : perf::catalogue()) {
            if (it.setting != (pass == 1)) continue;
            bool on = group_with(h, it.id) != nullptr;
            const std::string label = std::string(it.name) + "##pin_" + it.id;
            if (check(label.c_str(), &on)) {
                if (on) {
                    pins::Group* g = nullptr;
                    for (pins::Group& o : h.groups)
                        if (o.id == "perf") g = &o;
                    if (!g) {
                        pins::Group ng;
                        ng.id = "perf";
                        ng.name = "Performance";
                        ng.key = free_pin_key(h, f);
                        h.groups.insert(h.groups.begin(), ng);
                        g = &h.groups.front();
                    }
                    g->items.push_back(it.id);
                    g->shown = true;
                } else {
                    for (pins::Group& g : h.groups) g.items.erase(std::remove(g.items.begin(), g.items.end(), std::string(it.id)), g.items.end());
                }
                hud_edited(p);
            }
            tip(it.desc);
        }
    }

    // the groups: each a pinned panel of its own, with its own toggle
    const uint32_t free_bits = free_pad_buttons(p.edit.pad.remap);
    int remove = -1;
    for (size_t gi = 0; gi < h.groups.size(); ++gi) {
        pins::Group& g = h.groups[gi];
        const std::string title = "Pin: " + (g.name.empty() ? g.id : g.name) + "##pin_card_" + g.id;
        card(title.c_str());
        ImGui::PushID(g.id.c_str());
        char name[48];
        std::snprintf(name, sizeof name, "%s", g.name.c_str());
        if (ImGui::InputText(row("Name##pin_name"), name, sizeof name)) {
            g.name = name;
            hud_edited(p);
        }
        if (check("Shown##pin_shown", &g.shown)) hud_edited(p);
        tip("This pin's own toggle (its key does the same in the game).");
        static const char* const kLayouts[] = {"Vertical", "Horizontal", "Compact"};
        int layout = static_cast<int>(g.layout);
        if (segmented("Layout##pin_layout", kLayouts, 3, &layout)) {
            g.layout = static_cast<pins::Layout>(layout);
            hud_edited(p);
        }
        if (slider_int("Size##pin_size", &g.size, 50, 200, "%d%%")) hud_edited(p);
        if (slider_int("Opacity##pin_opacity", &g.opacity, 20, 100, "%d%%")) hud_edited(p);
        // its key: captured like a binding (the key never reaches the game)
        row("Toggle key##pin_key");
        const bool waiting = p.pin_capture == static_cast<int>(gi);
        const std::string kt = (waiting ? std::string("Press a key...") : g.key.empty() ? std::string("none") : key_chord_text(g.key)) + "##pin_key_btn";
        if (ImGui::Button(kt.c_str(), ImVec2(th::px(150.f), 0.f))) {
            stop_rebind(host, p);
            set_notice(p, "", false);
            p.pin_capture = static_cast<int>(gi);
            host.begin_capture();
        }
        tip("A key that shows or hides this pin in the game. It never reaches the game.");
        ImGui::SameLine();
        if (ImGui::Button("Clear##pin_key_clear")) {
            g.key = {};
            hud_edited(p);
        }
        // its controller chord: opt-in, off by default
        const std::string pad_now = g.pad ? pad_combo_text(g.pad) : std::string("None (keyboard only)");
        if (combo("Controller chord##pin_pad", pad_now.c_str())) {
            if (ImGui::Selectable("None (keyboard only)", g.pad == 0) && g.pad) {
                g.pad = 0;
                hud_edited(p);
            }
            for (const char* c : kPinPadChords) {
                uint32_t bits = 0;
                parse_pad_combo(c, &bits);
                const bool usable = pin_pad_problem(bits, free_bits).empty();
                ImGui::BeginDisabled(!usable);
                if (ImGui::Selectable(c, g.pad == bits) && g.pad != bits) {
                    g.pad = bits;
                    hud_edited(p);
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        tip("Optional. Hold the rear grip (or trackpad click) first, then the other button: the game sees neither, so it never presses anything in the game.");
        if (g.pad) {
            const std::string why = pin_pad_problem(g.pad, free_bits);
            if (!why.empty()) warn("This chord is not used: %s.", why.c_str());
        }
        // its items, in order
        row("Readouts##pin_items");
        ImGui::BeginGroup();
        int up = -1, down = -1, drop = -1;
        for (size_t ii = 0; ii < g.items.size(); ++ii) {
            ImGui::PushID(static_cast<int>(ii));
            const perf::ItemInfo* in = perf::item_info(g.items[ii]);
            if (th::icon_button("##up", th::Icon::Up, "Move up", th::Btn::Quiet, th::kControlSm)) up = static_cast<int>(ii);
            ImGui::SameLine(0.f, th::sp(th::S1));
            if (th::icon_button("##down", th::Icon::Down, "Move down", th::Btn::Quiet, th::kControlSm)) down = static_cast<int>(ii);
            ImGui::SameLine(0.f, th::sp(th::S1));
            if (th::icon_button("##drop", th::Icon::Close, "Remove from this pin", th::Btn::Quiet, th::kControlSm)) drop = static_cast<int>(ii);
            ImGui::SameLine(0.f, th::sp(th::S2));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(in ? in->name : g.items[ii].c_str());
            ImGui::PopID();
        }
        if (up > 0) std::swap(g.items[static_cast<size_t>(up)], g.items[static_cast<size_t>(up - 1)]);
        if (down >= 0 && down + 1 < static_cast<int>(g.items.size())) std::swap(g.items[static_cast<size_t>(down)], g.items[static_cast<size_t>(down + 1)]);
        if (drop >= 0) g.items.erase(g.items.begin() + drop);
        if (up > 0 || (down >= 0 && down + 1 <= static_cast<int>(g.items.size())) || drop >= 0) hud_edited(p);
        if (g.items.empty()) ImGui::TextDisabled("Nothing yet");
        ImGui::SetNextItemWidth(th::px(260.f));
        if (ImGui::BeginCombo("##pin_add", "Add a readout...")) {
            for (const perf::ItemInfo& it : perf::catalogue()) {
                if (std::find(g.items.begin(), g.items.end(), std::string(it.id)) != g.items.end()) continue;
                if (ImGui::Selectable(it.name)) {
                    g.items.push_back(it.id);
                    hud_edited(p);
                }
                tip(it.desc);
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
        if (th::button("Put it back in the corner##pin_reset", th::Icon::Undo, th::Btn::Quiet)) {
            g.x = g.y = -1.f;
            hud_edited(p);
        }
        ImGui::SameLine();
        if (th::button("Delete this pin##pin_delete", th::Icon::Trash, th::Btn::Quiet)) remove = static_cast<int>(gi);
        ImGui::PopID();
    }
    if (remove >= 0) {
        h.groups.erase(h.groups.begin() + remove);
        hud_edited(p);
    }
    end_card();
    if (th::button("New pin group", th::Icon::Plus, th::Btn::Secondary)) {
        pins::Board b;
        b.groups = h.groups;
        pins::Group g;
        g.id = pins::new_group_id(b);
        g.name = "Group " + std::to_string(h.groups.size() + 1);
        g.key = free_pin_key(h, f);
        h.groups.push_back(g);
        hud_edited(p);
    }
    tip("A pin that shows several readouts together: add them from its Readouts list.");
    ImGui::SameLine();
    th::text(th::Type::Small, pal.text3, "Drag a pin to move it while this menu is open.");
}

// ---- the menu --------------------------------------------------------------------------------------------------------------------------

enum MenuPage : int { PgDisplay = 0, PgFrameGen, PgUpscaler, PgGraphics, PgAudio, PgControls, PgPins, PgGame, PgAbout, PgCount };
constexpr const char* kPageNames[PgCount] = {"Display & FPS", "Frame generation", "Upscaler", "Graphics", "Audio", "Controls", "Performance HUD", "Game", "About"};
constexpr th::Icon kPageIcons[PgCount] = {th::Icon::Monitor, th::Icon::Layers, th::Icon::Expand, th::Icon::Camera, th::Icon::Volume,
                                          th::Icon::Pad, th::Icon::Pin, th::Icon::Star, th::Icon::Info};

bool page_shown(int i) {
    if (i == PgFrameGen) return false;   // PokoMac has no frame generation: the page stays in the list, hidden
    if (i != PgGame) return true;
    const GamePlay* gp = game_play();
    return gp && gp->tweaks_tab;
}

void step_page(Page& p, int d) {
    for (int n = 0; n < PgCount; ++n) {
        p.page = (p.page + d + PgCount) % PgCount;
        if (page_shown(p.page)) break;
    }
    p.page_changed = true;
}

void upscaler_page(Page& p) {
    upscale_group(p);
    card("Picture size");
    render_size_line();
    end_card();
}

void display_fps_page(UiHost& host, const LiveStatus& live, Page& p) {
    framerate_group(host, live, p);
    window_group(live, p);
    resolution_group(p);
    latency_group(p);
    card("Now");
    ImGui::Text("Window: %s, present mode: %s", cstr(live.window_mode), cstr(live.present_mode));
    ImGui::Text("Display %.2f Hz, window %d x %d, %.1f fps", live.refresh_hz, live.width, live.height, live.fps);
    end_card();
}

void about_page(UiHost& host, Page& p, const MenuFrame& f) {
    card("Settings file");
    ImGui::PushStyleColor(ImGuiCol_Text, th::pal().text2);
    ImGui::TextWrapped("%s", host.settings_path().c_str());
    ImGui::PopStyleColor();
    note("Changes apply and save on their own as you make them.");
    if (th::button("Apply and save", th::Icon::Check, th::Btn::Primary)) apply_now(host, p);
    tip("Changes apply and save on their own as you make them; this does it now. Items marked 'Applies at the next start' take effect then.");
    ImGui::SameLine();
    if (th::button("Revert", th::Icon::Undo)) revert_to_saved(host, p);
    tip("Drops the changes made since the last Apply and save.");
    ImGui::SameLine();
    if (th::button("Factory defaults", th::Icon::Refresh)) load_factory(host, p);
    tip("Puts every setting back to the factory default (applied and saved at once).");
    end_card();
    draw_about_page(host.settings_path());
    end_card();
    if (f.diagnostics) {
        card("Diagnostics");
        f.diagnostics();
        end_card();
    }
}

// The panel's head: the game's icon and name, what runs now, and the menu's buttons. True: close.
bool menu_header(UiHost& host, Page& p, const LiveStatus& live) {
    const th::Palette& pal = th::pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float disc = std::round(th::px(40.f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (ImTextureData* icon = cheats::ui::game_icon()) {
        dl->AddImageRounded(icon->GetTexRef(), pos, ImVec2(pos.x + disc, pos.y + disc), ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(IM_COL32_WHITE),
                            th::px(th::kRadiusSm));
        dl->AddRect(pos, ImVec2(pos.x + disc, pos.y + disc), ImGui::GetColorU32(pal.line_strong), th::px(th::kRadiusSm));
        const float b = std::round(th::px(18.f));
        const ImVec2 bc(pos.x + disc - b * 0.3f, pos.y + disc - b * 0.3f);
        dl->AddCircleFilled(bc, b * 0.5f + th::px(2.f), ImGui::GetColorU32(pal.panel | IM_COL32_A_MASK));
        dl->AddCircleFilled(bc, b * 0.5f, ImGui::GetColorU32(pal.accent));
        th::icon(dl, th::Icon::Gear, bc, b * 0.62f, ImGui::GetColorU32(pal.on_accent));
    } else {
        dl->AddCircleFilled(ImVec2(pos.x + disc * 0.5f, pos.y + disc * 0.5f), disc * 0.5f, ImGui::GetColorU32(pal.accent));
        th::icon(dl, th::Icon::Gear, ImVec2(pos.x + disc * 0.5f, pos.y + disc * 0.5f), th::px(22.f), ImGui::GetColorU32(pal.on_accent));
    }
    ImGui::Dummy(ImVec2(disc, disc));
    ImGui::SameLine(0, th::sp(th::S3));
    ImGui::BeginGroup();
    const char* title = game::info().title;
    const bool named = title && *title;
    th::push(th::Type::Title);
    ImGui::TextUnformatted(named ? title : "Settings");
    th::pop();
    char sub[160];
    if (live.fps > 0) std::snprintf(sub, sizeof sub, "%sSettings  \xC2\xB7  %.0f fps  \xC2\xB7  %d x %d", named ? "" : "AnySwitch ", live.fps, live.width, live.height);
    else std::snprintf(sub, sizeof sub, "%s", named ? "Settings" : "AnySwitch");
    th::text(th::Type::Small, pal.text2, "%s", sub);
    ImGui::EndGroup();
    // right side: the state chip, dock, quit and close
    const float bh = th::px(th::kControlH);
    th::push(th::Type::Body);
    const float quit_w = ImGui::CalcTextSize("Quit game").x + th::sp(th::S4) * 2.f + bh * 0.6f;
    th::pop();
    const char* chip = host.capturing() ? "Press a key" : p.dirty ? "Saving" : nullptr;
    th::push(th::Type::Small);
    const float chip_w = chip ? ImGui::CalcTextSize(chip).x + th::px(44.f) : 0.f;
    th::pop();
    const float buttons_w = bh * 4.f + th::sp(th::S1) * 4.f + quit_w + th::sp(th::S3) * 2.f + chip_w;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - buttons_w));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (disc - bh) * 0.5f);
    if (chip) {
        th::chip(chip, pal.warn_tint, pal.warn, th::Icon::Keyboard);
        ImGui::SameLine(0, th::sp(th::S2));
    }
    static const th::Icon dock_icons[] = {th::Icon::DockLeft, th::Icon::DockCentre, th::Icon::DockRight};
    static const char* dock_tips[] = {"Dock on the left", "In the centre", "Dock on the right"};
    for (int d = 0; d < 3; ++d) {
        ImGui::PushID(d);
        if (th::icon_button("##dock", dock_icons[d], dock_tips[d], p.dock == d ? th::Btn::Secondary : th::Btn::Quiet)) p.dock = d;
        ImGui::PopID();
        ImGui::SameLine(0, th::sp(th::S1));
    }
    ImGui::SameLine(0, th::sp(th::S2));
    if (th::button("Quit game", th::Icon::Close, th::Btn::Secondary)) host.request_quit_prompt();
    tip("Asks before closing the game. The game's own save is not touched: save in the game first.");
    ImGui::SameLine(0, th::sp(th::S1));
    const bool close = th::icon_button("##close", th::Icon::Close, "Close the menu");
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), pos.y - ImGui::GetWindowPos().y + disc + th::sp(th::S3)));
    return close;
}

// The pages down the left: an icon and a name each (only icons when the panel is narrow).
void menu_rail(Page& p, float w, bool compact) {
    const th::Palette& pal = th::pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < PgCount; ++i) {
        if (!page_shown(i)) continue;
        ImGui::PushID(i);
        th::push(th::Type::Strong);
        const float h = std::round(th::px(th::kControlH));
        const float isz = std::round(ImGui::GetFontSize() * 1.15f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
        const ImGuiID id = ImGui::GetID("##rail");
        ImGui::ItemSize(bb);
        if (ImGui::ItemAdd(bb, id)) {
            bool hov = false, held = false;
            if (ImGui::ButtonBehavior(bb, id, &hov, &held) && p.page != i) {
                p.page = i;
                p.page_changed = true;
            }
            const bool sel = p.page == i;
            if (sel || hov) dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(sel ? pal.accent_tint : pal.surface2), th::px(th::kRadiusSm));
            const ImU32 fg = ImGui::GetColorU32(sel ? pal.accent : hov ? pal.text : pal.text2);
            const float ix = compact ? bb.Min.x + w * 0.5f : bb.Min.x + th::sp(th::S3) + isz * 0.5f;
            th::icon(dl, kPageIcons[i], ImVec2(ix, bb.Min.y + h * 0.5f), isz, fg);
            if (!compact) {
                dl->PushClipRect(bb.Min, bb.Max, true);
                dl->AddText(ImVec2(bb.Min.x + th::sp(th::S3) + isz + th::sp(th::S2), bb.Min.y + (h - ImGui::GetFontSize()) * 0.5f), fg, kPageNames[i]);
                dl->PopClipRect();
            } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", kPageNames[i]);
            }
            if (sel && p.focus_rail) {
                ImGui::SetItemDefaultFocus();
                p.focus_rail = false;
            }
            ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, th::px(th::kRadiusSm));
        }
        th::pop();
        ImGui::PopID();
    }
}

// The foot of the panel: the description of the focused row, then the controls of the device in use and the menu's own toggles.
void menu_footer(const MenuFrame& f, Page& p, const std::string& desc) {
    const th::Palette& pal = th::pal();
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(lp.x, lp.y + th::sp(th::S1)), ImVec2(lp.x + avail, lp.y + th::sp(th::S1)), ImGui::GetColorU32(pal.line));
    ImGui::SetCursorScreenPos(ImVec2(lp.x, lp.y + th::sp(th::S2)));
    // one line of description (the notice of a binding wins)
    th::push(th::Type::Small);
    const float line_h = ImGui::GetFontSize() * 2.4f;
    const ImVec2 dp = ImGui::GetCursorScreenPos();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail);
    if (!p.notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.notice_warn ? pal.warn : pal.ok);
        ImGui::TextWrapped("%s", p.notice.c_str());
        ImGui::PopStyleColor();
    } else if (!f.conflicts.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.warn);
        ImGui::TextWrapped("Shortcut conflict: %s", f.conflicts.front().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, desc.empty() ? pal.text3 : pal.text2);
        ImGui::TextWrapped("%s", desc.empty() ? "Changes apply and save as you make them. Point at a setting to read what it does." : desc.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    th::pop();
    ImGui::SetCursorScreenPos(ImVec2(dp.x, dp.y + line_h));
    // hints on the left, toggles on the right
    struct H { const char* key; const char* label; };
    std::vector<H> hs;
    if (f.pad) hs = {{"A", "Select"}, {"B", "Back"}, {"L R", "Pages"}, {"+", "Close"}};
    else hs = {{"Enter", "Select"}, {"Esc", "Back"}, {"Ctrl+PgUp/PgDn", "Pages"}};
    std::string toggles = "Menu " + (f.menu_key.empty() ? std::string("none") : f.menu_key);
    if (!f.menu_pad.empty()) toggles += " / " + f.menu_pad;
    if (!f.cheats_key.empty()) toggles += "   \xC2\xB7   Cheats menu " + f.cheats_key;
    if (!f.pins_key.empty()) toggles += "   \xC2\xB7   Pins " + f.pins_key;
    th::push(th::Type::Small);
    const float tw = ImGui::CalcTextSize(toggles.c_str()).x;
    th::pop();
    const float gap = th::sp(th::S5);
    auto total = [&] {
        float t = 0.f;
        for (const H& h : hs) t += th::hint_width(h.key, h.label, f.pad) + gap;
        return t;
    };
    while (hs.size() > 2 && total() + tw > avail) hs.erase(hs.begin() + static_cast<long>(hs.size() - 2));
    const float y0 = ImGui::GetCursorPosY();
    for (size_t i = 0; i < hs.size(); ++i) {
        if (i) ImGui::SameLine(0, gap);
        th::hint(hs[i].key, hs[i].label, f.pad);
    }
    if (total() + tw <= avail + gap) {
        ImGui::SameLine(ImGui::GetCursorPosX() + 0.f);
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - tw, y0 + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.84f) * 0.5f));
        th::text(th::Type::Small, pal.text3, "%s", toggles.c_str());
    }
}

Page& the_page() {
    static Page page;
    return page;
}

void load_page(UiHost& host, Page& p) {
    static uint64_t seen_game_edits = 0;
    if (const uint64_t n = game_menu_edits(); n != seen_game_edits && !p.dirty) {   // changed in the game's own menu: F1's Apply must not put the old values back
        seen_game_edits = n;
        p.loaded = false;
    }
    if (!p.loaded) {
        p.edit = host.settings();
        p.edit.fps = host.settings().fps;
        refresh_out_text(p);
        p.loaded = true;
    } else if (!p.dirty && ImGui::GetTime() - p.synced_at > 0.25) {
        // the pins' toggles and the counter's hotkey change these in the game while the menu is open: follow them
        const Settings cur = host.settings();
        p.edit.hud = cur.hud;
        p.edit.fps = cur.fps;
        p.synced_at = ImGui::GetTime();
    }
}

}  // namespace

void draw_settings_page(UiHost& host) {
    MenuFrame f;
    draw_menu(host, f);
}

int menu_page() { return the_page().page; }
void menu_scroll(float units) { the_page().scroll_to = units; }

void menu_show(int page, int controls_part, int dock) {
    Page& p = the_page();
    if (page >= 0 && page < PgCount) {
        p.page = page;
        p.page_changed = true;
    }
    if (controls_part >= 0) p.controls_part = controls_part;
    if (dock >= 0) p.dock = dock;
}

void draw_menu(UiHost& host, MenuFrame& f) {
    Page& p = the_page();
    load_page(host, p);
    poll_capture(host, p);
    const LiveStatus live = host.status();
    const th::Palette& pal = th::pal();
    if (ImGui::GetFrameCount() > p.last_frame + 1) {   // opened again: the fade starts, the controller's focus goes to the rail
        p.focus_rail = true;
        p.shown_at = ImGui::GetTime();
    }
    p.last_frame = ImGui::GetFrameCount();
    // L / R (or Ctrl+PgUp / PgDn) step through the pages; ZL / ZR scroll the page
    const bool typing = ImGui::GetIO().WantTextInput || host.capturing() || ImGui::GetTopMostPopupModal() != nullptr;
    if (!typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageUp)) step_page(p, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageDown)) step_page(p, 1);
    }
    if (!page_shown(p.page)) p.page = PgDisplay;
    th::push(th::Type::Body);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (cheats::ui::animations()) ImGui::GetStyle().Alpha = static_cast<float>(std::clamp((ImGui::GetTime() - p.shown_at) / 0.15, 0.15, 1.0));
    ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), pal.scrim_top, pal.scrim_top,
                                                            pal.scrim_bottom, pal.scrim_bottom);
    // the panel: docked left or right (full height, about 44 % of the width) or in the centre
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;
    const float margin = th::sp(th::S6);
    if (p.dock != 1) {
        const float pw = std::min(W - 2.f * margin, std::clamp(W * 0.44f, th::px(600.f), th::px(820.f)));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (p.dock == 0 ? margin : W - margin - pw), vp->WorkPos.y + margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(pw, H - 2.f * margin), ImGuiCond_Always);
    } else {
        const float pw = std::min(W - 2.f * margin, std::max(th::px(900.f), W * 0.66f));
        const float ph = std::min(H - 2.f * margin, std::max(th::px(560.f), H * 0.86f));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
    }
    const bool glass = th::glass() && (f.backdrop.GetTexID() != ImTextureID_Invalid || f.backdrop._TexData != nullptr);
    f.glass = false;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, glass ? IM_COL32(0, 0, 0, 0) : pal.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, glass ? IM_COL32(0, 0, 0, 0) : pal.line_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(th::sp(th::S6), th::sp(th::S5)));
    const ImGuiWindowFlags mfl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("AnySwitch settings###menu_main", nullptr, mfl)) {
        ImGui::PopStyleVar();
        ImGuiWindow* mw = ImGui::GetCurrentWindow();
        if (glass) {
            const ImRect r = mw->Rect();
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float rr = ImGui::GetStyle().WindowRounding;
            mw->DrawList->AddImageRounded(f.backdrop, r.Min, r.Max, ImVec2(r.Min.x / disp.x, r.Min.y / disp.y), ImVec2(r.Max.x / disp.x, r.Max.y / disp.y),
                                          ImGui::GetColorU32(IM_COL32_WHITE), rr);
            mw->DrawList->AddRectFilled(r.Min, r.Max, ImGui::GetColorU32(pal.panel), rr);
            mw->DrawList->AddRect(r.Min, r.Max, ImGui::GetColorU32(pal.line_strong), rr, 0, std::max(1.f, th::px(1.f)));
            f.glass = true;
            f.glass_rect[0] = static_cast<int>(r.Min.x);
            f.glass_rect[1] = static_cast<int>(r.Min.y);
            f.glass_rect[2] = static_cast<int>(r.GetWidth() + 0.5f);
            f.glass_rect[3] = static_cast<int>(r.GetHeight() + 0.5f);
            f.glass_blur = th::glass_blur();
        }
        if (menu_header(host, p, live)) f.close = true;
        th::push(th::Type::Small);
        const float footer_h = ImGui::GetFontSize() * 2.4f + ImGui::GetFrameHeight() + th::sp(th::S4);
        th::pop();
        const float body_h = std::max(th::px(120.f), ImGui::GetContentRegionAvail().y - footer_h);
        const bool compact = ImGui::GetContentRegionAvail().x < th::px(760.f);
        const float rail_w = compact ? std::round(th::px(th::kControlH) + th::sp(th::S2)) : std::round(th::px(206.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, th::sp(th::S1)));
        ImGui::BeginChild("##menu_rail", ImVec2(rail_w, body_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
        menu_rail(p, rail_w, compact);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SameLine(0.f, th::sp(th::S4));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, th::px(8.f));
        ImGui::BeginChild("##menu_page", ImVec2(0.f, body_h), ImGuiChildFlags_NavFlattened);
        if (p.page_changed) {
            ImGui::SetScrollY(0.f);
            p.page_changed = false;
        } else if (p.scroll_to >= 0.f) {
            ImGui::SetScrollY(p.scroll_to * th::k());
            p.scroll_to = -1.f;
        }
        if (!typing) {   // ZL / ZR: a page up or down
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true)) ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - body_h * 0.8f));
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true)) ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + body_h * 0.8f));
        }
        th::push(th::Type::Title);
        ImGui::TextUnformatted(kPageNames[p.page]);
        th::pop();
        ImGui::Dummy(ImVec2(0.f, th::sp(th::S1)));
        ImGui::PushItemWidth(std::max(th::px(200.0f), ImGui::GetContentRegionAvail().x * 0.42f));
        switch (p.page) {
            case PgDisplay: display_fps_page(host, live, p); break;
            case PgFrameGen: break;
            case PgUpscaler: upscaler_page(p); break;
            case PgGraphics: graphics_page(p); break;
            case PgAudio: audio_page(p); break;
            case PgControls: controls_page(host, live, p); break;
            case PgPins: pins_page(host, p, f); break;
            case PgGame:
                if (const GamePlay* gp = game_play(); gp && gp->tweaks_tab) {   // the game's optional looks and start-up tweaks
                    card("This game");
                    if (gp->tweaks_tab(p.edit)) mark(p);
                    end_card();
                    card("Start-up and menus");
                    if (fastboot_ui(p.edit.fastboot)) mark(p);   // play_ui_fastboot.cpp
                }
                break;
            default: about_page(host, p, f); break;
        }
        end_card();
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        menu_footer(f, p, take_tip());
    } else {
        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    th::pop();
    // the pins over the game: movable while the menu is open (the working copy's, so a move is saved like any change)
    pins::Board b;
    b.groups = p.edit.hud.groups;
    b.all_shown = p.edit.hud.all_shown;
    bool moved = false;
    pins::draw(b, perf::item_lines, true, "menu", &moved);
    if (moved) {
        p.edit.hud.groups = b.groups;
        hud_edited(p);
    }
    // Changes apply and save as they are made: once the edit is finished (no slider held, no text field being typed in) and no binding is
    // being captured. A player changed the frame limit and the upscaler, closed F1 without pressing Apply and save, and the changes were
    // lost ("settings aren't saving"). Items marked 'restart' are saved now and take effect at the next start.
    if (p.dirty && !host.capturing() && !ImGui::IsAnyItemActive()) apply_now(host, p);
}

}  // namespace nxrt::play
