// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Tweaks tab: optional looks ([tweaks] in controls.ini, play_tweaks.h), off by default. S44: the glow colour, three 0..255 sliders with a
// colour swatch and a Reset button. A part with its own colour in the file (ball_glow / gun_glow = R,G,B) keeps it; the page only switches a
// part between following the glow colour and the game's own. S45: the suit and arm cannon colour (suit_color, gun_color), each a check box,
// three sliders and a swatch like the glow, the three colours side by side, with their own Reset.
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>

#include "pages.h"

namespace mprl {

namespace {

using nxrt::play::GlowMode;
using nxrt::play::GlowRgb;
using nxrt::play::TweakSettings;

constexpr const wchar_t* kSwatchClass = L"MprlGlowSwatch";

// A plain rectangle in one colour (GWLP_USERDATA holds the COLORREF).
LRESULT CALLBACK swatch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        FrameRect(dc, &rc, GetSysColorBrush(COLOR_WINDOWFRAME));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_swatch(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &swatch_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kSwatchClass;
    done = RegisterClassExW(&wc) != 0;
}

std::wstring byte_text(int v) { return std::to_wstring(v); }

// The part's drop-down: 0 follows the glow colour, 1 the game's own; a part with its own colour (only in the file) shows as 0 and is kept.
int part_index(GlowMode m) { return m == GlowMode::Off ? 1 : 0; }
void set_part(nxrt::play::GlowPart* p, int i) {
    if (i == 1) p->mode = GlowMode::Off;
    else if (p->mode == GlowMode::Off) p->mode = GlowMode::Auto;
}

class TweaksPage : public Page {
public:
    const wchar_t* title() const override { return L"Tweaks"; }

    void build(Form& f) override {
        register_swatch(host()->env().inst);
        // the page does not scroll: the three colours side by side, the glow (S44), the suit's paint and the arm cannon's paint (S45)
        f.heading(L"Glow and paint colours");
        f.compact(true);
        f.columns(3);
        f.checkbox(
            L"Glow colour", [](const Settings& s) { return s.tweaks.glow_on; }, [](Settings& s, bool v) { s.tweaks.glow_on = v; },
            [this]() { update_swatches(); });
        f.checkbox(
            L"Suit colour", [](const Settings& s) { return s.tweaks.suit_color_on; }, [](Settings& s, bool v) { s.tweaks.suit_color_on = v; },
            [this]() { update_swatches(); });
        f.checkbox(
            L"Arm cannon colour", [](const Settings& s) { return s.tweaks.gun_color_on; }, [](Settings& s, bool v) { s.tweaks.gun_color_on = v; },
            [this]() { update_swatches(); });
        rgb_rows(f, {&glow_, &suit_, &gun_}, {&TweakSettings::glow, &TweakSettings::suit_color, &TweakSettings::gun_color});
        reset_id_ = f.button(L"Reset the glow", 150, true);
        reset_paint_id_ = f.button(L"Reset the paint", 150, true);
        f.compact(false);
        f.columns(1);
        f.hint(L"Glow: the morph ball's glow (stripes, inner glow, boost glow, trail, light), the arm cannon's charge glow and lit panels, and the suit's lights; the colour multiplies the game's glow. Paint: the suit (the morph ball follows it) and the arm cannon's shell, black to white, keeping the game's shading and detail; the visor and the lights keep their colours. Off: the game's own.", 4, true);

        f.heading(L"Glow per part");
        f.columns(3);
        f.combo(
            L"Suit", {L"Glow colour", L"The game's own"}, [](const Settings& s) { return part_index(s.tweaks.suit.mode); },
            [](Settings& s, int i) { set_part(&s.tweaks.suit, i); });
        f.combo(
            L"Morph ball", {L"Glow colour", L"The game's own"}, [](const Settings& s) { return part_index(s.tweaks.ball.mode); },
            [](Settings& s, int i) { set_part(&s.tweaks.ball, i); });
        f.combo(
            L"Arm cannon", {L"Glow colour", L"The game's own"}, [](const Settings& s) { return part_index(s.tweaks.gun.mode); },
            [](Settings& s, int i) { set_part(&s.tweaks.gun, i); });
        f.columns(1);
        f.hint(L"A part can also get its own glow colour in controls.ini: [tweaks] suit_glow, ball_glow or gun_glow = R,G,B.", 1, true);
    }

    void load(const Settings& s) override {
        loaded_ = s;
        Page::load(s);
        update_swatches();
    }

    bool command(int id, int code, HWND) override {
        if ((id != reset_id_ && id != reset_paint_id_) || code != BN_CLICKED) return false;
        Settings s = loaded_;
        store(&s);
        const TweakSettings def;
        if (id == reset_id_) {   // the glow only
            s.tweaks.glow_on = def.glow_on;
            s.tweaks.glow = def.glow;
            s.tweaks.suit = def.suit;
            s.tweaks.ball = def.ball;
            s.tweaks.gun = def.gun;
        } else {                 // the paint only
            s.tweaks.suit_color_on = def.suit_color_on;
            s.tweaks.suit_color = def.suit_color;
            s.tweaks.gun_color_on = def.gun_color_on;
            s.tweaks.gun_color = def.gun_color;
        }
        Page::load(s);
        update_swatches();
        host()->changed();
        return true;
    }

    void rebuilt() override { update_swatches(); }

private:
    struct RgbRows {
        HWND r = nullptr, g = nullptr, b = nullptr, swatch = nullptr;
    };
    using RgbField = GlowRgb TweakSettings::*;

    static Form::GetInt channel_get(RgbField field, int ch) {
        return [field, ch](const Settings& s) {
            const GlowRgb& c = s.tweaks.*field;
            return int(ch == 0 ? c.r : ch == 1 ? c.g : c.b);
        };
    }
    static Form::SetInt channel_set(RgbField field, int ch) {
        return [field, ch](Settings& s, int v) {
            GlowRgb& c = s.tweaks.*field;
            (ch == 0 ? c.r : ch == 1 ? c.g : c.b) = uint8_t(v);
        };
    }

    // Red, Green and Blue sliders (0..255) and a swatch for each colour, one colour per column of the form (rows fill left to right).
    void rgb_rows(Form& f, std::initializer_list<RgbRows*> rows, std::initializer_list<RgbField> fields) {
        const wchar_t* names[3] = {L"Red", L"Green", L"Blue"};
        for (int ch = 0; ch < 3; ++ch) {
            auto field = fields.begin();
            for (RgbRows* r : rows) {
                HWND h = f.slider(names[ch], 0, 255, channel_get(*field, ch), channel_set(*field, ch), byte_text, [this]() { update_swatches(); });
                (ch == 0 ? r->r : ch == 1 ? r->g : r->b) = h;
                ++field;
            }
        }
        for (RgbRows* r : rows) {
            const RECT sw = f.custom_row(L"Colour", 28);
            const int swatch_w = std::min<int>(sw.right - sw.left, host()->env().S(160));
            r->swatch = f.create(kSwatchClass, L"", WS_CHILD | WS_VISIBLE, sw.left, sw.top, swatch_w, sw.bottom - sw.top);
        }
    }

    static void update_swatch(const RgbRows& rows) {
        if (!rows.swatch) return;
        auto pos = [](HWND h) { return h ? static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0)) : 255; };
        const COLORREF c = RGB(pos(rows.r), pos(rows.g), pos(rows.b));
        SetWindowLongPtrW(rows.swatch, GWLP_USERDATA, static_cast<LONG_PTR>(c));
        InvalidateRect(rows.swatch, nullptr, FALSE);
    }
    void update_swatches() {
        update_swatch(glow_);
        update_swatch(suit_);
        update_swatch(gun_);
    }

    RgbRows glow_, suit_, gun_;
    int reset_id_ = -1, reset_paint_id_ = -1;
    Settings loaded_;
};

}  // namespace

std::unique_ptr<Page> make_tweaks_page() { return std::make_unique<TweaksPage>(); }

}  // namespace mprl
