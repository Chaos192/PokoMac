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
//   - the pins' cost per frame (CPU: theme, the pins' windows, ImGui's own work) stays under 0.1 ms.
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

#include "../../src/gfx/play/cheats_ui.h"
#include "../../src/gfx/play/cheats_ui_internal.h"
#include "../../src/gfx/play/hotkeys.h"
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
    ImGuiStyle base;
    std::vector<uint8_t> bg;
};

enum class Mode { Menu, Pins };
Mode g_mode = Mode::Menu;
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
    ImGuiStyle& style = ImGui::GetStyle();
    style = c.base;
    style.ScaleAllSizes(c.scale);
    style.FontScaleDpi = c.scale;
    ImGui::NewFrame();
    if (g_clear_focus) {
        ImGui::FocusWindow(nullptr);
        g_clear_focus = false;
    }
    {
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
    g_glass = false;
    g_pad = true;
    g_host.s = play::default_settings();
    g_host.s.hud = shot_pins();
    g_host.s.hud.groups[1].shown = false;   // the wide group stays off while the menu is open (the "pins" screens show it)
    perf::set_board(g_host.s.hud);
    play::menu_show(0, 0, 1);
}

struct Screen {
    const char* name;
    void (*setup)();
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
        ImGui::StyleColorsDark();
        io.Fonts->AddFontDefaultVector();
        cheats::ui::load_fonts(io.Fonts);
        ImGui::GetStyle().FontSizeBase = 16.0f;
        Ctx c;
        c.w = r.w;
        c.h = r.h;
        c.scale = r.scale;
        c.base = ImGuiStyle();
        ImGui::StyleColorsDark(&c.base);
        c.base.FontSizeBase = 16.f;
        game_frame(c.bg, c.w, c.h);
        {
            int bw = 0, bh = 0;
            const std::vector<uint8_t> blurred = test::blur_reference(c.bg, c.w, c.h, th::load_theme("frost", nullptr).blur, nullptr, &bw, &bh);
            g_blur_tex = IM_NEW(ImTextureData)();
            g_blur_tex->Create(ImTextureFormat_RGBA32, bw, bh);
            std::memcpy(g_blur_tex->GetPixels(), blurred.data(), blurred.size());
            ImGui::RegisterUserTexture(g_blur_tex);
        }
        for (const Screen& sc : kScreens) {
            if (!only.empty() && only != sc.name) continue;
            reset();
            sc.setup();
            std::vector<uint8_t> px;
            for (int f = 0; f < 5; ++f) {
                if (f == 2 && g_scroll >= 0.f) {
                    play::menu_scroll(g_scroll);
                    g_scroll = -1.f;
                }
                frame(c, f == 4 ? &px : nullptr, false);   // windows size themselves over the first frames
                if (g_glass && f == 2 && g_last.glass) {   // what the GPU pass would give for this panel: the reference blur of its rectangle
                    int bw = 0, bh = 0;
                    const std::vector<uint8_t> blurred = test::blur_reference(c.bg, c.w, c.h, g_last.glass_blur, g_last.glass_rect, &bw, &bh);
                    std::memcpy(g_blur_tex->GetPixels(), blurred.data(), blurred.size());
                }
            }
            if (std::string(sc.name) == "glass") check("glass: the menu's panel is drawn as glass and asks for the blur of its rectangle", g_last.glass && g_last.glass_rect[2] > 0);
            ImGui::GetCurrentContext()->OpenPopupStack.resize(0);
            const std::string name = std::string(sc.name) + "@" + r.tag;
            const std::string png = (fs::path(out_dir) / (name + ".png")).string();
            if (!gfx::save_png_rgba(png, px.data(), static_cast<uint32_t>(c.w), static_cast<uint32_t>(c.h))) {
                std::printf("FAILED %s: cannot write %s\n", name.c_str(), png.c_str());
                ++g_failures;
                continue;
            }
            if (ref_dir.empty()) { std::printf("wrote  %s\n", png.c_str()); continue; }
            const int f = c.w / 640;
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
