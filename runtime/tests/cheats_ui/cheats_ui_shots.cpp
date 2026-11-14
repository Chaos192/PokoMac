// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's screenshot test: every main screen drawn offscreen (no window, no GPU: soft_raster.h renders ImGui's draw data on
// the CPU) at two UI scales over a synthetic game frame, written as PNGs and compared with the reference images in tests/cheats_ui/ref
// (stored at a quarter of the pixels to keep the repository small; the comparison downsamples the render the same way). At 1x every screen
// is drawn on light, dark and busy backgrounds too (references at 320 pixels wide: <screen>@<background>.png), and every screen passes
// the theme guard (soft_raster.h theme_guard: the style is the theme's, no vertex in ImGui's stock colours).
//   nxrt_cheats_ui_test [--out <dir>] [--ref <dir>] [--update] [--only <screen>]
// --update writes the references from this run. The guest memory the tools read is a real heap section (mem::heap_create), filled with
// known values, so the scans, the list and the viewers show live values as in a game.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/cheats/scanner.h"
#include "../../src/gfx/play/cheats_ui.h"
#include "../../src/gfx/play/cheats_ui_internal.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/input_source.h"
#include "nxrt/memory.h"
#include "blur_ref.h"
#include "soft_raster.h"

namespace fs = std::filesystem;
using namespace nxrt;
namespace ui = nxrt::cheats::ui;

namespace {

int g_failures = 0;
constexpr uint64_t kHeapBytes = 4ull << 20;

// The game under the overlay: a sky, a sun and two hills, so the panel's translucency and the backdrop show in the pictures.
void game_frame(std::vector<uint8_t>& px, int w, int h) {
    px.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float fy = static_cast<float>(y) / h, fx = static_cast<float>(x) / w;
            float r = 0.36f + 0.30f * fy, g = 0.62f + 0.18f * fy, b = 0.95f - 0.10f * fy;
            const float sx = fx - 0.78f, sy = fy - 0.22f;
            if (sx * sx * 1.78f * 1.78f + sy * sy < 0.006f) { r = 1.f; g = 0.86f; b = 0.35f; }
            const float hill1 = 0.70f + 0.08f * std::sin(fx * 7.0f), hill2 = 0.80f + 0.05f * std::sin(fx * 11.0f + 1.3f);
            if (fy > hill1) { r = 0.30f; g = 0.62f; b = 0.28f; }
            if (fy > hill2) { r = 0.22f; g = 0.48f; b = 0.20f; }
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(r * 255);
            p[1] = static_cast<uint8_t>(g * 255);
            p[2] = static_cast<uint8_t>(b * 255);
        }
}

// The guest memory the tools see: a heap block with a few known values.
void guest_setup() {
    mem::init();
    mem::heap_create(64ull << 20);
    mem::heap_commit(0, kHeapBytes);
    mem::vm_set(mem::kHeapBase, kHeapBytes, mem::kMemHeap, 3);
    auto put32 = [](uint64_t off, uint32_t v) { std::memcpy(reinterpret_cast<void*>(mem::kHeapBase + off), &v, 4); };
    auto putf = [](uint64_t off, float v) { std::memcpy(reinterpret_cast<void*>(mem::kHeapBase + off), &v, 4); };
    // coins = 100 at heap+0x1230, and 36 other 100s across the block (what a first scan for 100 finds)
    put32(0x1230, 100);
    for (int i = 0; i < 36; ++i) put32(0x8000 + static_cast<uint64_t>(i) * 0x10f0, 100);
    putf(0x2000, 87.5f);   // health
    put32(0x2010, 3);      // lives
    putf(0x2020, 12.25f);  // position x
    const char* name = "Link";
    std::memcpy(reinterpret_cast<void*>(mem::kHeapBase + 0x3000), name, 5);
}

struct Ctx {
    int w = 1280, h = 720;
    float scale = 1.f;
    std::vector<uint8_t> bg;
};

// One frame of the overlay as overlay.cpp draws it: the theme as the frame's base style, the cheats look over it, the cheats UI.
double g_ui_ms = 0;      // the CPU time of the overlay's frames (theme, pages, ImGui's own work; not this test's rasterizer)
int g_ui_frames = 0;
bool g_hidden = false;   // the screen is the game with the panel closed (the HUD)
bool g_glass = false;    // the glass backdrop is there (the CPU reference of the GPU blur, as a texture)
void frame(Ctx& c, std::vector<uint8_t>& out, bool timed) {
    const auto t0 = std::chrono::steady_clock::now();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(c.w), static_cast<float>(c.h));
    io.DeltaTime = 1.f / 60.f;
    ui::frame_style(c.scale, false);
    ImGui::NewFrame();
    {
        ImGuiStyle saved = ImGui::GetStyle();
        ui::apply_theme(g_glass);
        if (g_hidden) ui::draw_mini();   // the panel closed: only the HUD over the game
        else ui::draw(true, true);
        ImGui::GetStyle() = saved;
    }
    ImGui::Render();
    if (timed) g_ui_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_ui_frames += timed;
    out = c.bg;
    test::soft_render(ImGui::GetDrawData(), out, c.w, c.h);
}

// Downsamples by an integer factor (box filter), for the stored references.
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

std::string g_custom_theme;      // a theme file the test writes (tests the file reader: a theme as a player would share it)
void set_theme(const std::string& name) {
    std::string err;
    ui::theme::set_theme(ui::theme::load_theme(name, &err));
    if (!err.empty()) std::printf("FAILED theme %s: %s\n", name.c_str(), err.c_str());
    ui::st().theme_error = err;
}
// Every screen starts from the default look (the theme, accent, dock, peek and HUD of the screen before are put back).
void reset_look() {
    ui::State& s = ui::st();
    set_theme("sunrise");
    s.accent = "theme";
    s.style = -1;
    s.opacity = s.blur = s.tint = -1.f;
    s.dock = 1;
    ui::set_key_peek(false);
    s.hud_pos[0] = s.hud_pos[1] = -1;
    for (auto& e : s.list) e.pinned = false;
    g_hidden = false;
    g_glass = false;
    ui::set_backdrop(ImTextureRef());
}
ImTextureData* g_blur_tex = nullptr;   // the blurred game frame of the current scale (the glass screens' backdrop)

struct Screen {
    const char* name;
    void (*setup)();
    bool pad;          // drawn as with a controller in use (the hint bar shows its buttons)
};

void use_pad(bool on) {
    if (on) { ui::pad_input(hid::btn(hid::kBtnA)); ui::pad_input(0); }
    else { ui::key_event(SDL_SCANCODE_LSHIFT, true); ui::key_event(SDL_SCANCODE_LSHIFT, false); }
}

void reset_scan() {
    ui::State& s = ui::st();
    s.scanner->clear();
    s.first_done = false;
    s.last_count = 0;
    s.history.clear();
    s.kind_i = 2;
    s.find_now = 0;
    s.first_i = 0;
    s.val1[0] = 0;
}

// What the wizard's Search does, without the click: a first scan for 100 as a whole number (4 bytes).
void scan_first_100() {
    ui::State& s = ui::st();
    reset_scan();
    std::snprintf(s.val1, sizeof s.val1, "100");
    cheats::ScanOptions o;
    o.type = cheats::ScanType::U32;
    o.kind = cheats::ScanKind::Numeric;
    o.fast = true;
    o.filter = s.filter;
    cheats::Value v{};
    cheats::parse_value(cheats::ScanType::U32, "100", &v);
    s.scanner->first_scan(o, cheats::FirstScan::Exact, v, cheats::Value{}, s.prog);
    s.first_done = true;
    s.last_count = s.scanner->result_count();
    s.history.push_back("First search, exactly 100 (4 bytes): " + std::to_string(s.last_count) + " found");
}
// ... then the coins drop to 95 in the game and the player says "It went down".
void scan_went_down() {
    ui::State& s = ui::st();
    const uint32_t v95 = 95;
    std::memcpy(reinterpret_cast<void*>(mem::kHeapBase + 0x1230), &v95, 4);
    s.scanner->next_scan(cheats::NextScan::Decreased, cheats::Value{}, cheats::Value{}, s.prog);
    s.last_count = s.scanner->result_count();
    s.history.push_back("Then it went down: " + std::to_string(s.last_count) + " found");
}

void fill_table() {
    ui::State& s = ui::st();
    if (!s.list.empty()) return;
    auto add = [&](uint64_t off, cheats::ScanType t, const char* desc, const char* group, const char* hotkey) {
        ui::Entry e;
        e.addr = mem::kHeapBase + off;
        e.type = t;
        e.desc = desc;
        e.group = group;
        e.hotkey = hotkey;
        e.value = ui::value_text(e.addr, t);
        e.uid = s.next_uid++;
        s.list.push_back(e);
    };
    add(0x1230, cheats::ScanType::U32, "Coins", "Player", "F5");
    add(0x2000, cheats::ScanType::F32, "Health", "Player", "L+R+A");
    add(0x2010, cheats::ScanType::U32, "Lives", "Player", "");
    add(0x2020, cheats::ScanType::F32, "Position X", "World", "");
    ui::set_freeze(s.list[1], true);
}

const Screen kScreens[] = {
    {"cheats", [] { ui::set_page(ui::PageCheats); }, false},
    {"find", [] { reset_scan(); ui::set_page(ui::PageFind); }, true},
    {"find_matches", [] { scan_first_100(); ui::set_page(ui::PageFind); }, false},
    {"find_found", [] { scan_went_down(); ui::set_page(ui::PageFind); }, true},
    {"table_empty", [] { ui::set_page(ui::PageTable); }, false},
    {"table", [] { fill_table(); ui::set_page(ui::PageTable); }, true},
    {"start_over", [] { ui::set_page(ui::PageFind); ui::confirm("Start a new search?", "The 1 match of this search are dropped. My table keeps what you added.", "Start over", [] {}); }, true},
    {"keyboard", [] { static std::string t = "Coins"; ui::set_page(ui::PageTable); ui::osk_show(&t, ui::Keys::Text, "Name"); }, true},
    {"keypad", [] { static std::string t = "95"; ui::set_page(ui::PageTable); ui::osk_show(&t, ui::Keys::Number, "Value"); }, true},
    // round 2: themes, the player's accent, docking, peek, the HUD
    {"daylight", [] { set_theme("daylight"); fill_table(); ui::set_page(ui::PageTable); }, true},
    {"theme_file", [] { set_theme(g_custom_theme); ui::set_page(ui::PageFind); scan_first_100(); }, false},
    {"accent_hex", [] { ui::st().accent = "#e0457b"; ui::set_page(ui::PageCheats); }, true},
    {"dock_left", [] { ui::st().dock = 0; fill_table(); ui::set_page(ui::PageTable); }, true},
    {"peek", [] { ui::set_key_peek(true); ui::set_page(ui::PageTable); }, true},
    {"hud", [] {
         fill_table();
         ui::st().list[0].pinned = true;     // Coins
         ui::st().list[1].pinned = true;     // Health (frozen)
         ui::st().hud_pos[0] = 24;
         ui::st().hud_pos[1] = 24;
         g_hidden = true;
     }, false},
    {"glass", [] { set_theme("frost"); g_glass = true; ui::set_backdrop(g_blur_tex->GetTexRef()); ui::set_page(ui::PageFind); }, false},
    {"glass_table", [] { set_theme("frost"); g_glass = true; ui::set_backdrop(g_blur_tex->GetTexRef()); fill_table(); ui::set_page(ui::PageTable); }, true},
    {"advanced", [] { ui::st().view_addr = mem::kHeapBase + 0x1200; ui::st().view_sel = mem::kHeapBase + 0x1230; ui::open_window("Memory viewer"); }, false},
};

}  // namespace

namespace nxrt::test {
void game_frame_rgba(std::vector<uint8_t>& px, int w, int h) { game_frame(px, w, h); }
int run_gpu_glass_test(const std::string& out_dir);   // glass_gpu.cpp
}  // namespace nxrt::test

int main(int argc, char** argv) {
    std::string out_dir = "cheats_ui_shots", ref_dir, only;
    bool update = false, gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--ref" && i + 1 < argc) ref_dir = argv[++i];
        else if (a == "--update") update = true;
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--gpu") gpu = true;
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    // PokoMac: the title's files (icon.jpg) are read from the working directory, which the test moves into its stand-in title folder
    // below (AnySwitch: set_game_dir), so the folders it was given are made absolute first
    out_dir = fs::absolute(out_dir, ec).string();
    if (!ref_dir.empty()) ref_dir = fs::absolute(ref_dir, ec).string();
    LogOptions lo;
    const std::string log_path = (fs::path(out_dir) / "cheats_ui_test.log").string();
    lo.path = log_path.c_str();
    lo.level = LogLevel::Info;
    lo.to_stderr = false;
    lo.keep_previous = false;
    log_init(lo);
    config_set("cheats", "dir", (fs::path(out_dir) / "cheats").string().c_str());
#ifdef NXRT_CHEATS_UI_TEST_DIR
    fs::current_path(std::string(NXRT_CHEATS_UI_TEST_DIR) + "/title", ec);   // the stand-in title folder: its icon.jpg shows in the header
#endif
    static game::Info info;                                          // a game with a name, as the generic game module registers one
    info.title = "Test Game";
    game::set_current(&info);
    guest_setup();

    ui::init();
    ui::st().animations = false;
    {
        // the game's icon is decoded on a thread of its own (it took 5-17 ms of the render thread at the first show): the first request
        // answers at once, null while it decodes (the header draws its placeholder), and the icon is there after it
        const auto t0 = std::chrono::steady_clock::now();
        const bool first_null = ui::game_icon() == nullptr;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool ready = ui::game_icon_wait(5000);
        const bool ok = ms < 20.0 && ready;
        std::printf("%-6s game icon: the first request took %.2f ms (%s), decoded off the caller's thread: %s\n", ok ? "ok" : "FAILED",
                    ms, first_null ? "placeholder" : "icon", ready ? "yes" : "no");
        if (!ok) ++g_failures;
    }
    {
        // a player's theme file, as one would share it: a deep green glass-less theme with its own accent and square-ish corners
        g_custom_theme = (fs::path(out_dir) / "forest.json").string();
        FILE* f = std::fopen(g_custom_theme.c_str(), "wb");
        std::fputs("{\"format\": 1, \"name\": \"Forest\", \"author\": \"a player\", \"style\": \"solid\", \"opacity\": 94, \"radius\": 8,\n"
                   " \"accent\": \"#9be15d\", \"colors\": {\"panel\": \"#0f1a14\", \"dialog\": \"#16241c\", \"surface\": \"#dff5e6\",\n"
                   " \"well\": \"#00000044\", \"text\": \"#eef7f0\", \"on_accent\": \"#0c1a06\", \"ok\": \"#8fd3ff\", \"warn\": \"#f4c066\",\n"
                   " \"danger\": \"#ff8a80\", \"scrim\": \"#02080580\"}}\n", f);
        std::fclose(f);
        auto check = [](const char* what, bool ok) {
            std::printf("%-6s %s\n", ok ? "ok" : "FAILED", what);
            if (!ok) ++g_failures;
        };
        auto write = [&](const char* name, const char* text) {
            const std::string path = (fs::path(out_dir) / name).string();
            FILE* t = std::fopen(path.c_str(), "wb");
            std::fputs(text, t);
            std::fclose(t);
            return path;
        };
        std::string err;
        const auto forest = ui::theme::load_theme(g_custom_theme, &err);
        check("theme file: a valid player's theme loads", err.empty() && forest.name == "Forest" && forest.radius == 8.f);
        err.clear();
        auto t = ui::theme::load_theme(write("bad_contrast.json", "{\"colors\": {\"panel\": \"#808080\", \"text\": \"#909090\"}}"), &err);
        check("theme file: unreadable text is refused, the default theme used", err.find("contrast") != std::string::npos && t.source == "builtin:sunrise");
        err.clear();
        t = ui::theme::load_theme(write("bad_key.json", "{\"colours\": {\"panle\": \"#000000\"}}"), &err);
        check("theme file: an unknown colour is refused", err.find("panle") != std::string::npos && t.source == "builtin:sunrise");
        err.clear();
        t = ui::theme::load_theme(write("bad_value.json", "{\"opacity\": 5}"), &err);
        check("theme file: an opacity out of range is refused", err.find("opacity") != std::string::npos);
        err.clear();
        t = ui::theme::load_theme(write("not_json.json", "theme = forest"), &err);
        check("theme file: not JSON is refused", !err.empty() && t.source == "builtin:sunrise");
        err.clear();
        t = ui::theme::load_theme("no_such_theme", &err);
        check("theme: an unknown name gives the default theme", !err.empty() && t.source == "builtin:sunrise");
        for (const char* b : {"sunrise", "dusk", "midnight", "daylight", "paper", "frost"}) {
            err.clear();
            t = ui::theme::load_theme(b, &err);
            char what[96];
            std::snprintf(what, sizeof what, "theme: the built-in %s loads", b);
            check(what, err.empty() && t.source == std::string("builtin:") + b);
        }
        ImU32 c = 0;
        check("colour: #ffc23d", ui::theme::parse_colour("#ffc23d", &c) && c == IM_COL32(0xff, 0xc2, 0x3d, 0xff));
        check("colour: #fc3 and #ffc23d80", ui::theme::parse_colour("#fc3", &c) && c == IM_COL32(0xff, 0xcc, 0x33, 0xff) &&
                                                  ui::theme::parse_colour("#ffc23d80", &c) && c == IM_COL32(0xff, 0xc2, 0x3d, 0x80));
        check("colour: rgb(255, 194, 61) and 255,194,61", ui::theme::parse_colour("rgb(255, 194, 61)", &c) && c == IM_COL32(255, 194, 61, 255) &&
                                                               ui::theme::parse_colour("255,194,61", &c) && c == IM_COL32(255, 194, 61, 255));
        check("colour: rgba(0, 0, 0, 0.5)", ui::theme::parse_colour("rgba(0, 0, 0, 0.5)", &c) && c == IM_COL32(0, 0, 0, 128));
        check("colour: nonsense is refused", !ui::theme::parse_colour("#12345", &c) && !ui::theme::parse_colour("blue-ish", &c) &&
                                                 !ui::theme::parse_colour("300, 0, 0", &c));
        const auto& spec = ui::theme::look_spec();
        check("look spec: compiled in (opacity 30..100, theme sunrise, accent theme)", spec.opacity.min == 30 && spec.opacity.max == 100 &&
                                                                                          spec.theme == "sunrise" && spec.accent == "theme");
    }

    struct Res { int w, h; float scale; const char* tag; };
    const Res kRes[] = {{1280, 720, 1.f, "1x"}, {2560, 1440, 2.f, "2x"}};
    for (const Res& r : kRes) {
        // a context per scale, as a fresh start of the overlay at that size (fonts, window positions)
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        test::soft_init();
        ui::setup_context();   // as overlay.cpp: the overlay's fonts and the theme before the first frame
        ui::on_shown();
        Ctx c;
        c.w = r.w;
        c.h = r.h;
        c.scale = r.scale;
        game_frame(c.bg, c.w, c.h);
        {
            int bw = 0, bh = 0;
            const std::vector<uint8_t> blurred = test::blur_reference(c.bg, c.w, c.h, ui::theme::load_theme("frost", nullptr).blur, nullptr, &bw, &bh);
            g_blur_tex = IM_NEW(ImTextureData)();
            g_blur_tex->Create(ImTextureFormat_RGBA32, bw, bh);
            std::memcpy(g_blur_tex->GetPixels(), blurred.data(), blurred.size());
            ImGui::RegisterUserTexture(g_blur_tex);
        }
        for (auto& e : ui::st().list) ui::set_freeze(e, false);
        ui::st().list.clear();
        const uint32_t v100 = 100;
        std::memcpy(reinterpret_cast<void*>(mem::kHeapBase + 0x1230), &v100, 4);
        const std::vector<uint8_t> scene_bg = c.bg;
        for (int bgk = 0; bgk < 4; ++bgk) {
        if (bgk > 0 && r.scale != 1.f) break;
        if (bgk > 0) test::background(bgk, c.bg, c.w, c.h);
        for (const Screen& sc : kScreens) {
            if (!only.empty() && only != sc.name) continue;
            reset_look();
            use_pad(sc.pad);
            sc.setup();
            std::vector<uint8_t> px;
            for (int f = 0; f < 4; ++f) {
                frame(c, px, f > 0);   // windows size themselves over the first frames (the first also bakes glyphs: not timed)
                int rect[4];
                float strength = 0.f;
                if (g_glass && f == 1 && ui::backdrop_rect(rect, &strength)) {
                    // what the GPU pass would give for this panel: the reference blur of its rectangle
                    int bw = 0, bh = 0;
                    const std::vector<uint8_t> blurred = test::blur_reference(c.bg, c.w, c.h, strength, rect, &bw, &bh);
                    std::memcpy(g_blur_tex->GetPixels(), blurred.data(), blurred.size());
                }
            }
            const std::string name = std::string(sc.name) + "@" + (bgk ? test::background_name(bgk) : r.tag);
            if (!test::theme_guard(ImGui::GetDrawData(), name.c_str())) ++g_failures;
            ImGui::GetCurrentContext()->OpenPopupStack.resize(0);   // the next screen starts without the last one's popups and keyboard
            ui::osk_show(nullptr, ui::Keys::Text, nullptr);
            const std::string png = (fs::path(out_dir) / (name + ".png")).string();
            if (!gfx::save_png_rgba(png, px.data(), static_cast<uint32_t>(c.w), static_cast<uint32_t>(c.h))) {
                std::printf("FAILED %s: cannot write %s\n", name.c_str(), png.c_str());
                ++g_failures;
                continue;
            }
            if (ref_dir.empty()) { std::printf("wrote  %s\n", png.c_str()); continue; }
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
            std::printf("%-6s %s (mean %.2f, %.2f%% of pixels off by more than 8)\n", ok ? "ok" : "FAILED", name.c_str(), d.mean_abs,
                        d.fraction_over_8 * 100.0);
            if (!ok) ++g_failures;
        }
        }
        c.bg = scene_bg;
        if (r.scale == 1.f && only.empty()) {
            // the pad drives it: L / R step through the pages, - takes back what the last message offers
            std::vector<uint8_t> px;
            auto press = [&](uint64_t bit) {
                ui::pad_input(bit);
                frame(c, px, false);
                ui::pad_input(0);
                frame(c, px, false);
            };
            auto check = [](const char* what, bool ok) {
                std::printf("%-6s %s\n", ok ? "ok" : "FAILED", what);
                if (!ok) ++g_failures;
            };
            use_pad(true);
            ui::set_page(ui::PageCheats);
            frame(c, px, false);
            press(hid::btn(hid::kBtnL));
            check("pad: L from Cheats goes round to Advanced", ui::st().page == ui::PageAdvanced);
            press(hid::btn(hid::kBtnR));
            press(hid::btn(hid::kBtnR));
            check("pad: R twice goes on to Find a value", ui::st().page == ui::PageFind);
            const size_t n = ui::st().list.size();
            ui::add_entry(mem::kHeapBase + 0x2010, cheats::ScanType::U32, "Lives copy");
            check("a found value lands in My table", ui::st().list.size() == n + 1);
            press(hid::btn(hid::kBtnMinus));
            check("pad: - takes it back (the message's Undo)", ui::st().list.size() == n);
            press(hid::btn(hid::kBtnMinus));
            check("pad: - again does nothing (the undo is used)", ui::st().list.size() == n);
        }
        ImGui::DestroyContext();
    }
    if (gpu) g_failures += test::run_gpu_glass_test(out_dir);
    if (g_ui_frames)
        std::printf("overlay CPU per frame (theme + pages + ImGui, no rendering): %.3f ms average over %d frames\n", g_ui_ms / g_ui_frames, g_ui_frames);
    std::printf("%s\n", g_failures ? "cheats_ui_test: FAILED" : "cheats_ui_test: all ok");
    std::fflush(stdout);
    std::_Exit(g_failures ? 1 : 0);   // the engine's and Lua's threads are not joined: a test process ends here
}
