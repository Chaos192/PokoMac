// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, the mouse in the game's menus (pkp_menu.h). Offsets in `main` of the 2.0.1 update, found in IDA (work/re/menus.md).
//
// The game's UI is KTGL screen layouts: a layout object holds its panes in a list, each pane has a name, a size and a world matrix in a
// 1920x1080 canvas (origin at the centre, y up). The game itself never hit-tests them for its menus (the engine's script call
// kids::placeable::screenlayout::DoPaneHitTest exists, 0x2611d00, and is where the pane geometry below comes from), and its menus move a
// cursor part (0000_parts_cursor_finger_s, 0250_parts_talk_cursor, 0000_parts_cursor_triangle ...) next to the selected entry when the
// d-pad moves the selection. So, once a frame while a menu is up (play mode's menu context: the camera's input is not running):
//   * every layout in use in the UI manager's pools ([0x35aebf8] +0x84808 / +0x8c858, see kPoolA; placeable +40 -> +16 =
//     the layout) is walked and each visible pane's screen rectangle computed;
//   * the entry under the pointer is the smallest visible "item" pane (a button, list cell, choice, tab ... by its name) containing it;
//   * the selected entry is the item pane nearest to the game's cursor pane (a pane named "...cursor...");
//   * the pointer does not move the selection (that guess is not reliable enough to steer by, see menu_frame): a left click is A on the
//     game's own selection, or the button of a key guide under it. Right click (B) and the wheel (d-pad up / down) are menu-context
//     bindings (pkp_game.cpp).
// The hover and the selection still feed the log, the status line, the wheel over a slider and the in-game menu detection;
// [pkp] menu_log = 2 lists the panes of the layouts on screen.
#include "pkp_menu.h"
#include "pkp_craft.h"
#include "pkp_play.h"
#include "pkp_sort.h"
#include "pkp_title.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

// ---- offsets ----
constexpr uint64_t kUiMgr = 0x35aebf8;          // [ptr] the UI manager
// The UI manager (ctor 0x710087bd60) holds two placeable pools of one type (ctor 0x71009fee60), at +0x84808 (A) and +0x8c858 (B). Pool:
// +0x8 free-slot ring, +0x1030 {entry** data, u64 count} of the entries in use (cap 512), +0x8048 mutex*, slots of 48 bytes at +0x2048.
//   pool A entry (looked up by (id, sub) in 0x710087da00 / 0x7100911bc0): +8 u32 id, +12 u32 sub, +32 -> {placeable** data, u64 count} =
//     every instance of that layout, +40 -> ring {placeable** data, u64 size, u64 cap, u64 head, u64 count} = the instances not in use
//     (0x710087da00 pops one when a screen opens).
//   pool B entry (0x7100912aa0, 0x71009125e0): +0 placeable*, +8 u32 id, +12 u32 sub. Seen empty at run time (p5c: 0 layouts).
constexpr uint64_t kPoolA = 0x84808, kPoolB = 0x8c858;
constexpr uint64_t kPoolEntries = 0x1030;         // {entry** data, u64 count}
constexpr size_t kEntInstances = 32, kEntFree = 40;
constexpr uint64_t kMaxPoolEntries = 512, kMaxInstances = 64;
constexpr size_t kPlaceableObj = 40, kObjLayout = 16;     // placeable +40 -> object, +16 -> the layout (DoPaneHitTest 0x2611d00)
// layout
constexpr size_t kLayPaneCount = 146, kLayFirstNode = 32, kLaySentinel = 64;   // u16 pane count; pane list: node +8 next, +16 pane
// pane
constexpr size_t kPaneName = 176, kPaneTag = 184, kPaneParent = 208, kPaneWorld = 224, kPaneSize = 240, kPaneFlags = 260,
                 kPaneAnchor = 268, kPaneIndex = 270, kPaneNameLen = 272, kPaneAlpha = 275;
constexpr float kCanvasW = 1920.f, kCanvasH = 1080.f;
constexpr size_t kMaxLayouts = 256, kMaxPanes = 4096;

struct Pane {
    uint64_t ptr = 0;
    uint32_t lay_id = 0, lay_sub = 0;
    int lay = -1;                      // index into the frame's layouts
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // canvas pixels, y down
    float z = 0;
    uint8_t alpha = 0;
    uint32_t flags = 0;
    bool item = false, cursor = false, slider = false;
    char name[48] = {};
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
    float area() const { return (x1 - x0) * (y1 - y0); }
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float dist(float x, float y) const {
        const float dx = std::max({x0 - x, 0.f, x - x1}), dy = std::max({y0 - y, 0.f, y - y1});
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Layout {
    uint64_t ptr = 0;
    uint32_t id = 0, sub = 0;
    int panes = 0, visible = 0;
};

struct State {
    uint64_t base = 0x7100000000ull;
    bool installed = false;
    bool enabled = true;
    int log_level = 1;
    // the frame's scan
    std::vector<Layout> layouts;
    std::vector<Pane> panes;
    int hovered = -1, focused = -1, cursor = -1, slider = -1;
    // how pick() found the selection (menus outside play, see pick_extend): 0 the cursor's rule, 1 promoted to the list entry that holds
    // the cursor's part, 2 sibling instances of one button layout (group = their entry panes)
    int mode = 0;
    std::vector<int> group;
    uint64_t mode_sig = 0;
    std::unordered_map<uint32_t, play::Act> key_guides;   // [pkp] menu_key_guides: a key guide layout id -> the button its click sends
    bool active = false;
    uint64_t last_focus = 0, last_hover = 0;   // for the log
    // in-game menus (menu_in_game): a layout with the game's cursor on an entry while the game is in play
    bool ig_enabled = false;
    bool ig_up = false;                // the in-game menu is up (debounced)
    int ig_tick = 0, ig_hits = 0, ig_miss = 0;
    bool frame_scanned = false;        // menu_frame scanned this frame (the next poll reuses its result)
    bool frame_present = false;
    uint32_t ig_lay_id = 0, ig_lay_sub = 0;
    char ig_cursor[48] = {};
    std::unordered_set<uint64_t> ig_ignore;   // [pkp] menu_in_game_ignore: layouts (id << 32 | sub) whose cursor never counts (a HUD with a cursor)
    std::atomic<uint32_t> n_ig_opens{0};
    int n_not_drawn = 0;               // this scan: pooled layouts skipped as not drawn
    int rec_panes = 0, rec_with = 0, rec_fallback_layouts = 0;      // this scan: shown panes of drawn layouts, those with a draw record,
    std::vector<uint32_t> rec_ages;                                 // layouts with no fresh record (fallback), the stale records' ages
    // diagnostics
    bool was_up = false;
    uint64_t layout_sig = 0;
    std::unordered_set<uint64_t> dumped;    // layout (id, sub) whose panes were listed
    std::chrono::steady_clock::time_point last_log{};
    uint32_t log_lines = 0;
    std::atomic<uint32_t> n_layouts{0}, n_panes{0}, n_items{0}, n_clicks{0};
    char hover_name[48] = {}, focus_name[48] = {};
    // page readability cache (one VirtualQuery per page and scan)
    std::unordered_map<uint64_t, bool> pages;
};

State& st() {
    static State s;
    return s;
}

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

// The range lies in guest memory that can be read (cached per 4 KiB page for one scan).
bool ok(uint64_t a, uint64_t n) {
    // guest space: the heap and stacks (0x78........) and main's image at 0x7100000000 (the UI manager pointer lives there)
    if (a < 0x1000000000ull || a + n > 0x8000000000ull || n == 0 || n > 0x10000) return false;
    State& s = st();
    for (uint64_t p = a & ~0xfffull; p < a + n; p += 0x1000) {
        auto it = s.pages.find(p);
        bool m;
        if (it != s.pages.end()) m = it->second;
        else {
            m = mem::is_mapped(p, 0x1000);
            s.pages.emplace(p, m);
        }
        if (!m) return false;
    }
    return true;
}

uint64_t rptr(uint64_t a) { return ok(a, 8) ? rd<uint64_t>(a) : 0; }

void read_str(uint64_t p, size_t len_hint, char* out, size_t n) {
    out[0] = 0;
    if (!p) return;
    size_t len = len_hint ? std::min(len_hint, n - 1) : n - 1;
    if (!ok(p, len)) {
        len = 0;
        while (len < n - 1 && ok(p + len, 1) && rd<char>(p + len)) ++len;
    }
    size_t i = 0;
    for (; i < len; ++i) {
        const char c = rd<char>(p + i);
        if (!c) break;
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}

bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

void classify(Pane& p, const char* tag) {
    std::string n = p.name;
    n += '|';
    n += tag;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    p.cursor = has(n, "cursor") && !has(n, "mouse");
    p.slider = has(n, "slider") || has(n, "slide_bar");
    static const char* const kItem[] = {"btn", "button", "item", "cell", "choice", "list", "tab", "slot", "chip", "select", "menu"};
    static const char* const kNot[] = {"cursor", "txt", "text", "icon", "shadow", "_bg", "bg_", "frame", "base", "line", "effect", "eff_"};
    bool item = false;
    for (const char* k : kItem) item = item || has(n, k);
    for (const char* k : kNot) item = item && !has(n, k);
    p.item = item;
}

// One pane's rectangle on the canvas (the anchor rule of DoPaneHitTest's box, 0x2645000): the pane's origin is its left / centre / right
// edge by the anchor's high nibble (0 / 1 / 2) and its top / middle / bottom by the low nibble; x and y axes from the world matrix.
// The rectangle from the world matrix's X axis (m0, m1), Y axis (m4, m5) and translation (m12, m13), the size and the anchor: the corners the
// pane draws build (0x7101f53010: the same anchor offsets, then the matrix, then the UI camera).
bool rect_from(const float* m, float m14, uint32_t size, uint8_t anchor, Pane* out) {
    const float w = static_cast<float>(size & 0xffff), h = static_cast<float>(size >> 16);
    if (w <= 0.f || h <= 0.f) return false;
    const int ah = anchor >> 4, av = anchor & 0xf;
    const float ox = ah == 0 ? w * 0.5f : (ah == 2 ? -w * 0.5f : 0.f);
    const float oy = av == 0 ? -h * 0.5f : (av == 2 ? h * 0.5f : 0.f);
    const float cx = m[12] + m[0] * ox + m[4] * oy, cy = m[13] + m[1] * ox + m[5] * oy;
    const float hx = (std::fabs(m[0]) * w + std::fabs(m[4]) * h) * 0.5f, hy = (std::fabs(m[1]) * w + std::fabs(m[5]) * h) * 0.5f;
    out->x0 = kCanvasW * 0.5f + cx - hx;
    out->x1 = kCanvasW * 0.5f + cx + hx;
    out->y0 = kCanvasH * 0.5f - cy - hy;
    out->y1 = kCanvasH * 0.5f - cy + hy;
    out->z = m14;
    return true;
}

bool pane_rect(uint64_t pane, Pane* out) {
    if (!ok(pane, kPaneAlpha + 1)) return false;
    const uint64_t mp = rd<uint64_t>(pane + kPaneWorld);
    if (!ok(mp, 64)) return false;
    float m[16];
    std::memcpy(m, reinterpret_cast<const void*>(mp), sizeof m);
    for (float v : m)
        if (!std::isfinite(v)) return false;
    if (!rect_from(m, m[14], rd<uint32_t>(pane + kPaneSize), rd<uint8_t>(pane + kPaneAnchor), out)) return false;
    out->alpha = rd<uint8_t>(pane + kPaneAlpha);
    out->flags = rd<uint32_t>(pane + kPaneFlags);
    return true;
}

bool log_ok(bool force = false) {
    State& s = st();
    if (s.log_level <= 0 || s.log_lines > 4000) return false;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - s.last_log < std::chrono::milliseconds(200)) return false;
    s.last_log = now;
    ++s.log_lines;
    return true;
}

// [pkp] menu_log = 3: the chain from the UI manager global down to the panes, once a second while scanning.
// ---- the layouts the game draws (needs the midhooks in recomp/games/pkp/main.sites) ----
// Every KTGL pane class's draw (vtable slot +0x2b0; e.g. the picture pane 0x7101f5b248) starts the same way: it ORs 0x20 into the pane's
// flags (+0x104 = +260), then returns early unless flags bit 0x4000 (forced) or bit 0x100 (visible) with no ancestor flagged 0x40 (hidden,
// 0x7101f72780) and a nonzero effective alpha (+0x113 = +275); the visible ones then emit their primitives through the draw context (x1).
// Pooled layouts are never handed to the draw, so "a pane of this layout went through a draw this frame" is the drawn test. The hooks sit
// on each draw's flag store (pane in x0, x21 for 0x7101f77fe0); pane +216 is its layout (the draw reads [pane+0xd8]+0x2e0, the layout's
// renderer, as 0x7101f56180 does). The scan then keeps only layouts drawn in the last kDrawnFrames frames.
struct DrawSite { uint64_t off; int reg; };
constexpr DrawSite kDrawSites[] = {
    {0x1f5ac6c, 0}, {0x1f5b26c, 0}, {0x1f63094, 0}, {0x1f642a8, 0}, {0x1f6a028, 0}, {0x1f6a8c4, 0},
    {0x1f76168, 0}, {0x1f765b4, 0}, {0x1f78030, 21}, {0x1f78124, 0}, {0x1f7aa2c, 0}, {0x1f7fe48, 0},
};
constexpr size_t kPaneLayout = 216;
constexpr uint32_t kDrawnFrames = 3;
constexpr size_t kDrawnSlots = 4096;
std::atomic<uint64_t> g_drawn_key[kDrawnSlots];
std::atomic<uint32_t> g_drawn_frame[kDrawnSlots];
std::atomic<uint32_t> g_frame{1};             // play mode frames (menu_in_game runs once per frame)
std::atomic<uint64_t> g_draw_calls{0};
std::vector<uint32_t> g_draw_hooks;

size_t drawn_slot(uint64_t lay) { return static_cast<size_t>((lay >> 4) * 0x9e3779b97f4a7c15ull >> 52) & (kDrawnSlots - 1); }

void drawn_mark(uint64_t lay, uint32_t frame) {
    size_t h = drawn_slot(lay);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kDrawnSlots - 1)) {
        uint64_t key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0 && g_drawn_key[h].compare_exchange_strong(key, lay, std::memory_order_relaxed)) key = lay;
        if (key == lay) { g_drawn_frame[h].store(frame, std::memory_order_relaxed); return; }
    }
}

// The layout went through a pane draw in the last kDrawnFrames frames.
bool drawn_recent(uint64_t lay) {
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    size_t h = drawn_slot(lay);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kDrawnSlots - 1)) {
        const uint64_t key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0) return false;
        if (key == lay) return now - g_drawn_frame[h].load(std::memory_order_relaxed) <= kDrawnFrames;
    }
    return false;
}

bool draw_hooks_live() { return g_draw_calls.load(std::memory_order_relaxed) != 0; }

// Each pane that went through a draw: the frame, and the world matrix, size and anchor as the draw read them (the rect is the drawn one,
// whatever parent or animation moved it). Written by the drawing thread (the data, then the frame), read by the scan.
struct DrawnPane {
    std::atomic<uint64_t> key{0};
    std::atomic<uint32_t> frame{0};
    float m[16];
    uint32_t size = 0;
    uint8_t anchor = 0;
};
constexpr size_t kPaneSlots = 16384;
DrawnPane g_drawn_panes[kPaneSlots];
std::atomic<uint64_t> g_pane_table_full{0};      // marks dropped: 32 probes found no slot

size_t pane_slot(uint64_t pane) { return static_cast<size_t>((pane >> 3) * 0x9e3779b97f4a7c15ull >> 50) & (kPaneSlots - 1); }

bool pane_mark(uint64_t pane, uint32_t frame) {
    size_t h = pane_slot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kPaneSlots - 1)) {
        DrawnPane& d = g_drawn_panes[h];
        uint64_t key = d.key.load(std::memory_order_relaxed);
        if (key == 0 && d.key.compare_exchange_strong(key, pane, std::memory_order_relaxed)) key = pane;
        if (key != pane) continue;
        if (d.frame.load(std::memory_order_relaxed) == frame) return true;
        const uint64_t mp = *reinterpret_cast<const uint64_t*>(pane + kPaneWorld);   // the draw builds its corners from this matrix
        if (mp) std::memcpy(d.m, reinterpret_cast<const void*>(mp), sizeof d.m);
        d.size = *reinterpret_cast<const uint32_t*>(pane + kPaneSize);
        d.anchor = *reinterpret_cast<const uint8_t*>(pane + kPaneAnchor);
        d.frame.store(mp ? frame : 0, std::memory_order_release);
        return false;
    }
    g_pane_table_full.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// The pane's draw record: kNoRecord, kStale (drawn, but more than kDrawnFrames frames ago; *age set) or kFresh (*out = its drawn rectangle).
enum { kNoRecord = 0, kStale = 1, kFresh = 2 };
int pane_record(uint64_t pane, Pane* out, uint32_t* age) {
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    size_t h = pane_slot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kPaneSlots - 1)) {
        const DrawnPane& d = g_drawn_panes[h];
        const uint64_t key = d.key.load(std::memory_order_relaxed);
        if (key == 0) return kNoRecord;
        if (key != pane) continue;
        const uint32_t f = d.frame.load(std::memory_order_acquire);
        if (f == 0) return kNoRecord;
        *age = now - f;
        if (*age > kDrawnFrames) return kStale;
        float m[16];
        std::memcpy(m, d.m, sizeof m);
        for (float v : m)
            if (!std::isfinite(v)) return kStale;
        return rect_from(m, m[14], d.size, d.anchor, out) ? kFresh : kStale;
    }
    return kNoRecord;
}

// A drawn pane and its ancestors: entries and cursors are mostly Null panes and part instances ('CL_...') that hold the drawn pictures and
// texts but have no draw of their own (p5j: marking only the drawing panes left 2-4 visible panes on screens with dozens). An ancestor
// already marked this frame has its own ancestors marked too.
void mark_with_ancestors(uint64_t pane, uint32_t frame) {
    for (int depth = 0; pane && depth < 64; ++depth) {
        if (pane_mark(pane, frame)) return;
        pane = *reinterpret_cast<const uint64_t*>(pane + kPaneParent);
    }
}

int hook_pane_draw(::Ctx* c, uint64_t, void* user) {
    const uint64_t pane = c->x[reinterpret_cast<uintptr_t>(user)];
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    thread_local uint64_t last_pane_lay = 0;
    thread_local uint32_t last_frame = 0;
    if (!pane) return 0;
    const uint64_t lay = *reinterpret_cast<const uint64_t*>(pane + kPaneLayout);    // the draw itself reads it next
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (!(*reinterpret_cast<const uint32_t*>(pane + kPaneFlags) & 0x4000)) {
        // the draw goes on only for panes it shows (0x100, no hidden ancestor, alpha); record those
        const uint32_t fl = *reinterpret_cast<const uint32_t*>(pane + kPaneFlags);
        bool shown = (fl & 0x100) && *reinterpret_cast<const uint8_t*>(pane + kPaneAlpha) != 0;
        for (uint64_t q = pane; shown && q; q = *reinterpret_cast<const uint64_t*>(q + kPaneParent))
            if (*reinterpret_cast<const uint32_t*>(q + kPaneFlags) & 0x40) shown = false;
        if (shown) mark_with_ancestors(pane, frame);
    } else {
        mark_with_ancestors(pane, frame);
    }
    if (!lay || (lay == last_pane_lay && frame == last_frame)) return 0;
    last_pane_lay = lay;
    last_frame = frame;
    drawn_mark(lay, frame);
    return 0;
}

// The game's own visibility rule for a pane (the draw's checks above, without the draw-list membership).
bool pane_shown(uint64_t pane, uint32_t flags) {
    if (flags & 0x4000) return true;
    if (!(flags & 0x100)) return false;
    uint64_t q = pane;
    for (int depth = 0; q && depth < 64; ++depth) {
        if (!ok(q + kPaneFlags, 4)) return false;
        if (rd<uint32_t>(q + kPaneFlags) & 0x40) return false;
        q = ok(q + kPaneParent, 8) ? rd<uint64_t>(q + kPaneParent) : 0;
    }
    return true;
}

bool g_trace = false;
std::atomic<uint64_t> g_quit_guide{0};   // the layout of the title's Quit Game key guide (menu_set_quit_guide)
std::atomic<uint64_t> g_sort_guide{0};   // the layout of the bag screen's Sort key guide (menu_set_sort_guide)
std::atomic<uint64_t> g_craft_guide{0};  // the layout of the crafting screen's Search key guide (menu_set_craft_guide)
bool g_trace_pick = false;      // the scan traced: pick() logs its choice once
#define TRACE(...) do { if (g_trace) NXLOG_I(kTag, "menu chain: " __VA_ARGS__); } while (0)

// One layout's visible panes (layout = placeable +40 -> +16).
void add_layout(uint64_t plc, uint32_t id, uint32_t sub, int trace_n) {
    State& s = st();
    const uint64_t obj = rptr(plc + kPlaceableObj);
    const uint64_t lay = rptr(obj + kObjLayout);
    if (!lay || !ok(lay, kLaySentinel + 8)) {
        if (trace_n < 4) TRACE("    placeable 0x%llx -> obj 0x%llx -> layout 0x%llx: stop (layout not readable)", (unsigned long long)plc,
                               (unsigned long long)obj, (unsigned long long)lay);
        return;
    }
    for (const Layout& L : s.layouts)
        if (L.ptr == lay) return;
    if (draw_hooks_live() && !drawn_recent(lay)) { ++s.n_not_drawn; return; }
    Layout L;
    L.ptr = lay;
    L.id = id;
    L.sub = sub;
    const int n = rd<uint16_t>(lay + kLayPaneCount);
    uint64_t node = rd<uint64_t>(lay + kLayFirstNode);
    const uint64_t first = node;
    const int li = static_cast<int>(s.layouts.size());
    int unreadable = 0, alpha0 = 0, offscreen = 0;
    // the candidates: the panes the draw's own rule shows (flag 0x100 / 0x4000, no hidden ancestor, alpha above 0), each with its draw record
    struct Cand { uint64_t pane; Pane p; int rec; };
    std::vector<Cand> cands;
    const bool live = draw_hooks_live();
    int fresh = 0;
    for (int k = 0; k < n && k < static_cast<int>(kMaxPanes) && node && node != lay + kLaySentinel; ++k) {
        if (!ok(node, 24)) break;
        const uint64_t pane = rd<uint64_t>(node + 16);
        node = rd<uint64_t>(node + 8);
        ++L.panes;
        Pane p;
        if (!pane_rect(pane, &p)) { ++unreadable; continue; }
        if (p.alpha == 0 || !pane_shown(pane, p.flags)) { ++alpha0; continue; }
        if (!live && !(p.flags & 0x20)) { ++alpha0; continue; }     // no hooks (not re-lifted): never went through a draw
        Cand cd{pane, p, kNoRecord};
        if (live) {
            ++s.rec_panes;
            Pane r;
            uint32_t age = 0;
            cd.rec = pane_record(pane, &r, &age);
            if (cd.rec != kNoRecord) ++s.rec_with;
            if (cd.rec == kStale) s.rec_ages.push_back(age);
            if (cd.rec == kFresh) {
                ++fresh;
                cd.p.x0 = r.x0; cd.p.y0 = r.y0; cd.p.x1 = r.x1; cd.p.y1 = r.y1; cd.p.z = r.z;     // the rect it was drawn at
            }
        }
        cands.push_back(cd);
    }
    // With fresh records in this layout, its panes without one (or with an old one) were not drawn: dropped. Without any (a pane class the
    // hooks do not cover, a draw on another cadence), the layout-level filter alone decides, as before the per-pane records.
    // Only when most of the layout's shown panes have records: some pane classes draw without going through the hooked draws (the bag's
    // empty slots: p5k kept 4 of ~70 inventory panes), and dropping their panes would lose the menu.
    const bool refine = live && fresh > 0 && fresh * 2 >= static_cast<int>(cands.size());
    if (live && !refine && !cands.empty()) ++s.rec_fallback_layouts;
    for (Cand& cd : cands) {
        if (refine && cd.rec != kFresh) { ++alpha0; continue; }
        const uint64_t pane = cd.pane;
        Pane& p = cd.p;
        if (p.x1 < 0.f || p.y1 < 0.f || p.x0 > kCanvasW || p.y0 > kCanvasH) { ++offscreen; continue; }     // off screen (or a world-space layout)
        p.ptr = pane;
        p.lay = li;
        p.lay_id = L.id;
        p.lay_sub = L.sub;
        read_str(rd<uint64_t>(pane + kPaneName), rd<uint16_t>(pane + kPaneNameLen), p.name, sizeof p.name);
        char tag[48];
        read_str(rd<uint64_t>(pane + kPaneTag), 0, tag, sizeof tag);
        classify(p, tag);
        ++L.visible;
        s.panes.push_back(p);
    }
    // candidate "drawn" fields, logged so one run with a menu open shows which differs (all inferred, none confirmed): the placeable's
    // atomic flag word (+488, tested by 0x7101fdc1e0), the layout's counters at +744..+756 (+748 = pending textures, 0x7101f55880;
    // +756 feeds pane flag 0x200 in 0x7101f56060), the root pane's flags (+260).
    if (trace_n < 6 && L.visible > 0) {
        const uint64_t pflags = ok(plc + 488, 8) ? rd<uint64_t>(plc + 488) : 0;
        uint32_t lf[4] = {};
        if (ok(lay + 744, 16)) std::memcpy(lf, reinterpret_cast<const void*>(lay + 744), sizeof lf);
        const uint64_t root = ok(first, 24) ? rd<uint64_t>(first + 16) : 0;
        const uint32_t rflags = ok(root + kPaneFlags, 4) ? rd<uint32_t>(root + kPaneFlags) : 0;
        TRACE("    layout %u/%u 0x%llx (placeable 0x%llx, obj 0x%llx): panes %d, visible %d (unreadable %d, alpha 0 %d, off screen %d); "
              "placeable+488 0x%llx, layout+744 %u +748 %u +752 %u +756 %u, root pane flags 0x%x",
              id, sub, (unsigned long long)lay, (unsigned long long)plc, (unsigned long long)obj, L.panes, L.visible, unreadable, alpha0, offscreen,
              (unsigned long long)pflags, lf[0], lf[1], lf[2], lf[3], rflags);
    }
    s.layouts.push_back(L);
}

// A pool's entries {entry** data, u64 count}; false (and the reason in the trace) when the list cannot be read.
bool pool_entries(uint64_t mgr, uint64_t pool, const char* name, uint64_t* data, uint64_t* count) {
    const uint64_t a = mgr + pool + kPoolEntries;
    if (!ok(a, 16)) { TRACE("  pool %s at 0x%llx: stop (entry list header 0x%llx not readable)", name, (unsigned long long)(mgr + pool), (unsigned long long)a); return false; }
    *data = rd<uint64_t>(a);
    *count = rd<uint64_t>(a + 8);
    TRACE("  pool %s at 0x%llx: entries data 0x%llx count %llu", name, (unsigned long long)(mgr + pool), (unsigned long long)*data,
          (unsigned long long)*count);
    if (!*data || *count == 0) { TRACE("  pool %s: stop (empty)", name); return false; }
    if (*count > kMaxPoolEntries || !ok(*data, *count * 8)) { TRACE("  pool %s: stop (count over %llu or data not readable)", name, (unsigned long long)kMaxPoolEntries); return false; }
    return true;
}

// Walks every screen layout the UI manager's pools hold and collects the visible panes.
void scan() {
    State& s = st();
    s.layouts.clear();
    s.panes.clear();
    if (s.pages.size() > 20000) s.pages.clear();
    {
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        g_trace = s.log_level >= 3 && s.log_lines < 4000 && now - last >= std::chrono::seconds(1);
        if (g_trace) { last = now; s.log_lines += 24; }
        g_trace_pick = g_trace;
    }
    s.n_not_drawn = 0;
    s.rec_panes = s.rec_with = s.rec_fallback_layouts = 0;
    s.rec_ages.clear();
    TRACE("drawn set: hooks %s, %llu pane draws so far, frame %u", draw_hooks_live() ? "live" : "silent (not re-lifted? panes need flag 0x20 instead)",
          (unsigned long long)g_draw_calls.load(), g_frame.load());
    const uint64_t gaddr = s.base + kUiMgr;
    const uint64_t mgr = rptr(gaddr);
    TRACE("global 0x%llx (main+0x%llx) readable %d -> UI manager 0x%llx", (unsigned long long)gaddr, (unsigned long long)kUiMgr, ok(gaddr, 8) ? 1 : 0,
          (unsigned long long)mgr);
    if (!mgr) { TRACE("stop (no UI manager)"); g_trace = false; return; }
    uint64_t data = 0, count = 0;
    // pool A: per layout resource, the instances in use (all minus the free ring)
    if (pool_entries(mgr, kPoolA, "A", &data, &count)) {
        int traced = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t e = rd<uint64_t>(data + 8 * i);
            if (!ok(e, 48)) { if (i < 4) TRACE("   A[%llu] entry 0x%llx: stop (not readable)", (unsigned long long)i, (unsigned long long)e); continue; }
            const uint32_t id = rd<uint32_t>(e + 8), sub = rd<uint32_t>(e + 12);
            const uint64_t vec = rd<uint64_t>(e + kEntInstances), ring = rd<uint64_t>(e + kEntFree);
            uint64_t vdata = 0, vcount = 0;
            if (ok(vec, 16)) { vdata = rd<uint64_t>(vec); vcount = rd<uint64_t>(vec + 8); }
            uint64_t free_ptrs[kMaxInstances];
            size_t nfree = 0;
            uint64_t rcount = 0;
            if (ok(ring, 40)) {
                const uint64_t rdata = rd<uint64_t>(ring), rsize = rd<uint64_t>(ring + 8), rcap = rd<uint64_t>(ring + 16), rhead = rd<uint64_t>(ring + 24);
                rcount = rd<uint64_t>(ring + 32);
                if (rdata && rsize && rcap && rcount <= kMaxInstances && rsize <= 4096 && ok(rdata, rsize * 8))
                    for (uint64_t k = 0; k < rcount; ++k) {
                        const uint64_t idx = std::min((rhead + k) % rcap, rsize - 1);      // 0x710087da00's indexing
                        free_ptrs[nfree++] = rd<uint64_t>(rdata + 8 * idx);
                    }
            }
            if (i < 4)
                TRACE("   A[%llu] entry 0x%llx id %u sub %u: instances 0x%llx {data 0x%llx count %llu}, free ring 0x%llx count %llu", (unsigned long long)i,
                      (unsigned long long)e, id, sub, (unsigned long long)vec, (unsigned long long)vdata, (unsigned long long)vcount,
                      (unsigned long long)ring, (unsigned long long)rcount);
            if (!vdata || vcount == 0 || vcount > kMaxInstances || !ok(vdata, vcount * 8)) continue;
            for (uint64_t k = 0; k < vcount; ++k) {
                const uint64_t plc = rd<uint64_t>(vdata + 8 * k);
                if (!plc || std::find(free_ptrs, free_ptrs + nfree, plc) != free_ptrs + nfree) continue;     // free: not on screen
                add_layout(plc, id, sub, traced);
                if (!s.layouts.empty() && s.layouts.back().ptr && s.layouts.back().visible > 0) ++traced;
            }
        }
    }
    // pool B: entries are placeables
    if (pool_entries(mgr, kPoolB, "B", &data, &count)) {
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t e = rd<uint64_t>(data + 8 * i);
            if (!ok(e, 16)) continue;
            add_layout(rd<uint64_t>(e), rd<uint32_t>(e + 8), rd<uint32_t>(e + 12), static_cast<int>(i));
        }
    }
    TRACE("result: %zu layouts drawn (%d pooled ones not drawn skipped), %zu visible panes", s.layouts.size(), s.n_not_drawn, s.panes.size());
    if (g_trace) {
        std::vector<uint32_t> a = s.rec_ages;
        std::sort(a.begin(), a.end());
        TRACE("pane records: %d shown panes in drawn layouts, %d with a record, %d fresh, %zu too old (age min %u median %u max %u frames), "
              "%d layouts on the layout-level fallback, %llu marks dropped (table full), frame %u",
              s.rec_panes, s.rec_with, s.rec_with - static_cast<int>(a.size()), a.size(), a.empty() ? 0u : a.front(), a.empty() ? 0u : a[a.size() / 2],
              a.empty() ? 0u : a.back(), s.rec_fallback_layouts, (unsigned long long)g_pane_table_full.load(), g_frame.load());
    }
    if (g_trace) {
        char ids[900];
        size_t k = 0;
        ids[0] = 0;
        for (const Layout& L : s.layouts) {
            if (k + 24 >= sizeof ids) break;
            const int m = std::snprintf(ids + k, sizeof ids - k, " %u/%u(%d)", L.id, L.sub, L.visible);
            if (m > 0) k += static_cast<size_t>(m);
        }
        TRACE("drawn layouts id/sub(visible panes):%s", ids);
        // every visible pane of the drawn layouts, once each time their number changes (names for the cursor / entry rules)
        static size_t dumped = static_cast<size_t>(-1);
        if (s.panes.size() != dumped && s.panes.size() <= 400) {
            dumped = s.panes.size();
            for (const Pane& p : s.panes)
                TRACE("  pane %u/%u '%s'%s%s rect %.0f,%.0f..%.0f,%.0f", p.lay_id, p.lay_sub, p.name, p.item ? " [item]" : "", p.cursor ? " [cursor]" : "",
                      p.x0, p.y0, p.x1, p.y1);
        }
    }
    g_trace = false;
}

// The entry under the pointer, the cursor and the entry it marks.
bool advance_mark(const Pane& p) {
    // a message box's "next" arrow is a cursor pane with no choice behind it (p5d: 'CL_0000_parts_cursor_triangle' over '0000_btn_common_1')
    std::string n = p.name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return has(n, "triangle");
}

// Menus outside play (title, settings, language, save and yes / no dialogs) build their entries from parts, and in KTGL each part instance is
// a layout of its own in the pool (the bag's triangle 4148535219/43 is one too). Pane names come from the layout data only: main holds no
// pane-name string at all (no "cursor", "Win_", "btn_" bytes in the image), the screens are driven by their scripts, and no selection field
// could be read from code, so the selection is still read off the cursor panes. Two shapes the cursor rule above misses (from the
// settings / language layouts listed by p5d / p5e, menu_log 2):
//   * settings: the list 434523034 holds 'CL_0020_parts_setting_menu_btn_0..3'; each button is a part layout (2408049848) with its own
//     'Win_cursor' highlight and entries ('Null_type_1tab', 'CL_0000_parts_btn_choices' ...). The cursor rule picks an entry of that part,
//     and hover then stays inside the one button. Rule 1 (promotion): when the chosen entry's layout is small (at most 4 entries) and the
//     entry sits inside an entry of another list layout, the selection is that list entry (the smallest holding the entry's centre), so the
//     hover covers the whole list. Also, an entry chosen by a non-triangle cursor that has a smaller entry of its layout under the cursor's
//     centre gives way to it (a list frame 'posi_list' around the buttons).
//   * yes / no, language and save choices: each choice is an instance of one button layout ('Null_btn', '0000_btn_common_N', and the
//     language's '0030_prt_language_1'), the selected one showing 'CL_0000_parts_cursor_triangle' at its left; the cursor rule takes that
//     triangle for a message box's advance arrow (under 3 entries in its layout). Rule 2 (siblings): when no cursor was found, a cursor
//     whose layout has at least one other drawn instance (same layout id) is the selection; the entries are each instance's largest entry
//     (they must not all sit on one spot), and hover / steps move between them. Draw hooks only (pooled templates all look visible).
// The in-game menus seen so far (bag: 41 entries, chat choices: 8) never reach either rule, so their path is unchanged.
bool ignored_layout(const Pane& p) {
    const State& s = st();
    return !s.ig_ignore.empty() && s.ig_ignore.count(static_cast<uint64_t>(p.lay_id) << 32 | p.lay_sub);
}

void pick_extend(const std::vector<int>& items, float big) {
    State& s = st();
    s.mode = 0;
    s.group.clear();
    const int nl = static_cast<int>(items.size());
    if (s.focused >= 0 && s.cursor >= 0) {
        const Pane& cp = s.panes[s.cursor];
        // a frame entry around several entries ('posi_list'): the entry of that layout under the cursor's centre
        if (!advance_mark(cp)) {
            const Pane& f = s.panes[s.focused];
            int inner = 0, under = -1;
            for (size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (static_cast<int>(i) == s.focused || !p.item || p.lay != f.lay || p.area() >= f.area()) continue;
                if (!f.contains(p.cx(), p.cy())) continue;
                ++inner;
                if (p.contains(cp.cx(), cp.cy()) && (under < 0 || p.area() < s.panes[under].area())) under = static_cast<int>(i);
            }
            if (inner >= 2 && under >= 0) s.focused = under;
        }
        // rule 1: the chosen entry belongs to a part (a small layout) placed in an entry of a list layout
        const Pane& f = s.panes[s.focused];
        if (f.lay >= 0 && f.lay < nl && items[f.lay] <= 4) {
            int up = -1;
            for (size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (!p.item || p.area() > big || p.lay < 0 || p.lay >= nl || p.lay == f.lay || items[p.lay] < 2) continue;
                if (!p.contains(f.cx(), f.cy()) || f.area() > p.area() * 1.5f) continue;
                if (up < 0 || p.area() < s.panes[up].area()) up = static_cast<int>(i);
            }
            if (up >= 0) { s.focused = up; s.mode = 1; }
        }
        return;
    }
    // rule 2: sibling instances of one button layout, the cursor in one of them
    if (!draw_hooks_live()) return;
    std::vector<int> rep(s.layouts.size(), -1);       // each layout's largest entry
    for (size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        if (!p.item || p.area() > big || p.lay < 0 || p.lay >= static_cast<int>(rep.size())) continue;
        if (rep[p.lay] < 0 || p.area() > s.panes[rep[p.lay]].area()) rep[p.lay] = static_cast<int>(i);
    }
    float best = 160.f;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big || cp.lay < 0 || cp.lay >= static_cast<int>(rep.size()) || ignored_layout(cp)) continue;
        const int mine = rep[cp.lay];
        if (mine < 0) continue;
        const uint32_t id = s.layouts[cp.lay].id;
        std::vector<int> g;
        int apart = 0;
        for (size_t li = 0; li < s.layouts.size(); ++li) {
            if (s.layouts[li].id != id || rep[li] < 0) continue;
            g.push_back(rep[li]);
            const Pane& q = s.panes[rep[li]];
            if (std::fabs(q.cx() - s.panes[mine].cx()) > 8.f || std::fabs(q.cy() - s.panes[mine].cy()) > 8.f) ++apart;
        }
        if (g.size() < 2 || apart == 0) continue;
        const float d = s.panes[mine].dist(cp.cx(), cp.cy());
        if (d > best) continue;
        best = d;
        s.cursor = static_cast<int>(c);
        s.focused = mine;
        s.group = std::move(g);
        s.mode = 2;
    }
}

// The game's cursor: a visible cursor pane (not a message box's advance arrow, not in an ignored layout) whose own layout has at least two
// entries, one of them within 160 px; of those, the closest. The entry under the pointer only counts in the cursor's layout (pooled layouts
// that are not drawn still look visible: their panes keep alpha 255 and a rectangle).
void pick(float px, float py, bool inside) {
    State& s = st();
    s.hovered = s.focused = s.cursor = s.slider = -1;
    const float big = kCanvasW * kCanvasH * 0.5f;
    std::vector<int> items(s.layouts.size(), 0);
    for (const Pane& p : s.panes)
        if (p.item && p.area() <= big && p.lay >= 0 && p.lay < static_cast<int>(items.size())) ++items[p.lay];
    float best = 1e30f;
    int rejected_arrow = 0, rejected_lone = 0;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big) continue;
        if (!s.ig_ignore.empty() && s.ig_ignore.count(static_cast<uint64_t>(cp.lay_id) << 32 | cp.lay_sub)) continue;
        if (cp.lay < 0 || cp.lay >= static_cast<int>(items.size())) { ++rejected_lone; continue; }
        // The cursor's own layout holds the entries, or (only with the draw hooks live, so every layout here is on screen) the cursor is a
        // layout of its own over another drawn layout's list: the bag's triangle (615988817/11) sits on slot 0 of the inventory 4084873301/154.
        const bool own = items[cp.lay] >= 2;
        if (!own && !draw_hooks_live()) { ++rejected_lone; continue; }
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.item || p.area() > big || p.lay < 0) continue;
            if (own ? p.lay != cp.lay : items[p.lay] < 2) continue;
            // a triangle is a message box's advance arrow unless it marks an entry of a real list (3+ entries)
            if (advance_mark(cp) && items[p.lay] < 3) continue;
            // a triangle pointing down ('..._180_..', the bag) sits above its entry: its tip, a quarter of its height below the bottom, is
            // where it points; ties between overlapping slots go to the nearer centre
            const bool down = advance_mark(cp) && has(std::string(cp.name), "180");
            const float qx = cp.cx(), qy = down ? cp.y1 + (cp.y1 - cp.y0) * 0.25f : cp.cy();
            const float d = p.dist(qx, qy) + 0.001f * std::hypot(p.cx() - qx, p.cy() - qy);
            // on a tie (the cursor over a row container and its slot) the smaller entry wins
            if (d < best || (d == best && s.focused >= 0 && p.area() < s.panes[s.focused].area())) {
                best = d;
                s.cursor = static_cast<int>(c);
                s.focused = static_cast<int>(i);
            }
        }
        if (advance_mark(cp) && (s.cursor != static_cast<int>(c))) ++rejected_arrow;
    }
    if (best > 160.f) s.cursor = s.focused = -1;
    pick_extend(items, big);
    if (inside && s.mode == 2) {
        float hb = 1e30f;
        for (int i : s.group) {
            const Pane& p = s.panes[i];
            if (p.contains(px, py) && p.area() < hb) { hb = p.area(); s.hovered = i; }
        }
    } else if (inside) {
        float hb = 1e30f;
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.contains(px, py)) continue;
            if (s.focused >= 0 && p.lay != s.panes[s.focused].lay) continue;   // the entries' layout (may differ from the cursor's)
            if (p.slider && s.slider < 0) s.slider = static_cast<int>(i);
            if (!p.item || p.area() > big) continue;
            if (p.area() < hb) { hb = p.area(); s.hovered = static_cast<int>(i); }
        }
    }
    if (g_trace_pick) {
        g_trace_pick = false;
        if (s.cursor >= 0) {
            const Pane& c = s.panes[s.cursor];
            NXLOG_I(kTag, "menu chain: chosen layout %u/%u (cursor '%s' on '%s', %d entries in it); skipped %d advance arrows, %d cursors in layouts "
                    "with under 2 entries", c.lay_id, c.lay_sub, c.name, s.panes[s.focused].name, items[c.lay], rejected_arrow, rejected_lone);
        } else {
            NXLOG_I(kTag, "menu chain: no cursor chosen; skipped %d advance arrows, %d cursors in layouts with under 2 entries", rejected_arrow,
                    rejected_lone);
        }
    }
}

void log_layouts() {
    State& s = st();
    uint64_t sig = 0;
    int vis = 0, items = 0, cursors = 0;
    for (const Layout& L : s.layouts)
        if (L.visible) sig = sig * 1000003ull + (static_cast<uint64_t>(L.id) << 32 | L.sub) + 1;
    for (const Pane& p : s.panes) { ++vis; items += p.item; cursors += p.cursor; }
    s.n_layouts.store(static_cast<uint32_t>(s.layouts.size()), std::memory_order_relaxed);
    s.n_panes.store(static_cast<uint32_t>(vis), std::memory_order_relaxed);
    s.n_items.store(static_cast<uint32_t>(items), std::memory_order_relaxed);
    const bool opened = !s.was_up;
    if ((opened || sig != s.layout_sig) && log_ok(opened)) {
        NXLOG_I(kTag, "menu: %s, %zu layouts (%zu with visible panes), %d visible panes, %d entries, %d cursors", opened ? "open" : "changed",
                s.layouts.size(), static_cast<size_t>(std::count_if(s.layouts.begin(), s.layouts.end(), [](const Layout& L) { return L.visible > 0; })),
                vis, items, cursors);
    }
    s.layout_sig = sig;
    if (s.log_level < 2) return;
    // [pkp] menu_log = 2: each layout's visible panes, once per layout
    for (size_t li = 0; li < s.layouts.size(); ++li) {
        const Layout& L = s.layouts[li];
        const uint64_t key = static_cast<uint64_t>(L.id) << 32 | L.sub;
        if (!L.visible || s.dumped.count(key) || s.dumped.size() > 200 || s.log_lines > 4000) continue;
        s.dumped.insert(key);
        NXLOG_I(kTag, "menu: layout id=%u sub=%u ptr=0x%llx panes=%d visible=%d", L.id, L.sub, static_cast<unsigned long long>(L.ptr), L.panes,
                L.visible);
        int shown = 0;
        for (const Pane& p : s.panes) {
            if (p.lay != static_cast<int>(li) || shown >= 60) continue;
            ++shown;
            ++s.log_lines;
            NXLOG_I(kTag, "menu:   '%s'%s%s%s rect %.0f,%.0f..%.0f,%.0f z=%.1f a=%u flags=0x%x", p.name, p.item ? " [entry]" : "",
                    p.cursor ? " [cursor]" : "", p.slider ? " [slider]" : "", p.x0, p.y0, p.x1, p.y1, p.z, p.alpha, p.flags);
        }
    }
}

}  // namespace

void menu_install(uint64_t base) {
    State& s = st();
    s.base = base;
    s.enabled = config_get_bool("pkp", "menu_mouse", true);
    s.log_level = static_cast<int>(config_get_int("pkp", "menu_log", 1));
    s.ig_enabled = config_get_bool("pkp", "menu_in_game", true);     // on: drawn-layout detection confirmed with the bag (p5m)
    s.ig_ignore.clear();
    {   // "id/sub id/sub ..." (the ids the "in-game menu up" log line prints)
        const std::string list = config_get_str("pkp", "menu_in_game_ignore", "");
        unsigned id = 0, sub = 0;
        int used = 0;
        for (const char* q = list.c_str(); *q;) {
            if (std::sscanf(q, " %u/%u%n", &id, &sub, &used) == 2 && used > 0) {
                s.ig_ignore.insert(static_cast<uint64_t>(id) << 32 | sub);
                q += used;
            } else {
                ++q;
            }
        }
    }
    s.key_guides.clear();
    {   // "id=act ..." (act names as in the [switch] section: btn_minus, btn_plus, btn_x ...). 881468973 = the title's "(-) Settings" guide
        // (p5m: 'Win_000_btn_key_guide_0' at 1722,1003..1920,1075; gameplay's HUD guides use the layout too, see key_guide_drawn); "" turns them off
        const std::string list = config_get_str("pkp", "menu_key_guides", "881468973=btn_minus");  // gitleaks:allow (a layout id, not a key)
        char name[32];
        unsigned id = 0;
        int used = 0;
        for (const char* q = list.c_str(); *q;) {
            play::Act a;
            if (std::sscanf(q, " %u=%31[A-Za-z0-9_]%n", &id, name, &used) == 2 && used > 0) {
                if (play::parse_act(name, &a)) s.key_guides[id] = a;
                else NXLOG_W(kTag, "menu_key_guides: unknown action '%s'", name);
                q += used;
            } else {
                ++q;
            }
        }
    }
    for (const DrawSite& d : kDrawSites)
        g_draw_hooks.push_back(hook_add_mid(base + d.off, &hook_pane_draw, reinterpret_cast<void*>(static_cast<uintptr_t>(d.reg))));
    s.installed = true;
    NXLOG_I(kTag, "menu mouse %s (the wheel and keys move the selection, a left click confirms the selected entry, the pointer does not pick; [pkp] menu_log %d; 3 = the layout chain once a second)", s.enabled ? "on" : "off", s.log_level);
}

void menu_remove() {
    for (uint32_t id : g_draw_hooks) hook_remove(id);
    g_draw_hooks.clear();
    st().installed = false;
    st().ig_up = false;
}

// While the game is in play its menus (bag, crafting, shops, choice boxes) are KTGL layouts over the world and the game keeps calling its
// input function, so play mode cannot tell them from play. A layout with the game's selection cursor on an entry (pick's rule: a visible
// "...cursor..." pane within 160 px of an "item" pane) is such a menu. Closed: one scan every kPollFrames frames; up: menu_frame scans
// every frame and its result is reused. Debounced: up after kOpenHits positive polls in a row, down after kCloseFrames negative frames.
// A drawn key guide layout from [pkp] menu_key_guides (the title's "(-) Settings"): a screen with no selection cursor that still takes clicks.
// Gameplay's HUD draws the same key guide layout (Run / Bag / Focus), so it counts only while the follow camera is not running (the title).
bool key_guide_drawn(const State& s) {
    if (s.key_guides.empty() || play_follow_live()) return false;
    for (const Layout& L : s.layouts)
        if (L.visible > 0 && s.key_guides.count(L.id)) return true;
    return false;
}

bool menu_in_game(bool in_game) {
    State& s = st();
    constexpr int kPollFrames = 6, kOpenHits = 2, kCloseFrames = 10;
    g_frame.fetch_add(1, std::memory_order_relaxed);     // the drawn set's clock (once per play-mode frame)
    const bool scanned = s.frame_scanned, scanned_present = s.frame_present;
    s.frame_scanned = false;
    if (s.log_level >= 3) {   // once a second: why the menu code does or does not look at this frame
        static uint32_t n = 0;
        if (++n % 600 == 0) NXLOG_I(kTag, "menu: poll in_game %d installed %d enabled %d in_game_menus %d up %d layouts %zu", in_game ? 1 : 0, s.installed ? 1 : 0, s.enabled ? 1 : 0, s.ig_enabled ? 1 : 0, s.ig_up ? 1 : 0, s.layouts.size());
    }
    if (!in_game || !s.installed || !s.enabled || !s.ig_enabled) {
        if (s.ig_up && log_ok(true)) NXLOG_I(kTag, "menu: in-game menu closed (layout %u/%u, the game left play)", s.ig_lay_id, s.ig_lay_sub);
        s.ig_up = false;
        s.ig_hits = s.ig_miss = s.ig_tick = 0;
        return false;
    }
    bool present;
    if (s.ig_up && scanned) {
        present = scanned_present;
    } else {
        if (!s.ig_up && ++s.ig_tick < kPollFrames) return false;
        s.ig_tick = 0;
        s.pages.clear();
        scan();
        pick(-1.f, -1.f, false);
        if (s.log_level >= 3) log_layouts();   // what play-time screens (the title too) draw
        present = (s.cursor >= 0 && s.focused >= 0) || key_guide_drawn(s);
    }
    if (!s.ig_up) {
        s.ig_hits = present ? s.ig_hits + 1 : 0;
        if (s.ig_hits >= kOpenHits) {
            s.ig_up = true;
            s.ig_miss = 0;
            ++s.n_ig_opens;
            if (s.cursor < 0 || s.focused < 0) {   // a key guide screen (title): no cursor
                s.ig_lay_id = s.ig_lay_sub = 0;
                std::snprintf(s.ig_cursor, sizeof s.ig_cursor, "%s", "(key guide)");
                if (log_ok(true)) NXLOG_I(kTag, "menu: in-game menu up: a key guide screen with no cursor (%zu layouts); clicks go to the key guides, else A", s.layouts.size());
                return s.ig_up;
            }
            const Pane& c = s.panes[s.cursor];
            const Pane& f = s.panes[s.focused];
            s.ig_lay_id = c.lay_id;
            s.ig_lay_sub = c.lay_sub;
            std::snprintf(s.ig_cursor, sizeof s.ig_cursor, "%s", c.name);
            if (log_ok(true))
                NXLOG_I(kTag,
                        "menu: in-game menu up: layout %u/%u (cursor '%s' on '%s', entry layout %u/%u), %zu layouts, %zu visible panes; "
                        "the mouse is the menu pointer ([pkp] menu_in_game_ignore = %u/%u if this is not a menu)",
                        c.lay_id, c.lay_sub, c.name, f.name, f.lay_id, f.lay_sub, s.layouts.size(), s.panes.size(), c.lay_id, c.lay_sub);
        }
    } else {
        s.ig_miss = present ? 0 : s.ig_miss + 1;
        if (s.ig_miss >= kCloseFrames) {
            s.ig_up = false;
            s.ig_hits = 0;
            if (log_ok(true)) NXLOG_I(kTag, "menu: in-game menu closed (layout %u/%u, cursor '%s')", s.ig_lay_id, s.ig_lay_sub, s.ig_cursor);
        }
    }
    return s.ig_up;
}

play::Act menu_frame(bool menu_up, const play::mm::Geometry& g) {
    State& s = st();
    play::mm::Pointer& ptr = play::mm::pointer();
    const play::mm::Snapshot snap = ptr.begin_frame(menu_up, g);
    s.active = false;
    if (s.log_level >= 3) {
        static uint32_t n = 0;
        if (++n % 600 == 0) NXLOG_I(kTag, "menu: frame menu_up %d active %d inside %d shown %d", menu_up ? 1 : 0, snap.active ? 1 : 0, snap.inside ? 1 : 0, snap.shown ? 1 : 0);
    }
    if (!s.installed || !s.enabled || !snap.active) {
        if (s.was_up && log_ok(true)) NXLOG_I(kTag, "menu: closed");
        s.was_up = false;
        s.panes.clear();
        s.hovered = s.focused = s.cursor = s.slider = -1;
        return play::Act::None;
    }
    s.active = true;
    s.pages.clear();
    scan();
    // the UI plane: Pokopia keeps its menus and HUD 16:9 and centred on wider shapes (p5l at 21:9: the whole-picture mapping hit one slot left)
    const float px = (snap.nx + 1.f) * 0.5f * kCanvasW, py = (1.f - snap.ny) * 0.5f * kCanvasH;
    pick(px, py, snap.inside && snap.shown);
    s.frame_scanned = true;
    s.frame_present = (s.cursor >= 0 && s.focused >= 0) || key_guide_drawn(s);
    log_layouts();
    s.was_up = true;
    {   // which rule found the selection (pick_extend), once per change: the proof for menus outside play
        const Pane* c = s.cursor >= 0 ? &s.panes[s.cursor] : nullptr;
        const Pane* f = s.focused >= 0 ? &s.panes[s.focused] : nullptr;
        const uint64_t sig = c && f ? (static_cast<uint64_t>(s.mode) << 62) ^ (static_cast<uint64_t>(c->lay_id) << 32) ^ f->lay_id ^
                                          (static_cast<uint64_t>(s.group.size()) << 48)
                                    : 0;
        if (sig != s.mode_sig && c && f && log_ok(true)) {
            static const char* const kMode[] = {"cursor", "list entry holding the cursor's part", "sibling button layouts"};
            NXLOG_I(kTag, "menu: selection by %s: cursor '%s' (layout %u/%u) on '%s' (layout %u/%u), %zu sibling entries", kMode[s.mode], c->name,
                    c->lay_id, c->lay_sub, f->name, f->lay_id, f->lay_sub, s.group.size());
        }
        s.mode_sig = sig;
    }

    const Pane* hov = s.hovered >= 0 ? &s.panes[s.hovered] : nullptr;
    const Pane* foc = s.focused >= 0 ? &s.panes[s.focused] : nullptr;
    const uint64_t hov_ptr = hov ? hov->ptr : 0, foc_ptr = foc ? foc->ptr : 0;
    if (hov_ptr != s.last_hover) {
        std::snprintf(s.hover_name, sizeof s.hover_name, "%s", hov ? hov->name : "");
        if (log_ok()) {
            if (hov) NXLOG_I(kTag, "menu: hover '%s' (layout %u/%u, drawn rect %.0f,%.0f..%.0f,%.0f) at %.0f,%.0f; selected '%s'", hov->name, hov->lay_id,
                             hov->lay_sub, hov->x0, hov->y0, hov->x1, hov->y1, px, py, foc ? foc->name : "");
            else NXLOG_I(kTag, "menu: hover nothing at %.0f,%.0f", px, py);
        }
        s.last_hover = hov_ptr;
    }
    if (foc_ptr != s.last_focus) {
        std::snprintf(s.focus_name, sizeof s.focus_name, "%s", foc ? foc->name : "");
        if (foc && log_ok()) NXLOG_I(kTag, "menu: selected '%s' (cursor '%s')", foc->name, s.cursor >= 0 ? s.panes[s.cursor].name : "");
        s.last_focus = foc_ptr;
    }

    // Left click: a key guide under the pointer ("(-) Settings" on the title) sends its button, any other click is A on the game's own
    // selection. The pointer never moves the selection: the selected entry is only guessed from where the cursor part is drawn, and in
    // grids with a bobbing cursor (the main menu's triangle over CL_0300_parts_btn_mainmenu_0000..0008) the guess jumped between entries,
    // so d-pad steps toward the hovered entry started from the wrong one and sent the selection around the menu (fork issue #7).
    // totk-pc-recomp keeps the mouse out of its game's menus the same way. The selection moves with the wheel and the keys.
    if (!snap.click) return play::Act::None;
    ptr.click_used();
    ++s.n_clicks;
    if (snap.inside && snap.shown && !s.key_guides.empty()) {
        for (const Pane& p : s.panes) {
            if (!p.item || !p.contains(px, py)) continue;
            // the title's Quit Game is a second instance of the same key guide layout: its own instance sends (+) (pkp_title.cpp). Only
            // while the title's set is shown: the widget reuses its part instances for later sets
            const uint64_t quit = g_quit_guide.load(std::memory_order_relaxed);
            if (quit && title_up() && p.lay >= 0 && p.lay < static_cast<int>(s.layouts.size()) && s.layouts[p.lay].ptr == quit) {
                if (log_ok(true)) NXLOG_I(kTag, "menu: click on the title's Quit Game key guide -> btn_plus");
                return play::Act::BtnPlus;
            }
            // the bag screen's Sort is another instance of the key guide layout: its own instance sorts (pkp_sort.cpp). Only while the
            // screen shows the Sort entry: the widget reuses its part instances for other sets, whose clicks go on as below
            const uint64_t sort = g_sort_guide.load(std::memory_order_relaxed);
            if (sort && p.lay >= 0 && p.lay < static_cast<int>(s.layouts.size()) && s.layouts[p.lay].ptr == sort && sort_click()) {
                if (log_ok(true)) NXLOG_I(kTag, "menu: click on the Sort key guide -> inventory sort");
                return play::Act::None;
            }
            // the crafting screen's Search, the same way (pkp_craft.cpp): its own instance asks for a word
            const uint64_t search = g_craft_guide.load(std::memory_order_relaxed);
            if (search && p.lay >= 0 && p.lay < static_cast<int>(s.layouts.size()) && s.layouts[p.lay].ptr == search && craft_click()) {
                if (log_ok(true)) NXLOG_I(kTag, "menu: click on the Search key guide -> crafting search");
                return play::Act::None;
            }
            const auto it = s.key_guides.find(p.lay_id);
            if (it == s.key_guides.end()) continue;
            if (log_ok(true)) NXLOG_I(kTag, "menu: click on key guide '%s' (layout %u/%u) -> %s", p.name, p.lay_id, p.lay_sub, play::act_info(it->second).name);
            return it->second;
        }
    }
    if (log_ok(true)) NXLOG_I(kTag, "menu: click -> A on '%s'", foc ? foc->name : "");
    return play::Act::BtnA;
}

bool menu_over_value() { return st().active && st().slider >= 0; }

bool menu_active() { return st().active; }

bool menu_open() { return st().active && st().cursor >= 0; }

void menu_set_quit_guide(uint64_t layout) { g_quit_guide.store(layout, std::memory_order_relaxed); }
void menu_set_sort_guide(uint64_t layout) { g_sort_guide.store(layout, std::memory_order_relaxed); }
void menu_set_craft_guide(uint64_t layout) { g_craft_guide.store(layout, std::memory_order_relaxed); }

void menu_status_text(char* buf, size_t n) {
    const State& s = st();
    std::snprintf(buf, n, " menu_layouts=%u menu_panes=%u menu_entries=%u hover='%s' selected='%s' clicks=%u in_game_menu=%d (%u/%u) in_game_opens=%u",
                  s.n_layouts.load(), s.n_panes.load(), s.n_items.load(), s.hover_name, s.focus_name, s.n_clicks.load(),
                  s.ig_up ? 1 : 0, s.ig_lay_id, s.ig_lay_sub, s.n_ig_opens.load());
}

}  // namespace nxrt::games::pkp
