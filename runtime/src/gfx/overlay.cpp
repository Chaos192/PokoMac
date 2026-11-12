// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay (declarations in include/nxrt/overlay.h). ImGui and its Vulkan backend are pinned in cmake/gfx_deps.cmake.
#include "nxrt/overlay.h"

#include <atomic>
#include <mutex>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "nxrt/config.h"
#include "nxrt/host_device.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/nvn.h"
#include "nxrt/platform.h"
#include "nxrt/services_api.h"
#include "extras/backdrop_blur.h"
#include "extras/overlay_composite.h"
#include "fps_stats.h"
#include "../cheats/control.h"
#include "play/cheats_ui.h"

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "overlay";

ImGuiKey map_key(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return static_cast<ImGuiKey>(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return ImGuiKey_0;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    switch (sc) {
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

const double kFpsChoices[] = {0, 30, 60, 90, 120, 144, 165, 240, 360};

// ---- on-screen keyboard: text helpers and the key grid ----
// UTF-8 to code points (a bad byte becomes U+FFFD) and back. The applet's max_len counts characters, so the text is kept as code points.
std::u32string utf8_to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) { out.push_back(0xFFFD); ++i; continue; }
        char32_t v = n == 1 ? c : (c & (0x7Fu >> n));
        bool ok = true;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            v = (v << 6) | (cc & 0x3Fu);
        }
        out.push_back(ok ? v : 0xFFFD);
        i += ok ? n : 1;
    }
    return out;
}

std::string u32_to_utf8(const std::u32string& s, size_t from = 0, size_t to = std::u32string::npos) {
    std::string out;
    to = std::min(to, s.size());
    auto put = [&](uint32_t b) { out.push_back(static_cast<char>(b)); };
    for (size_t i = from; i < to; ++i) {
        const uint32_t c = s[i];
        if (c < 0x80) put(c);
        else if (c < 0x800) { put(0xC0 | (c >> 6)); put(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { put(0xE0 | (c >> 12)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
        else { put(0xF0 | (c >> 18)); put(0x80 | ((c >> 12) & 0x3F)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
    }
    return out;
}

// Typographic punctuation the overlay's font has no glyph for, as plain ASCII, for the game's header and guide text (Pokopia's name question
// is "What\u2019s your name?", drawn as "What?s" otherwise).
std::string fold_punct(const std::string& s) {
    std::u32string in = utf8_to_u32(s), out;
    for (char32_t c : in) {
        switch (c) {
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += U'\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += U'"'; break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: out += U'-'; break;
            case 0x2026: out += U"..."; break;
            case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x3000: out += U' '; break;
            default: out += c;
        }
    }
    return u32_to_utf8(out);
}

// The grid: rows of 10 column units, a key spans `w` of them. Shift turns letters upper case and punctuation into its second character (alt).
enum class KbOp : uint8_t { Char, Shift, Space, Back, Ok, Cancel };
struct KbKey { char32_t ch; char32_t alt; int w; KbOp op; };
constexpr int kKbUnits = 10;
const std::vector<std::vector<KbKey>>& kb_rows() {
    static const std::vector<std::vector<KbKey>> rows = [] {
        std::vector<std::vector<KbKey>> r;
        auto row = [&](const char* chars, const char* alts) {
            std::vector<KbKey> v;
            for (int i = 0; chars[i]; ++i) v.push_back({static_cast<char32_t>(chars[i]), static_cast<char32_t>(alts[i]), 1, KbOp::Char});
            r.push_back(v);
        };
        row("1234567890", "1234567890");
        row("qwertyuiop", "QWERTYUIOP");
        row("asdfghjkl-", "ASDFGHJKL_");
        row("zxcvbnm,.?", "ZXCVBNM;:/");
        r.push_back({{0, 0, 2, KbOp::Shift}, {U' ', U' ', 4, KbOp::Space}, {U'!', U'@', 1, KbOp::Char}, {U'\'', U'"', 1, KbOp::Char}, {0, 0, 2, KbOp::Back}});
        r.push_back({{0, 0, 5, KbOp::Cancel}, {0, 0, 5, KbOp::Ok}});
        return r;
    }();
    return rows;
}
// The key of a row that covers column unit u (past the end: the last key); *start = its first unit.
int kb_key_at(const std::vector<KbKey>& row, int u, int* start) {
    int x = 0;
    for (int i = 0; i < static_cast<int>(row.size()); ++i) {
        if (u < x + row[i].w || i + 1 == static_cast<int>(row.size())) {
            if (start) *start = x;
            return i;
        }
        x += row[i].w;
    }
    return 0;
}

// Pad directions the keyboard moves on: the d-pad, or the left stick past half way (set_keyboard_pad adds the stick bits).
constexpr uint64_t kKbDirBits[4] = {
    hid::btn(hid::kBtnUp) | hid::btn(hid::kBtnStickLUp), hid::btn(hid::kBtnDown) | hid::btn(hid::kBtnStickLDown),
    hid::btn(hid::kBtnLeft) | hid::btn(hid::kBtnStickLLeft), hid::btn(hid::kBtnRight) | hid::btn(hid::kBtnStickLRight)};
constexpr double kKbRepeatDelay = 0.40, kKbRepeatEvery = 0.07;   // seconds: a held direction or B repeats

// Drops pressed keys and mouse buttons so nothing stays latched in ImGui (the caller holds the context's mutex).
void clear_imgui_input(ImGuiContext* ctx) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    ImGui::SetCurrentContext(prev);
}
}  // namespace

namespace {
std::atomic<int> g_shader_backlog{0};
}
void set_shader_backlog(int pending) { g_shader_backlog.store(pending < 0 ? 0 : pending, std::memory_order_relaxed); }
int shader_backlog() { return g_shader_backlog.load(std::memory_order_relaxed); }

struct Overlay::Impl {
    Context ctx;
    ImGuiContext* imgui = nullptr;
    ImGuiStyle base_style;            // the style at scale 1; style = base_style scaled by ui_scale (update_scale)
    float ui_scale = 0.f;             // 0: not applied yet
    bool backend_ready = false;
    std::atomic<bool> visible{false};
    std::atomic<bool> touch_btn{false};      // set_touch_button: the corner button and finger-sized controls
    std::atomic<bool> touch_toggle_req{false};   // the ImGui button was tapped while the menu was open: apply_pending toggles outside the frame
    std::atomic<float> disp_w{0.f}, disp_h{0.f};   // the frame size of the last drawn frame (the button's place, for hit tests off the presenting thread)
    bool touch_armed = false;                // a finger went down on the corner button and has not come up yet (main thread)
    bool scaled_touch = false;               // the touch state the style was scaled for
    ImVec4 touch_rect() const;               // x, y, w, h of the corner button in frame pixels
    bool touch_button_event(const input::Event& e);   // the hidden menu's corner button; true = the event is the button's
    void draw_touch_button(bool open);       // inside an ImGui frame
    std::mutex mu;                    // guards the ImGui context: events arrive on the game's main thread, drawing happens on the presenting thread
    int toggle_sc = SDL_SCANCODE_F1;
    // The cheats overlay (phase 6): a separate layer with its own key and settings (cheats_ui.cpp draws it).
    std::atomic<bool> cheats_visible{false};
    int cheats_sc = SDL_SCANCODE_F3;
    bool cheats_enabled = false;      // [cheats] enable: the overlay and its key exist at all
    bool cheats_pass = false;         // [cheats] pass_input: input also reaches the game while it is open
    bool cheats_pause = false;        // [cheats] pause_game
    bool cheats_inited = false;       // cheats::ui::init ran (the table, hotkeys and frame callbacks run from the first frame on)
    bool cheats_paused_it = false;    // [cheats] pause_game: this overlay paused the game when it opened
    // the cheats key while the panel is open: a press closes it (on release), holding it peeks at the game (cheats_ui.h set_key_peek)
    std::atomic<bool> cheats_key_held{false};
    std::atomic<bool> cheats_key_peeking{false};
    std::atomic<int64_t> cheats_key_down_ms{0};
    // the auto accent: one frame without the overlay is read back when it opens (Renderer::capture_next_frame)
    std::atomic<bool> cheats_cap_req{false};
    bool cheats_cap_wait = false;
    std::vector<uint8_t> cheats_cap;
    uint32_t cheats_cap_w = 0, cheats_cap_h = 0;
    bool restore_relative = false;
    std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    uint64_t drawn = 0;

    struct Panel { std::string title; std::function<void()> fn; };
    std::vector<Panel> panels;

    // menu state applied by apply_pending()
    bool want_mode = false;
    PresentMode new_mode = PresentMode::Fifo;
    bool want_fullscreen = false;
    platform::FullscreenMode new_fullscreen = platform::FullscreenMode::Windowed;

    float frame_ms[120] = {};
    int frame_ms_pos = 0;
    int fps_choice = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    bool hdr = false;                 // HDR swapchain: the overlay is drawn into an RGBA8 target and composited (draw)
    extras::OverlayComposite composite;
    // the cheats overlay's glass panel: the frame blurred under it (made at the first glass frame; never while it is solid or closed)
    extras::BackdropBlur blur;
    bool blur_failed = false;
    VkDescriptorSet blur_tex = VK_NULL_HANDLE;   // the blur's level 0 as an ImGui texture
    VkImageView blur_tex_view = VK_NULL_HANDLE;

    // Corner frame-rate counter. set_counter() stores the settings from any thread; the rest is used by the presenting thread only.
    std::atomic<bool> ptr_show{false}, ptr_in_shots{true};
    std::atomic<float> ptr_x{0}, ptr_y{0};
    std::atomic<bool> counter_show{false};
    std::atomic<int> counter_corner{0};
    std::atomic<int> counter_detail{1};
    std::atomic<bool> counter_in_shots{false};
    bool counter_was_on = false;
    bool counter_have_prev = false;
    std::chrono::steady_clock::time_point counter_prev{};
    FpsStats stats;
    double text_s = -1e9;                 // time of the last text refresh, seconds (far past: refresh at once)
    FpsStats::Snapshot snap;              // the snapshot taken at text_s
    std::string counter_lines[4];
    int counter_n = 0;

    void build_ui();
    void update_scale(const VkExtent2D& extent);
    void refresh_counter_text(double now_s);
    void draw_counter(const FrameInfo& f, double now_s);
    // The "Preparing shaders" note: shown once the backlog has stood for a second and a half, gone a second after it is empty.
    bool note_enabled = true;
    bool note_read = false;
    double note_since_s = -1.0, note_empty_s = -1.0;
    int note_peak = 0;
    bool note_visible(double now_s);
    void draw_note(const FrameInfo& f);
    // The "Save failed" notice (services_api.h fsapi::save_failures): shown for 10 s after a save commit was refused. Always on, also in
    // screenshots: the player has to see it.
    uint32_t save_seen = 0;
    double save_until_s = -1.0;
    bool save_notice_visible(double now_s);
    void draw_save_notice(const FrameInfo& f);

    // The on-screen keyboard (services_api.h swkbdui). kb_mu guards the question and its text: key events edit it on the game's main thread,
    // the pad and the mouse in draw() on the presenting thread. Lock order: mu (ImGui), then kb_mu, then the swkbdui question's own lock.
    std::mutex kb_mu;
    std::atomic<bool> kb_open{false};
    std::atomic<bool> kb_by_label{false};
    uint64_t kb_id = 0;                   // the question on screen, 0 = none
    uint64_t kb_answered = 0;             // the last question this keyboard answered (ids only grow): never reopened
    services::swkbdui::Request kb_req;
    std::u32string kb_text;               // code points
    size_t kb_caret = 0;
    int kb_shift = 0;                     // 0 off, 1 the next character only, 2 locked
    int kb_row = 1, kb_unit = 0;          // the pad's key: row, column unit
    double kb_rep[5] = {};                // the next repeat of up, down, left, right, B (seconds; DBL_MAX: not held since a press)
    // The pad (set_keyboard_pad, on the thread that polls it): what is held, and presses not yet taken by draw().
    std::atomic<uint64_t> pad_held{0}, pad_new{0};
    uint64_t pad_prev = 0, pad_mask = 0;  // set_keyboard_pad only
    // Steam's on-screen keyboard (host_steam_keyboard_*): wanted for the question on screen (Proton on a Steam Deck, SteamOS, Steam's gamepad UI),
    // and asked for. It is asked for once the text field has a place on screen, and closed with the question.
    bool kb_steam_want = false, kb_steam_open = false;
    int kb_frames = 0;                    // frames drawn for the question on screen (its layout settles after the first)
    bool kb_keys = true;                  // the on-screen keys are shown (with Steam's keyboard: only after a pad press reached the game)
    void kb_steam_close();                // kb_mu held

    bool kb_poll();                       // follows swkbdui::pending(): opens a new question, drops one answered elsewhere; true while open
    bool kb_finish(bool ok);              // kb_mu held: answers the question; false when there is none or OK is not allowed yet (min_len)
    void kb_insert(const std::u32string& t);   // kb_mu held: at the caret, control characters dropped, up to max_len
    void kb_backspace();                  // kb_mu held
    bool kb_press(const KbKey& k);        // kb_mu held: an on-screen key; true when it answered the question
    bool kb_key_event(const input::Event& e);   // a physical key or text; kb_mu and mu not held
    void draw_keyboard(double now_s);     // mu held, inside an ImGui frame

    // The quit prompt (Overlay::open_quit_prompt). Flags only: opened from any thread, answered by key events (main thread), the pad
    // (set_keyboard_pad, main thread) or the mouse (draw, presenting thread). A confirmed prompt stays open until the process ends.
    std::atomic<bool> quit_open{false}, quit_confirmed{false}, settings_wanted{false};
    void quit_answer(bool ok) {
        if (ok) quit_confirmed = true;
        else quit_open = false;
    }
    void quit_to_settings() {   // the prompt's Settings choice: close it, the owner opens the F1 page (take_settings_wanted)
        settings_wanted = true;
        quit_open = false;
    }
    bool quit_key_event(const input::Event& e);   // a physical key while the prompt is open; always consumed
    void draw_quit_prompt();              // mu held, inside an ImGui frame
};

// The corner button: 52 x 52 UI units at the bottom left, 12 units in. ui_scale grows with the frame (and by a quarter with the touch button on).
ImVec4 Overlay::Impl::touch_rect() const {
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float w = 56.f * k, h = 52.f * k, m = 12.f * k;
    return ImVec4(m, disp_h.load(std::memory_order_relaxed) - h - m, w, h);
}

bool Overlay::Impl::touch_button_event(const input::Event& e) {
    using input::EventType;
    const ImVec4 r = touch_rect();
    auto inside = [&](float x, float y) { return x >= r.x && x < r.x + r.z && y >= r.y && y < r.y + r.w; };
    if (disp_h.load(std::memory_order_relaxed) <= 0.f) return false;   // nothing drawn yet
    if (e.type == EventType::MouseButtonDown && e.u.button.button == 1) {
        touch_armed = inside(e.u.button.x, e.u.button.y);
        return touch_armed;
    }
    if (e.type == EventType::MouseMotion) return touch_armed;            // the finger that is on the button does not move the game's pointer
    if (e.type == EventType::MouseButtonUp && e.u.button.button == 1 && touch_armed) {
        touch_armed = false;
        if (inside(e.u.button.x, e.u.button.y)) touch_toggle_req.store(true);   // the same toggle as the F1 key, applied by apply_pending
        return true;
    }
    return false;
}

void Overlay::Impl::draw_touch_button(bool open) {
    const ImVec4 r = touch_rect();
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const ImVec2 p0(r.x, r.y), p1(r.x + r.z, r.y + r.w);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, open ? 200 : 150), 10.f * k);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, open ? 230 : 160), 10.f * k, 0, 1.5f * k);
    const char* label = open ? "Close" : "Menu";
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + (r.z - ts.x) * 0.5f, p0.y + (r.w - ts.y) * 0.5f), IM_COL32(255, 255, 255, 235), label);
    if (open) {
        // the menu is up: ImGui gets the mouse, so the button is a real widget this time
        ImGui::SetNextWindowPos(p0, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(r.z, r.w), ImGuiCond_Always);
        const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("##nxrt_touch_menu", nullptr, wf)) {
            if (ImGui::InvisibleButton("##close", ImVec2(r.z, r.w))) touch_toggle_req.store(true);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

void Overlay::Impl::build_ui() {
    Renderer& r = *ctx.renderer;
    // First-use layout, in units of the UI scale (update_scale): the panels (the settings menu) sit at the top left, the diagnostics window
    // at the top right and folded up. The player moves them; nothing is remembered between runs.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float diag_w = std::min(420.f * k, std::max(200.f, disp.x - 32.f * k));
    ImGui::SetNextWindowPos(ImVec2(std::max(16.f * k, disp.x - diag_w - 16.f * k), 16.f * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(diag_w, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Diagnostics")) {
        const GpuInfo& g = ctx.device->gpu();
        ImGui::Text("GPU: %s", g.name.c_str());
        ImGui::Text("Vulkan 1.3, driver 0x%08x, validation %s%s", g.driver_version, ctx.device->validation_enabled() ? "on" : "off",
                    ctx.device->headless() ? ", headless" : "");
        const VkExtent2D e = r.extent();
        ImGui::Text("Target: %ux%u, %u images, %u frames in flight", e.width, e.height, r.image_count(), r.frames_in_flight());

        ImGui::SeparatorText("Presentation");
        const PresentMode modes[] = {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate};
        int cur = static_cast<int>(r.requested_present_mode());
        if (ImGui::BeginCombo("Present mode", present_mode_name(modes[cur]))) {
            for (int i = 0; i < 4; ++i) {
                const bool ok = r.present_mode_supported(modes[i]);
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(present_mode_name(modes[i]), i == cur)) { want_mode = true; new_mode = modes[i]; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (r.present_mode() != r.requested_present_mode()) ImGui::TextDisabled("in use: %s (requested mode unavailable)", present_mode_name(r.present_mode()));

        if (ctx.limiter) {
            char label[32];
            const double t = ctx.limiter->target_fps();
            std::snprintf(label, sizeof label, t > 0 ? "%.0f fps" : "unlimited", t);
            if (ImGui::BeginCombo("Frame cap", label)) {
                for (double v : kFpsChoices) {
                    std::snprintf(label, sizeof label, v > 0 ? "%.0f fps" : "unlimited", v);
                    if (ImGui::Selectable(label, v == t)) ctx.limiter->set_target_fps(v);
                }
                ImGui::EndCombo();
            }
            const float hz = platform::has_window() ? platform::display_refresh_hz() : 0.f;
            if (hz > 0) {
                ImGui::TextDisabled("display %.2f Hz; VRR-friendly cap %.1f fps", hz, suggest_vrr_cap(hz));
                if (ImGui::SmallButton("Use VRR cap")) ctx.limiter->set_target_fps(suggest_vrr_cap(hz));
            }
        }

        if (ctx.has_window && platform::has_window()) {
            ImGui::SeparatorText("Window");
            const auto fm = platform::fullscreen_mode();
            if (ImGui::RadioButton("Windowed", fm == platform::FullscreenMode::Windowed)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Windowed; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Borderless", fm == platform::FullscreenMode::Borderless)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Borderless; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Exclusive", fm == platform::FullscreenMode::Exclusive)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Exclusive; }
            bool rel = platform::relative_mouse();
            if (ImGui::Checkbox("Raw mouse (relative mode)", &rel)) platform::set_relative_mouse(rel);
            ImGui::SameLine();
            ImGui::TextDisabled(platform::relative_mouse_is_raw() ? "(unaccelerated)" : "(system scaling!)");
        }

        ImGui::SeparatorText("Timing");
        const RendererStats rs = r.stats();
        const float dt_ms = ImGui::GetIO().DeltaTime * 1000.f;
        frame_ms[frame_ms_pos] = dt_ms;
        frame_ms_pos = (frame_ms_pos + 1) % 120;
        ImGui::Text("CPU frame %.2f ms (%.0f fps)   GPU %.2f ms (avg %.2f)", dt_ms, dt_ms > 0 ? 1000.f / dt_ms : 0.f, rs.gpu_ms_last, rs.gpu_ms_avg);
        ImGui::PlotLines("##ft", frame_ms, 120, frame_ms_pos, nullptr, 0.f, 33.f, ImVec2(0, 48));
        ImGui::Text("frames %llu, swapchains %llu, out-of-date %llu, screenshots %llu", static_cast<unsigned long long>(rs.frames),
                    static_cast<unsigned long long>(rs.swapchain_creates), static_cast<unsigned long long>(rs.out_of_date),
                    static_cast<unsigned long long>(rs.screenshots));
        const ValidationStats vs = ctx.device->validation_stats();
        ImGui::Text("validation: %llu errors, %llu warnings", static_cast<unsigned long long>(vs.errors), static_cast<unsigned long long>(vs.warnings));

        ImGui::SeparatorText("NVN front end");
        ImGui::Text("%zu entry points, %zu implemented; stub calls %llu (%zu distinct)", nvn::entry_count(), nvn::implemented_count(),
                    static_cast<unsigned long long>(nvn::stub_calls_total()), nvn::stubs_called_distinct());
        if (ImGui::TreeNode("Most called stubs")) {
            auto calls = nvn::called_entries();
            int shown = 0;
            for (auto& c : calls) {
                if (c.implemented) continue;
                ImGui::Text("%8llu  %s", static_cast<unsigned long long>(c.calls), c.entry->name);
                if (++shown >= 12) break;
            }
            if (!shown) ImGui::TextDisabled("none called");
            ImGui::TreePop();
        }
    }
    ImGui::End();
    for (auto& p : panels) {
        ImGui::SetNextWindowPos(ImVec2(16.f * k, 16.f * k), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(std::min(760.f * k, std::max(240.f, disp.x - 32.f * k)), std::min(680.f * k, std::max(200.f, disp.y - 32.f * k))),
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin(p.title.c_str())) p.fn();
        ImGui::End();
    }
}

// DPI: the UI is laid out for a 1080 pixel high frame at 16 px text and grows with the frame (4K: twice the size; a Windows display scaling
// of 150% shows up as more pixels in the same window, so it is covered too). Never below 1: a 720p window keeps the 16 px text.
void Overlay::Impl::update_scale(const VkExtent2D& extent) {
    const bool touch = touch_btn.load(std::memory_order_relaxed);
    // finger-sized: a quarter bigger than the frame asks for, so rows and buttons reach about 40 px on a 720 to 800 pixel handheld screen
    const float want = std::round(std::clamp(static_cast<float>(extent.height) / 1080.f * (touch ? 1.25f : 1.f), touch ? 1.25f : 1.0f, 3.0f) * 20.f) / 20.f;   // steps of 0.05
    if (want == ui_scale && touch == scaled_touch) return;
    ui_scale = want;
    scaled_touch = touch;
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style;
    style.ScaleAllSizes(want);
    style.FontScaleDpi = want;
    if (touch) {   // easier to hit with a finger: a taller row and a wider scroll bar and slider grip
        style.FramePadding.y = std::max(style.FramePadding.y, 8.f * want);
        style.ItemSpacing.y = std::max(style.ItemSpacing.y, 8.f * want);
        style.ScrollbarSize = std::max(style.ScrollbarSize, 22.f * want);
        style.GrabMinSize = std::max(style.GrabMinSize, 20.f * want);
    }
}

std::unique_ptr<Overlay> Overlay::create(const Context& ctx, std::string* error) {
    if (!ctx.device || !ctx.renderer) {
        if (error) *error = "overlay needs a device and a renderer";
        return nullptr;
    }
    std::unique_ptr<Overlay> o(new Overlay());
    o->impl_ = new Impl();
    Impl& s = *o->impl_;
    s.ctx = ctx;
    s.color_format = ctx.renderer->format();
    if (ctx.renderer->output_kind() != extras::OutputKind::Sdr8) {
        // HDR swapchain: ImGui draws into an RGBA8 target that the composite maps onto the swapchain (see draw)
        if (s.composite.init(*ctx.device)) {
            s.hdr = true;
            s.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            NXLOG_W(kTag, "HDR overlay composite failed to initialise: the overlay is drawn straight into the HDR swapchain (colours will look wrong)");
        }
    }
    const std::string key = config_get_str("gfx", "overlay_key", "F1");
    if (const SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str()); sc != SDL_SCANCODE_UNKNOWN) s.toggle_sc = sc;
    else NXLOG_W(kTag, "unknown [gfx] overlay_key '%s': using F1", key.c_str());

    // The cheats overlay (phase 6): its own key, separate from the F1 dev menu above.
    s.cheats_enabled = config_get_bool("cheats", "enable", false);
    const std::string ckey = config_get_str("cheats", "key", "F3");
    if (const SDL_Scancode sc = SDL_GetScancodeFromName(ckey.c_str()); sc != SDL_SCANCODE_UNKNOWN) s.cheats_sc = sc;
    else NXLOG_W(kTag, "unknown [cheats] key '%s': using F3", ckey.c_str());
    if (s.cheats_sc == s.toggle_sc) { NXLOG_W(kTag, "[cheats] key equals [gfx] overlay_key (%s): keeping F3 for cheats", key.c_str()); s.cheats_sc = SDL_SCANCODE_F3; }
    s.cheats_pass = config_get_bool("cheats", "pass_input", false);
    s.cheats_pause = config_get_bool("cheats", "pause_game", false);

    IMGUI_CHECKVERSION();
    s.imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   // without it ImGui ignores the gamepad keys that set_keyboard_pad sends while the page is open
    ImGui::StyleColorsDark();
    io.Fonts->AddFontDefaultVector();          // the scalable default font: stays sharp at every UI scale (update_scale)
    cheats::ui::load_fonts(io.Fonts);          // the cheats overlay's Inter (the launcher's typeface); the default above stays the menu's
    ImGui::GetStyle().FontSizeBase = 16.0f;
    s.base_style = ImGui::GetStyle();
    const VkExtent2D e = ctx.renderer->extent();
    io.DisplaySize = ImVec2(static_cast<float>(e.width), static_cast<float>(e.height));

    VkPipelineRenderingCreateInfoKHR rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &s.color_format;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = ctx.device->instance();
    ii.PhysicalDevice = ctx.device->physical();
    ii.Device = ctx.device->device();
    ii.QueueFamily = ctx.device->queue_family();
    ii.Queue = ctx.device->queue();
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, ctx.renderer->image_count());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = rci;
    ii.CheckVkResultFn = [](VkResult r) { if (r != VK_SUCCESS) NXLOG_E(kTag, "ImGui Vulkan call failed (%d)", static_cast<int>(r)); };
    if (!ImGui_ImplVulkan_Init(&ii)) {
        if (error) *error = "ImGui_ImplVulkan_Init failed";
        ImGui::DestroyContext(s.imgui);
        s.imgui = nullptr;
        return nullptr;
    }
    s.backend_ready = true;
    NXLOG_I(kTag, "overlay ready (toggle key %s)", SDL_GetScancodeName(static_cast<SDL_Scancode>(s.toggle_sc)));
    return o;
}

Overlay::~Overlay() {
    if (!impl_) return;
    if (impl_->ctx.device) impl_->ctx.device->wait_idle();
    impl_->composite.shutdown();
    if (impl_->blur_tex && impl_->backend_ready) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(impl_->imgui);
        ImGui_ImplVulkan_RemoveTexture(impl_->blur_tex);
        ImGui::SetCurrentContext(prev);
    }
    impl_->blur.shutdown();
    if (impl_->backend_ready) ImGui_ImplVulkan_Shutdown();
    if (impl_->imgui) ImGui::DestroyContext(impl_->imgui);
    delete impl_;
}

void Overlay::set_visible(bool v) {
    Impl& s = *impl_;
    if (s.visible == v) return;
    s.visible = v;
    if (v) {
        s.restore_relative = platform::has_window() && platform::relative_mouse();
        if (s.restore_relative) platform::set_relative_mouse(false);
    } else {
        if (s.restore_relative && platform::has_window()) platform::set_relative_mouse(true);
        s.restore_relative = false;
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();   // drop pressed keys and buttons so nothing stays latched
        io.ClearInputKeys();
        io.ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    NXLOG_I(kTag, "overlay %s", v ? "shown" : "hidden");
}

void Overlay::set_touch_button(bool on) { impl_->touch_btn.store(on); }

bool Overlay::visible() const { return impl_->visible; }
void Overlay::toggle() { set_visible(!impl_->visible); }
int Overlay::toggle_scancode() const { return impl_->toggle_sc; }
void Overlay::set_toggle_scancode(int sc) { impl_->toggle_sc = sc; }

void Overlay::set_cheats_visible(bool v) {
    Impl& s = *impl_;
    if (s.cheats_visible == v) return;
    s.cheats_visible = v;
    if (v) {
        cheats::ui::on_shown();
        if (cheats::ui::wants_frame_colour()) s.cheats_cap_req = true;   // the accent from the game frame under it (draw)
        if ((s.cheats_pause || cheats::ui::pause_while_open()) && !cheats::ctl::paused()) {
            cheats::ctl::pause(true);
            s.cheats_paused_it = true;
        }
    }
    if (!v) {
        s.cheats_key_held = false;            // a peek ends with the panel
        if (s.cheats_key_peeking.exchange(false)) cheats::ui::set_key_peek(false);
        if (s.cheats_paused_it) {
            cheats::ctl::pause(false);
            s.cheats_paused_it = false;
        }
        cheats::ui::save();
        std::lock_guard<std::mutex> lk(s.mu);     // drop any latched input when it closes
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    NXLOG_I(kTag, "cheats overlay %s", v ? "shown" : "hidden");
}
void Overlay::toggle_cheats() { set_cheats_visible(!impl_->cheats_visible); }
bool Overlay::cheats_visible() const { return impl_->cheats_visible; }
int Overlay::cheats_scancode() const { return impl_->cheats_sc; }
void Overlay::set_cheats_scancode(int sc) { impl_->cheats_sc = sc; }
bool Overlay::cheats_blocks_game() const { return impl_->cheats_visible && !impl_->cheats_pass; }
uint64_t Overlay::frames_drawn() const { return impl_->drawn; }

void Overlay::set_pointer(bool show, float x, float y, bool in_screenshots) {
    Impl& s = *impl_;
    s.ptr_x.store(x, std::memory_order_relaxed);
    s.ptr_y.store(y, std::memory_order_relaxed);
    s.ptr_in_shots.store(in_screenshots, std::memory_order_relaxed);
    s.ptr_show.store(show, std::memory_order_relaxed);
}

void Overlay::set_counter(const CounterConfig& c) {
    Impl& s = *impl_;
    s.counter_corner.store(std::clamp(c.corner, 0, 3), std::memory_order_relaxed);
    s.counter_detail.store(std::clamp(c.detail, 0, 2), std::memory_order_relaxed);
    s.counter_in_shots.store(c.in_screenshots, std::memory_order_relaxed);
    s.counter_show.store(c.show, std::memory_order_relaxed);   // last: the other settings are in place when it turns on
}

namespace {
bool imgui_wants(std::mutex& mu, ImGuiContext* ctx, bool mouse, bool keyboard) {
    std::lock_guard<std::mutex> lk(mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    const ImGuiIO& io = ImGui::GetIO();
    const bool w = (mouse && io.WantCaptureMouse) || (keyboard && io.WantCaptureKeyboard);
    ImGui::SetCurrentContext(prev);
    return w;
}
}  // namespace

bool Overlay::Impl::kb_poll() {
    namespace sk = services::swkbdui;
    const uint64_t id = sk::pending(nullptr);
    {
        std::lock_guard<std::mutex> lk(kb_mu);
        if (id == kb_id || (id && id <= kb_answered)) return kb_id != 0;
    }
    sk::Request r;
    uint64_t now_id = sk::pending(&r);
    std::lock_guard<std::mutex> lk(kb_mu);
    if (now_id && now_id <= kb_answered) now_id = 0;    // answered here meanwhile
    if (now_id == kb_id) return kb_id != 0;
    if (kb_id && !now_id) NXLOG_I(kTag, "on-screen keyboard: question %llu closed elsewhere", static_cast<unsigned long long>(kb_id));
    kb_steam_close();
    kb_id = now_id;
    kb_steam_want = now_id != 0 && host_steam_keyboard_wanted();
    kb_frames = 0;
    kb_keys = !kb_steam_want;
    if (now_id) {
        kb_req = std::move(r);
        for (std::string* t : {&kb_req.title, &kb_req.header, &kb_req.sub, &kb_req.guide, &kb_req.ok_text}) *t = fold_punct(*t);
        kb_text = utf8_to_u32(kb_req.initial);
        if (kb_req.max_len && kb_text.size() > kb_req.max_len) kb_text.resize(kb_req.max_len);
        kb_caret = kb_text.size();
        kb_shift = 0;
        kb_row = 1;
        kb_unit = 0;
        for (double& t : kb_rep) t = DBL_MAX;
        pad_new.store(0);
        NXLOG_I(kTag, "on-screen keyboard: question %llu (max %u characters%s)", static_cast<unsigned long long>(now_id), kb_req.max_len,
                kb_req.password ? ", password" : "");
    }
    kb_open = now_id != 0;
    return kb_open;
}

bool Overlay::Impl::kb_finish(bool ok) {
    if (!kb_id || (ok && kb_text.size() < kb_req.min_len)) return false;
    const uint64_t id = kb_id;
    kb_answered = id;
    kb_id = 0;
    kb_open = false;
    NXLOG_I(kTag, "on-screen keyboard: question %llu %s", static_cast<unsigned long long>(id), ok ? "answered" : "cancelled");
    services::swkbdui::answer(id, ok, ok ? u32_to_utf8(kb_text) : std::string());
    kb_steam_close();
    kb_steam_want = false;
    return true;
}

void Overlay::Impl::kb_steam_close() {
    if (!kb_steam_open) return;
    kb_steam_open = false;
    host_steam_keyboard_close();
}

void Overlay::Impl::kb_insert(const std::u32string& t) {
    for (const char32_t c : t) {
        if (c < 0x20 || c == 0x7F) continue;
        if (kb_req.max_len && kb_text.size() >= kb_req.max_len) break;
        kb_text.insert(kb_caret++, 1, c);
    }
}

void Overlay::Impl::kb_backspace() {
    if (kb_caret == 0) return;
    kb_text.erase(--kb_caret, 1);
}

bool Overlay::Impl::kb_press(const KbKey& k) {
    switch (k.op) {
        case KbOp::Char:
            kb_insert(std::u32string(1, kb_shift ? k.alt : k.ch));
            if (kb_shift == 1) kb_shift = 0;
            return false;
        case KbOp::Shift: kb_shift = (kb_shift + 1) % 3; return false;
        case KbOp::Space: kb_insert(U" "); return false;
        case KbOp::Back: kb_backspace(); return false;
        case KbOp::Ok: return kb_finish(true);
        case KbOp::Cancel: return kb_finish(false);
    }
    return false;
}

// The physical keyboard while the on-screen keyboard is up: typing goes straight into its text (SDL text events), Enter answers, Escape
// cancels. Every key is consumed, the game sees none.
bool Overlay::Impl::kb_key_event(const input::Event& e) {
    bool closed = false;
    {
        std::lock_guard<std::mutex> lk(kb_mu);
        if (e.type == input::EventType::Text) {
            kb_insert(utf8_to_u32(e.u.text.utf8));
        } else if (e.type == input::EventType::KeyDown && kb_id) {
            const bool rep = (e.flags & input::kFlagRepeat) != 0;
            switch (e.u.key.scancode) {
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER: if (!rep) closed = kb_finish(true); break;
                case SDL_SCANCODE_ESCAPE: if (!rep) closed = kb_finish(false); break;
                case SDL_SCANCODE_BACKSPACE: kb_backspace(); break;
                case SDL_SCANCODE_DELETE: if (kb_caret < kb_text.size()) kb_text.erase(kb_caret, 1); break;
                case SDL_SCANCODE_LEFT: if (kb_caret > 0) --kb_caret; break;
                case SDL_SCANCODE_RIGHT: if (kb_caret < kb_text.size()) ++kb_caret; break;
                case SDL_SCANCODE_HOME: kb_caret = 0; break;
                case SDL_SCANCODE_END: kb_caret = kb_text.size(); break;
                case SDL_SCANCODE_V:
                    if (e.mods & SDL_KMOD_CTRL) {
                        if (char* clip = SDL_GetClipboardText()) {
                            kb_insert(utf8_to_u32(clip));
                            SDL_free(clip);
                        }
                    }
                    break;
                default: break;
            }
        }
    }
    if (closed) {
        std::lock_guard<std::mutex> lk(mu);
        clear_imgui_input(imgui);
    }
    return true;
}

void Overlay::Impl::draw_keyboard(double now_s) {
    std::lock_guard<std::mutex> lk(kb_mu);
    if (!kb_id) return;
    const auto& rows = kb_rows();
    const int nrows = static_cast<int>(rows.size());
    bool closed = false;

    // The pad: presses since the last frame; a direction or B held past the delay repeats.
    const uint64_t held = pad_held.load();
    uint64_t fresh = pad_new.exchange(0);
    // With Steam's keyboard the panel is only the question and the text field. Steam takes the pad while its keyboard is up, so a press
    // that reaches the game means it is closed: the on-screen keys come up, and that press only shows them (+ and - keep their meaning).
    const uint64_t plus_minus = hid::btn(hid::kBtnPlus) | hid::btn(hid::kBtnMinus);
    if (!kb_keys && (fresh & ~plus_minus)) {
        kb_keys = true;
        fresh &= plus_minus;
    }
    auto fired = [&](int slot, uint64_t bits) {
        bool f = false;
        if (fresh & bits) { f = true; kb_rep[slot] = now_s + kKbRepeatDelay; }
        else if ((held & bits) && now_s >= kb_rep[slot]) { f = true; kb_rep[slot] = now_s + kKbRepeatEvery; }
        if (!(held & bits)) kb_rep[slot] = DBL_MAX;
        return f;
    };
    auto move_h = [&](int d) {
        const auto& row = rows[kb_row];
        const int n = static_cast<int>(row.size());
        const int i = (kb_key_at(row, kb_unit, nullptr) + d + n) % n;
        int x = 0;
        for (int j = 0; j < i; ++j) x += row[j].w;
        kb_unit = x;
    };
    if (fired(0, kKbDirBits[0])) kb_row = (kb_row + nrows - 1) % nrows;
    if (fired(1, kKbDirBits[1])) kb_row = (kb_row + 1) % nrows;
    if (fired(2, kKbDirBits[2])) move_h(-1);
    if (fired(3, kKbDirBits[3])) move_h(1);
    const int sel = kb_key_at(rows[kb_row], kb_unit, nullptr);
    if (fresh & hid::btn(hid::kBtnA)) closed = kb_press(rows[kb_row][sel]);
    if (!closed && fired(4, hid::btn(hid::kBtnB))) kb_backspace();
    if (!closed && (fresh & hid::btn(hid::kBtnY))) kb_insert(U" ");
    if (!closed && (fresh & hid::btn(hid::kBtnX))) kb_shift = (kb_shift + 1) % 3;
    if (!closed && (fresh & hid::btn(hid::kBtnL)) && kb_caret > 0) --kb_caret;
    if (!closed && (fresh & hid::btn(hid::kBtnR)) && kb_caret < kb_text.size()) ++kb_caret;
    if (!closed && (fresh & hid::btn(hid::kBtnPlus))) closed = kb_finish(true);
    if (!closed && (fresh & hid::btn(hid::kBtnMinus))) closed = kb_finish(false);
    if (closed) {
        clear_imgui_input(imgui);
        return;
    }

    // A centred panel over the dimmed game, sized in units of the UI scale like the menu.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float w = std::max(320.f, std::min(760.f * k, disp.x - 32.f * k));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 110));
    // centred; at the top when Steam's keyboard comes up from the bottom of the screen
    if (kb_steam_want) ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, std::round(disp.y * 0.04f)), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    else ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    const std::string title = (kb_req.title.empty() ? std::string("Keyboard") : kb_req.title) + "###nxrt_swkbd";
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | (kb_keys ? 0 : ImGuiWindowFlags_NoTitleBar);
    if (ImGui::Begin(title.c_str(), nullptr, wf)) {
        ImGui::PushTextWrapPos(0.f);
        if (!kb_req.header.empty()) ImGui::TextUnformatted(kb_req.header.c_str());
        if (!kb_req.sub.empty()) ImGui::TextDisabled("%s", kb_req.sub.c_str());
        ImGui::PopTextWrapPos();

        // the text field: drawn here, not an ImGui InputText, so the keyboard, the pad and the mouse all edit the same text and caret
        const ImGuiStyle& style = ImGui::GetStyle();
        ImFont* font = ImGui::GetFont();
        const float big = ImGui::GetFontSize() * 1.25f;
        const float fh = std::round(big * 1.7f), pad = style.FramePadding.x * 1.5f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float fw = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(fw, fh));
        const ImVec2 p1(p0.x + fw, p0.y + fh);
        // Steam's keyboard, asked for once the field has its place (an auto-sized window takes its size in the frames after the first):
        // Steam keeps the field uncovered and types into the window
        if (kb_steam_want && !kb_steam_open && ++kb_frames >= 3) {
            kb_steam_open = true;
            host_steam_keyboard_open(static_cast<int>(p0.x), static_cast<int>(p0.y), static_cast<int>(fw), static_cast<int>(fh));
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
        std::string shown, before;
        if (kb_req.password) {
            shown.assign(kb_text.size(), '*');
            before.assign(kb_caret, '*');
        } else {
            shown = u32_to_utf8(kb_text);
            before = u32_to_utf8(kb_text, 0, kb_caret);
        }
        const float cx = font->CalcTextSizeA(big, FLT_MAX, 0.0f, before.c_str()).x;
        const float scroll = std::max(0.f, cx - (fw - 2.f * pad) + 2.f);   // keep the caret in view
        const float ty = p0.y + (fh - big) * 0.5f;
        dl->PushClipRect(ImVec2(p0.x + pad * 0.5f, p0.y), ImVec2(p1.x - pad * 0.5f, p1.y), true);
        if (shown.empty() && !kb_req.guide.empty()) dl->AddText(font, big, ImVec2(p0.x + pad, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), kb_req.guide.c_str());
        else dl->AddText(font, big, ImVec2(p0.x + pad - scroll, ty), ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        if (std::fmod(now_s, 1.0) < 0.6) {
            const float x = p0.x + pad - scroll + cx;
            dl->AddLine(ImVec2(x, ty), ImVec2(x, ty + big), ImGui::GetColorU32(ImGuiCol_Text), std::max(1.f, 1.5f * k));
        }
        dl->PopClipRect();
        if (kb_req.max_len) {
            char count[32];
            std::snprintf(count, sizeof count, "%zu / %u", kb_text.size(), kb_req.max_len);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fw - ImGui::CalcTextSize(count).x);
            ImGui::TextDisabled("%s", count);
        }
        ImGui::Spacing();

        // the keys; the pad's key has a bright frame, the mouse clicks any of them
        const float gap = std::round(style.ItemSpacing.x * 0.75f);
        const float unit = (ImGui::GetContentRegionAvail().x - gap * (kKbUnits - 1)) / kKbUnits;
        const float kh = std::round(ImGui::GetFontSize() * 2.3f);
        const bool ok_allowed = kb_text.size() >= kb_req.min_len;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (int r = 0; r < nrows && !closed && kb_keys; ++r) {
            int x = 0;
            for (int i = 0; i < static_cast<int>(rows[r].size()) && !closed; ++i) {
                const KbKey& key = rows[r][i];
                if (i) ImGui::SameLine();
                std::string label;
                switch (key.op) {
                    case KbOp::Char: label = u32_to_utf8(std::u32string(1, kb_shift ? key.alt : key.ch)); break;
                    case KbOp::Shift: label = kb_shift == 2 ? "CAPS" : kb_shift ? "SHIFT" : "Shift"; break;
                    case KbOp::Space: label = "Space"; break;
                    case KbOp::Back: label = "Delete"; break;
                    case KbOp::Ok: label = kb_req.ok_text.empty() ? "OK" : kb_req.ok_text; break;
                    case KbOp::Cancel: label = "Cancel"; break;
                }
                ImGui::PushID(r * 32 + i);
                const bool is_sel = r == kb_row && i == sel;
                const bool lit = key.op == KbOp::Shift && kb_shift;
                if (lit) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
                ImGui::BeginDisabled(key.op == KbOp::Ok && !ok_allowed);
                if (ImGui::Button(label.c_str(), ImVec2(unit * key.w + gap * (key.w - 1), kh))) {
                    kb_row = r;
                    kb_unit = x;
                    closed = kb_press(key);
                }
                ImGui::EndDisabled();
                if (lit) ImGui::PopStyleColor();
                if (is_sel)
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 200, 40, 255), style.FrameRounding,
                                                        0, std::max(2.f, 2.5f * k));
                ImGui::PopID();
                x += key.w;
            }
        }
        ImGui::PopStyleVar();
        if (!closed) {
            ImGui::Spacing();
            // the face buttons are Switch buttons after the player's mapping: by printed label they are named so, by position they sit
            // where a Switch pad has them
            const bool lab = kb_by_label.load(std::memory_order_relaxed);
            ImGui::PushTextWrapPos(0.f);
            if (!kb_keys) {
                ImGui::TextDisabled("Type on Steam's keyboard, Enter to confirm. Steam + X brings it back; any button shows the on-screen keys.");
            } else {
                ImGui::TextDisabled("Pad: %s type, %s delete, %s space, %s shift, L/R move, + OK, - cancel", lab ? "A" : "A (right)",
                                    lab ? "B" : "B (bottom)", lab ? "Y" : "Y (left)", lab ? "X" : "X (top)");
                ImGui::TextDisabled("Keyboard: type, Enter OK, Esc cancel, Ctrl+V paste");
                if (kb_steam_want) ImGui::TextDisabled("Steam keyboard: Steam + X");
            }
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();
    if (closed) clear_imgui_input(imgui);
}

bool Overlay::Impl::quit_key_event(const input::Event& e) {
    if (e.type == input::EventType::KeyDown && !(e.flags & input::kFlagRepeat) && !quit_confirmed) {
        switch (e.u.key.scancode) {
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SPACE: quit_answer(true); break;
            case SDL_SCANCODE_ESCAPE: quit_answer(false); break;
            default: break;
        }
    }
    return true;
}

void Overlay::Impl::draw_quit_prompt() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float w = std::max(280.f, std::min(440.f * k, disp.x - 32.f * k));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    bool cancelled = false;
    if (ImGui::Begin("Close the game?###nxrt_quit", nullptr, wf)) {
        const bool done = quit_confirmed.load();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(done ? "Closing ..." : "Progress since the game's last save is lost.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 bs((ImGui::GetContentRegionAvail().x - gap) * 0.5f, std::round(ImGui::GetFontSize() * 2.2f));
        ImGui::BeginDisabled(done);
        if (ImGui::Button("Quit", bs)) quit_answer(true);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", bs)) {
            quit_answer(false);
            cancelled = true;
        }
        if (ImGui::Button("Settings", ImVec2(bs.x * 2.f + gap, bs.y))) {
            quit_to_settings();
            cancelled = true;
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        // the face buttons are Switch buttons after the player's mapping, named as in the on-screen keyboard's help line
        const bool lab = kb_by_label.load(std::memory_order_relaxed);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Pad: %s quit, %s cancel, %s settings", lab ? "A" : "A (right)", lab ? "B" : "B (bottom)", lab ? "X" : "X (top)");
        ImGui::TextDisabled("Keyboard: Enter quit, Esc cancel");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    if (cancelled) clear_imgui_input(imgui);
}

void Overlay::open_quit_prompt() {
    if (!impl_->quit_open.exchange(true)) NXLOG_I(kTag, "quit prompt opened");
}
bool Overlay::quit_prompt_open() const { return impl_->quit_open; }
bool Overlay::take_quit_confirmed() { return impl_->quit_confirmed.exchange(false); }
bool Overlay::take_settings_wanted() { return impl_->settings_wanted.exchange(false); }

bool Overlay::keyboard_open() const { return impl_->kb_poll(); }
bool Overlay::blocks_game() const { return impl_->visible || impl_->quit_open || impl_->kb_poll() || cheats_blocks_game(); }
void Overlay::set_keyboard_abxy(bool by_label) { impl_->kb_by_label.store(by_label, std::memory_order_relaxed); }

uint64_t Overlay::set_keyboard_pad(uint64_t buttons, int lx, int ly) {
    Impl& s = *impl_;
    constexpr int kHalf = 16384;     // half of the hid stick range
    uint64_t b = buttons;
    if (lx <= -kHalf) b |= hid::btn(hid::kBtnStickLLeft);
    if (lx >= kHalf) b |= hid::btn(hid::kBtnStickLRight);
    if (ly >= kHalf) b |= hid::btn(hid::kBtnStickLUp);       // hid y: up = +
    if (ly <= -kHalf) b |= hid::btn(hid::kBtnStickLDown);
    const bool quit = s.quit_open;
    const bool page = !quit && s.visible && !s.kb_poll();   // the F1 page: the pad moves its focus, and stays away from the game
    // the cheats overlay: its hotkeys see the pad always; while it is open (and does not pass input through) the pad drives its windows
    if (s.cheats_enabled) cheats::ui::pad_input(b);
    const bool cheats_page = !quit && !page && !s.kb_poll() && s.cheats_visible && !s.cheats_pass;
    bool close_cheats = false;
    if (cheats_page) {
        // A activates and B backs out (after the player's mapping, as on the F1 page), X opens a row's actions (ImGui's context menu), - the
        // menu bar; Y, L, R, ZL, ZR and + are the overlay's own (cheats_ui.cpp: the keyboard, window switching, paging, close)
        struct Map { uint64_t bits; ImGuiKey key; };
        const Map keys[] = {{kKbDirBits[0], ImGuiKey_GamepadDpadUp}, {kKbDirBits[1], ImGuiKey_GamepadDpadDown}, {kKbDirBits[2], ImGuiKey_GamepadDpadLeft},
                            {kKbDirBits[3], ImGuiKey_GamepadDpadRight}, {hid::btn(hid::kBtnA), ImGuiKey_GamepadFaceDown},
                            {hid::btn(hid::kBtnB), ImGuiKey_GamepadFaceRight}, {hid::btn(hid::kBtnX), ImGuiKey_GamepadFaceUp},
                            {hid::btn(hid::kBtnMinus), ImGuiKey_GamepadFaceLeft}};
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();
        for (const Map& m : keys)
            if (((b ^ s.pad_prev) & m.bits) != 0) io.AddKeyEvent(m.key, (b & m.bits) != 0);
        ImGui::SetCurrentContext(prev);
        close_cheats = (b & ~s.pad_prev & hid::btn(hid::kBtnPlus)) != 0 && !cheats::ui::hotkey_capture_active() && !cheats::ui::keyboard_open();
    }
    const bool open = quit || page || s.kb_poll() || cheats_page;
    bool close_page = false;
    if (page) {
        // Switch A / B are after the player's mapping, as in the quit prompt. + closes the page.
        struct Map { uint64_t bits; ImGuiKey key; };
        const Map keys[] = {{kKbDirBits[0], ImGuiKey_GamepadDpadUp}, {kKbDirBits[1], ImGuiKey_GamepadDpadDown}, {kKbDirBits[2], ImGuiKey_GamepadDpadLeft},
                            {kKbDirBits[3], ImGuiKey_GamepadDpadRight}, {hid::btn(hid::kBtnA), ImGuiKey_GamepadFaceDown},
                            {hid::btn(hid::kBtnB), ImGuiKey_GamepadFaceRight}};
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();
        for (const Map& m : keys)
            if (((b ^ s.pad_prev) & m.bits) != 0) io.AddKeyEvent(m.key, (b & m.bits) != 0);
        ImGui::SetCurrentContext(prev);
        close_page = (b & ~s.pad_prev & hid::btn(hid::kBtnPlus)) != 0;
    }
    if (quit) {                      // the prompt takes the pad before the keyboard does: fresh presses of A and B answer it
        const uint64_t fresh = b & ~s.pad_prev;
        if (!s.quit_confirmed && (fresh & hid::btn(hid::kBtnA))) s.quit_answer(true);
        else if (!s.quit_confirmed && (fresh & hid::btn(hid::kBtnB))) s.quit_answer(false);
        else if (!s.quit_confirmed && (fresh & hid::btn(hid::kBtnX))) s.quit_to_settings();
        s.pad_mask = b;
    } else if (page || cheats_page) {
        s.pad_mask = b;
    } else if (open) {
        s.pad_new.fetch_or(b & ~s.pad_prev);
        s.pad_mask = b;
    } else {
        s.pad_mask &= b;             // a button leaves the mask once released
    }
    s.pad_held.store(b);
    s.pad_prev = b;
    if (close_page) set_visible(false);   // after the lock is released: set_visible takes it too
    if (close_cheats) set_cheats_visible(false);
    return open ? ~0ull : s.pad_mask;
}

bool Overlay::wants_input() const { return (impl_->visible || cheats_blocks_game()) && imgui_wants(impl_->mu, impl_->imgui, true, true); }
bool Overlay::wants_mouse() const { return (impl_->visible || cheats_blocks_game()) && imgui_wants(impl_->mu, impl_->imgui, true, false); }
bool Overlay::wants_keyboard() const { return (impl_->visible || cheats_blocks_game()) && imgui_wants(impl_->mu, impl_->imgui, false, true); }

void Overlay::add_panel(const std::string& title, std::function<void()> fn) { impl_->panels.push_back({title, std::move(fn)}); }

bool Overlay::handle_event(const input::Event& e) {
    Impl& s = *impl_;
    using input::EventType;
    if ((e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.u.key.scancode == s.toggle_sc) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle();
        return true;   // the toggle key never reaches the game (press, repeat and release)
    }
    if (s.cheats_enabled && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.u.key.scancode == s.cheats_sc) {
        // closed: a press opens it at once. Open: a press closes it when the key is let go, unless it was held long enough to peek (draw)
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) {
            if (!s.cheats_visible) toggle_cheats();
            else {
                s.cheats_key_down_ms = ms;
                s.cheats_key_held = true;
            }
        } else if (e.type == EventType::KeyUp && s.cheats_key_held.exchange(false)) {
            if (s.cheats_key_peeking.exchange(false)) cheats::ui::set_key_peek(false);
            else if (s.cheats_visible) toggle_cheats();
        }
        return true;   // the cheats key is the overlay's, never the game's
    }
    // the cheats table's and scripts' hotkeys see every key, also while the cheats overlay is hidden (they do not take it from the game);
    // while a hotkey is being bound, the key is the binding's only
    if (s.cheats_enabled && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && !(e.flags & input::kFlagRepeat)) {
        const bool capture = cheats::ui::hotkey_capture_active();
        cheats::ui::key_event(e.u.key.scancode, e.type == EventType::KeyDown);
        if (capture && s.cheats_visible) return true;
    }
    if (s.quit_open) {               // the quit prompt takes every key; the mouse goes to ImGui (its buttons), not to the game
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp || e.type == EventType::Text) return s.quit_key_event(e);
    }
    const bool kb = s.kb_poll() || s.quit_open;
    if (kb && !s.quit_open && (e.type == EventType::KeyDown || e.type == EventType::KeyUp || e.type == EventType::Text)) return s.kb_key_event(e);
    // a finger on the hidden menu's corner button (the finger's mouse events carry kFlagTouch)
    if (!s.visible && !kb && s.touch_btn.load(std::memory_order_relaxed) && (e.flags & input::kFlagTouch) &&
        (e.type == EventType::MouseMotion || e.type == EventType::MouseButtonDown || e.type == EventType::MouseButtonUp))
        return s.touch_button_event(e);
    // The cheats overlay takes input while it is open, unless "pass input through" is set.
    const bool cheats_take = s.cheats_visible && !s.cheats_pass;
    if (!s.visible && !kb && !cheats_take) return false;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    bool consumed = false;
    switch (e.type) {
        case EventType::KeyDown:
        case EventType::KeyUp: {
            const ImGuiKey k = map_key(e.u.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == EventType::KeyDown);
            consumed = io.WantCaptureKeyboard;
            break;
        }
        case EventType::Text:
            io.AddInputCharactersUTF8(e.u.text.utf8);
            consumed = io.WantTextInput;
            break;
        case EventType::MouseMotion:
            io.AddMousePosEvent(e.u.motion.x, e.u.motion.y);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp: {
            const int b = e.u.button.button;   // SDL: 1 left, 2 middle, 3 right
            const int ib = b == 1 ? 0 : b == 3 ? 1 : b == 2 ? 2 : b - 1;
            io.AddMousePosEvent(e.u.button.x, e.u.button.y);
            io.AddMouseButtonEvent(ib, e.type == EventType::MouseButtonDown);
            consumed = io.WantCaptureMouse;
            break;
        }
        case EventType::MouseWheel:
            io.AddMouseWheelEvent(e.u.wheel.dx, e.u.wheel.dy);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::WindowFocusLost: io.AddFocusEvent(false); break;
        case EventType::WindowFocusGained: io.AddFocusEvent(true); break;
        default: break;
    }
    ImGui::SetCurrentContext(prev);
    // the on-screen keyboard and the quit prompt take the whole mouse while they are up
    if (kb) consumed = e.type == EventType::MouseMotion || e.type == EventType::MouseButtonDown || e.type == EventType::MouseButtonUp || e.type == EventType::MouseWheel;
    return consumed;
}

void Overlay::apply_pending() {
    if (impl_->touch_toggle_req.exchange(false)) toggle();   // the corner button was tapped (outside the ImGui frame: set_visible takes the lock)
    apply_present_pending();
    apply_window_pending();
}

void Overlay::apply_present_pending() {
    Impl& s = *impl_;
    if (s.want_mode) {
        s.want_mode = false;
        s.ctx.renderer->set_present_mode(s.new_mode);
    }
}

void Overlay::apply_window_pending() {
    Impl& s = *impl_;
    if (s.want_fullscreen) {
        s.want_fullscreen = false;
        platform::set_fullscreen(s.new_fullscreen);
    }
}

void Overlay::Impl::refresh_counter_text(double now_s) {
    if (now_s - text_s < 0.25) return;    // the text is rebuilt at most every 250 ms
    text_s = now_s;
    snap = stats.snapshot(now_s);
    const int detail = counter_detail.load(std::memory_order_relaxed);
    char buf[64];
    counter_n = 0;
    if (snap.valid) std::snprintf(buf, sizeof buf, "%.0f FPS", snap.fps);
    else std::snprintf(buf, sizeof buf, "-- FPS");
    counter_lines[counter_n++] = buf;
    if (detail >= 1) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "%.1f ms", snap.avg_ms);
        else std::snprintf(buf, sizeof buf, "-- ms");
        counter_lines[counter_n++] = buf;
    }
    if (detail >= 2) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "1%% low %.0f", snap.low1_fps);
        else std::snprintf(buf, sizeof buf, "1%% low --");
        counter_lines[counter_n++] = buf;
        if (snap.valid) std::snprintf(buf, sizeof buf, "max %.1f ms", snap.max_ms);
        else std::snprintf(buf, sizeof buf, "max -- ms");
        counter_lines[counter_n++] = buf;
    }
}

// Draws the counter with the foreground draw list: no window, so it never takes mouse or keyboard input.
void Overlay::Impl::draw_counter(const FrameInfo& f, double now_s) {
    refresh_counter_text(now_s);
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 45.f), 14.f, 40.f);
    const float pad = size * 0.4f;
    const float gap = size * 0.15f;
    const float margin = size * 0.8f;
    const int n = counter_n;
    float lines_w[4] = {};
    float line_h = 0, text_w = 0;
    for (int i = 0; i < n; ++i) {
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, counter_lines[i].c_str());
        lines_w[i] = sz.x;
        line_h = std::max(line_h, sz.y);
        text_w = std::max(text_w, sz.x);
    }
    const float box_w = text_w + 2.f * pad;
    const float box_h = static_cast<float>(n) * line_h + static_cast<float>(n - 1) * gap + 2.f * pad;
    const int corner = counter_corner.load(std::memory_order_relaxed);
    const bool right = corner == 1 || corner == 3;
    const bool bottom = corner >= 2;
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = right ? W - margin - box_w : margin;
    const float y0 = bottom ? H - margin - box_h : margin;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + box_w, y0 + box_h), IM_COL32(0, 0, 0, 140), size * 0.3f);
    for (int i = 0; i < n; ++i) {
        const float x = right ? x0 + box_w - pad - lines_w[i] : x0 + pad;
        const float y = y0 + pad + static_cast<float>(i) * (line_h + gap);
        dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), counter_lines[i].c_str());
    }
}

bool Overlay::Impl::note_visible(double now_s) {
    if (!note_read) {
        note_read = true;
        note_enabled = nxrt::config_get_bool("nvn", "compile_note", true);
    }
    const int pending = shader_backlog();
    if (!note_enabled) return false;
    if (pending > 0) {
        if (note_since_s < 0) { note_since_s = now_s; note_peak = 0; }
        note_empty_s = -1.0;
        note_peak = std::max(note_peak, pending);
        return now_s - note_since_s >= 1.5;
    }
    if (note_since_s < 0) return false;
    if (note_empty_s < 0) note_empty_s = now_s;
    if (now_s - note_empty_s >= 1.0) { note_since_s = -1.0; return false; }
    return now_s - note_since_s >= 1.5;
}

void Overlay::Impl::draw_note(const FrameInfo& f) {
    char text[96];
    const int pending = shader_backlog();
    if (pending > 0) std::snprintf(text, sizeof text, "Preparing shaders (%d left)", pending);
    else std::snprintf(text, sizeof text, "Preparing shaders (done)");
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 60.f), 12.f, 28.f);
    const float pad = size * 0.4f;
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = (W - sz.x) * 0.5f - pad, y0 = H - size * 0.8f - sz.y - 2.f * pad;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 2.f * pad, y0 + sz.y + 2.f * pad), IM_COL32(0, 0, 0, 120), size * 0.3f);
    dl->AddText(font, size, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 220), text);
}

bool Overlay::Impl::save_notice_visible(double now_s) {
    const uint32_t n = services::fsapi::save_failures().count;
    if (n != save_seen) {
        save_seen = n;
        save_until_s = now_s + 10.0;
    }
    return now_s < save_until_s;
}

void Overlay::Impl::draw_save_notice(const FrameInfo& f) {
    char text[160];
    std::snprintf(text, sizeof text, "Save failed: %s. The last good save is kept.", services::fsapi::save_failure_reason(services::fsapi::save_failures().host_error));
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 45.f), 14.f, 36.f);
    const float pad = size * 0.5f;
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float W = static_cast<float>(f.extent.width);
    const float x0 = (W - sz.x) * 0.5f - pad, y0 = size;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 2.f * pad, y0 + sz.y + 2.f * pad), IM_COL32(120, 0, 0, 200), size * 0.3f);
    dl->AddText(font, size, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 255), text);
}

void Overlay::draw(const FrameInfo& f) {
    Impl& s = *impl_;
    const bool counter_on = s.counter_show.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const double now_s = std::chrono::duration<double>(now.time_since_epoch()).count();
    const bool note_on = s.note_visible(now_s) && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    const bool ptr_on = s.ptr_show.load(std::memory_order_relaxed) && (s.ptr_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    const bool quit_on = s.quit_open;
    const bool kb_on = !quit_on && s.kb_poll();
    const bool save_on = s.save_notice_visible(now_s);
    const bool touch_on = s.touch_btn.load(std::memory_order_relaxed) && !kb_on && !quit_on;   // the corner button (menu closed: "Menu", open: "Close")
    if (s.cheats_enabled && !s.cheats_inited) {          // the cheat table, its hotkeys and the frame callbacks start with the first frame
        s.cheats_inited = true;
        cheats::ui::init();
    }
    if (s.cheats_visible.load(std::memory_order_relaxed) && cheats::ui::take_close_request()) set_cheats_visible(false);   // its close button
    // the cheats key held for 0.3 s while the panel is open: peek at the game until it is let go (handle_event)
    if (s.cheats_key_held.load(std::memory_order_relaxed) && !s.cheats_key_peeking.load(std::memory_order_relaxed)) {
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (ms - s.cheats_key_down_ms.load(std::memory_order_relaxed) >= 300) {
            s.cheats_key_peeking = true;
            cheats::ui::set_key_peek(true);
        }
    }
    // the auto accent: the frame after the open request is read back without the cheats overlay on it; the next one has its colour
    bool cheats_sampling = false;
    if (s.cheats_cap_req.load(std::memory_order_relaxed) && !s.ctx.renderer->capture_pending()) {
        s.cheats_cap_req = false;
        s.cheats_cap.clear();
        s.ctx.renderer->capture_next_frame(&s.cheats_cap, &s.cheats_cap_w, &s.cheats_cap_h);
        s.cheats_cap_wait = true;
        cheats_sampling = true;
    } else if (s.cheats_cap_wait && !s.ctx.renderer->capture_pending()) {
        s.cheats_cap_wait = false;
        // the colour that stands out: pixels weighted by saturation and brightness (a grey frame stays grey)
        double r = 0, g = 0, b = 0, wsum = 0;
        const size_t n = s.cheats_cap.size() / 4;
        for (size_t i = 0; i < n; i += 61) {
            const uint8_t* p = s.cheats_cap.data() + i * 4;
            const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
            const double wgt = (mx - mn) / 255.0 * (mx / 255.0) + 0.02;
            r += p[0] * wgt; g += p[1] * wgt; b += p[2] * wgt; wsum += wgt;
        }
        if (wsum > 0) {
            const unsigned char c[4] = {static_cast<unsigned char>(r / wsum), static_cast<unsigned char>(g / wsum), static_cast<unsigned char>(b / wsum), 255};
            cheats::ui::set_frame_colour(c);
        }
        s.cheats_cap.clear();
        s.cheats_cap.shrink_to_fit();
    }
    const bool cheats_on = s.cheats_visible.load(std::memory_order_relaxed) && !kb_on && !quit_on && !cheats_sampling;
    // the HUD over the game (pinned values and cheats, or [cheats] mini's list): only when it has something to show
    const bool cheats_mini_on = s.cheats_enabled && cheats::ui::hud_wanted() && !cheats_on && !kb_on && !quit_on && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    if (!s.visible && !counter_on && !note_on && !save_on && !ptr_on && !kb_on && !quit_on && !touch_on && !cheats_on && !cheats_mini_on) {
        s.last_frame = now;
        s.counter_was_on = false;
        return;
    }
    if (counter_on) {
        if (!s.counter_was_on) {          // just switched on: start from an empty history
            s.counter_was_on = true;
            s.counter_have_prev = false;
            s.stats.reset();
            s.text_s = -1e9;
        }
        if (s.counter_have_prev) {
            const double dt_ms = std::chrono::duration<double, std::milli>(now - s.counter_prev).count();
            if (dt_ms <= 1000.0) s.stats.push(now_s, dt_ms);   // over a second: a pause (debugger, window in the background), not a frame
        }
        s.counter_prev = now;
        s.counter_have_prev = true;
    } else {
        s.counter_was_on = false;
    }
    // a screenshot leaves the counter out unless asked for; with the menu hidden there is then nothing to draw
    const bool show_counter = counter_on && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    if (!s.visible && !show_counter && !note_on && !save_on && !ptr_on && !kb_on && !quit_on && !touch_on && !cheats_on && !cheats_mini_on) {
        s.last_frame = now;
        return;
    }
    const bool hdr_frame = s.hdr && f.hdr_output;
    if (s.hdr && !f.hdr_output) return;      // the display left HDR mode: the RGBA8 overlay pipeline does not match the SDR swapchain, so no overlay until the next start
    if (hdr_frame && !s.composite.ensure_target(f.extent)) return;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - s.last_frame).count(), 1.0f / 1000.f, 0.25f);
    s.last_frame = now;
    io.DisplaySize = ImVec2(static_cast<float>(f.extent.width), static_cast<float>(f.extent.height));
    s.disp_w.store(io.DisplaySize.x, std::memory_order_relaxed);
    s.disp_h.store(io.DisplaySize.y, std::memory_order_relaxed);

    s.update_scale(f.extent);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    // the keyboard and the quit prompt are modal: the menu (and a keyboard under the prompt) comes back once they are answered
    if (s.visible && !kb_on && !quit_on) s.build_ui();
    bool glass_ok = false;
    if (cheats_on) {
        // glass: the frame under the panel blurred on the GPU (SDR frames the hook can read; an HDR frame draws the panel solid)
        if (!hdr_frame && cheats::ui::wants_glass() && s.ctx.renderer->frame_readable() && !s.blur_failed) {
            if (!s.blur.initialised()) s.blur_failed = !s.blur.init(*s.ctx.device);
            if (s.blur.initialised() && s.blur.ensure(f.extent)) {
                if (s.blur_tex_view != s.blur.view()) {       // new images (first use, a new frame size): ImGui's texture for them
                    if (s.blur_tex) ImGui_ImplVulkan_RemoveTexture(s.blur_tex);
                    s.blur_tex = ImGui_ImplVulkan_AddTexture(s.blur.sampler(), s.blur.view(), VK_IMAGE_LAYOUT_GENERAL);
                    s.blur_tex_view = s.blur.view();
                }
                glass_ok = s.blur_tex != VK_NULL_HANDLE;
            }
        }
        cheats::ui::set_backdrop(glass_ok ? ImTextureRef(reinterpret_cast<ImTextureID>(s.blur_tex)) : ImTextureRef());
        // the cheats overlay uses its own theme; save and restore the F1 menu's style around it so the two never clash
        ImGuiStyle saved = ImGui::GetStyle();
        cheats::ui::apply_theme(glass_ok);   // its look is its own ([cheats] theme and friends, read by cheats::ui::init)
        cheats::ui::draw(true, s.ctx.has_window);
        ImGui::GetStyle() = saved;
    }
    if (cheats_mini_on) cheats::ui::draw_mini();
    if (kb_on) s.draw_keyboard(now_s);
    if (quit_on) s.draw_quit_prompt();
    if (touch_on) s.draw_touch_button(s.visible);
    if (show_counter) s.draw_counter(f, now_s);
    if (note_on) s.draw_note(f);
    if (save_on) s.draw_save_notice(f);
    if (ptr_on) {   // a plain arrow, white with a dark outline, sized with the frame height
        const float k = std::max(1.f, static_cast<float>(f.extent.height) / 720.f);
        const float x = s.ptr_x.load(std::memory_order_relaxed), y = s.ptr_y.load(std::memory_order_relaxed);
        const ImVec2 pts[7] = {{0, 0}, {0, 16 * k}, {4 * k, 12.5f * k}, {7.5f * k, 19 * k}, {10 * k, 18 * k}, {6.5f * k, 11.5f * k}, {12 * k, 11.5f * k}};
        ImVec2 poly[7];
        for (int i = 0; i < 7; ++i) poly[i] = ImVec2(x + pts[i].x, y + pts[i].y);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddConvexPolyFilled(poly, 7, IM_COL32(255, 255, 255, 255));
        dl->AddPolyline(poly, 7, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, 1.5f * k);
    }
    ImGui::Render();
    // the glass panel's backdrop: the frame under it blurred before ImGui draws over the frame (only while it is open and glass)
    if (glass_ok) {
        int rect[4];
        float strength = 0.f;
        if (cheats::ui::backdrop_rect(rect, &strength)) s.blur.record(f.cmd, f.image, f.extent, rect, strength);
    }

    // one dynamic-rendering pass that draws the ImGui draw data into view
    auto render_into = [&](VkImageView view, VkAttachmentLoadOp load) {
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = load;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, f.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(f.cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
        vkCmdEndRendering(f.cmd);
    };
    if (hdr_frame) {
        // HDR: the overlay goes into the cleared RGBA8 target, then the composite maps it onto the swapchain image (UI white = paper white)
        const extras::HdrState hs = extras::hdr_state();
        s.composite.begin_overlay(f.cmd);
        render_into(s.composite.target_view(), VK_ATTACHMENT_LOAD_OP_CLEAR);
        s.composite.composite(f.cmd, f.view, f.format, f.extent, s.ctx.renderer->output_kind(), hs.paper_white_nits, hs.peak_nits);
    } else {
        render_into(f.view, VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    s.drawn++;
    ImGui::SetCurrentContext(prev);
}

void Overlay::install_on_renderer() {
    impl_->ctx.renderer->set_overlay_hook([this](const FrameInfo& f) { draw(f); });
}

}  // namespace nxrt::gfx
