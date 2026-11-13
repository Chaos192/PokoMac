// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay UI (cheats_ui.h): the shell. One panel over a dimmed game with four pages, simple first (Cheats, Find a value, My
// table, Advanced), a button-hint bar, toasts with undo, confirmations, the saved table and layout, hotkeys, the frame callback that
// drives hotkeys / Lua / value graphs, and the mini strip. The look is cheats_theme.cpp's; the pages and tools are in cheats_ui_scan.cpp,
// cheats_ui_mem.cpp, cheats_ui_tools.cpp, the on-screen keyboard in cheats_ui_osk.cpp. Talks only to the cheats backend
// (runtime/src/cheats/*). ImGui 1.92.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include "cheats_ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../../cheats/aob.h"
#include "../../cheats/codepatch.h"
#include "../../cheats/control.h"
#include "../../cheats/gmem.h"
#include "cheats_theme.h"
#include "cheats_ui_internal.h"
#include "pin_board.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/input_source.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"

namespace nxrt::cheats::ui {
namespace fs = std::filesystem;
namespace th = theme;
constexpr const char* kTag = "cheats";

State& st() {
    static State s;
    return s;
}

// ---- small helpers ----------------------------------------------------------------------------------------------------------------
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
namespace {
void set_toast(const std::string& msg, int kind, std::function<void()> undo) {
    State& s = st();
    s.toast.text = msg;
    s.toast.kind = kind;
    s.toast.undo = std::move(undo);
    s.toast.until = now_s() + (s.toast.undo ? 6.0 : 4.0);
    NXLOG_I(kTag, "overlay: %s", msg.c_str());
}
}  // namespace
void toast(const std::string& msg) { set_toast(msg, 0, nullptr); }
void toast_warn(const std::string& msg) { set_toast(msg, 1, nullptr); }
void toast_undo(const std::string& msg, std::function<void()> undo) { set_toast(msg, 0, std::move(undo)); }
void confirm(const std::string& title, const std::string& body, const std::string& ok, std::function<void()> fn) {
    Confirm& c = st().confirm;
    c.title = title;
    c.body = body;
    c.ok = ok;
    c.fn = std::move(fn);
    c.pending = true;
}
std::string hexs(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(v));
    return b;
}
void mark_dirty() { st().dirty = true; }

namespace {
const char* kKindNames[] = {"Byte", "2 bytes", "4 bytes", "8 bytes", "Float", "Double", "All numeric types", "Text (UTF-8)", "Text (UTF-16)",
                            "Array of bytes"};
const ScanType kKindTypes[] = {ScanType::U8, ScanType::U16, ScanType::U32, ScanType::U64, ScanType::F32, ScanType::F64};
const char* kPageNames[PageCount] = {"Cheats", "Find a value", "My table", "Advanced"};
const th::Icon kPageIcons[PageCount] = {th::Icon::Cheats, th::Icon::Search, th::Icon::Table, th::Icon::Tools};
}  // namespace
const char* const* kind_names() { return kKindNames; }
int kind_count() { return 10; }
ScanType kind_type(int i) { return i >= 0 && i < 6 ? kKindTypes[i] : ScanType::U32; }

std::string value_text(uint64_t addr, ScanType t) {
    uint64_t bits = 0;
    if (!addr || !gmem::read(addr, &bits, type_size(t))) return "??";
    return Value{t, bits}.text();
}

std::string label_at(uint64_t addr) {
    for (const auto& l : st().labels)
        if (l.address == addr) return l.name;
    return std::string();
}

bool parse_addr(const char* text, uint64_t* out) {
    std::string s = text ? text : "";
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty()) return false;
    for (const auto& l : st().labels)
        if (l.name == s) { *out = l.address; return true; }
    // module+offset ("main+0x1234", "main+1234")
    const size_t plus = s.find('+');
    if (plus != std::string::npos && plus > 0) {
        const std::string mod = s.substr(0, plus);
        uint64_t base = 0;
        if (mod == "heap" || mod == "alias") base = gmem::region_base(mod == "heap" ? 1 : 2);
        else if (Module* m = module_by_name(mod.c_str())) base = m->base();
        if (base) {
            char* end = nullptr;
            const uint64_t off = std::strtoull(s.c_str() + plus + 1, &end, 16);
            if (end && *end == 0) { *out = base + off; return true; }
        }
    }
    char* end = nullptr;
    const uint64_t v = std::strtoull(s.c_str(), &end, 16);
    if (!end || *end != 0) return false;
    *out = v;
    return true;
}

namespace {
uint64_t module_base_of(const std::string& name) {
    if (Module* m = module_by_name(name.c_str())) return m->base();
    return 0;
}
}  // namespace

uint64_t entry_address(Entry& e) {
    State& s = st();
    if (e.is_pointer) {
        const uint64_t a = pointer_resolve(*s.src, e.path, &module_base_of);
        if (a) e.addr = a;
        return a;
    }
    if (!e.signature.empty() && !e.sig_resolved) {
        e.sig_resolved = true;
        AobPattern pat;
        std::string err;
        if (aob_parse(e.signature, &pat, &err)) {
            RegionFilter f;
            f.main = f.heap = f.alias = f.code = f.other = true;
            if (Module* m = module_by_name("main")) { f.lo = m->base(); f.hi = m->base() + m->size(); }
            auto hits = aob_scan(*s.src, pat, f, 2);
            if (hits.empty()) {
                e.status = "its signature is not in this build of the game: turned off";
                e.addr = 0;
            } else {
                e.addr = static_cast<uint64_t>(static_cast<int64_t>(hits[0].addr) + e.sig_offset);
                e.status = hits.size() > 1 ? "signature matched more than once: the first match is used" : "";
            }
        }
    }
    return e.addr;
}

Entry* entry_by_uid(uint64_t uid) {
    for (auto& e : st().list)
        if (e.uid == uid) return &e;
    return nullptr;
}

void add_entry(uint64_t addr, ScanType t, const std::string& desc) {
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    Entry e;
    e.addr = addr;
    e.type = t;
    e.desc = desc.empty() ? gmem::describe(addr) : desc;
    e.value = value_text(addr, t);
    e.uid = s.next_uid++;
    s.list.push_back(e);
    mark_dirty();
    const uint64_t uid = e.uid;
    toast_undo("Added \"" + e.desc + "\" to My table", [uid] {
        State& s2 = st();
        std::lock_guard<std::recursive_mutex> lk2(s2.mu);
        for (size_t i = 0; i < s2.list.size(); ++i)
            if (s2.list[i].uid == uid) {
                set_freeze(s2.list[i], false);
                s2.list.erase(s2.list.begin() + static_cast<long>(i));
                mark_dirty();
                break;
            }
    });
}

void set_freeze(Entry& e, bool on) {
    Engine& eng = Engine::instance();
    if (!on) {
        if (e.freeze_id) eng.freeze_remove(e.freeze_id);
        e.freeze_id = 0;
        mark_dirty();
        return;
    }
    if (e.freeze_id) return;
    const uint64_t a = entry_address(e);
    if (!a) {
        e.status = "the address does not resolve now: not frozen";
        return;
    }
    Value v{};
    if (!parse_value(e.type, e.value, &v)) {
        uint64_t bits = 0;
        gmem::read(a, &bits, type_size(e.type));
        v = Value{e.type, bits};
    }
    if (!eng.enabled()) eng.enable(true);
    e.freeze_id = eng.freeze_add(a, static_cast<unsigned>(type_size(e.type)), v.bits, e.mode, type_is_float(e.type));
    mark_dirty();
}

void nav_focus_here(const char* name) {
    State& s = st();
    if (s.nav_focus.empty() || s.nav_focus != name) return;
    s.nav_focus.clear();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::FocusWindow(w);
    ImGui::SetFocusID(ImGui::GetItemID(), w);
    ImGui::SetNavCursorVisible(true);
}

// ---- pages and tools ----------------------------------------------------------------------------------------------------------------
Win* find_win(const char* title) {
    for (auto& w : st().wins)
        if (std::strcmp(w.title, title) == 0) return &w;
    return nullptr;
}

void set_page(int page) {
    State& s = st();
    page = std::clamp(page, 0, PageCount - 1);
    if (s.page != page) mark_dirty();
    s.page = page;
}

void open_window(const char* title) {
    State& s = st();
    // the beginner's tools are pages of their own
    if (!std::strcmp(title, "Scanner")) { set_page(PageFind); return; }
    if (!std::strcmp(title, "Address list")) { set_page(PageTable); return; }
    if (!std::strcmp(title, "Cheat files")) { set_page(PageCheats); return; }
    Win* w = find_win(title);
    if (!w) return;
    if (w->open) {
        ImGui::SetWindowFocus(title);     // already in a window of its own: to the front
        return;
    }
    s.tool = title;
    set_page(PageAdvanced);
}
bool window_open(const char* title) {
    const State& s = st();
    if (!std::strcmp(title, "Scanner")) return s.page == PageFind;
    if (!std::strcmp(title, "Address list")) return s.page == PageTable;
    if (!std::strcmp(title, "Cheat files")) return s.page == PageCheats;
    for (const auto& w : s.wins)
        if (std::strcmp(w.title, title) == 0) return w.open || (s.page == PageAdvanced && s.tool == title);
    return false;
}

namespace {
const char* g_embed = nullptr;            // the tool the Advanced page is drawing inside itself
std::vector<bool> g_tool_stack;           // per tool_begin: drawn inside the page (child) or in its own window
}  // namespace
bool tool_begin(const char* title, bool* open) {
    const bool inside = g_embed && std::strcmp(g_embed, title) == 0;
    g_tool_stack.push_back(inside);
    if (inside) return ImGui::BeginChild("##tool_body", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    return ImGui::Begin(title, open);
}
void tool_end() {
    const bool inside = !g_tool_stack.empty() && g_tool_stack.back();
    if (!g_tool_stack.empty()) g_tool_stack.pop_back();
    if (inside) ImGui::EndChild();
    else ImGui::End();
}

void goto_viewer(uint64_t addr) {
    st().view_addr = addr & ~0xfull;
    st().view_sel = addr;
    open_window("Memory viewer");
}
void goto_disasm(uint64_t addr) {
    State& s = st();
    if (s.dis_addr && s.dis_addr != (addr & ~3ull)) {
        s.dis_history.push_back(s.dis_addr);
        if (s.dis_history.size() > 64) s.dis_history.erase(s.dis_history.begin());
    }
    s.dis_addr = addr & ~3ull;
    s.dis_sel = addr & ~3ull;
    open_window("Disassembler");
}

bool pad_pressed(uint64_t bit) { return (st().pad_edge & bit) != 0; }
bool pad_mode() { return st().pad_last.load(); }

// ---- hotkeys --------------------------------------------------------------------------------------------------------------------------
namespace {
struct PadName { const char* n; hid::NpadButtonBit b; };
const PadName kPadNames[] = {{"A", hid::kBtnA}, {"B", hid::kBtnB}, {"X", hid::kBtnX}, {"Y", hid::kBtnY}, {"L", hid::kBtnL}, {"R", hid::kBtnR},
                             {"ZL", hid::kBtnZL}, {"ZR", hid::kBtnZR}, {"PLUS", hid::kBtnPlus}, {"MINUS", hid::kBtnMinus},
                             {"LSTICK", hid::kBtnStickL}, {"RSTICK", hid::kBtnStickR}, {"UP", hid::kBtnUp}, {"DOWN", hid::kBtnDown},
                             {"LEFT", hid::kBtnLeft}, {"RIGHT", hid::kBtnRight}};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
// A hotkey name is held: every part of "L+R+A" / "Left Ctrl+F5" is down.
bool held_name(const std::string& name) {
    if (name.empty()) return false;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.in_mu);
    size_t i = 0;
    bool any = false;
    while (i <= name.size()) {
        size_t j = name.find('+', i);
        if (j == std::string::npos) j = name.size();
        std::string part = name.substr(i, j - i);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        i = j + 1;
        if (part.empty()) { if (j >= name.size()) break; continue; }
        any = true;
        bool ok = false;
        const std::string up = upper(part);
        for (const PadName& p : kPadNames)
            if (up == p.n) { ok = (s.pad_down & hid::btn(p.b)) != 0; goto next; }
        {
            const SDL_Scancode sc = SDL_GetScancodeFromName(part.c_str());
            ok = sc != SDL_SCANCODE_UNKNOWN && s.keys_down.count(static_cast<int>(sc));
        }
    next:
        if (!ok) return false;
        if (j >= name.size()) break;
    }
    return any;
}
std::string pad_combo_name(uint64_t bits) {
    std::string out;
    for (const PadName& p : kPadNames)
        if (bits & hid::btn(p.b)) out += (out.empty() ? "" : "+") + std::string(p.n);
    return out;
}
uint64_t g_capture_bits = 0;

// ---- the live Lua host ----
class LiveLuaHost : public lua::Host {
public:
    MemSource& mem() override { return *st().src; }
    bool write(uint64_t a, const void* src, size_t n) override { return gmem::write(a, src, n); }
    bool module_range(const std::string& name, uint64_t* lo, uint64_t* hi) override {
        Module* m = module_by_name(name.c_str());
        if (!m) return false;
        *lo = m->base();
        *hi = m->base() + m->size();
        return true;
    }
    uint64_t freeze(uint64_t a, ScanType t, uint64_t bits) override {
        Engine& e = Engine::instance();
        if (!e.enabled()) e.enable(true);
        return e.freeze_add(a, static_cast<unsigned>(type_size(t)), bits, FreezeMode::Normal, type_is_float(t));
    }
    void unfreeze(uint64_t id) override { Engine::instance().freeze_remove(id); }
    bool held(const std::string& name) override { return held_name(name); }
    void pause(bool on) override { ctl::pause(on); }
    void advance() override { ctl::frame_advance(1); }
    double speed(double set) override {
        if (set > 0) { ctl::set_speed(set); st().speed = static_cast<float>(ctl::speed()); }
        return ctl::speed();
    }
    uint64_t frame() override { return ctl::frames(); }
    uint64_t tick() override { return kernel::system_tick(); }
    double host_seconds() override { return now_s(); }
    std::string folder() override { return Engine::instance().folder(); }
    void console(const std::string& line) override {
        NXLOG_I(kTag, "lua: %s", line.c_str());
        State& s = st();
        std::lock_guard<std::recursive_mutex> lk(s.mu);
        s.console.push_back(line);
        if (s.console.size() > 400) s.console.erase(s.console.begin(), s.console.begin() + 100);
    }
};

// The frame boundary (cheats/control.h; the presenting thread): table hotkeys, Lua, value graphs and their triggers.
void on_frame(void*) {
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    for (Entry& e : s.list) {
        if (e.hotkey.empty()) continue;
        const bool down = held_name(e.hotkey);
        const bool fire = down && !e.hk_down;
        e.hk_down = down;
        if (!fire) continue;
        const uint64_t a = entry_address(e);
        if (e.is_script) {
            // sync_scripts (below) runs its [ENABLE] or [DISABLE] part
            e.script_on = !e.script_on;
            continue;
        }
        switch (e.hotkey_action) {
            case 0: set_freeze(e, e.freeze_id == 0); break;
            case 1: {
                Value v{};
                if (a && parse_value(e.type, e.value, &v)) gmem::write(a, &v.bits, type_size(e.type));
                break;
            }
            default: {
                uint64_t bits = 0;
                if (a && gmem::read(a, &bits, type_size(e.type))) {
                    Value v{e.type, bits};
                    v.set_double(v.as_double() + (e.hotkey_action == 2 ? 1.0 : -1.0));
                    if (!type_is_float(e.type)) v.bits = static_cast<uint64_t>(Value{e.type, bits}.as_int() + (e.hotkey_action == 2 ? 1 : -1));
                    gmem::write(a, &v.bits, type_size(e.type));
                }
                break;
            }
        }
        NXLOG_I(kTag, "hotkey %s: %s", e.hotkey.c_str(), e.desc.c_str());
    }
    sync_scripts();                      // AOB script entries switched by a hotkey or loaded on
    test_tick();                         // [cheats] test: the scripted test's next step
    const bool frame = !ctl::held();     // called from inside a pause: no frame passed
    if (s.lua) s.lua->on_frame(frame);
    if (!frame) return;
    // value graphs: one sample per frame
    for (Watch& w : s.watches) {
        Entry* e = entry_by_uid(w.uid);
        if (!e) continue;
        const uint64_t a = entry_address(*e);
        uint64_t bits = 0;
        if (!a || !gmem::read(a, &bits, type_size(e->type))) continue;
        const double v = Value{e->type, bits}.as_double();
        if (w.samples.size() < 600) w.samples.resize(600, 0.f);
        w.samples[w.head] = static_cast<float>(v);
        w.head = (w.head + 1) % w.samples.size();
        if (w.head == 0) w.full = true;
        bool fire = false;
        switch (w.trig) {
            case 1: fire = w.have_last && v != w.last; break;
            case 2: fire = v > w.trig_value; break;
            case 3: fire = v < w.trig_value; break;
            case 4: fire = v == w.trig_value; break;
            default: break;
        }
        if (fire && !w.fired) {
            w.fired = true;
            NXLOG_I(kTag, "trigger on %s fired at frame %llu: %g -> %g", e->desc.c_str(), static_cast<unsigned long long>(ctl::frames()), w.last, v);
            if (w.trig_pause) ctl::pause(true);
        }
        w.last = v;
        w.have_last = true;
    }
}
}  // namespace

// ---- theme ------------------------------------------------------------------------------------------------------------------------------
void apply_theme(bool blur_ok) {
    State& s = st();
    th::Look lk;
    lk.accent = s.accent;
    lk.style = s.style;
    lk.opacity = s.opacity;
    lk.blur = s.blur;
    lk.tint = s.tint;
    lk.scale = s.scale > 0 ? s.scale : 1.f;
    th::apply(lk, s.have_frame_rgba ? s.frame_rgba : nullptr, blur_ok);
}

void load_fonts(ImFontAtlas* atlas) { th::load_fonts(atlas); }

// ---- persistence ------------------------------------------------------------------------------------------------------------------------
namespace {
std::string table_path() {
    const std::string bid = Engine::instance().build_id_hex();
    if (bid.empty()) return std::string();
    return Engine::instance().folder() + "/" + bid + ".cheattable.json";
}
std::string layout_path() {
    const std::string bid = Engine::instance().build_id_hex();
    if (bid.empty()) return std::string();
    return Engine::instance().folder() + "/" + bid + ".overlay.json";
}
std::string read_file(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool write_file(const std::string& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::u8path(p).parent_path(), ec);
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(fs::u8path(tmp), std::ios::binary);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    fs::rename(fs::u8path(tmp), fs::u8path(p), ec);
    return !ec;
}

table::TableEntry to_saved(const Entry& e) {
    table::TableEntry t;
    t.description = e.desc;
    t.is_pointer = e.is_pointer;
    t.address = e.addr;
    t.path.module = e.path.module;
    t.path.mem_type = e.path.mem_type;
    t.path.base_offset = e.path.base_offset;
    t.path.offsets = e.path.offsets;
    t.type = static_cast<int>(e.type);
    t.value = e.value;
    t.freeze = e.is_script ? e.script_on : e.freeze_id != 0;
    t.freeze_mode = static_cast<int>(e.mode);
    t.hotkey_name = e.hotkey;
    t.hotkey_action = e.hotkey_action;
    t.group = e.group;
    t.signature = e.signature;
    t.sig_offset = e.sig_offset;
    t.is_script = e.is_script;
    t.script = e.script;
    t.pinned = e.pinned;
    return t;
}
Entry from_saved(const table::TableEntry& t) {
    Entry e;
    e.desc = t.description;
    e.is_pointer = t.is_pointer;
    e.addr = t.address;
    e.path.module = t.path.module;
    e.path.mem_type = t.path.mem_type;
    e.path.base_offset = t.path.base_offset;
    e.path.offsets = t.path.offsets;
    e.type = static_cast<ScanType>(std::clamp(t.type, 0, 5));
    e.value = t.value;
    e.mode = static_cast<FreezeMode>(std::clamp(t.freeze_mode, 0, 2));
    e.hotkey = t.hotkey_name;
    e.hotkey_action = t.hotkey_action;
    e.group = t.group;
    e.signature = t.signature;
    e.sig_offset = t.sig_offset;
    e.is_script = t.is_script;
    e.script = t.script;
    e.pinned = t.pinned;
    return e;
}

int page_by_name(const std::string& n) {
    for (int i = 0; i < PageCount; ++i)
        if (n == kPageNames[i]) return i;
    return -1;
}

void load_saved() {
    State& s = st();
    const std::string tp = table_path();
    if (tp.empty()) return;
    std::vector<table::TableEntry> rows;
    table::Extras ex;
    const std::string text = read_file(tp);
    if (!text.empty() && table::parse_table(text, &rows, &ex)) {
        s.list.clear();
        std::vector<bool> want_on;
        for (const auto& r : rows) {
            Entry e = from_saved(r);
            e.uid = s.next_uid++;
            s.list.push_back(e);
            want_on.push_back(r.freeze);
        }
        s.labels = ex.labels;
        s.lua_scripts = ex.lua;
        // freezes come back on; scripts that were on are enabled again by the list window (it owns the script host)
        for (size_t i = 0; i < s.list.size(); ++i) {
            if (!want_on[i]) continue;
            if (s.list[i].is_script) s.list[i].status = "on at start (pending)", s.list[i].script_on = true, s.list[i].script_run.ok = false;
            else set_freeze(s.list[i], true);
        }
        NXLOG_I(kTag, "cheat table loaded: %zu entries, %zu labels, %zu Lua scripts from %s", s.list.size(), s.labels.size(), s.lua_scripts.size(), tp.c_str());
    }
    table::Layout lo;
    const std::string lt = read_file(layout_path());
    if (!lt.empty() && table::parse_layout(lt, &lo)) {
        for (auto& w : s.wins) {
            auto it = lo.open.find(w.title);
            if (it != lo.open.end()) w.open = it->second;
            auto r = lo.rect.find(w.title);
            if (r != lo.rect.end() && r->second.size() == 4 && r->second[2] > 40 && r->second[3] > 40)
                for (int k = 0; k < 4; ++k) w.rect[k] = r->second[k];
        }
        if (auto r = lo.rect.find("Main"); r != lo.rect.end() && r->second.size() == 4 && r->second[2] > 200 && r->second[3] > 150)   // UI units
            for (int k = 0; k < 4; ++k) s.main_rect[k] = r->second[k];
        if (auto p = lo.ui.find("page"); p != lo.ui.end() && page_by_name(p->second) >= 0) s.page = page_by_name(p->second);
        if (auto t = lo.ui.find("tool"); t != lo.ui.end() && find_win(t->second.c_str())) s.tool = t->second;
        if (auto d = lo.ui.find("dock"); d != lo.ui.end()) s.dock = d->second == "left" ? 0 : d->second == "right" ? 2 : 1;
        if (auto hs = lo.ui.find("hud_shown"); hs != lo.ui.end()) s.hud_shown = hs->second != "0";
        if (auto h = lo.rect.find("HUD"); h != lo.rect.end() && h->second.size() == 4) { s.hud_pos[0] = h->second[0]; s.hud_pos[1] = h->second[1]; }
        if (auto pc = lo.ui.find("pinned_cheats"); pc != lo.ui.end()) {
            size_t a = 0;
            while (a < pc->second.size()) {
                size_t b = pc->second.find('\n', a);
                if (b == std::string::npos) b = pc->second.size();
                if (b > a) s.pinned_cheats.insert(pc->second.substr(a, b - a));
                a = b + 1;
            }
        }
        s.view_addr = lo.viewer_addr;
    }
    s.dirty = false;
}
}  // namespace

void save() {
    State& s = st();
    if (!s.inited) return;
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    const std::string tp = table_path();
    if (tp.empty()) return;
    std::vector<table::TableEntry> rows;
    for (const Entry& e : s.list) rows.push_back(to_saved(e));
    table::Extras ex;
    ex.labels = s.labels;
    ex.lua = s.lua_scripts;
    for (auto& l : ex.lua) l.run = s.lua_ids.count(l.name) != 0;
    const bool ok1 = write_file(tp, table::dump_table(rows, ex));
    table::Layout lo;
    for (const auto& w : s.wins) {
        lo.open[w.title] = w.open;
        if (w.rect[2] > 0) lo.rect[w.title] = {w.rect[0], w.rect[1], w.rect[2], w.rect[3]};
    }
    if (s.main_rect[2] > 0) lo.rect["Main"] = {s.main_rect[0], s.main_rect[1], s.main_rect[2], s.main_rect[3]};
    lo.ui["page"] = kPageNames[std::clamp(s.page, 0, PageCount - 1)];
    lo.ui["tool"] = s.tool;
    lo.ui["dock"] = s.dock == 0 ? "left" : s.dock == 2 ? "right" : "centre";
    lo.ui["hud_shown"] = s.hud_shown ? "1" : "0";
    if (s.hud_pos[0] >= 0) lo.rect["HUD"] = {s.hud_pos[0], s.hud_pos[1], 0, 0};
    if (!s.pinned_cheats.empty()) {
        std::string names;
        for (const auto& n : s.pinned_cheats) names += (names.empty() ? "" : "\n") + n;
        lo.ui["pinned_cheats"] = names;
    }
    lo.viewer_addr = s.view_addr;
    const bool ok2 = write_file(layout_path(), table::dump_layout(lo));
    s.dirty = false;
    s.last_save = now_s();
    if (!ok1 || !ok2) NXLOG_W(kTag, "the cheat table or layout could not be saved next to the title (%s)", tp.c_str());
}

// ---- the tools ----------------------------------------------------------------------------------------------------------------------
namespace {
void build_windows() {
    State& s = st();
    using I = th::Icon;
    s.wins = {
        {"Pointer scanner", "Search", I::Pointer, "Paths from the game's code to a value, so it is found again after a restart", &win_pointer},
        {"Snapshots", "Search", I::Camera, "Copies of memory to compare: every word that changed between two moments", &win_snapshots},
        {"Memory viewer", "Memory", I::Memory, "Browse and edit memory as hex and text, with a value inspector", &win_viewer},
        {"Structure", "Memory", I::Layers, "Read memory as fields: pointers, numbers, text", &win_struct},
        {"Memory map", "Memory", I::Map, "Every block of the game's memory: where, how big, what it is", &win_memmap},
        {"Labels", "Memory", I::Tag, "Names for addresses, usable in every address field", &win_labels},
        {"Disassembler", "Code", I::Code, "The game's code: patch an instruction, make a signature, set breakpoints", &win_disasm},
        {"Find what accesses", "Code", I::Eye, "Which code writes (or reads) a value", &win_access},
        {"Breakpoints", "Code", I::Breakpoint, "Stop or trace code, see its registers, step", &win_breakpoints},
        {"Code patches", "Code", I::Edit, "Live code changes; save them as IPS patches for the next build", &win_patches},
        {"Threads", "Code", I::Thread, "The game's threads and the code each one runs", &win_threads},
        {"Game control", "Game", I::Speed, "Pause, step frames, play slower or faster", &win_control},
        {"Value graphs", "Game", I::Graph, "A value over time, with triggers that pause the game", &win_watch},
        {"Lua", "Game", I::Code, "Scripts that run every frame, and a console", &win_lua},
        {"Settings", "Overlay", I::Gear, "Accent colour, panel opacity and size", &win_settings},
        {"Help", "Overlay", I::Help, "The controls and how the pages fit together", &win_help},
    };
}

// The page tabs: icon + name pills, the selected one tinted in the accent; L / R (shown at the ends with a pad) step through them.
void draw_tabs() {
    State& s = st();
    const th::Palette& p = th::pal();
    const bool pad = pad_mode();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // the whole row: the caps, every tab with its name; too wide (a docked panel): only the chosen tab keeps its name
    th::push(th::Type::Strong);
    float full = pad ? 2.f * (th::hint_width("L", "", true) + th::sp(th::S2)) : 0.f;
    for (int i = 0; i < PageCount; ++i)
        full += ImGui::CalcTextSize(kPageNames[i]).x + std::round(ImGui::GetFontSize() * 1.15f) + th::sp(th::S2) + 2.f * th::sp(th::S4) + th::sp(th::S1);
    th::pop();
    const bool compact = full > ImGui::GetContentRegionAvail().x;
    if (pad) {
        th::hint("L", "", true);
        ImGui::SameLine(0, th::sp(th::S2));
    }
    for (int i = 0; i < PageCount; ++i) {
        ImGui::PushID(i);
        th::push(th::Type::Strong);
        const bool named = !compact || s.page == i;
        const ImVec2 ts = named ? ImGui::CalcTextSize(kPageNames[i]) : ImVec2(0, 0);
        const float h = std::round(th::px(th::kControlH)), isz = std::round(ImGui::GetFontSize() * 1.15f);
        const float w = named ? ts.x + isz + th::sp(th::S2) + 2.f * th::sp(th::S4) : h;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
        const ImGuiID id = ImGui::GetID("##tab");
        ImGui::ItemSize(bb);
        if (ImGui::ItemAdd(bb, id)) {
            bool hov = false, held = false;
            if (ImGui::ButtonBehavior(bb, id, &hov, &held)) set_page(i);
            const bool sel = s.page == i;
            const ImU32 bg = sel ? p.accent_tint : hov ? p.surface2 : 0;
            if (bg) dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(bg), h * 0.5f);
            const ImU32 fg = ImGui::GetColorU32(sel ? p.accent : hov ? p.text : p.text2);
            const float ix = named ? bb.Min.x + th::sp(th::S4) + isz * 0.5f : bb.Min.x + w * 0.5f;
            th::icon(dl, kPageIcons[i], ImVec2(ix, bb.Min.y + h * 0.5f), isz, fg);
            if (named) dl->AddText(ImVec2(bb.Min.x + th::sp(th::S4) + isz + th::sp(th::S2), bb.Min.y + (h - ts.y) * 0.5f), fg, kPageNames[i]);
            if (!named && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", kPageNames[i]);
            ImGui::RenderNavCursor(bb, id, ImGuiNavRenderCursorFlags_None, h * 0.5f);
        }
        th::pop();
        ImGui::PopID();
        ImGui::SameLine(0, th::sp(th::S1));
    }
    if (pad) {
        ImGui::SameLine(0, th::sp(th::S2));
        th::hint("R", "", true);
    } else {
        ImGui::NewLine();
    }
}

// The panel's head: the game (its icon and name) and the overlay's mark, what is running, and the close button.
bool draw_header(bool has_game) {
    State& s = st();
    const th::Palette& p = th::pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float disc = std::round(th::px(40.f));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;   // the line's right edge (window coordinates)
    if (ImTextureData* icon = game_icon()) {
        // the game's icon as a rounded tile (the launcher's tile shape), the overlay's mark as a badge on its corner
        dl->AddImageRounded(icon->GetTexRef(), pos, ImVec2(pos.x + disc, pos.y + disc), ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(IM_COL32_WHITE),
                            th::px(th::kRadiusSm));
        dl->AddRect(pos, ImVec2(pos.x + disc, pos.y + disc), ImGui::GetColorU32(p.line_strong), th::px(th::kRadiusSm));
        const float b = std::round(th::px(18.f));
        const ImVec2 bc(pos.x + disc - b * 0.3f, pos.y + disc - b * 0.3f);
        dl->AddCircleFilled(bc, b * 0.5f + th::px(2.f), ImGui::GetColorU32(p.panel | IM_COL32_A_MASK));
        dl->AddCircleFilled(bc, b * 0.5f, ImGui::GetColorU32(p.accent));
        th::icon(dl, th::Icon::Cheats, bc, b * 0.62f, ImGui::GetColorU32(p.on_accent));
    } else {
        dl->AddCircleFilled(ImVec2(pos.x + disc * 0.5f, pos.y + disc * 0.5f), disc * 0.5f, ImGui::GetColorU32(p.accent));
        th::icon(dl, th::Icon::Cheats, ImVec2(pos.x + disc * 0.5f, pos.y + disc * 0.5f), th::px(22.f), ImGui::GetColorU32(p.on_accent));
    }
    ImGui::Dummy(ImVec2(disc, disc));
    ImGui::SameLine(0, th::sp(th::S3));
    ImGui::BeginGroup();
    // the game's name as the title (the overlay's own name when no game is known), then "Cheats" and the build
    const EngineStats es = Engine::instance().stats();
    const char* title = game::info().title;
    const bool named = title && *title;
    th::push(th::Type::Title);
    ImGui::TextUnformatted(named ? title : "Cheats");
    th::pop();
    std::string sub = named ? "Cheats" : "";
    if (!es.program_id.empty()) sub += (sub.empty() ? "Title " : "  \xC2\xB7  ") + es.program_id;
    if (!es.build_id.empty()) sub += "  \xC2\xB7  build " + es.build_id.substr(0, 8);
    if (sub.empty()) sub = has_game ? "No title loaded" : "Preview";
    th::text(th::Type::Small, p.text2, "%s", sub.c_str());
    ImGui::EndGroup();
    // right side: status chips and close
    size_t frozen = 0;
    for (const Entry& e : s.list) frozen += e.freeze_id != 0 || (e.is_script && e.script_on);
    const bool paused = ctl::paused();
    const float close_w = th::px(th::kControlH) * 5.f + th::sp(th::S1) * 4.f + th::sp(th::S3);   // dock x3, peek, close
    char fz[48];
    std::snprintf(fz, sizeof fz, "%zu active", frozen);
    float chips_w = 0.f;
    th::push(th::Type::Small);
    if (paused) chips_w += ImGui::CalcTextSize("Paused").x + th::px(44.f);
    if (frozen) chips_w += ImGui::CalcTextSize(fz).x + th::px(44.f);
    th::pop();
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - chips_w - close_w - th::sp(th::S2)));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (disc - th::px(th::kControlH)) * 0.5f);
    if (paused) { th::chip("Paused", p.warn_tint, p.warn, th::Icon::Pause); ImGui::SameLine(0, th::sp(th::S2)); }
    if (frozen) { th::chip(fz, p.accent_tint, p.accent, th::Icon::Snow); ImGui::SameLine(0, th::sp(th::S2)); }
    // dock the panel: left, centre (where it was put), right
    static const th::Icon dock_icons[] = {th::Icon::DockLeft, th::Icon::DockCentre, th::Icon::DockRight};
    static const char* dock_tips[] = {"Dock on the left", "In the centre (where you put it)", "Dock on the right"};
    for (int d = 0; d < 3; ++d) {
        ImGui::PushID(d);
        if (th::icon_button("##dock", dock_icons[d], dock_tips[d], s.dock == d ? th::Btn::Secondary : th::Btn::Quiet)) {
            s.dock = d;
            s.main_rect_applied = false;
            mark_dirty();
        }
        ImGui::PopID();
        ImGui::SameLine(0, th::sp(th::S1));
    }
    ImGui::SameLine(0, th::sp(th::S3));
    // Peek: held, the panel fades almost out so the game shows
    const std::string peek_tip = std::string("Peek at the game while held (") + (pad_mode() ? "or hold ZR" : "or hold " + s.close_key) + ")";
    th::icon_button("##peek", th::Icon::Eye, peek_tip.c_str());
    s.peek_ui = ImGui::IsItemActive();
    ImGui::SameLine(0, th::sp(th::S1));
    std::string tip = "Close (" + s.close_key + (pad_mode() ? ", or +)" : ")");
    const bool close = th::icon_button("##close", th::Icon::Close, tip.c_str());
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), pos.y - ImGui::GetWindowPos().y + disc + th::sp(th::S3)));
    return close;
}

// The button hints at the foot of the panel: the pad's buttons while a pad is in use, else the keyboard's; the least important ones
// drop out when the panel is narrow.
void draw_hints() {
    State& s = st();
    const bool pad = pad_mode();
    struct H { const char* key; const char* label; };
    std::vector<H> hs;
    if (pad) {
        hs = {{"A", "Select"}, {"B", "Back"}};
        if (s.page == PageTable || s.page == PageFind) hs.push_back({"X", "Actions"});
        hs.push_back({"Y", "Type"});
        hs.push_back({"L R", "Pages"});
        if (s.toast.undo && s.toast.until > now_s()) hs.push_back({"-", "Undo"});
        hs.push_back({"ZR", "Hold: peek"});
        hs.push_back({"+", "Close"});
    } else {
        hs = {{"Enter", "Select"}, {"Esc", "Back"}};
        if (s.page == PageTable || s.page == PageFind) hs.push_back({"Right-click", "Actions"});
        hs.push_back({"Ctrl+PgUp/PgDn", "Pages"});
        if (s.toast.undo && s.toast.until > now_s()) hs.push_back({"Ctrl+Z", "Undo"});
        hs.push_back({s.close_key.c_str(), "Close, hold: peek"});
    }
    const float gap = th::sp(th::S5);
    const float avail = ImGui::GetContentRegionAvail().x;
    // drop from the middle (Type, Actions, Pages) until it fits, keeping Select / Back and Close
    auto total = [&] {
        float t = 0.f;
        for (const H& h : hs) t += th::hint_width(h.key, h.label, pad) + gap;
        return t - gap;
    };
    while (hs.size() > 3 && total() > avail) hs.erase(hs.begin() + static_cast<long>(hs.size() - 2));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, avail - total()));
    for (size_t i = 0; i < hs.size(); ++i) {
        if (i) ImGui::SameLine(0, gap);
        th::hint(hs[i].key, hs[i].label, pad);
    }
}

// The Advanced page: the tools listed by group on the left, the chosen one on the right (or a note when it has its own window).
void page_advanced() {
    State& s = st();
    const th::Palette& p = th::pal();
    const float list_w = std::round(th::px(250.f));
    ImGui::BeginChild("##tools", ImVec2(list_w, 0), ImGuiChildFlags_NavFlattened);
    const char* group = "";
    for (Win& w : s.wins) {
        if (std::strcmp(group, w.group) != 0) {
            group = w.group;
            if (ImGui::GetCursorPosY() > 1.f) ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
            th::text(th::Type::Small, p.text3, "%s", group);
        }
        ImGui::PushID(w.title);
        const bool sel = s.tool == w.title;
        const float h = std::round(th::px(36.f));
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float wd = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##t", sel, ImGuiSelectableFlags_None, ImVec2(wd, h))) { s.tool = w.title; mark_dirty(); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", w.blurb);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float isz = std::round(ImGui::GetFontSize() * 1.1f);
        const ImU32 fg = ImGui::GetColorU32(sel ? p.accent : p.text2);
        th::icon(dl, w.icon, ImVec2(pos.x + th::sp(th::S2) + isz * 0.5f, pos.y + h * 0.5f), isz, fg);
        dl->AddText(ImVec2(pos.x + th::sp(th::S2) + isz + th::sp(th::S3), pos.y + (h - ImGui::GetFontSize()) * 0.5f),
                    ImGui::GetColorU32(sel ? p.text : p.text2), w.title);
        if (w.open) th::icon(dl, th::Icon::Popout, ImVec2(pos.x + wd - th::sp(th::S3), pos.y + h * 0.5f), isz * 0.8f, ImGui::GetColorU32(p.text3));
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, th::sp(th::S4));
    // the chosen tool
    Win* w = find_win(s.tool.c_str());
    if (!w) w = &s.wins[2];
    ImGui::BeginChild("##tool", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    {
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float isz = std::round(th::px(24.f));
        th::icon_item(w->icon, isz, p.accent);
        ImGui::SameLine(0, th::sp(th::S2));
        ImGui::BeginGroup();
        th::push(th::Type::Title);
        ImGui::TextUnformatted(w->title);
        th::pop();
        th::text(th::Type::Small, p.text2, "%s", w->blurb);
        ImGui::EndGroup();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - th::px(th::kControlH)));
        if (!w->open && th::icon_button("##pop", th::Icon::Popout, "Open in its own window (several tools at once)")) { w->open = true; mark_dirty(); }
        ImGui::Separator();
    }
    if (w->open) {
        th::empty_state(th::Icon::Popout, "This tool is in its own window",
                        "Several tools can be open at once. Hold - on the pad (or drag) to move between windows.");
        const float bw = th::px(220.f);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f);
        if (th::button("Bring it back here", th::Icon::Back, th::Btn::Secondary, 220.f)) { w->open = false; mark_dirty(); }
    } else {
        g_embed = w->title;
        th::push(th::Type::Body);
        w->draw();
        th::pop();
        g_embed = nullptr;
    }
    ImGui::EndChild();
}

// ZL / ZR page the window the pad is in.
void pad_paging() {
    State& s = st();
    // ZR: a short press pages down (when it is let go), holding it peeks at the game
    bool zr_page = false;
    const bool zr = (s.pad_down & hid::btn(hid::kBtnZR)) != 0;
    if (zr) {
        if (s.zr_since == 0) s.zr_since = now_s();
        if (now_s() - s.zr_since >= 0.25) s.zr_peek = true;
    } else {
        if (s.zr_since != 0 && !s.zr_peek) zr_page = true;
        s.zr_since = 0;
        s.zr_peek = false;
    }
    ImGuiContext& g = *GImGui;
    ImGuiWindow* w = g.NavWindow;
    if (!w) return;
    while (w->ParentWindow && (w->Flags & ImGuiWindowFlags_ChildWindow) == 0) w = w->ParentWindow;
    if (pad_pressed(hid::btn(hid::kBtnZL))) w->Scroll.y = std::max(0.f, w->Scroll.y - w->InnerRect.GetHeight() * 0.9f);
    if (zr_page) w->Scroll.y = std::min(w->ScrollMax.y, w->Scroll.y + w->InnerRect.GetHeight() * 0.9f);
}

// A toast at the foot of the panel: a solid pill with an icon, the message and, for an undoable action, Undo.
void draw_toast(const ImRect& panel, float hints_h) {
    State& s = st();
    Toast& t = s.toast;
    const double now = now_s();
    if (t.text.empty() || t.until <= now) { if (t.until <= now) t.undo = nullptr; return; }
    const th::Palette& p = th::pal();
    const float fade = static_cast<float>(std::clamp((t.until - now) / 0.3, 0.0, 1.0));
    ImGui::SetNextWindowPos(ImVec2((panel.Min.x + panel.Max.x) * 0.5f, panel.Max.y - hints_h - th::sp(th::S3)), ImGuiCond_Always, ImVec2(0.5f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##cheats_toast", nullptr, fl)) {
        const th::Icon ic = t.kind == 0 ? th::Icon::Check : th::Icon::Warn;
        const ImU32 icol = t.kind == 0 ? p.ok : t.kind == 1 ? p.warn : p.danger;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = std::round(th::px(44.f));
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float tw = std::min(ImGui::CalcTextSize(t.text.c_str()).x, th::px(560.f));
        const bool undo = static_cast<bool>(t.undo);
        const char* ulabel = "Undo";
        const float uw = undo ? ImGui::CalcTextSize(ulabel).x + th::sp(th::S5) * 2.f : 0.f;
        const float w = th::sp(th::S4) + ImGui::GetFontSize() * 1.2f + th::sp(th::S3) + tw + (undo ? th::sp(th::S4) + uw : 0.f) + th::sp(th::S5);
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), ImGui::GetColorU32(p.solid), h * 0.5f);
        dl->AddRect(o, ImVec2(o.x + w, o.y + h), ImGui::GetColorU32(p.line_strong), h * 0.5f);
        const float isz = ImGui::GetFontSize() * 1.2f;
        th::icon(dl, ic, ImVec2(o.x + th::sp(th::S4) + isz * 0.5f, o.y + h * 0.5f), isz, ImGui::GetColorU32(icol));
        const float tx = o.x + th::sp(th::S4) + isz + th::sp(th::S3);
        dl->PushClipRect(ImVec2(tx, o.y), ImVec2(tx + tw, o.y + h), true);
        dl->AddText(ImVec2(tx, o.y + (h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(p.text), t.text.c_str());
        dl->PopClipRect();
        ImGui::Dummy(ImVec2(w, h));
        if (undo) {
            ImGui::SetCursorScreenPos(ImVec2(tx + tw + th::sp(th::S4), o.y + (h - th::px(th::kControlSm)) * 0.5f));
            if (th::button(ulabel, th::Icon::None, th::Btn::Quiet, 0.f, th::kControlSm)) {
                auto fn = std::move(t.undo);
                t.undo = nullptr;
                t.until = 0;
                if (fn) fn();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// The confirmation before a destructive action.
void draw_confirm() {
    State& s = st();
    Confirm& c = s.confirm;
    const char* id = "##cheats_confirm";
    if (c.pending) {
        ImGui::OpenPopup(id);
        c.pending = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(th::px(460.f), vp->WorkSize.x - th::sp(th::S6)), 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(th::sp(th::S6), th::sp(th::S6)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, th::px(th::kRadiusLg));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        th::push(th::Type::Title);
        ImGui::TextWrapped("%s", c.title.c_str());
        th::pop();
        if (!c.body.empty()) th::text_wrapped(th::Type::Body, th::pal().text2, "%s", c.body.c_str());
        ImGui::Dummy(ImVec2(0, th::sp(th::S3)));
        const float bw = (ImGui::GetContentRegionAvail().x - th::sp(th::S3)) * 0.5f / th::k();
        bool close = false;
        if (th::button("Cancel", th::Icon::None, th::Btn::Secondary, bw)) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);   // the safe choice has the focus
        ImGui::SameLine(0, th::sp(th::S3));
        if (th::button(c.ok.c_str(), th::Icon::None, th::Btn::Danger, bw)) {
            auto fn = std::move(c.fn);
            c.fn = nullptr;
            close = true;
            if (fn) fn();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || pad_pressed(hid::btn(hid::kBtnB))) close = true;
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
}  // namespace

void init_look() {
    State& s = st();
    if (s.look_inited) return;
    s.look_inited = true;
    // the look: defaults and ranges from runtime/assets/cheats/look.json (the launcher's too); a value outside its range is clamped
    const th::LookSpec& spec = th::look_spec();
    s.theme_name = config_get_str("cheats", "theme", spec.theme.c_str());
    {
        std::string err;
        th::set_theme(th::load_theme(s.theme_name, &err));
        if (!err.empty()) {
            s.theme_error = err;
            NXLOG_W(kTag, "[cheats] theme: %s: the %s theme is used instead", err.c_str(), spec.theme.c_str());
        }
    }
    s.accent = config_get_str("cheats", "accent", spec.accent.c_str());
    auto pct = [](const char* key, const th::Range& r) {
        const int v = config_get_int("cheats", key, r.def);
        return v < 0 ? -1.f : static_cast<float>(std::clamp(v, r.min, r.max)) / 100.f;
    };
    s.opacity = pct("opacity", spec.opacity);
    s.blur = pct("blur", spec.blur);
    s.tint = pct("tint", spec.tint);
    s.scale = static_cast<float>(std::clamp(static_cast<int>(config_get_int("cheats", "scale", spec.scale.def)), spec.scale.min, spec.scale.max)) / 100.f;
    const std::string style = config_get_str("cheats", "style", "");
    s.style = style == "glass" ? 1 : style == "solid" ? 0 : -1;
    s.animations = config_get_bool("cheats", "animations", spec.animations);
    s.hud_opacity = static_cast<float>(std::clamp(static_cast<int>(config_get_int("cheats", "hud_opacity", spec.hud_opacity.def)), spec.hud_opacity.min,
                                                  spec.hud_opacity.max)) / 100.f;
}

bool animations() {
    init_look();
    return st().animations;
}

std::string scan_spill_dir() {
    if (std::string d = run_report_dir(); !d.empty()) return d;
    return Engine::instance().folder();
}

void after_scan(const char* what) {
    State& s = st();
    if (!s.scanner->error().empty()) {
        NXLOG_W(kTag, "%s: %s", what, s.scanner->error().c_str());
        std::lock_guard<std::recursive_mutex> lk(s.mu);   // the page reads it while it draws
        s.scan_msg = s.scanner->error();
    }
    const SnapshotStats ss = s.scanner->snapshot_stats();
    if (ss.bytes == 0 && ss.file_peak == 0) return;
    const double mib = 1048576.0;
    NXLOG_I(kTag, "%s: the search's snapshot stands for %.0f MiB (%.0f MiB of it all-zero blocks, not kept): %.0f MiB in RAM (peak %.0f, "
            "budget %.0f), %.0f MiB in its file (peak %.0f) %s", what, static_cast<double>(ss.bytes) / mib, static_cast<double>(ss.zero) / mib,
            static_cast<double>(ss.ram) / mib, static_cast<double>(ss.ram_peak) / mib, static_cast<double>(ss.ram_budget) / mib,
            static_cast<double>(ss.file) / mib, static_cast<double>(ss.file_peak) / mib, ss.file_path.c_str());
}

void init() {
    State& s = st();
    if (s.inited) return;
    s.inited = true;
    s.src = live_source();
    s.scanner = std::make_unique<Scanner>(s.src);
    s.filter = RegionFilter{};
    init_look();
    const th::LookSpec& spec = th::look_spec();
    s.pause_while_open = config_get_bool("cheats", "pause_game", false);
    s.hud_mini = config_get_bool("cheats", "mini", false);
    s.dock = spec.dock == "left" ? 0 : spec.dock == "right" ? 2 : 1;
    s.close_key = config_get_str("cheats", "key", "F3");
    build_windows();
    Engine::instance().reload();
    // the first page: the game's cheat codes when it has some, else finding a value
    s.page = Engine::instance().cheats().empty() ? PageFind : PageCheats;
    s.lua_host = std::make_unique<LiveLuaHost>();
    s.lua = std::make_unique<lua::Vm>(s.lua_host.get());
    load_saved();
    // Lua scripts that were running come back
    for (const auto& l : s.lua_scripts)
        if (l.run) {
            std::string err;
            const uint32_t id = s.lua->load(l.name, l.source, &err);
            if (id && err.empty()) s.lua_ids[l.name] = id;
        }
    test_init();
    s.frame_cb = ctl::add_frame_callback(&on_frame, nullptr);
    NXLOG_I(kTag, "overlay ready: %zu table entries, theme %s (%s), accent %s", s.list.size(), th::theme().name.c_str(), th::theme().source.c_str(),
            s.accent.c_str());
}

namespace {
bool g_close_req = false;
void draw_hud(bool interactive);   // the HUD (below)
}
bool take_close_request() {
    const bool r = g_close_req;
    g_close_req = false;
    return r;
}

void draw(bool visible, bool has_game) {
    State& s = st();
    if (!visible) return;
    if (!s.inited) init();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    const th::Palette& p = th::pal();
    // a 150 ms fade-in when it opens ([cheats] animations); off: it appears at once
    if (s.animations) ImGui::GetStyle().Alpha = static_cast<float>(std::clamp((now_s() - s.shown_at) / 0.15, 0.15, 1.0));
    // the pad's edges for this UI frame (X / Y / L / R / ZL / ZR / -)
    {
        std::lock_guard<std::mutex> lk2(s.in_mu);
        s.pad_edge = s.pad_down & ~s.pad_prev_ui;
        s.pad_prev_ui = s.pad_down;
    }
    const bool modal = ImGui::GetTopMostPopupModal() != nullptr;
    if (!osk_open() && !modal && s.hotkey_capture < 0) {
        if (pad_pressed(hid::btn(hid::kBtnL)) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageUp)) set_page((s.page + PageCount - 1) % PageCount);
        if (pad_pressed(hid::btn(hid::kBtnR)) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageDown)) set_page((s.page + 1) % PageCount);
        if (s.toast.undo && s.toast.until > now_s() &&
            (pad_pressed(hid::btn(hid::kBtnMinus)) || (!ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)))) {
            auto fn = std::move(s.toast.undo);
            s.toast.undo = nullptr;
            s.toast.until = 0;
            if (fn) fn();
        }
        pad_paging();
    }
    th::push(th::Type::Body);
    // Peek (ZR, the overlay key or the eye button held): the panel fades almost out so the game shows; it keeps running behind it either way
    const bool peeking = s.peek_ui || s.peek_key || s.zr_peek;
    if (peeking) ImGui::GetStyle().Alpha = 0.07f;
    // the backdrop: the game stays visible, a little darker, so the panel reads
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!peeking)
        ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), p.scrim_top, p.scrim_top,
                                                                p.scrim_bottom, p.scrim_bottom);
    // the main panel: docked left or right (full height, about 40 % of the width), or in the centre where it was put for this game
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;
    const float margin = th::sp(th::S6);
    const float dw = std::min(W - 2.f * margin, std::max(th::px(880.f), W * 0.64f));
    const float dh = std::min(H - 2.f * margin, std::max(th::px(560.f), H * 0.84f));
    const bool docked = s.dock != 1;
    if (docked) {
        const float pw = std::min(W - 2.f * margin, std::clamp(W * 0.40f, th::px(560.f), th::px(760.f)));
        const float x = s.dock == 0 ? margin : W - margin - pw;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + x, vp->WorkPos.y + margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(pw, H - 2.f * margin), ImGuiCond_Always);
        s.main_rect_applied = false;   // back to the centre: where it was
    } else if (!s.main_rect_applied) {
        s.main_rect_applied = true;
        // the saved rectangle is in UI units (pixels / UI scale): the same place and size on a 4K display as at 1080p
        const float k = th::k();
        const float rx = s.main_rect[0] * k, ry = s.main_rect[1] * k, rw = s.main_rect[2] * k, rh = s.main_rect[3] * k;
        const bool fits = s.main_rect[2] > 0 && rx >= 0 && ry >= 0 && rx + rw <= W + 1.f && ry + rh <= H + 1.f;
        if (fits) {
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + rx, vp->WorkPos.y + ry), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(rw, rh), ImGuiCond_Always);
        } else {
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(dw, dh), ImGuiCond_Always);
        }
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(W, th::px(520.f)), std::min(H, th::px(380.f))), ImVec2(W, H));
    const bool glass = th::glass() && s.backdrop_on && !peeking;
    s.glass_drawn = false;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, glass ? IM_COL32(0, 0, 0, 0) : p.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, glass ? IM_COL32(0, 0, 0, 0) : p.line_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(th::sp(th::S6), th::sp(th::S5)));
    const ImGuiWindowFlags mfl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 (docked ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize : 0);
    ImRect panel;
    float hints_h = 0.f;
    if (ImGui::Begin("AnySwitch cheats###cheats_main", nullptr, mfl)) {
        ImGui::PopStyleVar();
        if (glass) {
            // the game blurred behind the panel (overlay.cpp blurs this rectangle of the frame), the theme's tint over it, a light rim
            ImGuiWindow* gw = ImGui::GetCurrentWindow();
            const ImRect r = gw->Rect();
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float rr = ImGui::GetStyle().WindowRounding;
            ImDrawList* dl = gw->DrawList;
            dl->AddImageRounded(s.backdrop, r.Min, r.Max, ImVec2(r.Min.x / disp.x, r.Min.y / disp.y), ImVec2(r.Max.x / disp.x, r.Max.y / disp.y),
                                ImGui::GetColorU32(IM_COL32_WHITE), rr);
            dl->AddRectFilled(r.Min, r.Max, ImGui::GetColorU32(p.panel), rr);
            dl->AddRect(r.Min, r.Max, ImGui::GetColorU32(p.line_strong), rr, 0, std::max(1.f, th::px(1.f)));
            s.glass_rect[0] = static_cast<int>(r.Min.x);
            s.glass_rect[1] = static_cast<int>(r.Min.y);
            s.glass_rect[2] = static_cast<int>(r.GetWidth() + 0.5f);
            s.glass_rect[3] = static_cast<int>(r.GetHeight() + 0.5f);
            s.glass_drawn = true;
        }
        if (draw_header(has_game)) g_close_req = true;
        draw_tabs();
        ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
        hints_h = ImGui::GetFrameHeight() + th::sp(th::S4);
        ImGui::BeginChild("##page", ImVec2(0, -hints_h), ImGuiChildFlags_NavFlattened);
        switch (s.page) {
            case PageCheats: page_cheats(); break;
            case PageFind: page_find(); break;
            case PageTable: page_table(); break;
            default: page_advanced(); break;
        }
        ImGui::EndChild();
        // the hint bar: a hairline, then the hints
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lp.x, lp.y + th::sp(th::S1)), ImVec2(lp.x + ImGui::GetContentRegionAvail().x, lp.y + th::sp(th::S1)),
                                            ImGui::GetColorU32(p.line));
        ImGui::SetCursorScreenPos(ImVec2(lp.x, lp.y + th::sp(th::S3)));
        draw_hints();
        ImGuiWindow* mw = ImGui::GetCurrentWindow();
        panel = mw->Rect();
        const float k = th::k();
        const int r[4] = {static_cast<int>(std::lround((mw->Pos.x - vp->WorkPos.x) / k)), static_cast<int>(std::lround((mw->Pos.y - vp->WorkPos.y) / k)),
                          static_cast<int>(std::lround(mw->Size.x / k)), static_cast<int>(std::lround(mw->Size.y / k))};
        if (!docked && std::memcmp(r, s.main_rect, sizeof r) != 0) {
            std::memcpy(s.main_rect, r, sizeof r);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) mark_dirty();   // moved or resized: saved after the drag
        }
    } else {
        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);

    // tools in windows of their own
    for (size_t wi = 0; wi < s.wins.size(); ++wi) {
        Win& w = s.wins[wi];
        if (!w.open) continue;
        if (w.rect[2] > 0) {
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(w.rect[0]), static_cast<float>(w.rect[1])), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w.rect[2]), static_cast<float>(w.rect[3])), ImGuiCond_FirstUseEver);
        } else {
            const float step = static_cast<float>(wi % 6) * th::px(28.f);
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + th::sp(th::S6) + step, vp->WorkPos.y + th::sp(th::S6) + step), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(std::min(th::px(680.f), W * 0.5f), std::min(th::px(560.f), H * 0.66f)), ImGuiCond_FirstUseEver);
        }
        const bool was = w.open;
        w.draw();
        if (was != w.open) mark_dirty();
        if (ImGuiWindow* iw = ImGui::FindWindowByName(w.title)) {     // remembered for the saved layout
            w.rect[0] = static_cast<int>(iw->Pos.x);
            w.rect[1] = static_cast<int>(iw->Pos.y);
            w.rect[2] = static_cast<int>(iw->Size.x);
            w.rect[3] = static_cast<int>(iw->Size.y);
        }
    }
    win_script_editor();
    draw_signature_maker();
    draw_hud(true);
    draw_toast(panel, hints_h);
    draw_confirm();
    osk_draw();
    if (peeking) {   // a small note at full strength: the panel comes back on release
        const char* t = "Peeking: let go to come back";
        th::push(th::Type::Small);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const ImVec2 c(vp->WorkPos.x + W * 0.5f, vp->WorkPos.y + th::sp(th::S4) + ts.y);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float pw = ts.x + th::sp(th::S5) * 2.f, ph = ts.y + th::sp(th::S3);
        fg->AddRectFilled(ImVec2(c.x - pw * 0.5f, c.y - ph * 0.5f), ImVec2(c.x + pw * 0.5f, c.y + ph * 0.5f), th::with_alpha(th::theme().dialog, 0.9f), ph * 0.5f);
        fg->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), th::pal().text | IM_COL32_A_MASK, t);
        th::pop();
    }
    th::pop();
    if (s.dirty && now_s() - s.last_save > 5.0) save();
}

// ---- the HUD -----------------------------------------------------------------------------------------------------------------------
namespace {
struct HudLine { std::string name, value; int kind; bool on; };   // kind 0 a table value, 1 a cheat code, 2 the game's state
std::vector<HudLine> hud_lines() {
    State& s = st();
    std::vector<HudLine> out;
    const std::string ct = ctl::status_text();
    if (!ct.empty()) out.push_back({ct, "", 2, true});
    for (Entry& e : s.list) {
        const bool active = e.freeze_id || (e.is_script && e.script_on);
        if (!e.pinned && !(s.hud_mini && (active || !e.hotkey.empty()))) continue;
        HudLine l{e.desc, "", 0, active};
        if (e.is_script) l.value = e.script_on ? "on" : "off";
        else l.value = e.freeze_id ? e.value : value_text(entry_address(e), e.type);
        if (!e.hotkey.empty()) l.name += "   " + e.hotkey;
        out.push_back(l);
        if (out.size() >= 12) break;
    }
    if (!s.pinned_cheats.empty())
        for (const auto& r : Engine::instance().cheats())
            if (!r.master && s.pinned_cheats.count(r.name)) out.push_back({r.name, r.enabled ? "on" : "off", 1, r.enabled});
    return out;
}

// The HUD: the cheats menu's pins, drawn by the shared pin engine (pin_board.h) as one group over the game while the panel is closed (no
// input: the game keeps it), and over the panel while it is open (drag it to move it; its place is saved for the game). Its own toggle
// ([cheats] hud_key) hides it without unpinning anything.
void draw_hud(bool interactive) {
    State& s = st();
    if (!s.hud_shown && !interactive) return;
    const std::vector<HudLine> lines = hud_lines();
    if (lines.empty()) return;
    play::pins::Board b;
    play::pins::Group g;
    g.id = "hud";
    g.items = {"hud"};
    g.name = interactive ? (s.hud_shown ? "HUD: drag to move" : "HUD (hidden over the game)") : "";
    g.x = static_cast<float>(s.hud_pos[0]);
    g.y = static_cast<float>(s.hud_pos[1]);
    g.opacity = static_cast<int>(std::lround(s.hud_opacity * 100.f));
    b.groups.push_back(g);
    auto items = [&](const std::string&, std::vector<play::pins::Line>& out) {
        for (const HudLine& l : lines) {
            play::pins::Line pl;
            pl.label = l.name;
            pl.value = l.value;
            pl.tone = l.kind == 2 ? 2 : l.on ? 1 : 0;
            // the HUD's marks: the game paused, a frozen value (a snowflake), a cheat code on or off (a dot)
            pl.icon = l.kind == 2 ? static_cast<int>(th::Icon::Pause) : (l.kind == 0 && l.on) ? static_cast<int>(th::Icon::Snow) : 0;
            out.push_back(std::move(pl));
        }
    };
    bool moved = false;
    play::pins::draw(b, items, interactive, "cheats", &moved);
    if (moved) {
        s.hud_pos[0] = static_cast<int>(b.groups[0].x);
        s.hud_pos[1] = static_cast<int>(b.groups[0].y);
        mark_dirty();
    }
}
}  // namespace

bool hud_wanted() {
    State& s = st();
    if (!s.inited || !s.hud_shown) return false;
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    if (!s.pinned_cheats.empty() || !ctl::status_text().empty()) return true;
    for (const Entry& e : s.list)
        if (e.pinned || (s.hud_mini && (e.freeze_id || (e.is_script && e.script_on) || !e.hotkey.empty()))) return true;
    return false;
}

void set_key_peek(bool on) { st().peek_key = on; }

void toggle_hud() {
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    s.hud_shown = !s.hud_shown;
    mark_dirty();
    NXLOG_I(kTag, "HUD %s", s.hud_shown ? "shown" : "hidden");
}
bool hud_shown() { return st().hud_shown; }

bool wants_glass() {
    const State& s = st();
    return s.look_inited && (s.style == 1 || (s.style < 0 && th::theme().glass));
}

void set_backdrop(ImTextureRef tex) {
    State& s = st();
    s.backdrop = tex;
    s.backdrop_on = tex.GetTexID() != ImTextureID_Invalid || tex._TexData != nullptr;
}
bool backdrop_rect(int out[4], float* strength) {
    State& s = st();
    if (!s.glass_drawn) return false;
    for (int i = 0; i < 4; ++i) out[i] = s.glass_rect[i];
    *strength = th::glass_blur();
    return true;
}

void draw_mini() {
    State& s = st();
    if (!s.inited) return;
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    // drawn while the panel is closed, under the dev menu's style: the overlay's own theme around it
    ImGuiStyle saved = ImGui::GetStyle();
    apply_theme(false);
    th::push(th::Type::Body);
    draw_hud(false);
    th::pop();
    ImGui::GetStyle() = saved;
}

void key_event(int sc, bool down) {
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.in_mu);
        if (down) s.keys_down.insert(sc);
        else s.keys_down.erase(sc);
        s.pad_last = false;
    }
    if (down && s.hotkey_capture >= 0 && sc != SDL_SCANCODE_ESCAPE) {
        std::lock_guard<std::recursive_mutex> lk(s.mu);
        if (s.hotkey_capture < static_cast<int>(s.list.size())) {
            s.list[s.hotkey_capture].hotkey = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc));
            s.list[s.hotkey_capture].hk_down = true;
            toast("Hotkey " + s.list[s.hotkey_capture].hotkey + " set for \"" + s.list[s.hotkey_capture].desc + "\"");
            mark_dirty();
        }
        s.hotkey_capture = -1;
    } else if (down && sc == SDL_SCANCODE_ESCAPE) {
        s.hotkey_capture = -1;
    }
}

void pad_input(uint64_t b) {
    State& s = st();
    uint64_t prev;
    {
        std::lock_guard<std::mutex> lk(s.in_mu);
        prev = s.pad_down;
        s.pad_down = b;
        if (b & ~prev) s.pad_last = true;
    }
    if (s.hotkey_capture >= 0) {
        g_capture_bits |= b & 0xffffull;            // the face, shoulder, stick-click, +/- and d-pad bits
        if (b == 0 && g_capture_bits) {
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            if (s.hotkey_capture < static_cast<int>(s.list.size())) {
                s.list[s.hotkey_capture].hotkey = pad_combo_name(g_capture_bits);
                s.list[s.hotkey_capture].hk_down = true;
                toast("Hotkey " + s.list[s.hotkey_capture].hotkey + " set for \"" + s.list[s.hotkey_capture].desc + "\"");
                mark_dirty();
            }
            s.hotkey_capture = -1;
            g_capture_bits = 0;
        }
    } else {
        g_capture_bits = 0;
    }
}

bool hotkey_capture_active() { return st().hotkey_capture >= 0; }
bool keyboard_open() { return osk_open(); }

bool pause_while_open() { return st().pause_while_open; }
void on_shown() {
    State& s = st();
    s.shown_at = now_s();
    s.main_rect_applied = false;      // where it was saved (or centred), also after the window changed size
}

bool wants_frame_colour() { return st().accent == "auto" || !st().inited; }
void set_frame_colour(const unsigned char rgba[4]) {
    State& s = st();
    std::memcpy(s.frame_rgba, rgba, 4);
    s.have_frame_rgba = true;
    const ImU32 a = th::accent_colour("auto", rgba);   // what `accent = auto` makes of it (logged: the evidence that it follows the game's frame)
    NXLOG_I(kTag, "auto accent: the frame's standing-out colour #%02x%02x%02x -> accent #%02x%02x%02x", rgba[0], rgba[1], rgba[2],
            static_cast<unsigned>((a >> IM_COL32_R_SHIFT) & 0xFF), static_cast<unsigned>((a >> IM_COL32_G_SHIFT) & 0xFF),
            static_cast<unsigned>((a >> IM_COL32_B_SHIFT) & 0xFF));
}

}  // namespace nxrt::cheats::ui
