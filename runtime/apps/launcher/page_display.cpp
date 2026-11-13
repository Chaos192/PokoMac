// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Display tab: window mode and size, frame rate limit, VSync, field of view, HDR and low latency. Extras tab: HDR, low latency and the frame rate counter.
// This page is the pattern for the others: build() describes the rows, the Form moves values between the controls and Settings.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pages.h"

namespace mprl {

namespace {

using nxrt::play::Corner;
using nxrt::play::InKind;
using nxrt::play::InputId;
namespace extras = nxrt::extras;

struct Size {
    int w, h;
};
// Window sizes in the drop-down. The last entry of the list is "Custom".
const Size kSizes[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
constexpr int kSizeCount = static_cast<int>(sizeof kSizes / sizeof kSizes[0]);

const int kCaps[] = {0, 30, 60, 72, 90, 120, 144, 165, 240, 360};
constexpr int kCapCount = static_cast<int>(sizeof kCaps / sizeof kCaps[0]);

const wchar_t* const kModeNames[] = {L"Windowed", L"Borderless fullscreen", L"Exclusive fullscreen"};
const char* const kModeKeys[] = {"windowed", "borderless", "exclusive"};

int size_index(int w, int h) {
    for (int i = 0; i < kSizeCount; ++i)
        if (kSizes[i].w == w && kSizes[i].h == h) return i;
    return kSizeCount;   // Custom
}

int cap_index(int fps) {
    for (int i = 0; i < kCapCount; ++i)
        if (kCaps[i] == fps) return i;
    return kCapCount;    // a value from the file that is not in the list
}

std::wstring fov_text(int v) {
    if (v == 0) return L"Game default";
    return (v > 0 ? L"+" : L"-") + std::to_wstring(std::abs(v)) + L"°";
}

class DisplayPage : public Page {
public:
    const wchar_t* title() const override { return L"Display"; }

    void build(Form& f) override {
        f.heading(L"Window");
        f.combo(
            L"Window mode", {kModeNames[0], kModeNames[1], kModeNames[2]},
            [](const Settings& s) {
                for (int i = 0; i < 3; ++i)
                    if (s.display.window_mode == kModeKeys[i]) return i;
                return 0;
            },
            [](Settings& s, int i) { s.display.window_mode = kModeKeys[std::clamp(i, 0, 2)]; });

        std::vector<std::wstring> sizes;
        for (const Size& z : kSizes) sizes.push_back(std::to_wstring(z.w) + L" x " + std::to_wstring(z.h));
        sizes.push_back(L"Custom");
        size_combo_ = f.combo(
            L"Window size", sizes, [](const Settings& s) { return size_index(s.display.width, s.display.height); },
            [](Settings&, int) {},   // the width and height boxes below carry the value
            [this]() { size_chosen(); });
        {
            const RECT r = f.custom_row(L"Custom size", 24);
            const Env& env = host()->env();
            const int bw = env.S(80), lw = env.S(24);
            width_edit_ = f.create(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, r.left, r.top, bw, env.S(24), &width_id_, WS_EX_CLIENTEDGE);
            f.create(L"STATIC", L"x", SS_CENTER | SS_NOPREFIX, r.left + bw, r.top + env.S(4), lw, env.S(18));
            height_edit_ = f.create(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, r.left + bw + lw, r.top, bw, env.S(24), &height_id_, WS_EX_CLIENTEDGE);
            SendMessageW(width_edit_, EM_LIMITTEXT, 5, 0);
            SendMessageW(height_edit_, EM_LIMITTEXT, 5, 0);
            f.create(L"STATIC", L"pixels", SS_LEFT | SS_NOPREFIX, r.left + 2 * bw + lw + env.S(8), r.top + env.S(4), env.S(70), env.S(18));
        }
        f.hint(L"The size of the game window. Fullscreen modes use your desktop resolution.");

        f.heading(L"Frame rate");
        std::vector<std::wstring> caps;
        for (int c : kCaps) caps.push_back(c == 0 ? L"Unlimited" : std::to_wstring(c) + L" fps");
        caps.push_back(L"Custom (from controls.ini)");
        cap_combo_ = f.combo(
            L"Frame rate limit", caps, [](const Settings& s) { return cap_index(s.display.fps_cap); },
            [this](Settings& s, int i) { s.display.fps_cap = i < kCapCount ? kCaps[i] : custom_cap_; });
        f.checkbox(L"VSync", [](const Settings& s) { return s.display.vsync; }, [](Settings& s, bool v) { s.display.vsync = v; });
        f.hint(L"With VSync on, the display's refresh rate caps the frame rate. With G-Sync or FreeSync, leave it off and set a limit.", 2);
        f.checkbox(L"Faster GPU memory (uses more video memory)", [](const Settings& s) { return s.display.fast_gpu_memory; },
                   [](Settings& s, bool v) { s.display.fast_gpu_memory = v; });
        f.checkbox(L"Fewer GPU pipeline waits", [](const Settings& s) { return s.display.fewer_gpu_waits; },
                   [](Settings& s, bool v) { s.display.fewer_gpu_waits = v; });
        f.hint(L"Both raise the frame rate in the larger areas. Faster GPU memory keeps copies of the game's vertex data in video memory (up to 2 GiB; not on cards with less than 4 GiB). Both apply at the next start.", 2);

        f.heading(L"Picture");
        f.slider(
            L"Field of view", -30, 40, [](const Settings& s) { return static_cast<int>(std::lround(s.display.fov_offset)); },
            [](Settings& s, int v) { s.display.fov_offset = v; }, fov_text);
    }

    void load(const Settings& s) override {
        custom_cap_ = s.display.fps_cap;
        Page::load(s);
        loading_boxes_ = true;
        SetWindowTextW(width_edit_, std::to_wstring(s.display.width).c_str());
        SetWindowTextW(height_edit_, std::to_wstring(s.display.height).c_str());
        loading_boxes_ = false;
        if (cap_index(s.display.fps_cap) == kCapCount) {
            const std::wstring t = std::to_wstring(s.display.fps_cap) + L" fps (from controls.ini)";
            SendMessageW(cap_combo_, CB_DELETESTRING, kCapCount, 0);
            SendMessageW(cap_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
            SendMessageW(cap_combo_, CB_SETCURSEL, kCapCount, 0);
        }
    }

private:
    void store(Settings* s) override {
        Page::store(s);
        s->display.width = read_box(width_edit_, 320, 16384);
        s->display.height = read_box(height_edit_, 320, 16384);
    }

    bool command(int id, int code, HWND) override {
        if ((id == width_id_ || id == height_id_) && code == EN_CHANGE) {
            if (!loading_boxes_) {
                size_typed();
                host()->changed();
            }
            return true;
        }
        return false;
    }

private:
    static int read_box(HWND h, int lo, int hi) {
        wchar_t t[16] = {};
        GetWindowTextW(h, t, 15);
        return std::clamp(_wtoi(t), lo, hi);
    }

    // The preset list was used: copy the size into the boxes (Custom leaves them alone).
    void size_chosen() {
        const int i = static_cast<int>(SendMessageW(size_combo_, CB_GETCURSEL, 0, 0));
        if (i < 0 || i >= kSizeCount) return;
        loading_boxes_ = true;
        SetWindowTextW(width_edit_, std::to_wstring(kSizes[i].w).c_str());
        SetWindowTextW(height_edit_, std::to_wstring(kSizes[i].h).c_str());
        loading_boxes_ = false;
    }
    // The boxes were typed in: show the preset that matches, or Custom.
    void size_typed() {
        wchar_t w[16] = {}, h[16] = {};
        GetWindowTextW(width_edit_, w, 15);
        GetWindowTextW(height_edit_, h, 15);
        SendMessageW(size_combo_, CB_SETCURSEL, size_index(_wtoi(w), _wtoi(h)), 0);
    }

    HWND size_combo_ = nullptr, width_edit_ = nullptr, height_edit_ = nullptr, cap_combo_ = nullptr;
    int width_id_ = -1, height_id_ = -1;
    bool loading_boxes_ = false;
    int custom_cap_ = 60;
};

// Extras tab: HDR, low latency and the frame rate counter.
class ExtrasPage : public Page {
public:
    const wchar_t* title() const override { return L"Extras"; }

    void build(Form& f) override {
        f.heading(L"Output");
        f.combo(
            L"HDR", {L"Off", L"Auto (when the display offers it)", L"On", L"Native (the game's own HDR TV levels)"},
            [](const Settings& s) { return static_cast<int>(s.extras.hdr.mode); },
            [](Settings& s, int i) { s.extras.hdr.mode = static_cast<extras::HdrMode>(std::clamp(i, 0, 3)); });
        f.combo(
            L"Low latency", {L"Off", L"Auto (NVIDIA Reflex or AMD Anti-Lag 2)", L"On", L"Boost (Reflex)"},
            [](const Settings& s) { return static_cast<int>(s.extras.latency.mode); },
            [](Settings& s, int i) { s.extras.latency.mode = static_cast<extras::LatencyMode>(std::clamp(i, 0, 3)); });
        f.heading(L"Frame rate counter");
        f.hint(L"A small box in a corner of the game with the frame rate and frame time. Top left keeps clear of the game's own text; the bottom corners sit over the menu prompts and the title screen's copyright line. F1 in the game opens the same options.", 1, true);
        f.checkbox(L"Show the counter when the game starts", [](const Settings& s) { return s.fps.show; }, [](Settings& s, bool v) { s.fps.show = v; });
        f.combo(
            L"Corner", {L"Top left", L"Top right", L"Bottom left", L"Bottom right"}, [](const Settings& s) { return static_cast<int>(s.fps.corner); },
            [](Settings& s, int i) { s.fps.corner = static_cast<Corner>(std::clamp(i, 0, 3)); });
        f.combo(
            L"Detail", {L"Frame rate", L"Frame rate and frame time", L"Also the 1% low and the worst frame"},
            [](const Settings& s) { return s.fps.detail; }, [](Settings& s, int i) { s.fps.detail = std::clamp(i, 0, 2); });
        key_text_ = f.text_row(L"Hotkey in the game", L"");
        key_button_ = f.button(L"Change...", 110);
        f.heading(L"Shaders");
        prep_hint_ = f.hint(L"Compiles the shaders now so the first hours of play do not stutter. Takes a few minutes.", 1, true);
        const RECT r = f.custom_row(nullptr, 26);
        prep_button_ = f.create(L"BUTTON", L"Prepare shaders", BS_PUSHBUTTON | WS_TABSTOP, r.left, r.top, host()->env().S(150),
                                host()->env().S(26), &prep_id_);
        add_fastboot_rows(f);
    }

    void load(const Settings& s) override {
        fps_key_ = s.fps.key;
        Page::load(s);
        SetWindowTextW(key_text_, input_label(fps_key_).c_str());
    }

    void state_changed() override {
        if (!prep_button_) return;
        const bool offered = host()->shader_prepare_offered();
        ShowWindow(prep_hint_, offered ? SW_SHOW : SW_HIDE);
        ShowWindow(prep_button_, offered ? SW_SHOW : SW_HIDE);
        EnableWindow(prep_button_, !host()->shader_prepare_busy());
    }

    void store(Settings* s) override {
        Page::store(s);
        s->fps.key = fps_key_;
    }

    bool command(int id, int code, HWND) override {
        if (id == prep_id_ && code == BN_CLICKED) {
            host()->prepare_shaders_now();
            return true;
        }
        if (id != key_button_ || code != BN_CLICKED) return false;
        InputId in;
        if (!capture_input(host()->window(), host()->env(), L"the frame rate counter", &in)) return true;
        if (in.kind != InKind::Key) {
            message(host()->window(), L"The counter hotkey has to be a keyboard key.", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        fps_key_ = in;
        SetWindowTextW(key_text_, input_label(fps_key_).c_str());
        host()->changed();
        return true;
    }

private:
    HWND key_text_ = nullptr;
    int key_button_ = -1;
    InputId fps_key_;
    HWND prep_hint_ = nullptr;
    HWND prep_button_ = nullptr;
    int prep_id_ = -1;
};

}  // namespace

std::unique_ptr<Page> make_display_page() { return std::make_unique<DisplayPage>(); }
std::unique_ptr<Page> make_extras_page() { return std::make_unique<ExtrasPage>(); }

}  // namespace mprl
