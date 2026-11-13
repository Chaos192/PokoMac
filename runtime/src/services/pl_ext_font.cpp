// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The Nintendo-extension shared font stand-in (pl_ext_font.h): icons drawn from circles, rounded rectangles and straight strokes, written as
// a TrueType font in memory. The outlines are this file's own; only the code points follow the console's font (see the header).
// TrueType layout: Apple TrueType Reference Manual / OpenType 1.9 (tables head, hhea, maxp 1.0, OS/2 4, name 0, cmap 4, loca long, glyf
// simple glyphs with on-curve points only, hmtx, post 3). Outer contours run clockwise, holes counter-clockwise (non-zero fill).
//
// Every face (Switch, Xbox, PlayStation, Steam, keyboard) gives the same file layout: the same glyph count and cmap, every table the same
// size (glyf padded to kGlyfCapacity, the totals in head / hhea / maxp fixed), the same total size (kExtensionFontSize). So a new face can
// be written over the font a game has loaded (shared_font_set_face): only glyph outlines, offsets and advances change.
#include "pl_ext_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

namespace nxrt::services {
namespace {

constexpr int kEm = 1000;
constexpr int kAscent = 928, kDescent = -236;   // DejaVu Sans' proportions (the standard font the runtime serves)
constexpr double kCy = 330;                     // icon centre: about the middle of a capital letter
constexpr double kR = 420;                      // icon radius
constexpr double kPi = 3.14159265358979323846;
// Fixed totals: every face's font has these (they bound every glyph the faces draw; encode() checks).
constexpr int kMaxPoints = 4000, kMaxContours = 400, kMaxAdvance = 3000;
constexpr int kBoxXMin = 0, kBoxYMin = -260, kBoxXMax = 3000, kBoxYMax = 960;
constexpr size_t kGlyfCapacity = 160 * 1024;

struct Pt {
    double x, y;
};
using Contour = std::vector<Pt>;
using Shape = std::vector<Contour>;

double area(const Contour& c) {
    double a = 0;
    for (size_t i = 0; i < c.size(); i++) {
        const Pt& p = c[i];
        const Pt& q = c[(i + 1) % c.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

// Filled area: clockwise (negative area with y up). A hole: counter-clockwise.
void fill(Shape& s, Contour c) {
    if (area(c) > 0) std::reverse(c.begin(), c.end());
    s.push_back(std::move(c));
}
void hole(Shape& s, Contour c) {
    if (area(c) < 0) std::reverse(c.begin(), c.end());
    s.push_back(std::move(c));
}

Contour circle(double cx, double cy, double r, int n = 48) {
    Contour c;
    for (int i = 0; i < n; i++) c.push_back({cx + r * std::cos(2 * kPi * i / n), cy + r * std::sin(2 * kPi * i / n)});
    return c;
}

Contour rrect(double x0, double y0, double x1, double y1, double rad, int per = 8) {
    rad = std::min({rad, (x1 - x0) / 2, (y1 - y0) / 2});
    const std::array<Pt, 4> centres = {{{x1 - rad, y1 - rad}, {x0 + rad, y1 - rad}, {x0 + rad, y0 + rad}, {x1 - rad, y0 + rad}}};
    Contour c;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= per; i++) {
            const double a = kPi / 2 * (k + double(i) / per);
            c.push_back({centres[k].x + rad * std::cos(a), centres[k].y + rad * std::sin(a)});
        }
    return c;
}

void ring(Shape& s, double cx, double cy, double r, double th) {
    fill(s, circle(cx, cy, r));
    hole(s, circle(cx, cy, r - th));
}

// A straight stroke of width w from p to q; square ends (extended by w / 2) so that strokes meeting at a corner close it.
void stroke(Shape& s, Pt p, Pt q, double w) {
    const double dx = q.x - p.x, dy = q.y - p.y, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) return;
    const double ux = dx / len * w / 2, uy = dy / len * w / 2;   // half a width along the stroke
    const double nx = -uy, ny = ux;                               // and across it
    fill(s, {{p.x - ux + nx, p.y - uy + ny}, {q.x + ux + nx, q.y + uy + ny}, {q.x + ux - nx, q.y + uy - ny}, {p.x - ux - nx, p.y - uy - ny}});
}

// ---- a stroke alphabet: capitals, digits and a few signs on a unit box (u right, v up) ------------------------------------------------
using Strokes = std::vector<std::array<double, 4>>;
const Strokes& glyph_strokes(char ch) {
    static const Strokes O = {{0.25, 0, 0.75, 0}, {0.75, 0, 0.94, 0.2}, {0.94, 0.2, 0.94, 0.8}, {0.94, 0.8, 0.75, 1},
                              {0.75, 1, 0.25, 1}, {0.25, 1, 0.06, 0.8}, {0.06, 0.8, 0.06, 0.2}, {0.06, 0.2, 0.25, 0}};
    static const Strokes C = {{0.92, 0.84, 0.75, 1}, {0.75, 1, 0.25, 1}, {0.25, 1, 0.06, 0.8}, {0.06, 0.8, 0.06, 0.2},
                              {0.06, 0.2, 0.25, 0}, {0.25, 0, 0.75, 0}, {0.75, 0, 0.92, 0.16}};
    static const std::array<Strokes, 128> table = [] {
        std::array<Strokes, 128> t{};
        t['A'] = {{0.04, 0, 0.5, 1}, {0.96, 0, 0.5, 1}, {0.24, 0.36, 0.76, 0.36}};
        t['B'] = {{0.1, 0, 0.1, 1}, {0.1, 1, 0.7, 1}, {0.7, 1, 0.9, 0.84}, {0.9, 0.84, 0.9, 0.66}, {0.9, 0.66, 0.72, 0.5},
                  {0.1, 0.5, 0.72, 0.5}, {0.72, 0.5, 0.94, 0.34}, {0.94, 0.34, 0.94, 0.16}, {0.94, 0.16, 0.74, 0}, {0.1, 0, 0.74, 0}};
        t['C'] = C;
        t['D'] = {{0.1, 0, 0.1, 1}, {0.1, 1, 0.6, 1}, {0.6, 1, 0.92, 0.75}, {0.92, 0.75, 0.92, 0.25}, {0.92, 0.25, 0.6, 0}, {0.6, 0, 0.1, 0}};
        t['E'] = {{0.12, 0, 0.12, 1}, {0.12, 1, 0.9, 1}, {0.12, 0.5, 0.75, 0.5}, {0.12, 0, 0.9, 0}};
        t['F'] = {{0.12, 0, 0.12, 1}, {0.12, 1, 0.9, 1}, {0.12, 0.5, 0.75, 0.5}};
        t['G'] = C;
        t['G'].push_back({0.92, 0.16, 0.92, 0.45});
        t['G'].push_back({0.92, 0.45, 0.55, 0.45});
        t['H'] = {{0.1, 0, 0.1, 1}, {0.9, 0, 0.9, 1}, {0.1, 0.5, 0.9, 0.5}};
        t['I'] = {{0.5, 0, 0.5, 1}, {0.22, 1, 0.78, 1}, {0.22, 0, 0.78, 0}};
        t['J'] = {{0.85, 1, 0.85, 0.2}, {0.85, 0.2, 0.65, 0}, {0.65, 0, 0.3, 0}, {0.3, 0, 0.1, 0.2}};
        t['K'] = {{0.12, 0, 0.12, 1}, {0.9, 1, 0.12, 0.4}, {0.4, 0.62, 0.92, 0}};
        t['L'] = {{0.14, 1, 0.14, 0}, {0.14, 0, 0.9, 0}};
        t['M'] = {{0.06, 0, 0.06, 1}, {0.06, 1, 0.5, 0.4}, {0.5, 0.4, 0.94, 1}, {0.94, 1, 0.94, 0}};
        t['N'] = {{0.1, 0, 0.1, 1}, {0.1, 1, 0.9, 0}, {0.9, 0, 0.9, 1}};
        t['O'] = O;
        t['P'] = {{0.1, 0, 0.1, 1}, {0.1, 1, 0.7, 1}, {0.7, 1, 0.9, 0.84}, {0.9, 0.84, 0.9, 0.64}, {0.9, 0.64, 0.7, 0.48}, {0.7, 0.48, 0.1, 0.48}};
        t['Q'] = O;
        t['Q'].push_back({0.6, 0.3, 0.98, -0.04});
        t['R'] = {{0.1, 0, 0.1, 1}, {0.1, 1, 0.7, 1}, {0.7, 1, 0.9, 0.84}, {0.9, 0.84, 0.9, 0.64}, {0.9, 0.64, 0.7, 0.48},
                  {0.1, 0.48, 0.7, 0.48}, {0.5, 0.48, 0.94, 0}};
        t['S'] = {{0.92, 1, 0.28, 1}, {0.28, 1, 0.06, 0.84}, {0.06, 0.84, 0.06, 0.66}, {0.06, 0.66, 0.28, 0.5}, {0.28, 0.5, 0.72, 0.5},
                  {0.72, 0.5, 0.94, 0.34}, {0.94, 0.34, 0.94, 0.16}, {0.94, 0.16, 0.72, 0}, {0.72, 0, 0.08, 0}};
        t['T'] = {{0.02, 1, 0.98, 1}, {0.5, 1, 0.5, 0}};
        t['U'] = {{0.1, 1, 0.1, 0.2}, {0.1, 0.2, 0.3, 0}, {0.3, 0, 0.7, 0}, {0.7, 0, 0.9, 0.2}, {0.9, 0.2, 0.9, 1}};
        t['V'] = {{0.02, 1, 0.5, 0}, {0.5, 0, 0.98, 1}};
        t['W'] = {{0.02, 1, 0.25, 0}, {0.25, 0, 0.5, 0.6}, {0.5, 0.6, 0.75, 0}, {0.75, 0, 0.98, 1}};
        t['X'] = {{0.04, 0, 0.96, 1}, {0.04, 1, 0.96, 0}};
        t['Y'] = {{0.02, 1, 0.5, 0.48}, {0.98, 1, 0.5, 0.48}, {0.5, 0.48, 0.5, 0}};
        t['Z'] = {{0.04, 1, 0.96, 1}, {0.94, 1, 0.06, 0}, {0.04, 0, 0.96, 0}};
        t['0'] = O;
        t['0'].push_back({0.8, 0.85, 0.2, 0.15});
        t['1'] = {{0.25, 0.8, 0.55, 1}, {0.55, 1, 0.55, 0}, {0.25, 0, 0.85, 0}};
        t['2'] = {{0.08, 0.82, 0.28, 1}, {0.28, 1, 0.72, 1}, {0.72, 1, 0.92, 0.82}, {0.92, 0.82, 0.92, 0.62}, {0.92, 0.62, 0.08, 0}, {0.08, 0, 0.94, 0}};
        t['3'] = {{0.08, 1, 0.9, 1}, {0.9, 1, 0.45, 0.58}, {0.45, 0.58, 0.72, 0.58}, {0.72, 0.58, 0.94, 0.4}, {0.94, 0.4, 0.94, 0.18},
                  {0.94, 0.18, 0.72, 0}, {0.72, 0, 0.08, 0}};
        t['4'] = {{0.7, 0, 0.7, 1}, {0.7, 1, 0.04, 0.3}, {0.04, 0.3, 0.96, 0.3}};
        t['5'] = {{0.9, 1, 0.12, 1}, {0.12, 1, 0.1, 0.56}, {0.1, 0.56, 0.72, 0.56}, {0.72, 0.56, 0.94, 0.38}, {0.94, 0.38, 0.94, 0.18},
                  {0.94, 0.18, 0.72, 0}, {0.72, 0, 0.08, 0}};
        t['6'] = {{0.85, 1, 0.3, 1}, {0.3, 1, 0.08, 0.75}, {0.08, 0.75, 0.08, 0.2}, {0.08, 0.2, 0.28, 0}, {0.28, 0, 0.72, 0},
                  {0.72, 0, 0.92, 0.2}, {0.92, 0.2, 0.92, 0.38}, {0.92, 0.38, 0.72, 0.56}, {0.72, 0.56, 0.08, 0.56}};
        t['7'] = {{0.06, 1, 0.94, 1}, {0.94, 1, 0.35, 0}};
        t['8'] = {{0.2, 0.5, 0.8, 0.5}, {0.2, 0.5, 0.1, 0.62}, {0.1, 0.62, 0.1, 0.88}, {0.1, 0.88, 0.25, 1}, {0.25, 1, 0.75, 1},
                  {0.75, 1, 0.9, 0.88}, {0.9, 0.88, 0.9, 0.62}, {0.9, 0.62, 0.8, 0.5}, {0.2, 0.5, 0.06, 0.36}, {0.06, 0.36, 0.06, 0.14},
                  {0.06, 0.14, 0.25, 0}, {0.25, 0, 0.75, 0}, {0.75, 0, 0.94, 0.14}, {0.94, 0.14, 0.94, 0.36}, {0.94, 0.36, 0.8, 0.5}};
        t['9'] = {{0.15, 0, 0.7, 0}, {0.7, 0, 0.92, 0.25}, {0.92, 0.25, 0.92, 0.8}, {0.92, 0.8, 0.72, 1}, {0.72, 1, 0.28, 1},
                  {0.28, 1, 0.08, 0.8}, {0.08, 0.8, 0.08, 0.62}, {0.08, 0.62, 0.28, 0.44}, {0.28, 0.44, 0.92, 0.44}};
        t['-'] = {{0.1, 0.5, 0.9, 0.5}};
        t['+'] = {{0.1, 0.5, 0.9, 0.5}, {0.5, 0.1, 0.5, 0.9}};
        t['='] = {{0.1, 0.32, 0.9, 0.32}, {0.1, 0.68, 0.9, 0.68}};
        t['/'] = {{0.15, 0, 0.85, 1}};
        t['\\'] = {{0.15, 1, 0.85, 0}};
        t['.'] = {{0.45, 0.02, 0.55, 0.02}};
        t[','] = {{0.55, 0.1, 0.4, -0.12}};
        t['\''] = {{0.5, 1, 0.5, 0.72}};
        t[';'] = {{0.5, 0.7, 0.5, 0.72}, {0.55, 0.1, 0.4, -0.12}};
        t['['] = {{0.7, 1, 0.3, 1}, {0.3, 1, 0.3, 0}, {0.3, 0, 0.7, 0}};
        t[']'] = {{0.3, 1, 0.7, 1}, {0.7, 1, 0.7, 0}, {0.7, 0, 0.3, 0}};
        t['`'] = {{0.35, 1, 0.6, 0.78}};
        return t;
    }();
    static const Strokes none;
    const unsigned char u = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch)));
    return u < 128 ? table[u] : none;
}

constexpr double kLetterAspect = 0.72;

void letter(Shape& s, char ch, double cx, double cy, double h, double w_stroke) {
    const double w = h * kLetterAspect;
    // the strokes' centre lines stay inside the box less half a stroke, so the letter's ink fills the box
    const double x0 = cx - w / 2 + w_stroke / 2, y0 = cy - h / 2 + w_stroke / 2;
    const double sx = w - w_stroke, sy = h - w_stroke;
    for (const auto& k : glyph_strokes(ch)) stroke(s, {x0 + k[0] * sx, y0 + k[1] * sy}, {x0 + k[2] * sx, y0 + k[3] * sy}, w_stroke);
}

double text_width(size_t n, double h) { return n ? n * h * kLetterAspect + (n - 1) * h * 0.16 : 0; }

void text(Shape& s, const std::string& t, double cx, double cy, double h, double w_stroke) {
    const double w = h * kLetterAspect, gap = h * 0.16;
    const double total = text_width(t.size(), h);
    for (size_t i = 0; i < t.size(); i++) letter(s, t[i], cx - total / 2 + w / 2 + i * (w + gap), cy, h, w_stroke);
}

// ---- icons ------------------------------------------------------------------------------------------------------------------------------
struct GlyphDef {
    Shape shape;
    int advance = kEm;
    size_t inner = 0;   // shape[inner..] is what lies inside the ring or plate (shape[0] its outline); 0: no ring or plate
};

constexpr double kMid = kEm / 2.0;

// A round button with something inside: a letter, or a mark drawn by `inner` around the centre.
GlyphDef round_with(void (*inner)(Shape&)) {
    GlyphDef g;
    ring(g.shape, kMid, kCy, kR, 80);
    g.inner = g.shape.size();
    inner(g.shape);
    return g;
}
GlyphDef round_letter(char ch) {
    GlyphDef g;
    ring(g.shape, kMid, kCy, kR, 80);
    g.inner = g.shape.size();
    letter(g.shape, ch, kMid, kCy, 450, 95);
    return g;
}

// A rounded plate (shoulder buttons, the trigger, keys) with text; wide enough for it.
GlyphDef plate(const std::string& t, double text_h = 370, double min_w = 760) {
    GlyphDef g;
    const double w = std::max(min_w, text_width(t.size(), text_h) + 300), h = 640;
    g.advance = static_cast<int>(w + 100);
    const double cx = g.advance / 2.0;
    fill(g.shape, rrect(cx - w / 2, kCy - h / 2, cx + w / 2, kCy + h / 2, 210));
    hole(g.shape, rrect(cx - w / 2 + 75, kCy - h / 2 + 75, cx + w / 2 - 75, kCy + h / 2 - 75, 140));
    g.inner = g.shape.size();
    text(g.shape, t, cx, kCy, text_h, text_h * 0.23);
    return g;
}

// A key cap: a square-ish outline with a flatter top edge look, and the key's label (short names stay large, long ones shrink).
GlyphDef keycap(const std::string& label) {
    GlyphDef g;
    const double text_h = label.size() <= 1 ? 400 : label.size() <= 3 ? 330 : 260;
    const double w = std::max(760.0, text_width(label.size(), text_h) + 260), h = 760;
    g.advance = static_cast<int>(std::min<double>(kMaxAdvance - 60, w + 100));
    const double cx = g.advance / 2.0;
    fill(g.shape, rrect(cx - w / 2, kCy - h / 2, cx + w / 2, kCy + h / 2, 120));
    hole(g.shape, rrect(cx - w / 2 + 70, kCy - h / 2 + 70, cx + w / 2 - 70, kCy + h / 2 - 70, 70));
    g.inner = g.shape.size();
    text(g.shape, label, cx, kCy + 10, text_h, text_h * 0.24);
    return g;
}

void mark_cross(Shape& s) {   // PlayStation cross
    stroke(s, {kMid - 190, kCy - 190}, {kMid + 190, kCy + 190}, 90);
    stroke(s, {kMid - 190, kCy + 190}, {kMid + 190, kCy - 190}, 90);
}
void mark_circle(Shape& s) { ring(s, kMid, kCy, 220, 80); }
void mark_square(Shape& s) {
    fill(s, {{kMid - 200, kCy + 200}, {kMid + 200, kCy + 200}, {kMid + 200, kCy - 200}, {kMid - 200, kCy - 200}});
    hole(s, {{kMid - 125, kCy + 125}, {kMid + 125, kCy + 125}, {kMid + 125, kCy - 125}, {kMid - 125, kCy - 125}});
}
void mark_triangle(Shape& s) {
    const Pt top{kMid, kCy + 230}, l{kMid - 230, kCy - 170}, r{kMid + 230, kCy - 170};
    stroke(s, top, l, 80), stroke(s, l, r, 80), stroke(s, r, top, 80);
}
void mark_menu(Shape& s) {    // three lines (Xbox Menu, PlayStation Options)
    for (double dy : {-150.0, 0.0, 150.0}) stroke(s, {kMid - 190, kCy + dy}, {kMid + 190, kCy + dy}, 70);
}
void mark_view(Shape& s) {    // two overlapping windows (Xbox View)
    for (double d : {-70.0, 70.0}) {
        const double x = kMid + d, y = kCy - d;
        fill(s, {{x - 140, y + 110}, {x + 140, y + 110}, {x + 140, y - 110}, {x - 140, y - 110}});
        hole(s, {{x - 90, y + 60}, {x + 90, y + 60}, {x + 90, y - 60}, {x - 90, y - 60}});
    }
}
void mark_create(Shape& s) {  // an arrow up out of a tray (PlayStation Create / Share)
    stroke(s, {kMid, kCy - 60}, {kMid, kCy + 220}, 70);
    stroke(s, {kMid - 110, kCy + 110}, {kMid, kCy + 220}, 70);
    stroke(s, {kMid + 110, kCy + 110}, {kMid, kCy + 220}, 70);
    stroke(s, {kMid - 190, kCy - 20}, {kMid - 190, kCy - 200}, 70);
    stroke(s, {kMid - 190, kCy - 200}, {kMid + 190, kCy - 200}, 70);
    stroke(s, {kMid + 190, kCy - 200}, {kMid + 190, kCy - 20}, 70);
}
void mark_next(Shape& s) { fill(s, {{kMid - 110, kCy + 190}, {kMid + 170, kCy}, {kMid - 110, kCy - 190}}); }   // Steam >
void mark_prev(Shape& s) { fill(s, {{kMid + 110, kCy + 190}, {kMid - 170, kCy}, {kMid + 110, kCy - 190}}); }   // Steam <

GlyphDef sign(bool minus) {
    GlyphDef g;
    ring(g.shape, kMid, kCy, kR, 80);
    g.inner = g.shape.size();
    stroke(g.shape, {kMid - 190, kCy}, {kMid + 190, kCy}, 100);
    if (!minus) stroke(g.shape, {kMid, kCy - 190}, {kMid, kCy + 190}, 100);
    return g;
}

Contour cross(double cx, double cy, double a, double e) {
    return {{cx - a, cy + e}, {cx + a, cy + e}, {cx + a, cy + a}, {cx + e, cy + a}, {cx + e, cy - a}, {cx + a, cy - a},
            {cx + a, cy - e}, {cx - a, cy - e}, {cx - a, cy - a}, {cx - e, cy - a}, {cx - e, cy + a}, {cx - a, cy + a}};
}

// arm: -1 = the whole cross solid; 0 right, 1 down, 2 up, 3 left = an outline with that arm filled
GlyphDef dpad(int arm) {
    GlyphDef g;
    const double cx = kMid, a = 150, e = kR, th = 60;
    fill(g.shape, cross(cx, kCy, a, e));
    if (arm < 0) return g;
    hole(g.shape, cross(cx, kCy, a - th, e - th));
    const double i = a - th, o = e - th;
    switch (arm) {
        case 0: fill(g.shape, {{cx + i, kCy + i}, {cx + o, kCy + i}, {cx + o, kCy - i}, {cx + i, kCy - i}}); break;
        case 1: fill(g.shape, {{cx - i, kCy - i}, {cx + i, kCy - i}, {cx + i, kCy - o}, {cx - i, kCy - o}}); break;
        case 2: fill(g.shape, {{cx - i, kCy + o}, {cx + i, kCy + o}, {cx + i, kCy + i}, {cx - i, kCy + i}}); break;
        default: fill(g.shape, {{cx - o, kCy + i}, {cx - i, kCy + i}, {cx - i, kCy - i}, {cx - o, kCy - i}}); break;
    }
    return g;
}

// A ring with a solid arrow head pointing at `deg` (0 = right, counter-clockwise).
GlyphDef arrow(double deg) {
    GlyphDef g;
    ring(g.shape, kMid, kCy, kR, 70);
    const double r = deg * kPi / 180, c = std::cos(r), s = std::sin(r);
    auto at = [&](double u, double v) { return Pt{kMid + u * c - v * s, kCy + u * s + v * c}; };
    fill(g.shape, {at(250, 0), at(-90, 210), at(-90, -210)});
    stroke(g.shape, at(-80, 0), at(-230, 0), 110);
    return g;
}

// A key cap with an arrow (the arrow keys).
GlyphDef arrow_key(double deg) {
    GlyphDef g = keycap("");
    const double cx = g.advance / 2.0, r = deg * kPi / 180, c = std::cos(r), s = std::sin(r);
    auto at = [&](double u, double v) { return Pt{cx + u * c - v * s, kCy + u * s + v * c}; };
    fill(g.shape, {at(220, 0), at(-60, 190), at(-60, -190)});
    stroke(g.shape, at(-50, 0), at(-200, 0), 100);
    return g;
}

// A mouse: the body outline, the wheel, and the pressed button filled (1 left, 2 right, 3 middle, 0 none).
GlyphDef mouse(int button) {
    GlyphDef g;
    const double cx = kMid, w = 520, h = 780;
    fill(g.shape, rrect(cx - w / 2, kCy - h / 2, cx + w / 2, kCy + h / 2, 250));
    hole(g.shape, rrect(cx - w / 2 + 65, kCy - h / 2 + 65, cx + w / 2 - 65, kCy + h / 2 - 65, 190));
    stroke(g.shape, {cx - w / 2 + 40, kCy + 60}, {cx + w / 2 - 40, kCy + 60}, 50);   // the line under the buttons
    stroke(g.shape, {cx, kCy + 60}, {cx, kCy + h / 2 - 40}, 50);                      // between them
    const double top = kCy + h / 2 - 100;
    if (button == 1) fill(g.shape, {{cx - 200, top}, {cx - 40, top}, {cx - 40, kCy + 100}, {cx - 200, kCy + 100}});
    if (button == 2) fill(g.shape, {{cx + 40, top}, {cx + 200, top}, {cx + 200, kCy + 100}, {cx + 40, kCy + 100}});
    if (button == 3) fill(g.shape, circle(cx, kCy + 200, 60, 16));
    return g;
}

// A stick: a ring with the stick's cap (a disc) or its letter.
GlyphDef stick(char ch) {
    GlyphDef g;
    ring(g.shape, kMid, kCy, kR, 70);
    g.inner = g.shape.size();
    if (ch) {
        fill(g.shape, circle(kMid, kCy, kR - 120));
        hole(g.shape, circle(kMid, kCy, kR - 190));
        letter(g.shape, ch, kMid, kCy, 280, 70);
    } else {
        fill(g.shape, circle(kMid, kCy, 210));
    }
    return g;
}

GlyphDef plain_ring() {
    GlyphDef g;
    ring(g.shape, kMid, kCy, kR, 70);
    return g;
}

GlyphDef notdef() {
    GlyphDef g;
    g.advance = 600;
    fill(g.shape, {{50, 0}, {50, 700}, {550, 700}, {550, 0}});
    hole(g.shape, {{110, 60}, {110, 640}, {490, 640}, {490, 60}});
    return g;
}

// The Switch's own icon for a role.
GlyphDef switch_icon(FaceRole r) {
    switch (r) {
        case FaceRole::A: return round_letter('A');
        case FaceRole::B: return round_letter('B');
        case FaceRole::X: return round_letter('X');
        case FaceRole::Y: return round_letter('Y');
        case FaceRole::L: return plate("L");
        case FaceRole::R: return plate("R");
        case FaceRole::ZL: return plate("ZL");
        case FaceRole::ZR: return plate("ZR");
        case FaceRole::SL: return plate("SL");
        case FaceRole::SR: return plate("SR");
        case FaceRole::Plus: return sign(false);
        case FaceRole::Minus: return sign(true);
        case FaceRole::Right: return dpad(0);
        case FaceRole::Down: return dpad(1);
        case FaceRole::Up: return dpad(2);
        case FaceRole::Left: return dpad(3);
        case FaceRole::LStick: return stick('L');
        case FaceRole::RStick: return stick('R');
        default: return plain_ring();
    }
}

// A key's label from its input name ("space", "left ctrl", "f1", "w").
std::string key_label(std::string_view n) {
    static const std::pair<std::string_view, std::string_view> kNames[] = {
        {"space", "SPACE"}, {"enter", "ENTER"}, {"return", "ENTER"}, {"escape", "ESC"}, {"esc", "ESC"}, {"tab", "TAB"},
        {"backspace", "BKSP"}, {"delete", "DEL"}, {"insert", "INS"}, {"home", "HOME"}, {"end", "END"}, {"page up", "PGUP"},
        {"page down", "PGDN"}, {"left shift", "SHIFT"}, {"right shift", "SHIFT"}, {"shift", "SHIFT"}, {"left ctrl", "CTRL"},
        {"right ctrl", "CTRL"}, {"ctrl", "CTRL"}, {"left alt", "ALT"}, {"right alt", "ALT"}, {"alt", "ALT"}, {"caps lock", "CAPS"},
        {"keypad enter", "ENTER"},
    };
    for (const auto& [k, v] : kNames)
        if (n == k) return std::string(v);
    std::string out;
    for (char ch : n) {
        if (ch == ' ') continue;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (out.rfind("KEYPAD", 0) == 0) out = "KP" + out.substr(6);
    return out.size() > 6 ? out.substr(0, 6) : out;
}

// The icon a face name stands for; the Switch's own when the name is empty or not known.
GlyphDef face_icon(FaceRole role, const std::string& name) {
    const size_t slash = name.find('/');
    if (name.empty() || slash == std::string::npos) return switch_icon(role);
    const std::string_view set(name.data(), slash), what(name.data() + slash + 1, name.size() - slash - 1);
    if (what.rfind("dpad", 0) == 0) {
        if (what == "dpad_right") return dpad(0);
        if (what == "dpad_down") return dpad(1);
        if (what == "dpad_up") return dpad(2);
        if (what == "dpad_left") return dpad(3);
        return dpad(-1);
    }
    if (set == "ps") {
        if (what == "cross") return round_with(mark_cross);
        if (what == "circle") return round_with(mark_circle);
        if (what == "square") return round_with(mark_square);
        if (what == "triangle") return round_with(mark_triangle);
        if (what == "options") return round_with(mark_menu);
        if (what == "create") return round_with(mark_create);
        if (what.size() == 2) return plate(std::string(what));   // l1 r1 l2 r2
    } else if (set == "xbox" || set == "steam" || set == "deck") {
        if (what.size() == 1) return round_letter(what[0]);       // a b x y
        if (what == "menu") return set == "steam" ? round_with(mark_next) : round_with(mark_menu);
        if (what == "view") return set == "steam" ? round_with(mark_prev) : round_with(mark_view);
        if (what.size() == 2) return plate(std::string(what));   // lb rb lt rt, steam lg rg lp rp
    } else if (set == "kbm") {
        if (what == "mouse1") return mouse(1);
        if (what == "mouse2") return mouse(2);
        if (what == "mouse3") return mouse(3);
        if (what == "up") return arrow_key(90);
        if (what == "down") return arrow_key(270);
        if (what == "left") return arrow_key(180);
        if (what == "right") return arrow_key(0);
        if (what.rfind("wheel", 0) == 0) return mouse(3);
        const std::string label = key_label(what);
        if (!label.empty()) return keycap(label);
    }
    return switch_icon(role);
}

// ---- TrueType writer ------------------------------------------------------------------------------------------------------------------
struct Be {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
    void u16(uint32_t v) { u8(v >> 8), u8(v); }
    void i16(int v) { u16(static_cast<uint16_t>(static_cast<int16_t>(v))); }
    void u32(uint32_t v) { u16(v >> 16), u16(v & 0xFFFF); }
    void pad4() {
        while (b.size() % 4) u8(0);
    }
};

struct Encoded {
    std::vector<uint8_t> data;
    int xmin = 0, points = 0, contours = 0, advance = 0;
};

Encoded encode(const GlyphDef& g) {
    Encoded e;
    e.advance = std::min(g.advance, kMaxAdvance);
    std::vector<std::vector<std::array<int, 2>>> cs;
    for (const Contour& c : g.shape) {
        std::vector<std::array<int, 2>> pts;
        for (const Pt& p : c) {
            const std::array<int, 2> q = {std::clamp(static_cast<int>(std::lround(p.x)), kBoxXMin, kBoxXMax),
                                          std::clamp(static_cast<int>(std::lround(p.y)), kBoxYMin, kBoxYMax)};
            if (pts.empty() || pts.back() != q) pts.push_back(q);
        }
        while (pts.size() > 1 && pts.front() == pts.back()) pts.pop_back();
        if (pts.size() >= 3) cs.push_back(std::move(pts));
    }
    // a glyph past the fixed totals loses its last contours (none of the faces comes near them)
    int pts_total = 0;
    size_t keep = 0;
    while (keep < cs.size() && keep < static_cast<size_t>(kMaxContours) && pts_total + static_cast<int>(cs[keep].size()) <= kMaxPoints)
        pts_total += static_cast<int>(cs[keep++].size());
    cs.resize(keep);
    if (cs.empty()) return e;
    int xmin = 32767, ymin = 32767, xmax = -32768, ymax = -32768;
    for (const auto& c : cs)
        for (const auto& p : c) {
            xmin = std::min(xmin, p[0]), xmax = std::max(xmax, p[0]);
            ymin = std::min(ymin, p[1]), ymax = std::max(ymax, p[1]);
            e.points++;
        }
    e.xmin = xmin;
    e.contours = static_cast<int>(cs.size());
    Be o;
    o.i16(e.contours);
    o.i16(xmin), o.i16(ymin), o.i16(xmax), o.i16(ymax);
    int end = -1;
    for (const auto& c : cs) o.u16(static_cast<uint32_t>(end += static_cast<int>(c.size())));
    o.u16(0);                                         // no instructions
    for (int i = 0; i < e.points; i++) o.u8(0x01);    // on-curve; x and y as 16-bit deltas
    int last = 0;
    for (const auto& c : cs)
        for (const auto& p : c) o.i16(p[0] - last), last = p[0];
    last = 0;
    for (const auto& c : cs)
        for (const auto& p : c) o.i16(p[1] - last), last = p[1];
    e.data = std::move(o.b);
    return e;
}

uint32_t checksum(const std::vector<uint8_t>& t) {
    uint32_t sum = 0;
    for (size_t i = 0; i < t.size(); i += 4) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; k++) w = (w << 8) | (i + k < t.size() ? t[i + k] : 0);
        sum += w;
    }
    return sum;
}

std::vector<uint8_t> name_table() {
    struct Rec {
        uint16_t platform, encoding, language, id;
        std::string text;
    };
    const char* family = "AnySwitch Button Icons";
    std::vector<Rec> recs;
    for (uint16_t platform : {uint16_t(1), uint16_t(3)}) {
        const uint16_t enc = platform == 3 ? 1 : 0, lang = platform == 3 ? 0x409 : 0;
        recs.push_back({platform, enc, lang, 1, family});
        recs.push_back({platform, enc, lang, 2, "Regular"});
        recs.push_back({platform, enc, lang, 3, std::string(family) + " Regular"});
        recs.push_back({platform, enc, lang, 4, family});
        recs.push_back({platform, enc, lang, 6, "AnySwitchButtonIcons-Regular"});
    }
    Be strings;
    Be o;
    o.u16(0);
    o.u16(static_cast<uint32_t>(recs.size()));
    o.u16(static_cast<uint32_t>(6 + 12 * recs.size()));
    for (const Rec& r : recs) {
        const size_t at = strings.b.size();
        for (char ch : r.text) {
            if (r.platform == 3) strings.u16(static_cast<uint8_t>(ch));   // UTF-16BE (ASCII text)
            else strings.u8(static_cast<uint8_t>(ch));                    // Mac Roman
        }
        o.u16(r.platform), o.u16(r.encoding), o.u16(r.language), o.u16(r.id);
        o.u16(static_cast<uint32_t>(strings.b.size() - at)), o.u16(static_cast<uint32_t>(at));
    }
    o.b.insert(o.b.end(), strings.b.begin(), strings.b.end());
    return o.b;
}

// Glyph order (the same for every face): 0 .notdef, 1 + role for the 18 roles, then the stick arrows and sticks, then the plain ring.
enum : uint16_t { kGlyphRoles = 1, kGlyphArrows = kGlyphRoles + kFaceRoleCount, kGlyphStick = kGlyphArrows + 8, kGlyphDpadAll, kGlyphRing,
                  kGlyphCount };

}  // namespace

std::vector<uint8_t> make_extension_font(const ButtonFace& face) {
    std::vector<GlyphDef> glyphs(kGlyphCount);
    glyphs[0] = notdef();
    for (size_t r = 0; r < kFaceRoleCount; ++r) glyphs[kGlyphRoles + r] = face_icon(static_cast<FaceRole>(r), face.name[r]);
    const double dirs[] = {0, 180, 90, 270, 45, 315, 225, 135};   // U+E090..E097: right, left, up, down, right-up, right-down, left-down, left-up
    for (int i = 0; i < 8; i++) glyphs[kGlyphArrows + i] = arrow(dirs[i]);
    glyphs[kGlyphStick] = stick(0);
    glyphs[kGlyphDpadAll] = dpad(-1);   // the whole d-pad looks the same on every pad
    glyphs[kGlyphRing] = plain_ring();

    std::array<uint16_t, 0x200> cmap{};   // U+E000 + i -> glyph
    auto map = [&](uint32_t code, uint16_t glyph) { cmap[code - 0xE000] = glyph; };
    auto role = [&](FaceRole r) { return static_cast<uint16_t>(kGlyphRoles + static_cast<size_t>(r)); };
    const FaceRole abxy[] = {FaceRole::A, FaceRole::B, FaceRole::X, FaceRole::Y};
    for (uint32_t i = 0; i < 4; i++) map(0xE0A0 + i, role(abxy[i])), map(0xE0E0 + i, role(abxy[i]));
    const FaceRole plates[] = {FaceRole::L, FaceRole::R, FaceRole::ZL, FaceRole::ZR, FaceRole::SL, FaceRole::SR};
    for (uint32_t i = 0; i < 6; i++) map(0xE0A4 + i, role(plates[i])), map(0xE0E4 + i, role(plates[i]));
    map(0xE0B5, role(FaceRole::Plus)), map(0xE0EF, role(FaceRole::Plus));
    map(0xE0B6, role(FaceRole::Minus)), map(0xE0F0, role(FaceRole::Minus));
    map(0xE0EA, kGlyphDpadAll);
    const FaceRole arms[] = {FaceRole::Right, FaceRole::Down, FaceRole::Up, FaceRole::Left};   // U+E0AB..E0AE and +0x40
    for (uint32_t i = 0; i < 4; i++) map(0xE0AB + i, role(arms[i])), map(0xE0EB + i, role(arms[i]));
    for (uint32_t i = 0; i < 8; i++) map(0xE090 + i, static_cast<uint16_t>(kGlyphArrows + i));
    map(0xE100, kGlyphStick);
    map(0xE101, role(FaceRole::LStick)), map(0xE0C1, role(FaceRole::LStick));
    map(0xE102, role(FaceRole::RStick)), map(0xE0C2, role(FaceRole::RStick));
    for (uint16_t& g : cmap)
        if (g == 0) g = kGlyphRing;

    // glyf (padded to its fixed capacity), loca, hmtx
    Be glyf, loca, hmtx;
    for (const GlyphDef& g : glyphs) {
        Encoded e = encode(g);
        if (glyf.b.size() + e.data.size() > kGlyfCapacity) e = Encoded{{}, 0, 0, 0, e.advance};   // never with these icons
        loca.u32(static_cast<uint32_t>(glyf.b.size()));
        glyf.b.insert(glyf.b.end(), e.data.begin(), e.data.end());
        glyf.pad4();
        hmtx.u16(static_cast<uint32_t>(e.advance)), hmtx.i16(e.xmin);
    }
    loca.u32(static_cast<uint32_t>(glyf.b.size()));
    glyf.b.resize(kGlyfCapacity, 0);
    const uint32_t n = kGlyphCount;

    Be head;
    head.u16(1), head.u16(0), head.u32(0x00010000), head.u32(0), head.u32(0x5F0F3CF5);
    head.u16(0x0009), head.u16(kEm);
    head.u32(0), head.u32(0), head.u32(0), head.u32(0);   // created, modified
    head.i16(kBoxXMin), head.i16(kBoxYMin), head.i16(kBoxXMax), head.i16(kBoxYMax);
    head.u16(0), head.u16(8), head.i16(2), head.i16(1), head.i16(0);

    Be hhea;
    hhea.u16(1), hhea.u16(0), hhea.i16(kAscent), hhea.i16(kDescent), hhea.i16(0);
    hhea.u16(kMaxAdvance), hhea.i16(0), hhea.i16(0), hhea.i16(kBoxXMax);
    hhea.i16(1), hhea.i16(0), hhea.i16(0), hhea.i16(0), hhea.i16(0), hhea.i16(0), hhea.i16(0), hhea.i16(0);
    hhea.u16(n);

    Be maxp;
    maxp.u32(0x00010000), maxp.u16(n), maxp.u16(kMaxPoints), maxp.u16(kMaxContours);
    maxp.u16(0), maxp.u16(0), maxp.u16(2);
    for (int i = 0; i < 8; i++) maxp.u16(0);

    Be os2;
    os2.u16(4), os2.i16(kEm), os2.u16(400), os2.u16(5), os2.u16(0);
    for (int v : {650, 600, 0, 75, 650, 600, 0, 350, 50, 300}) os2.i16(v);
    os2.i16(0);
    for (int i = 0; i < 10; i++) os2.u8(0);   // panose
    os2.u32(0), os2.u32(1u << 28), os2.u32(0), os2.u32(0);   // Unicode ranges: bit 60, the private use area
    for (char ch : std::string("NONE")) os2.u8(static_cast<uint8_t>(ch));
    os2.u16(0x40), os2.u16(0xE000), os2.u16(0xE1FF);
    os2.i16(kAscent), os2.i16(kDescent), os2.i16(0), os2.u16(kAscent), os2.u16(-kDescent);
    os2.u32(1), os2.u32(0), os2.i16(500), os2.i16(700), os2.u16(0), os2.u16(0x20), os2.u16(0);

    Be cmapt;   // header, two encoding records (Unicode BMP and Windows Unicode BMP) sharing one format 4 subtable
    cmapt.u16(0), cmapt.u16(2);
    cmapt.u16(0), cmapt.u16(3), cmapt.u32(20);
    cmapt.u16(3), cmapt.u16(1), cmapt.u32(20);
    const uint32_t seg_count = 2;
    cmapt.u16(4), cmapt.u16(16 + 8 * seg_count + 2 * 0x200), cmapt.u16(0);
    cmapt.u16(2 * seg_count), cmapt.u16(4), cmapt.u16(1), cmapt.u16(0);   // searchRange, entrySelector, rangeShift for 2 segments
    cmapt.u16(0xE1FF), cmapt.u16(0xFFFF);                                  // end codes
    cmapt.u16(0);                                                          // reserved
    cmapt.u16(0xE000), cmapt.u16(0xFFFF);                                  // start codes
    cmapt.i16(0), cmapt.i16(1);                                            // deltas
    cmapt.u16(4), cmapt.u16(0);                                            // range offsets: segment 0 reads glyphIdArray
    for (uint16_t g : cmap) cmapt.u16(g);

    Be post;
    post.u32(0x00030000), post.u32(0), post.i16(-100), post.i16(50);
    for (int i = 0; i < 5; i++) post.u32(0);

    struct Table {
        const char* tag;
        std::vector<uint8_t> data;
    };
    std::vector<Table> tables = {{"OS/2", os2.b},   {"cmap", cmapt.b}, {"glyf", glyf.b}, {"head", head.b}, {"hhea", hhea.b},
                                 {"hmtx", hmtx.b},  {"loca", loca.b},  {"maxp", maxp.b}, {"name", name_table()}, {"post", post.b}};
    const uint32_t nt = static_cast<uint32_t>(tables.size());
    uint32_t pow2 = 1, log2 = 0;
    while (pow2 * 2 <= nt) pow2 *= 2, log2++;
    Be font;
    font.u32(0x00010000), font.u16(nt), font.u16(pow2 * 16), font.u16(log2), font.u16(nt * 16 - pow2 * 16);
    uint32_t offset = 12 + 16 * nt;
    size_t head_at = 0;
    for (const Table& t : tables) {
        for (int k = 0; k < 4; k++) font.u8(static_cast<uint8_t>(t.tag[k]));
        font.u32(checksum(t.data)), font.u32(offset), font.u32(static_cast<uint32_t>(t.data.size()));
        if (std::strcmp(t.tag, "head") == 0) head_at = offset;
        offset += static_cast<uint32_t>((t.data.size() + 3) & ~size_t(3));
    }
    for (const Table& t : tables) {
        font.b.insert(font.b.end(), t.data.begin(), t.data.end());
        font.pad4();
    }
    const uint32_t adjust = 0xB1B0AFBAu - checksum(font.b);
    for (int k = 0; k < 4; k++) font.b[head_at + 8 + k] = static_cast<uint8_t>(adjust >> (24 - 8 * k));
    return font.b;
}

// ---- the icons as pictures (nxrt/shared_font.h shared_font_icon_coverage) ----------------------------------------------------------------
namespace {

// Coverage of the contours `cs` (straight edges, non-zero winding, y up) over a w x h grid that shows the glyph box [x0, x1] x [y0, y1];
// 4 x 4 samples a pixel. Rows from the top unless bottom_up.
void rasterize(const std::vector<const Contour*>& cs, double x0, double y0, double x1, double y1, uint32_t w, uint32_t h, bool bottom_up,
               std::vector<float>& cov) {
    constexpr int kSub = 4;
    cov.assign(static_cast<size_t>(w) * h, 0.0f);
    struct Edge {
        double xa, ya, xb, yb;
        int dir;
    };
    std::vector<Edge> edges;
    for (const Contour* c : cs)
        for (size_t i = 0; i < c->size(); i++) {
            const Pt& p = (*c)[i];
            const Pt& q = (*c)[(i + 1) % c->size()];
            if (p.y == q.y) continue;
            edges.push_back(p.y < q.y ? Edge{p.x, p.y, q.x, q.y, 1} : Edge{q.x, q.y, p.x, p.y, -1});
        }
    const double sx = (x1 - x0) / w, sy = (y1 - y0) / h;
    std::vector<std::pair<double, int>> cross;
    for (uint32_t row = 0; row < h; row++)
        for (int k = 0; k < kSub; k++) {
            const double fy = row + (k + 0.5) / kSub;                       // from the top of the grid (or the bottom when bottom_up)
            const double gy = bottom_up ? y0 + fy * sy : y1 - fy * sy;
            cross.clear();
            for (const Edge& e : edges)
                if (gy >= e.ya && gy < e.yb) cross.push_back({e.xa + (gy - e.ya) * (e.xb - e.xa) / (e.yb - e.ya), e.dir});
            if (cross.empty()) continue;
            std::sort(cross.begin(), cross.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < cross.size(); i++) {
                wind += cross[i].second;
                if (wind == 0) continue;
                // samples at x = (col + (j + 0.5) / kSub) inside [cross[i], cross[i + 1])
                const double a = (cross[i].first - x0) / sx, b = (cross[i + 1].first - x0) / sx;
                const int s0 = std::max(0, static_cast<int>(std::ceil(a * kSub - 0.5)));
                const int s1 = std::min(static_cast<int>(w) * kSub - 1, static_cast<int>(std::ceil(b * kSub - 0.5)) - 1);
                for (int s = s0; s <= s1; s++) cov[static_cast<size_t>(row) * w + s / kSub] += 1.0f / (kSub * kSub);
            }
        }
}

}  // namespace

bool shared_font_icon_coverage(FaceRole role, const std::string& name, IconPart part, uint32_t w, uint32_t h, bool bottom_up,
                               std::vector<uint8_t>& coverage) {
    coverage.clear();
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || static_cast<size_t>(role) >= kFaceRoleCount) return false;
    const GlyphDef g = face_icon(role, name);
    if (g.shape.empty()) return false;
    const size_t inner = part == IconPart::Whole ? 0 : g.inner;
    // the box: the ring's or plate's outline (all of the icon without one), fitted into w x h with its aspect kept, centred
    double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
    auto grow = [&](const Contour& c) {
        for (const Pt& p : c) bx0 = std::min(bx0, p.x), by0 = std::min(by0, p.y), bx1 = std::max(bx1, p.x), by1 = std::max(by1, p.y);
    };
    if (g.inner) grow(g.shape[0]);
    else
        for (const Contour& c : g.shape) grow(c);
    const double bw = bx1 - bx0, bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return false;
    const double scale = std::max(bw / w, bh / h);                         // glyph units per pixel
    const double cx = (bx0 + bx1) / 2, cy = (by0 + by1) / 2, hw = scale * w / 2, hh = scale * h / 2;
    std::vector<const Contour*> mark, disc;
    for (size_t i = inner; i < g.shape.size(); i++) mark.push_back(&g.shape[i]);
    std::vector<float> a, b;
    if (part == IconPart::Disc && g.inner) {
        disc.push_back(&g.shape[0]);
        rasterize(disc, cx - hw, cy - hh, cx + hw, cy + hh, w, h, bottom_up, a);
        rasterize(mark, cx - hw, cy - hh, cx + hw, cy + hh, w, h, bottom_up, b);
        for (size_t i = 0; i < a.size(); i++) a[i] = std::min(1.0f, a[i]) * (1.0f - std::min(1.0f, b[i]));
    } else {
        rasterize(mark, cx - hw, cy - hh, cx + hw, cy + hh, w, h, bottom_up, a);
    }
    coverage.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) coverage[i] = static_cast<uint8_t>(std::lround(std::clamp(a[i], 0.0f, 1.0f) * 255.0f));
    return true;
}

}  // namespace nxrt::services
