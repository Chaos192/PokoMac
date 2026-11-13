// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Mouse pointer in menus: the arithmetic and the pointer state (no game, no host state). See menu_mouse.h.
#include "menu_mouse.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play::mm {

Rect picture_rect(const Geometry& g) {
    Rect r;
    const double dw = std::max(1, g.win_w), dh = std::max(1, g.win_h);
    r.w = dw;
    r.h = dh;
    if (g.keep_aspect && g.render_w > 0 && g.render_h > 0) {
        const double a = static_cast<double>(g.render_w) / g.render_h, b = dw / dh;
        if (a > b) {
            r.w = dw;
            r.h = dw / a;
        } else {
            r.h = dh;
            r.w = dh * a;
        }
        r.x = (dw - r.w) * 0.5;
        r.y = (dh - r.h) * 0.5;
    }
    return r;
}

void window_to_picture(const Geometry& g, float px, float py, float* u, float* v, bool* inside) {
    const Rect r = picture_rect(g);
    const double uu = (px - r.x) / r.w, vv = (py - r.y) / r.h;
    if (u) *u = static_cast<float>(uu);
    if (v) *v = static_cast<float>(vv);
    if (inside) *inside = uu >= 0 && uu < 1 && vv >= 0 && vv < 1;
}

void window_to_ui(const Geometry& g, float px, float py, float* nx, float* ny, bool* inside) {
    const Rect r = picture_rect(g);
    const double u = (px - r.x) / r.w, v = (py - r.y) / r.h;        // 0..1 across the picture, v down
    double aspect = g.display_aspect > 0 ? g.display_aspect : (g.render_h > 0 ? static_cast<double>(g.render_w) / g.render_h : kUiAspect);
    if (aspect < 0.1) aspect = kUiAspect;
    // The UI camera is authored for 16:9 and kept at that shape in the middle of a wider (or narrower) picture: its width is kUiAspect / aspect of the
    // picture's width.
    // Narrower than 16:9 the camera keeps its full width and shows more above and below (display options, hud_ortho), so the picture's
    // width is the camera's 16 units and its height is 16 / aspect units.
    const bool narrow = aspect < kUiAspect - 0.02;
    const double x = narrow ? (u - 0.5) * 2.0 : (u - 0.5) * 2.0 * (aspect / kUiAspect);
    const double y = narrow ? (0.5 - v) * 2.0 * (kUiAspect / aspect) : (0.5 - v) * 2.0;
    if (nx) *nx = static_cast<float>(x);
    if (ny) *ny = static_cast<float>(y);
    if (inside) *inside = u >= 0 && u < 1 && v >= 0 && v < 1 && std::fabs(x) <= 1.0 && std::fabs(y) <= 1.0;
}

void transform_point(const float m[12], float x, float y, float z, float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m[4 * i + 0] * x + m[4 * i + 1] * y + m[4 * i + 2] * z + m[4 * i + 3];
}

void Pointer::on_motion(float x, float y) {
    if (have_pos_ && x == x_ && y == y_) return;
    have_pos_ = true;
    x_ = x;
    y_ = y;
    moved_ = true;
    cursor_shown_ = true;
}

void Pointer::on_button(int button, bool down) {
    if (button == 2) { mid_held_ = down; return; }
    if (button != 1) return;
    held_ = down;
    if (down) {
        pressed_ = true;
        click_age_ = 0;
        click_used_ = false;
        cursor_shown_ = true;
    }
}

void Pointer::on_other_input() {
    cursor_shown_ = false;
}

void Pointer::on_focus_lost() {
    pressed_ = false;
    held_ = false;
    mid_held_ = false;
    moved_ = false;
}

Snapshot Pointer::begin_frame(bool active, const Geometry& g) {
    Snapshot s;
    s.active = active && enabled_;
    if (!s.active) {
        moved_ = false;
        pressed_ = false;
        return s;
    }
    s.hover = moved_;
    moved_ = false;
    // a click stays pending for up to three frames (a frame without a game tick, a menu that is not up yet) and ends when a widget took it
    if (pressed_ && (click_used_ || click_age_ >= 3)) {
        s.click_unclaimed = !click_used_;
        pressed_ = false;
        click_age_ = 0;
        click_used_ = false;
    }
    if (pressed_) {
        s.click = true;
        ++click_age_;
    }
    s.shown = cursor_shown_ && have_pos_;
    s.held = held_;
    window_to_ui(g, x_, y_, &s.nx, &s.ny, &s.inside);
    {
        float u = 0, v = 0;
        window_to_picture(g, x_, y_, &u, &v, &s.in_picture);
        s.gx = 2.f * u - 1.f;
        s.gy = 1.f - 2.f * v;
    }
    return s;
}

Pointer& pointer() {
    static Pointer p;
    return p;
}

}  // namespace nxrt::play::mm
