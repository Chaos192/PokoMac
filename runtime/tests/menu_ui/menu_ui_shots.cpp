// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The F1 settings menu's test: its pages, the pins and a group pin drawn offscreen (no window, no GPU: the cheats test's CPU rasterizer,
// tests/cheats_ui/soft_raster.h) at two UI scales over a synthetic game frame, written as PNGs and compared with tests/menu_ui/ref (stored
// at a quarter of the pixels), plus the checks of the toggles and the pins:
//   - a remapped menu key opens the menu and F1 no longer does (it reaches the game);
//   - a pin's own key hides only that pin, the hide-all key hides every pin, and the keys never reach the game;
//   - a pin's controller chord fires only with its free button held first, and holds its other buttons back from the game meanwhile;
//   - game input (keys, the mouse over a pin, the pad) reaches the game the same with the pins shown as hidden;
//   - the settings file keeps the pins ([hud]) and the configuration's default groups are read;
//   - the pins' cost per frame (CPU: theme, the pins' windows, ImGui's own work) stays under 0.1 ms;
//   - the overlay's own screens (overlay_screens.h: the software keyboard, the quit and controller prompts, the counter and notices, a
//     debug window and every themed control) and keyboard / controller focus;
//   - every screen on light, dark and busy backgrounds too (refs at 320 pixels wide: <screen>@<background>.png), and on every screen the
//     theme guard (soft_raster.h theme_guard: the style is the theme's, no vertex in ImGui's stock colours).
//   nxrt_menu_ui_test [--out <dir>] [--ref <dir>] [--update] [--only <screen>]
// Everything it reads and writes is in its own --out folder: never the player's settings or the app's.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/gfx/play/cheats_theme.h"
#include "../../src/gfx/play/cheats_ui.h"
#include "../../src/gfx/play/cheats_ui_internal.h"
#include "../../src/gfx/play/hotkeys.h"
#include "../../src/gfx/play/overlay_screens.h"
#include "../../src/gfx/play/perf_pins.h"
#include "../../src/gfx/play/pin_board.h"
#include "../../src/gfx/play/play_types.h"
#include "../../src/gfx/play/play_ui.h"
#include "../cheats_ui/blur_ref.h"
#include "../cheats_ui/soft_raster.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/gfx.h"
#include "nxrt/input.h"
#include "nxrt/log.h"

namespace fs = std::filesystem;
using namespace nxrt;
namespace th = nxrt::cheats::ui::theme;
namespace pins = nxrt::play::pins;
namespace perf = nxrt::play::perf;

namespace {

int g_failures = 0;
void check(const char* what, bool ok) {
    std::printf("%-6s %s\n", ok ? "ok" : "FAILED", what);
    if (!ok) ++g_failures;
}

// The game under the menu: an evening sky over hills, so translucency and glass show in the pictures.
void game_frame(std::vector<uint8_t>& px, int w, int h) {
    px.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float fy = static_cast<float>(y) / h, fx = static_cast<float>(x) / w;
            float r = 0.95f - 0.45f * fy, g = 0.55f - 0.20f * fy, b = 0.45f + 0.25f * fy;
            const float sx = fx - 0.70f, sy = fy - 0.30f;
            if (sx * sx * 1.78f * 1.78f + sy * sy < 0.010f) { r = 1.f; g = 0.92f; b = 0.55f; }
            const float hill1 = 0.62f + 0.07f * std::sin(fx * 6.0f + 0.4f), hill2 = 0.76f + 0.05f * std::sin(fx * 13.0f + 1.1f);
            if (fy > hill1) { r = 0.20f; g = 0.30f; b = 0.38f; }
            if (fy > hill2) { r = 0.10f; g = 0.18f; b = 0.22f; }
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(r * 255);
            p[1] = static_cast<uint8_t>(g * 255);
            p[2] = static_cast<uint8_t>(b * 255);
        }
}

// The play host as the menu sees it, with the settings in memory (nothing is written anywhere).
struct FakeHost : play::UiHost {
    play::Settings s = play::default_settings();
    bool capturing_ = false;
    play::Settings settings() const override { return s; }
    play::Settings factory() const override { return play::default_settings(); }
    void apply(const play::Settings& n) override { s = n; }
    play::LiveStatus status() const override {
        play::LiveStatus l;
        l.ctx = play::Ctx::Play;
        l.pads = 1;
        l.pad_name = "Xbox Wireless Controller";
        l.prompt_now = "xbox";
        l.fps = 59.9;
        l.present_mode = "fifo";
        l.window_mode = "borderless";
        l.refresh_hz = 144.f;
        l.width = 1920;
        l.height = 1080;
        return l;
    }
    std::string settings_path() const override { return "<game folder>/controls.ini"; }
    void begin_capture() override { capturing_ = true; }
    bool capturing() const override { return capturing_; }
    bool capture_result(play::InputId*) override { return false; }
    void cancel_capture() override { capturing_ = false; }
};
FakeHost g_host;

// The pins' numbers as a game shows them.
void fill_sample() {
    perf::Sample& s = perf::sample();
    s.fps = 59.9;
    s.frame_ms = 16.7;
    s.low1_fps = 54;
    s.max_ms = 21.4;
    s.gpu_ms = 6.42;
    s.render_cpu_ms = 3.18;
    s.fg_on = true;
    s.fg_real_fps = 60;
    s.fg_shown_fps = 120;
    s.fg_backend = "FSR 3";
    s.upscaler = "FSR 1";
    s.scene_pct = 67;
    s.shaders_in_play = 0;
    s.mem_bytes = 3ull << 30;
    s.vram_used = 5ull << 30;
    s.vram_budget = 30ull << 30;
    s.src_w = 1920;
    s.src_h = 1080;
    s.out_w = 2560;
    s.out_h = 1440;
    for (int i = 0; i < perf::Sample::kGraph; ++i)
        s.frame_times[i] = 16.7f + 2.5f * std::sin(i * 0.35f) + (i % 37 == 0 ? 7.f : 0.f);
    s.ft_head = 0;
    s.set_vsync = "on";
    s.set_fps_limit = "60 fps";
    s.set_framegen = "2x";
}

// The pins of the shots: a Performance group (vertical, with the graph), a group pin of several readouts (horizontal) and a compact one.
perf::HudSettings shot_pins() {
    perf::HudSettings h = perf::default_hud();
    std::string err;
    pins::Group g;
    pins::parse_group("perf:fps+frametime+low1+gpu+cpu;name=Performance;at=24,24;size=100;opacity=85;layout=vertical;key=F8", &g, &err);
    h.groups.push_back(g);
    pins::parse_group("g2:upscaler+resolution+vram+shaders;name=Render;at=24,330;size=100;opacity=80;layout=horizontal;key=F9", &g, &err);
    h.groups.push_back(g);
    pins::parse_group("g3:fps+set.vsync+set.fps_limit;at=1000,24;size=90;opacity=70;layout=compact;key=F10", &g, &err);
    h.groups.push_back(g);
    h.in_file = true;
    return h;
}

struct Ctx {
    int w = 1280, h = 720;
    float scale = 1.f;
    std::vector<uint8_t> bg;
};

enum class Mode { Menu, Pins, Overlay };
Mode g_mode = Mode::Menu;
void (*g_overlay)() = nullptr;   // Mode::Overlay: one of the overlay's own screens (overlay_screens.h), drawn on the frame's base style
ImTextureData* g_blur_tex = nullptr;
bool g_glass = false;
bool g_pad = true;
bool g_clear_focus = false;
float g_scroll = -1.f;        // a screen scrolled down: set after the page's first frame   // the next frame starts with no window focused (the menu of an earlier screen closed)
play::MenuFrame g_last;
double g_pins_ms = 0;   // the CPU time of the pins-only frames
int g_pins_frames = 0;

// One frame as overlay.cpp draws it: its scaled style, the menus' theme over it, the menu (or the pins alone, the menu closed).
void frame(Ctx& c, std::vector<uint8_t>* out, bool timed) {
    const auto t0 = std::chrono::steady_clock::now();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(c.w), static_cast<float>(c.h));
    io.DeltaTime = 1.f / 60.f;
    cheats::ui::frame_style(c.scale, false);   // as overlay.cpp: the theme is every frame's base style
    ImGui::NewFrame();
    if (g_clear_focus) {
        ImGui::FocusWindow(nullptr);
        g_clear_focus = false;
    }
    if (g_mode == Mode::Overlay) {
        if (g_overlay) g_overlay();
    } else {
        ImGuiStyle saved = ImGui::GetStyle();
        cheats::ui::apply_theme(g_glass);
        if (g_mode == Mode::Pins) {
            perf::draw_board();
        } else {
            play::MenuFrame f;
            f.pad = g_pad;
            f.menu_key = "F1";
            f.menu_pad = "back+right_stick";
            f.cheats_key = "F3";
            f.pins_key = "F7";
            if (g_glass) f.backdrop = g_blur_tex->GetTexRef();
            f.diagnostics = [] { ImGui::TextUnformatted("GPU: (the test has none)"); };
            play::draw_menu(g_host, f);
            g_last = f;
        }
        ImGui::GetStyle() = saved;
    }
    ImGui::Render();
    if (timed) {
        g_pins_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++g_pins_frames;
    }
    if (out) {
        *out = c.bg;
        test::soft_render(ImGui::GetDrawData(), *out, c.w, c.h);
    }
}

std::vector<uint8_t> shrink(const std::vector<uint8_t>& px, int w, int h, int f, int* ow, int* oh) {
    *ow = w / f;
    *oh = h / f;
    std::vector<uint8_t> o(static_cast<size_t>(*ow) * *oh * 4);
    for (int y = 0; y < *oh; ++y)
        for (int x = 0; x < *ow; ++x)
            for (int k = 0; k < 4; ++k) {
                int sum = 0;
                for (int j = 0; j < f; ++j)
                    for (int i = 0; i < f; ++i) sum += px[(static_cast<size_t>(y * f + j) * w + (x * f + i)) * 4 + k];
                o[(static_cast<size_t>(y) * *ow + x) * 4 + k] = static_cast<uint8_t>(sum / (f * f));
            }
    return o;
}

void set_theme(const char* name) {
    std::string err;
    th::set_theme(th::load_theme(name, &err));
    if (!err.empty()) std::printf("FAILED theme %s: %s\n", name, err.c_str());
}
void reset() {
    set_theme("sunrise");
    cheats::ui::State& s = cheats::ui::st();
    s.accent = "theme";
    s.style = -1;
    s.opacity = s.blur = s.tint = -1.f;
    g_mode = Mode::Menu;
    g_overlay = nullptr;
    g_glass = false;
    g_pad = true;
    g_host.s = play::default_settings();
    g_host.s.hud = shot_pins();
    g_host.s.hud.groups[1].shown = false;   // the wide group stays off while the menu is open (the "pins" screens show it)
    perf::set_board(g_host.s.hud);
    play::menu_show(0, 0, 1);
}

// ---- the overlay's own screens, as overlay.cpp draws them (overlay_screens.h) ---------------------------------------------------------
namespace scr = nxrt::play::screens;
scr::KeyboardView g_kb;
void kb_view(bool pad) {
    g_kb = scr::KeyboardView();
    g_kb.title = "Name your island";
    g_kb.header = "What should your island be called?";
    g_kb.sub = "Up to 12 characters. You can change it later at the town hall.";
    g_kb.guide = "Island name";
    g_kb.shown = g_kb.before_caret = "Sunny Isle";
    g_kb.count = "10 / 12";
    g_kb.pad = pad;
    g_kb.by_label = true;
    g_kb.sel_row = pad ? 2 : -1;
    g_kb.sel_key = pad ? 4 : -1;
    for (const char* row : {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,.?"}) {
        std::vector<scr::KeyboardKey> r;
        for (const char* c = row; *c; ++c) {
            scr::KeyboardKey k;
            k.label = std::string(1, *c);
            r.push_back(k);
        }
        g_kb.rows.push_back(r);
    }
    auto key = [](const char* l, int w, bool accent = false, bool on = false) {
        scr::KeyboardKey k;
        k.label = l;
        k.w = w;
        k.accent = accent;
        k.on = on;
        return k;
    };
    g_kb.rows.push_back({key("Shift", 2, false, true), key("Space", 4), key("!", 1), key("'", 1), key("Delete", 2)});
    g_kb.rows.push_back({key("Cancel", 5), key("OK", 5, true)});
}
void draw_keyboard() { scr::keyboard(g_kb, nullptr); }
void draw_quit_pad() { scr::quit_prompt(false, true, true); }
void draw_quit_keys() { scr::quit_prompt(false, false, false); }
void draw_ctrl() { scr::ctrl_prompt(2, 4, 1, true, true); }
void draw_notices() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const std::string lines[4] = {"60 FPS", "16.7 ms", "1% low 54", "max 21.4 ms"};
    scr::fps_counter(dl, lines, 4, 1, d);
    const std::string two[2] = {"144 FPS", "6.9 ms"};
    scr::fps_counter(dl, two, 2, 2, d);
    scr::shader_note(dl, 37, d);
    scr::save_notice(dl, "the disk is full", d);
    const float k = cheats::ui::theme::k();
    scr::touch_button(dl, ImVec4(12.f * k, d.y * 0.5f, 56.f * k, 52.f * k), false);
    scr::touch_button(dl, ImVec4(80.f * k, d.y * 0.5f, 56.f * k, 52.f * k), true);
}
// A debug window (Diagnostics without a menu, Overlay::add_panel) and every themed control (cheats_theme.h): a select open on its list,
// switches, boxes, rings, sliders, a stepper, progress, a graph, a disclosure, colour swatches and a tooltip.
int g_frame_no = 0;
void draw_debug() {
    namespace t = cheats::ui::theme;
    const float k = t::k();
    ImGui::SetNextWindowPos(ImVec2(24.f * k, 24.f * k), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(560.f * k, 0.f), ImGuiCond_Always);
    bool open = true;
    if (t::begin_window("Diagnostics", &open)) {
        ImGui::Text("GPU: NVIDIA GeForce RTX 5090");
        ImGui::TextDisabled("Vulkan 1.3, driver 0x%08x, validation off", 0x8d3c4000u);
        ImGui::SeparatorText("Presentation");
        static int mode = 2;
        const ImGuiID combo_id = ImGui::GetID("Present mode");
        if (g_frame_no == 1) ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, combo_id));
        static const char* const modes[] = {"Immediate", "Mailbox", "FIFO (vsync)", "FIFO relaxed"};
        t::combo("Present mode", &mode, modes, 4);
        static int win = 1;
        t::radio("Windowed", &win, 0);
        ImGui::SameLine();
        t::radio("Borderless", &win, 1);
        ImGui::SameLine();
        t::radio("Exclusive", &win, 2);
        static bool raw = true, hdr = false;
        t::checkbox("Raw mouse (relative mode)", &raw);
        ImGui::SameLine();
        t::checkbox("HDR", &hdr);
        static float cap = 60.f;
        t::slider_float("Frame cap", &cap, 30.f, 240.f, "%.0f fps");
        static int threads = 6;
        t::input_int("Shader threads", &threads);
        ImGui::SeparatorText("Timing");
        float ft[120];
        for (int i = 0; i < 120; ++i) ft[i] = 16.7f + 2.5f * std::sin(i * 0.35f) + (i % 37 == 0 ? 7.f : 0.f);
        t::plot_lines("##ft", ft, 120, 0, "frame time", 0.f, 33.f, ImVec2(ImGui::GetContentRegionAvail().x, 48.f * k));
        t::progress(0.62f, ImVec2(-1.f, 0.f), "Preparing shaders 62%");
        if (t::tree_node("Most called stubs", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%8u  %s", 1024u, "nvnCommandBufferSetDepthBounds");
            ImGui::Text("%8u  %s", 96u, "nvnQueueGetTotalTime");
            ImGui::TreePop();
        }
        t::collapsing_header("Swap chain");
        static const ImU32 sw[4] = {IM_COL32(255, 194, 61, 255), IM_COL32(255, 111, 97, 255), IM_COL32(184, 63, 134, 255), IM_COL32(63, 182, 255, 255)};
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            t::colour_swatch("##sw", sw[i], 0.f, i == 0);
            ImGui::PopID();
            if (i < 3) ImGui::SameLine();
        }
    }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(620.f * k, 380.f * k), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300.f * k, 0.f), ImGuiCond_Always);
    if (t::begin_window("Value graph###dbg2", nullptr)) {
        static bool on = true;
        t::toggle("##on", &on);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Freeze");
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 12.f * k), ImGuiCond_Always);
        t::tooltip("Tooltips: the dialog colour, small type, the theme's corners");
    }
    ImGui::End();
}
// Type: Inter's Latin, Greek and Cyrillic, the symbols, AnySwitch Mono and the CJK fallback (the system's faces: not compared with a
// reference, it differs from machine to machine).
void draw_type() {
    namespace t = cheats::ui::theme;
    const float k = t::k();
    ImGui::SetNextWindowPos(ImVec2(24.f * k, 24.f * k), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(760.f * k, 0.f), ImGuiCond_Always);
    if (t::begin_window("Type", nullptr)) {
        t::text(t::Type::Display, 0, "AnySwitch 60 fps");
        t::text(t::Type::Title, 0, "Title: The Legend of Zelda");
        t::text(t::Type::Body, 0, "Body: \u0395\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac \u00b7 \u0420\u0443\u0441\u0441\u043a\u0438\u0439 \u00b7 Espa\u00f1ol \u00b7 \u010ce\u0161tina \u00b7 Ti\u1ebfng Vi\u1ec7t");
        t::text(t::Type::Small, t::pal().text2, "Small: tabular figures 0123456789 / 1111111111");
        t::text(t::Type::Body, 0, "Symbols: \u2713 \u2717 \u2605 \u2606 \u2190 \u2192 \u2191 \u2193 \u26a0 \u2665 \u25cf \u25c6 \u25b6 \u25a0 \u25a1 \u2265 \u2264 \u221e \u00a9 \u2122 \u20ac");
        t::text(t::Type::Mono, 0, "Mono: 7800001230  64 00 00 00 5F 00 00 00  d...____");
        t::text(t::Type::Body, 0, "CJK: \u30bc\u30eb\u30c0\u306e\u4f1d\u8aac \u00b7 \u8d85\u7ea7\u9a6c\u529b\u6b27\u5144\u5f1f \u00b7 \u5bf6\u53ef\u5922 \u00b7 \ud3ec\ucf13\ubaac\uc2a4\ud130");
        t::text(t::Type::Strong, 0, "Cheat: \u65e0\u9650\u91d1\u5e01 / \u4f53\u529b\u6700\u5927 / \ubb34\ud55c \uccb4\ub825");
    }
    ImGui::End();
}

struct Screen {
    const char* name;
    void (*setup)();
    ImGuiKey nav = ImGuiKey_None;   // pressed `presses` times after the screen's first frames: keyboard or controller focus
    int presses = 0;
    bool compare = true;            // false: the shot depends on the machine's system fonts (CJK)
};
const Screen kScreens[] = {
    {"display", [] { play::menu_show(0); }},
    // PokoMac's upscalers differ by system (MetalFX on the Mac, DLSS / FSR on Windows): this page has a reference for each
#ifdef __APPLE__
    {"upscaler_mac", [] { play::menu_show(2); g_pad = false; }},
#else
    {"upscaler_win", [] { play::menu_show(2); g_pad = false; }},
#endif
    {"graphics", [] { play::menu_show(3); }},
    {"audio", [] { play::menu_show(4); g_pad = false; }},
    {"controls", [] { play::menu_show(5, 2); }},
    {"pins_page", [] { play::menu_show(6); }},
    {"pins_group", [] { play::menu_show(6); g_scroll = 1010.f; }},
    {"about", [] { play::menu_show(8); g_pad = false; }},
    {"glass", [] { set_theme("frost"); g_glass = true; play::menu_show(0); }},
    {"daylight", [] { set_theme("daylight"); play::menu_show(3); }},
    {"accent_hex", [] { cheats::ui::st().accent = "#3fb6ff"; play::menu_show(6); }},
    {"dock_right", [] { play::menu_show(0, -1, 2); }},
    {"pins", [] { g_mode = Mode::Pins; perf::set_board(shot_pins()); }},
    {"pins_dusk", [] { set_theme("dusk"); g_mode = Mode::Pins; perf::set_board(shot_pins()); }},
    // focus: the keyboard's and the controller's (the accent ring on the focused row)
    {"focus_keys", [] { play::menu_show(3); g_pad = false; }, ImGuiKey_DownArrow, 3},
    {"focus_pad", [] { play::menu_show(0); }, ImGuiKey_GamepadDpadDown, 2},
    // the overlay's own screens
    {"keyboard_pad", [] { kb_view(true); g_mode = Mode::Overlay; g_overlay = draw_keyboard; }},
    {"keyboard_keys", [] { kb_view(false); g_mode = Mode::Overlay; g_overlay = draw_keyboard; }},
    {"quit_pad", [] { g_mode = Mode::Overlay; g_overlay = draw_quit_pad; }},
    {"quit_keys", [] { g_mode = Mode::Overlay; g_overlay = draw_quit_keys; }},
    {"ctrl_prompt", [] { g_mode = Mode::Overlay; g_overlay = draw_ctrl; }},
    {"notices", [] { g_mode = Mode::Overlay; g_overlay = draw_notices; }},
    {"debug_window", [] { g_mode = Mode::Overlay; g_overlay = draw_debug; }},
    {"debug_daylight", [] { set_theme("daylight"); g_mode = Mode::Overlay; g_overlay = draw_debug; }},
    {"type", [] { g_mode = Mode::Overlay; g_overlay = draw_type; }, ImGuiKey_None, 0, false},
};

// ---- the checks that need no pictures --------------------------------------------------------------------------------------------------

input::Event key(int sc, bool down, uint16_t mods = 0, bool repeat = false) {
    input::Event e;
    e.type = down ? input::EventType::KeyDown : input::EventType::KeyUp;
    e.u.key.scancode = sc;
    e.mods = mods;
    if (repeat) e.flags |= input::kFlagRepeat;
    return e;
}

void toggle_checks(const std::string& dir) {
    using play::KeyChord;
    using play::KeyToggle;
    KeyChord k;
    check("key chord: F1", play::parse_key_chord("F1", &k) && k.sc == SDL_SCANCODE_F1 && k.mods == 0);
    check("key chord: ctrl + shift + f5", play::parse_key_chord("ctrl + shift + f5", &k) && k.sc == SDL_SCANCODE_F5 && k.mods == (play::kModCtrl | play::kModShift));
    check("key chord: Ctrl+F1 prints back", play::parse_key_chord("Ctrl+F1", &k) && play::key_chord_text(k) == "Ctrl+F1");
    check("key chord: a modifier alone and junk are refused", !play::parse_key_chord("Ctrl", &k) && !play::parse_key_chord("Ctrl+Nope", &k) && !play::parse_key_chord("Hyper+F1", &k));
    check("key chord: none is empty", play::parse_key_chord("none", &k) && k.empty());

    // the menu's key from the configuration: [menu] key wins over [gfx] overlay_key; F1 then no longer opens the menu and reaches the game
    config_set("gfx", "overlay_key", "F2");
    config_set("menu", "key", "Ctrl+F5");
    KeyToggle menu;
    menu.chord = play::menu_key_from_config();
    check("menu key: [menu] key = Ctrl+F5 is used", menu.chord.sc == SDL_SCANCODE_F5 && menu.chord.mods == play::kModCtrl);
    int opened = 0, game_saw_f1 = 0;
    auto feed = [&](const input::Event& e) {
        const KeyToggle::Hit h = menu.event(e.u.key.scancode, e.mods, e.type == input::EventType::KeyDown, (e.flags & input::kFlagRepeat) != 0);
        if (h == KeyToggle::Fired) ++opened;
        if (h == KeyToggle::NotOurs && e.u.key.scancode == SDL_SCANCODE_F1) ++game_saw_f1;
    };
    for (const input::Event& e : {key(SDL_SCANCODE_F1, true), key(SDL_SCANCODE_F1, false)}) feed(e);
    check("menu key remapped: F1 does not open the menu, it reaches the game (press and release)", opened == 0 && game_saw_f1 == 2);
    for (const input::Event& e : {key(SDL_SCANCODE_LCTRL, true, SDL_KMOD_LCTRL), key(SDL_SCANCODE_F5, true, SDL_KMOD_LCTRL), key(SDL_SCANCODE_F5, true, SDL_KMOD_LCTRL, true),
                                  key(SDL_SCANCODE_F5, false, 0)})
        feed(e);
    check("menu key remapped: Ctrl+F5 opens the menu once (repeat and release stay the menu's)", opened == 1);
    opened = 0;
    feed(key(SDL_SCANCODE_F5, true));
    feed(key(SDL_SCANCODE_F5, false));
    check("menu key remapped: F5 without Ctrl is the game's", opened == 0);
    config_set("menu", "key", "");
    check("menu key: the older [gfx] overlay_key when [menu] key is empty", play::menu_key_from_config().sc == SDL_SCANCODE_F2);
    config_set("gfx", "overlay_key", "");
    std::string why;
    config_set("menu", "key", "NotAKey");
    check("menu key: a bad [menu] key falls back to F1 and says why", play::menu_key_from_config(&why).sc == SDL_SCANCODE_F1 && !why.empty());
    config_set("menu", "key", "");

    // a pin's own key hides only that pin; the hide-all key hides all; the keys never reach the game
    perf::set_board(shot_pins());
    std::vector<std::string> saved_toggles;
    perf::set_toggle_handler([&](const std::string& g) { saved_toggles.push_back(g.empty() ? "*" : g); });
    const bool f8 = perf::key_event(SDL_SCANCODE_F8, 0, true, false);
    const bool f8_up = perf::key_event(SDL_SCANCODE_F8, 0, false, false);
    perf::HudSettings b = perf::board();
    check("pin key: F8 hides the Performance pin only", f8 && f8_up && !b.groups[0].shown && b.groups[1].shown && b.groups[2].shown && b.all_shown);
    check("pin key: the toggle is saved through the host (once per press)", saved_toggles.size() == 1 && saved_toggles[0] == "perf");
    perf::key_event(SDL_SCANCODE_F8, 0, true, false);
    perf::key_event(SDL_SCANCODE_F8, 0, false, false);
    check("pin key: F8 again shows it again", perf::board().groups[0].shown);
    perf::key_event(SDL_SCANCODE_F7, 0, true, false);
    perf::key_event(SDL_SCANCODE_F7, 0, false, false);
    b = perf::board();
    check("hide-all key: F7 hides every pin, each pin keeps its own state", !b.all_shown && b.groups[0].shown && b.groups[1].shown && !perf::any_shown());
    perf::key_event(SDL_SCANCODE_F7, 0, true, false);
    perf::key_event(SDL_SCANCODE_F7, 0, false, false);
    check("hide-all key: F7 again shows them", perf::board().all_shown && perf::any_shown());
    check("pin key: other keys are not the pins' (Space, F8 with Shift)", !perf::key_event(SDL_SCANCODE_SPACE, 0, true, false) &&
                                                                              !perf::key_event(SDL_SCANCODE_F8, SDL_KMOD_LSHIFT, true, false));
    perf::set_toggle_handler(nullptr);

    // a pin's controller chord: opt-in, a free button first, the game never sees it
    using play::PadSrc;
    const uint32_t paddle = play::pad_src_bit(PadSrc::LeftPaddle1), up = play::pad_src_bit(PadSrc::DpadUp), a = play::pad_src_bit(PadSrc::South);
    const uint32_t free_bits = play::free_pad_buttons(play::PadSettings{}.remap);
    check("pad chord: the rear grips and trackpad clicks are free by default, the face buttons are not", (free_bits & paddle) && !(free_bits & a));
    check("pad chord: one button, or no free button, is refused", !play::pin_pad_problem(paddle, free_bits).empty() && !play::pin_pad_problem(a | up, free_bits).empty() &&
                                                                       play::pin_pad_problem(paddle | up, free_bits).empty());
    play::PadToggle t;
    t.chord = paddle | up;
    t.free = free_bits;
    uint32_t mask = 0;
    bool fired = t.step(paddle, &mask);
    check("pad chord: the grip alone does nothing and holds nothing back", !fired && mask == 0);
    mask = 0;
    fired = t.step(paddle | up, &mask);
    check("pad chord: grip, then up: fires, and up is held back from the game", fired && mask == up);
    mask = 0;
    fired = t.step(paddle | up, &mask);
    check("pad chord: held on: fires once, up still held back", !fired && mask == up);
    mask = 0;
    t.step(paddle, &mask);
    t.step(0, &mask);
    mask = 0;
    t.step(up, &mask);
    fired = t.step(up | paddle, &mask);
    check("pad chord: up first, then the grip: nothing fires, up stays the game's", !fired && mask == 0);

    // the settings file keeps the pins; the configuration's default groups
    play::Settings s = play::default_settings();
    s.hud = shot_pins();
    play::Settings back;
    std::string err;
    const bool parsed = play::parse_settings(play::format_settings(s), &back, &err);
    check("settings file: [hud] round-trips (groups, items, place, size, opacity, layout, keys, shown)",
          parsed && back.hud.in_file && back.hud.groups == s.hud.groups && back.hud.all_shown == s.hud.all_shown);
    play::Settings plain = play::default_settings();
    check("settings file: no [hud] until the player arranged pins", play::format_settings(plain).find("[hud]") == std::string::npos);
    config_set("hud", "default", "perf:fps+frametime;name=Performance;layout=compact|g2:gpu");
    const std::vector<pins::Group> def = perf::configured_default_groups(&err);
    check("configuration: [hud] default gives the default groups", def.size() == 2 && def[0].items.size() == 2 && def[0].layout == pins::Layout::Compact &&
                                                                     def[1].items == std::vector<std::string>{"gpu"});
    config_set("hud", "default", "perf:fps;size=banana");
    err.clear();
    check("configuration: a bad [hud] default gives no pins and a reason", perf::configured_default_groups(&err).empty() && !err.empty());
    config_set("hud", "default", "");
    pins::Group bad;
    check("group text: an unknown field is refused", !pins::parse_group("perf:fps;colour=red", &bad, &err));

    // conflicts between toggles and with the game's keys
    std::vector<play::ToggleSpec> specs = {{"Settings menu", {SDL_SCANCODE_F1, 0}, 0}, {"Cheats menu", {SDL_SCANCODE_F3, 0}, 0},
                                           {"Pin 'Render'", {SDL_SCANCODE_F3, 0}, 0}, {"Pin 'Performance'", {SDL_SCANCODE_SPACE, 0}, paddle | up}};
    const auto cs = play::toggle_conflicts(specs, {SDL_SCANCODE_SPACE}, play::pad_src_bit(PadSrc::Back) | play::pad_src_bit(PadSrc::Start));
    bool same_key = false, game_key = false;
    for (const auto& c : cs) {
        same_key = same_key || (c.a == 1 && c.b == 2);
        game_key = game_key || (c.a == 3 && c.b == -1);
    }
    check("conflicts: a key used twice and a key the game uses are reported", same_key && game_key && cs.size() == 2);
    (void)dir;
}

// Game input reaches the game the same with the pins shown as hidden: the keys (none of them is a pin's), the pad (no chord held) and
// the mouse over a pin (the pins take no input: ImGui does not want the mouse or the keyboard, and no pin window is hovered).
void input_equality(Ctx& c) {
    struct Out {
        std::vector<int> keys_to_game;
        std::vector<uint32_t> pad_masks;
        std::vector<int> want;       // per mouse step: WantCaptureMouse, WantCaptureKeyboard, a window hovered
    };
    auto run = [&](bool shown) {
        Out o;
        perf::HudSettings h = shot_pins();
        h.all_shown = shown;
        perf::set_board(h);
        // keys: the game's own and the arrows, letters, Space, Enter (each press and release)
        const int keys[] = {SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_UP, SDL_SCANCODE_LSHIFT,
                            SDL_SCANCODE_E, SDL_SCANCODE_TAB, SDL_SCANCODE_1};
        for (int k : keys)
            for (bool d : {true, false})
                if (!perf::key_event(k, 0, d, false)) o.keys_to_game.push_back(k * 2 + d);
        // the pad: every button pressed and released, sticks and triggers as buttons
        for (int i = 0; i < play::kPadSrcCount; ++i)
            for (uint32_t held : {play::pad_src_bit(static_cast<play::PadSrc>(i)), 0u}) o.pad_masks.push_back(perf::pad_step(0, held, 0));
        // the mouse: moves over each pin and clicks; the frame drawn with the menu closed (and nothing focused, as after closing it)
        g_mode = Mode::Pins;
        g_clear_focus = true;
        frame(c, nullptr, false);
        const ImVec2 spots[] = {{60, 60}, {120, 200}, {60, 360}, {1100, 40}, {640, 400}};
        for (const ImVec2& sp : spots) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(sp.x * c.scale, sp.y * c.scale);
            frame(c, nullptr, false);
            io.AddMouseButtonEvent(0, true);
            frame(c, nullptr, false);
            o.want.push_back(io.WantCaptureMouse);
            o.want.push_back(io.WantCaptureKeyboard);
            o.want.push_back(GImGui->HoveredWindow != nullptr);
            io.AddMouseButtonEvent(0, false);
            frame(c, nullptr, false);
            o.want.push_back(io.WantCaptureMouse);
        }
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return o;
    };
    const Out shown = run(true), hidden = run(false);
    if (shown.want != hidden.want || !std::all_of(shown.want.begin(), shown.want.end(), [](int v) { return v == 0; })) {
        std::printf("       mouse (shown):  ");
        for (int v : shown.want) std::printf("%d", v);
        std::printf("\n       mouse (hidden): ");
        for (int v : hidden.want) std::printf("%d", v);
        std::printf("\n");
    }
    check("input equality: the same keys reach the game with the pins shown and hidden", shown.keys_to_game == hidden.keys_to_game && shown.keys_to_game.size() == 20);
    check("input equality: the pad is never held back by the pins (no chord held)", shown.pad_masks == hidden.pad_masks &&
                                                                                       std::all_of(shown.pad_masks.begin(), shown.pad_masks.end(), [](uint32_t m) { return m == 0; }));
    check("input equality: the mouse over a pin: ImGui wants nothing, no window is hovered (as with no pins)",
          shown.want == hidden.want && std::all_of(shown.want.begin(), shown.want.end(), [](int v) { return v == 0; }));
    // the pins' windows themselves: no input, no focus, no nav
    perf::set_board(shot_pins());
    frame(c, nullptr, false);
    bool flags_ok = true;
    int found = 0;
    for (const pins::Group& g : shot_pins().groups) {
        ImGuiWindow* w = ImGui::FindWindowByName(pins::window_name("menu", g).c_str());
        if (!w) continue;
        ++found;
        const ImGuiWindowFlags need = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        flags_ok = flags_ok && (w->Flags & need) == need;
    }
    check("pins: every pin window takes no input, no focus and no nav", found == 3 && flags_ok);
    g_mode = Mode::Menu;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = "menu_ui_shots", ref_dir, only;
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--ref" && i + 1 < argc) ref_dir = argv[++i];
        else if (a == "--update") update = true;
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    // PokoMac: the title's files (icon.jpg) are read from the working directory, which the test moves into the cheats test's stand-in
    // title folder below (AnySwitch: set_game_dir), so the folders it was given are made absolute first
    out_dir = fs::absolute(out_dir, ec).string();
    if (!ref_dir.empty()) ref_dir = fs::absolute(ref_dir, ec).string();
    LogOptions lo;
    const std::string log_path = (fs::path(out_dir) / "menu_ui_test.log").string();
    lo.path = log_path.c_str();
    lo.level = LogLevel::Info;
    lo.to_stderr = false;
    lo.keep_previous = false;
    log_init(lo);
    config_set("cheats", "dir", (fs::path(out_dir) / "cheats").string().c_str());
#ifdef NXRT_CHEATS_UI_TEST_DIR
    fs::current_path(std::string(NXRT_CHEATS_UI_TEST_DIR) + "/title", ec);   // the cheats test's stand-in title folder: its icon.jpg shows in the header
#endif
    static game::Info info;
    info.title = "Test Game";
    game::set_current(&info);
    cheats::ui::init_look();
    cheats::ui::st().animations = false;
    cheats::ui::game_icon_wait(5000);   // the header's icon, decoded on a thread of its own: in every shot
    fill_sample();

    toggle_checks(out_dir);

    struct Res { int w, h; float scale; const char* tag; };
    const Res kRes[] = {{1280, 720, 1.f, "1x"}, {2560, 1440, 2.f, "2x"}};
    for (const Res& r : kRes) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        test::soft_init();
        cheats::ui::setup_context();   // as overlay.cpp: the overlay's fonts and the theme before the first frame
        Ctx c;
        c.w = r.w;
        c.h = r.h;
        c.scale = r.scale;
        game_frame(c.bg, c.w, c.h);
        {
            int bw = 0, bh = 0;
            const std::vector<uint8_t> blurred = test::blur_reference(c.bg, c.w, c.h, th::load_theme("frost", nullptr).blur, nullptr, &bw, &bh);
            g_blur_tex = IM_NEW(ImTextureData)();
            g_blur_tex->Create(ImTextureFormat_RGBA32, bw, bh);
            std::memcpy(g_blur_tex->GetPixels(), blurred.data(), blurred.size());
            ImGui::RegisterUserTexture(g_blur_tex);
        }
        // every screen on the test's scene at both scales, then (1x) on the light, dark and busy backgrounds
        std::vector<uint8_t> scene_bg = c.bg;
        for (int bgk = 0; bgk < 4; ++bgk) {
        if (bgk > 0 && r.scale != 1.f) break;
        if (bgk > 0) test::background(bgk, c.bg, c.w, c.h);
        for (const Screen& sc : kScreens) {
            if (!only.empty() && only != sc.name) continue;
            reset();
            sc.setup();
            std::vector<uint8_t> px;
            for (int f = 0; f < 5; ++f) {
                g_frame_no = f;
                if (f == 2 && g_scroll >= 0.f) {
                    play::menu_scroll(g_scroll);
                    g_scroll = -1.f;
                }
                if (f == 2 && sc.nav != ImGuiKey_None)   // keyboard or controller focus: the presses, a frame each
                    for (int n = 0; n < sc.presses; ++n) {
                        io.AddKeyEvent(sc.nav, true);
                        frame(c, nullptr, false);
                        io.AddKeyEvent(sc.nav, false);
                        frame(c, nullptr, false);
                    }
                frame(c, f == 4 ? &px : nullptr, false);   // windows size themselves over the first frames
                if (g_glass && f == 2 && g_last.glass) {   // what the GPU pass would give for this panel: the reference blur of its rectangle
                    int bw = 0, bh = 0;
                    const std::vector<uint8_t> blurred = test::blur_reference(c.bg, c.w, c.h, g_last.glass_blur, g_last.glass_rect, &bw, &bh);
                    std::memcpy(g_blur_tex->GetPixels(), blurred.data(), blurred.size());
                }
            }
            if (std::string(sc.name) == "glass" && bgk == 0)
                check("glass: the menu's panel is drawn as glass and asks for the blur of its rectangle", g_last.glass && g_last.glass_rect[2] > 0);
            const std::string name = std::string(sc.name) + "@" + (bgk ? test::background_name(bgk) : r.tag);
            if (!test::theme_guard(ImGui::GetDrawData(), name.c_str())) ++g_failures;
            ImGui::GetCurrentContext()->OpenPopupStack.resize(0);
            ImGui::FocusWindow(nullptr);
            const std::string png = (fs::path(out_dir) / (name + ".png")).string();
            if (!gfx::save_png_rgba(png, px.data(), static_cast<uint32_t>(c.w), static_cast<uint32_t>(c.h))) {
                std::printf("FAILED %s: cannot write %s\n", name.c_str(), png.c_str());
                ++g_failures;
                continue;
            }
            if (ref_dir.empty()) { std::printf("wrote  %s\n", png.c_str()); continue; }
            if (!sc.compare) { std::printf("shot   %s (the system's CJK fonts: no reference)\n", name.c_str()); continue; }
            const int f = c.w / (bgk ? 320 : 640);   // the references are 640 pixels wide (320 for the backgrounds)
            int sw = 0, sh = 0;
            const std::vector<uint8_t> small = shrink(px, c.w, c.h, f, &sw, &sh);
            const std::string ref = (fs::path(ref_dir) / (name + ".png")).string();
            if (update) {
                gfx::save_png_rgba(ref, small.data(), static_cast<uint32_t>(sw), static_cast<uint32_t>(sh));
                std::printf("ref    %s\n", ref.c_str());
                continue;
            }
            std::vector<uint8_t> want;
            uint32_t rw = 0, rh = 0;
            if (!gfx::load_png_rgba(ref, &want, &rw, &rh)) {
                std::printf("FAILED %s: no reference %s\n", name.c_str(), ref.c_str());
                ++g_failures;
                continue;
            }
            const gfx::ImageDiff d = gfx::compare_rgba(small.data(), static_cast<uint32_t>(sw), static_cast<uint32_t>(sh), want.data(), rw, rh);
            const bool ok = d.same_size && d.fraction_over_8 < 0.01 && d.mean_abs < 2.0;
            std::printf("%-6s %s (mean %.2f, %.2f%% of pixels off by more than 8)\n", ok ? "ok" : "FAILED", name.c_str(), d.mean_abs, d.fraction_over_8 * 100.0);
            if (!ok) ++g_failures;
        }
        }
        c.bg = scene_bg;
        if (r.scale == 1.f && only.empty()) {
            // the pad drives the menu: L / R step through the pages
            reset();
            frame(c, nullptr, false);
            auto press = [&](ImGuiKey k) {
                io.AddKeyEvent(k, true);
                frame(c, nullptr, false);
                io.AddKeyEvent(k, false);
                frame(c, nullptr, false);
            };
            press(ImGuiKey_GamepadR1);
            check("pad: R goes from Display & FPS to Upscaler (PokoMac has no frame generation page)", play::menu_page() == 2);
            press(ImGuiKey_GamepadL1);
            press(ImGuiKey_GamepadL1);
            check("pad: L twice goes round to About (the Game page is skipped: this game has no tweaks)", play::menu_page() == 8);
            reset();
            input_equality(c);
            // the pins' cost: the menu closed, three groups (eleven readouts and a graph) over the game; the CPU time of the whole frame
            reset();
            g_mode = Mode::Pins;
            perf::set_board(shot_pins());
            for (int i = 0; i < 20; ++i) frame(c, nullptr, false);
            g_pins_ms = 0;
            g_pins_frames = 0;
            for (int i = 0; i < 600; ++i) frame(c, nullptr, true);
            const double ms = g_pins_ms / g_pins_frames;
            int verts = 0;
            for (const ImDrawList* dl : ImGui::GetDrawData()->CmdLists) verts += dl->VtxBuffer.Size;
            std::printf("pins: %.4f ms CPU per frame (theme, three pins, ImGui; no rendering), %d vertices to draw\n", ms, verts);
            check("pins: under 0.1 ms CPU per frame", ms < 0.1);
            g_mode = Mode::Menu;
        }
        ImGui::DestroyContext();
    }
    std::printf("%s\n", g_failures ? "menu_ui_test: FAILED" : "menu_ui_test: all ok");
    std::fflush(stdout);
    std::_Exit(g_failures ? 1 : 0);
}
