// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia, title screen: a "Quit Game" key guide next to "(-) Settings" (issue #319). [pkp] title_quit, on by default.
//
// The title's prompts are the game's key guide widget (one per player, manager [0x3483fb8], ctor 0x7100ec4958): 0x7100e60be4 (manager,
// player, set) gives it a key guide set, and 0x7100ec5de0 (widget, set) fills it: a set is a row of the data table at [0x35618f0] +0x1048
// holding up to six key guide ids (u16, 1..399). Each id is a row of the table at +0x1050: {u16 action, ff, 00, u8 text, 7f, ..} (the
// action picks the button icon, the text byte is an entry of the key guide text bank). The widget lays out one instance of the key guide
// part (layout 881468973: icon '000_key_1', texts 'TextBox_0000/0001') per id and the area 'CL_0000_area_btn_keyguide' of the title layout
// places them from the right. The title uses set 152 = {233}: action 0x80 (the (-) button) and "Settings".
//   * entry 0x7100ec5de0, set 152: the set's second id becomes key guide 267 (action 4, the (+) button, "Credits"; the game's only other
//     user is set 183 = {233, 267}, the title with a Credits prompt, which is left alone), so the widget builds a second prompt in the
//     game's own style, left of Settings. Written into the table row once.
//   * inline 0x7100ec69b4, the end of the part's fill (0x7100ec6800, a vtable call from 0x7100ec54a0 whose x19 is the widget: saved at
//     [x29 + 0x38]): for that key guide on the title widget its text is replaced with "Quit Game" through text 0xa8 ("<string;0(lstr1)>",
//     0x7100f35f20 (part, text slot w20, 0xa8, utf8)). x21 = the key guide's table row, x19 = the part.
// The title itself takes no (+) (A, B, the d-pad and (-) only), so the button is free there: once a frame (main thread) while the title's
// widget shows set 152 (+0xf0 = the set, +0x90 = state 1 or 2 while shown), a press of (+) on any pad as the hid service samples it
// (the keyboard's Escape by default) opens the runtime's "Close the game?" prompt. A click on the prompt sends (+) too (pkp_menu.cpp,
// menu_set_quit_guide).
#include "pkp_title.h"

#include <cstring>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "pkp_menu.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kFillSet = 0xec5de0;        // entry: (widget, set id): the key guide widget takes a set
constexpr uint64_t kPartFilled = 0xec69b4;     // inline: a key guide part has its icon and text (x19 part, x21 row, w20 text slot)
constexpr uint64_t kTextString = 0xf35f20;     // (widget, slot, text id, const char*)
constexpr uint64_t kLayoutOf = 0x9132fc;       // (source) -> layout
constexpr uint64_t kTables = 0x35618f0;        // qword: the data-table manager
constexpr uint64_t kLanguage = 0x36ca680;      // qword -> u32 language 0..8
constexpr uint64_t kSetTable = 0x1048, kGuideTable = 0x1050, kTableVersion = 0x2b70;
constexpr uint64_t kWidgetSet = 0xf0, kWidgetState = 0x90;
constexpr uint32_t kTitleSet = 152, kQuitGuide = 267, kStringText = 0xa8;
constexpr int kSetSlots = 6;

// "Quit Game" in the game's languages (0..8, the order of its text banks: the key guide bank's own "Settings" / "Quit" words checked)
constexpr const char* kLabel[9] = {
    "ゲームをやめる", "Quit Game", "Quitter le jeu", "Esci dal gioco", "Spiel beenden", "Salir del juego", "結束遊戲", "게임 종료", "结束游戏",
};

struct State {
    uint64_t base = 0;
    play::GameHost* host = nullptr;
    uint64_t widget = 0;        // the title's key guide widget (set 152 seen last on it)
    uint64_t row = 0;           // key guide 267's table row
    bool patched = false;
    uint64_t prev_buttons = 0;
} g;

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }
bool ok(uint64_t a, uint64_t n) { return a >= 0x1000000000ull && mem::is_mapped(a, n); }

uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t sp = 0) {
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    if (sp) c->x[31] = sp;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// A row of one of the manager's data tables, as the game looks it up (table->vtable[0x38](0)->vtable[0x30](id)); 0 if none.
uint64_t table_row(::Ctx* c, uint64_t table_off, uint32_t id) {
    const uint64_t mgr = rd<uint64_t>(g.base + kTables);
    if (!mgr || !ok(mgr + kTableVersion, 4) || rd<uint32_t>(mgr + kTableVersion) <= 0x20a) return 0;
    const uint64_t t = rd<uint64_t>(mgr + table_off + 0x10);
    if (!t) return 0;
    const uint64_t sub = call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x38), t, 0);
    if (!sub) return 0;
    return call(c, rd<uint64_t>(rd<uint64_t>(sub) + 0x30), sub, id);
}

int hook_fill_set(::Ctx* c, uint64_t, void*) {
    const uint64_t widget = c->x[0];
    const uint32_t set = static_cast<uint32_t>(c->x[1] & 0xff);
    if (set != kTitleSet) {
        if (widget == g.widget) g.widget = 0;
        return 0;
    }
    g.widget = widget;
    if (g.patched) return 0;
    g.patched = true;
    const uint64_t srow = table_row(c, kSetTable, kTitleSet);
    const uint64_t ids = srow ? rd<uint64_t>(srow + 8) : 0;
    g.row = table_row(c, kGuideTable, kQuitGuide);
    if (!ids || !ok(ids, 2 * kSetSlots) || !g.row) {
        NXLOG_W(kTag, "title quit: the title's key guide set or key guide %u not found, no Quit Game prompt", kQuitGuide);
        return 0;
    }
    for (int i = 0; i < kSetSlots; ++i) {
        const uint16_t v = rd<uint16_t>(ids + 2 * i);
        if (v == kQuitGuide) break;
        if (v == 0) {
            wr<uint16_t>(ids + 2 * i, static_cast<uint16_t>(kQuitGuide));
            NXLOG_I(kTag, "title quit: key guide %u added to the title's set %u (slot %d): (+) Quit Game", kQuitGuide, kTitleSet, i);
            break;
        }
    }
    return 0;
}

void hook_part_filled(::Ctx* c, uint64_t, void*) {
    if (!g.widget || !g.row || c->x[21] != g.row) return;
    const uint64_t fp = c->x[29];
    if (!ok(fp + 0x38, 8) || rd<uint64_t>(fp + 0x38) != g.widget) return;     // the caller's (0x7100ec54a0) x19: this part's widget
    const uint64_t part = c->x[19];
    const uint32_t slot = static_cast<uint32_t>(c->x[20]);
    const uint64_t lp = rd<uint64_t>(g.base + kLanguage);
    const uint32_t lang = lp && ok(lp, 4) ? rd<uint32_t>(lp) : 1;
    const char* s = kLabel[lang < 9 ? lang : 1];
    const size_t n = std::strlen(s);
    const uint64_t buf = (c->x[31] - (n + 0x40)) & ~uint64_t{0xf};     // below the guest stack pointer: the game reads it as guest memory
    std::memcpy(reinterpret_cast<void*>(buf), s, n + 1);
    call(c, g.base + kTextString, part, slot, kStringText, buf, buf - 0x10);
    // the part's layout, for the menu pointer: a click on it is (+)
    const uint64_t src = rd<uint64_t>(part + 0x38);
    const uint64_t lay = src ? call(c, g.base + kLayoutOf, src, 0, 0, 0, buf - 0x10) : 0;
    menu_set_quit_guide(lay);
    NXLOG_I(kTag, "title quit: prompt text set (part 0x%llx, slot %u, language %u, layout 0x%llx)", static_cast<unsigned long long>(part), slot, lang,
            static_cast<unsigned long long>(lay));
}

}  // namespace

void title_install(uint64_t base, play::GameHost* host) {
    if (!config_get_bool("pkp", "title_quit", true)) {
        NXLOG_I(kTag, "title quit: off ([pkp] title_quit)");
        return;
    }
    g.base = base;
    g.host = host;
    hook_add_entry(base + kFillSet, &hook_fill_set);
    hook_add_inline(base + kPartFilled, &hook_part_filled);
    NXLOG_I(kTag, "title quit: on (a (+) Quit Game prompt next to (-) Settings on the title; hooks main+0x%llx, +0x%llx)",
            static_cast<unsigned long long>(kFillSet), static_cast<unsigned long long>(kPartFilled));
}

bool title_up() {
    const uint64_t w = g.widget;
    if (!w || !ok(w + kWidgetSet, 8)) return false;
    return rd<uint8_t>(w + kWidgetSet) == kTitleSet && rd<int32_t>(w + kWidgetState) - 1u < 2u;
}

void title_frame() {
    if (!g.host) return;
    const uint64_t buttons = hid::sampled_buttons();
    const uint64_t plus = hid::btn(hid::kBtnPlus);
    const bool press = (buttons & plus) && !(g.prev_buttons & plus);
    g.prev_buttons = buttons;
    if (!press || !title_up()) return;
    NXLOG_I(kTag, "title quit: (+) on the title, the Close the game? prompt opens");
    g.host->request_quit_prompt();
}

}  // namespace nxrt::games::pkp
