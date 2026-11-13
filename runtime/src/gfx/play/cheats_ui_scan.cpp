// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's search pages and tools: Find a value (the guided scan: every value type, text and AOB, every first and next
// comparison), My table (freeze, edit, hotkeys, groups, pointer and AOB-anchored entries, AOB script entries, Atmosphère export, undo) and
// the Pointer scanner (multi-level paths, the restart rescan).
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../cheats/aob.h"
#include "../../cheats/codepatch.h"
#include "../../cheats/control.h"
#include "../../cheats/export.h"
#include "../../cheats/gmem.h"
#include "cheats_ui_internal.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/module.h"

namespace nxrt::cheats::ui {
namespace {

const char* kFirstNames[] = {"Exact value", "Unknown initial value", "Value between", "Bigger than", "Smaller than", "At least", "At most"};
const FirstScan kFirsts[] = {FirstScan::Exact, FirstScan::Unknown, FirstScan::Between, FirstScan::Greater, FirstScan::Less,
                             FirstScan::GreaterOrEqual, FirstScan::LessOrEqual};
const char* kNextNames[] = {"Exact value", "Changed", "Unchanged", "Increased", "Decreased", "Increased by", "Decreased by", "Value between",
                            "Bigger than", "Smaller than", "Same as first scan"};
const NextScan kNexts[] = {NextScan::Exact, NextScan::Changed, NextScan::Unchanged, NextScan::Increased, NextScan::Decreased,
                           NextScan::IncreasedBy, NextScan::DecreasedBy, NextScan::Between, NextScan::Greater, NextScan::Less,
                           NextScan::SameAsFirst};

bool scanning() { return st().prog.running.load(); }

ScanOptions options() {
    State& s = st();
    ScanOptions o;
    o.type = kind_type(s.kind_i);
    o.kind = s.kind_i < 6 ? ScanKind::Numeric : s.kind_i == 6 ? ScanKind::AllNumeric : s.kind_i == 7 ? ScanKind::Utf8 : s.kind_i == 8 ? ScanKind::Utf16 : ScanKind::Aob;
    o.fast = s.fast;
    o.filter = s.filter;
    o.ignore_case = s.ignore_case;
    if (o.kind != ScanKind::Numeric) o.text = s.val1;
    o.spill_dir = scan_spill_dir();
    return o;
}

void join_scan() {
    State& s = st();
    if (s.scan_thread && s.scan_thread->joinable()) s.scan_thread->join();
    s.scan_thread.reset();
}

void start_first_scan() {
    State& s = st();
    if (scanning()) return;
    const ScanOptions opt = options();
    const FirstScan cmp = kFirsts[s.first_i];
    Value v1{}, v2{};
    if (opt.kind == ScanKind::Numeric || opt.kind == ScanKind::AllNumeric) {
        const ScanType pt = opt.kind == ScanKind::AllNumeric ? ScanType::F64 : opt.type;
        if (cmp != FirstScan::Unknown && !parse_value(pt, s.val1, &v1)) { s.scan_msg = "\"" + std::string(s.val1) + "\" is not a number"; return; }
        if (opt.kind == ScanKind::AllNumeric) parse_value(ScanType::U64, s.val1, &v1);   // the text is what each type parses
        if (cmp == FirstScan::Between && !parse_value(opt.kind == ScanKind::AllNumeric ? ScanType::U64 : opt.type, s.val2, &v2)) {
            s.scan_msg = "\"" + std::string(s.val2) + "\" is not a number";
            return;
        }
    } else if (!s.val1[0]) {
        s.scan_msg = opt.kind == ScanKind::Aob ? "Type a byte pattern first (like 48 8B ?? 4?)" : "Type the text to find first";
        return;
    }
    join_scan();
    s.prog.cancel = false;
    s.prog.running = true;
    s.scan_msg.clear();
    s.result_sel = SIZE_MAX;
    const bool pause = s.pause_scan;
    s.scan_thread = std::make_unique<std::thread>([opt, cmp, v1, v2, pause] {
        State& s = st();
        const bool was = ctl::paused();
        if (pause && !was) ctl::pause(true);
        s.scanner->first_scan(opt, cmp, v1, v2, s.prog);
        if (pause && !was) ctl::pause(false);
        s.first_done = true;
        s.last_count = s.scanner->result_count();
        NXLOG_I("cheats", "first scan (%s): %zu results", kind_name(opt.kind), s.last_count);
        after_scan("first scan");
        {
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            char b[256];
            // the step in plain words: "First search, exactly 100 (4 bytes): 37 found"
            static const char* plain[] = {"exactly", "an unknown value", "between", "bigger than", "smaller than", "at least", "at most"};
            const int ci = static_cast<int>(cmp);
            std::string what = plain[ci];
            if (cmp != FirstScan::Unknown) what += std::string(" ") + s.val1;
            if (cmp == FirstScan::Between) what += std::string(" and ") + s.val2;
            std::snprintf(b, sizeof b, "First search, %s (%s): %zu found", what.c_str(), opt.kind == ScanKind::Numeric ? type_name(opt.type) : kind_name(opt.kind),
                          s.last_count);
            s.history.push_back(b);
        }
    });
}

void start_next_scan() {
    State& s = st();
    if (scanning() || !s.first_done) return;
    const NextScan cmp = kNexts[s.next_i];
    const ScanType t = s.scanner->type();
    Value v1{}, v2{};
    const bool needs = cmp == NextScan::Exact || cmp == NextScan::IncreasedBy || cmp == NextScan::DecreasedBy || cmp == NextScan::Between ||
                       cmp == NextScan::Greater || cmp == NextScan::Less;
    const bool pattern = s.scanner->kind() == ScanKind::Utf8 || s.scanner->kind() == ScanKind::Utf16 || s.scanner->kind() == ScanKind::Aob;
    if (needs && !pattern && !parse_value(t, s.val1, &v1)) { s.scan_msg = "\"" + std::string(s.val1) + "\" is not a number"; return; }
    if (cmp == NextScan::Between) parse_value(t, s.val2, &v2);
    join_scan();
    s.prog.cancel = false;
    s.prog.running = true;
    s.scan_msg.clear();
    s.result_sel = SIZE_MAX;
    s.scan_thread = std::make_unique<std::thread>([cmp, v1, v2] {
        State& s = st();
        s.scanner->next_scan(cmp, v1, v2, s.prog);
        s.last_count = s.scanner->result_count();
        NXLOG_I("cheats", "next scan: %zu results", s.last_count);
        after_scan("next scan");
        {
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            char b[256];
            const bool v = cmp == NextScan::Exact || cmp == NextScan::IncreasedBy || cmp == NextScan::DecreasedBy || cmp == NextScan::Between ||
                           cmp == NextScan::Greater || cmp == NextScan::Less;
            static const char* plain[] = {"it is now", "it changed", "it didn't change", "it went up", "it went down", "it went up by", "it went down by",
                                          "it is between", "it is bigger than", "it is smaller than", "it is as at the first search"};
            std::string what = plain[static_cast<int>(cmp)];
            if (v) what += std::string(" ") + s.val1;
            if (cmp == NextScan::Between) what += std::string(" and ") + s.val2;
            std::snprintf(b, sizeof b, "Then %s: %zu found", what.c_str(), s.last_count);
            s.history.push_back(b);
        }
    });
}

std::string bytes_text(uint64_t addr, size_t n, bool ascii) {
    std::string out;
    std::vector<uint8_t> b(std::min<size_t>(n, 32));
    if (!gmem::read(addr, b.data(), b.size())) return "??";
    char h[8];
    for (size_t i = 0; i < b.size(); ++i) {
        if (ascii) out += (b[i] >= 32 && b[i] < 127) ? static_cast<char>(b[i]) : '.';
        else { std::snprintf(h, sizeof h, "%s%02X", i ? " " : "", b[i]); out += h; }
    }
    return out;
}

// ---- AOB scripts in the table --------------------------------------------------------------------------------------------------------
// Writes go to guest memory; a write into code goes through the live code patcher (codepatch.h), so the running code changes too.
class LiveScriptHost : public aa::ScriptHost {
public:
    MemSource& mem() override { return *st().src; }
    bool module_range(const std::string& name, uint64_t* lo, uint64_t* hi) override {
        Module* m = module_by_name(name.c_str());
        if (!m) return false;
        *lo = m->base();
        *hi = m->base() + m->size();
        return true;
    }
    bool write(const aa::Write& w, std::string* err) override {
        std::string mod;
        uint64_t fs = 0, fe = 0;
        if (cp::function_at(w.addr, &mod, &fs, &fe) && (w.addr & 3) == 0 && (w.bytes.size() & 3) == 0) {
            if (w.bytes == w.original) return true;
            // putting the original bytes back is a revert of the patch made there
            for (const cp::Patch& p : cp::patches())
                if (p.addr == w.addr && p.state != cp::State::Reverted && p.original == w.bytes) return cp::revert(w.addr, err);
            return cp::apply(w.addr, w.bytes, err);
        }
        if (!gmem::write(w.addr, w.bytes.data(), w.bytes.size())) {
            *err = "the memory is not writable";
            return false;
        }
        return true;
    }
    std::map<std::string, uint64_t>& symbols() override { return syms_; }
private:
    std::map<std::string, uint64_t> syms_;
};
LiveScriptHost& script_host() {
    static LiveScriptHost h;
    return h;
}

// Applies an AOB script entry's wanted state (on / off) when it differs from what was last run.
void sync_script(Entry& e) {
    if (!e.is_script) return;
    const bool want = e.script_on;
    const bool have = e.script_run.ok;
    if (want == have) return;
    aa::Script sc;
    std::string err;
    if (!aa::parse(e.script, &sc, &err)) {
        e.status = "the script does not parse: " + err;
        e.script_on = false;
        e.script_run = aa::RunResult();
        return;
    }
    if (want) {
        aa::RunResult r = aa::enable(sc, script_host());
        if (r.ok) {
            e.script_run = r;
            e.status = "on (" + std::to_string(r.writes.size()) + " writes)";
        } else {
            e.script_on = false;
            e.script_run = aa::RunResult();
            e.status = r.error + ": turned off";
        }
        NXLOG_I("cheats", "script %s: %s", e.desc.c_str(), e.status.c_str());
    } else {
        aa::RunResult r = aa::disable(sc, script_host(), e.script_run);
        e.status = r.ok ? "off" : "the [DISABLE] part failed: " + r.error;
        e.script_run = aa::RunResult();
        NXLOG_I("cheats", "script %s: %s", e.desc.c_str(), e.status.c_str());
    }
    mark_dirty();
}

// The Atmosphère cheat for an entry (a static write, or a pointer path).
std::string atmosphere_text(Entry& e) {
    Value v{};
    if (!parse_value(e.type, e.value, &v)) {
        uint64_t bits = 0;
        gmem::read(entry_address(e), &bits, type_size(e.type));
        v = Value{e.type, bits};
    }
    std::string body;
    if (e.is_pointer) body = export_pointer_body(e.path.mem_type, e.path.base_offset, e.path.offsets, v);
    else {
        const uint64_t a = entry_address(e);
        const uint64_t mb = gmem::main_base();
        if (a >= mb && a < mb + gmem::main_size()) body = export_static_body(0, a - mb, v);
        else if (a >= gmem::region_base(1) && gmem::region_base(1)) body = export_static_body(1, a - gmem::region_base(1), v);
        else body = export_static_body(0, a - mb, v);
    }
    return export_cheat(e.desc, body);
}

}  // namespace

void sync_scripts() {
    State& s = st();
    std::lock_guard<std::recursive_mutex> lk(s.mu);
    for (Entry& e : s.list) sync_script(e);
}

// ================================================================================================================ Find a value
// A guided scan: (1) what kind of value, in plain words, (2) what it is now (a number, or "I don't know"), search; then (3) change it in
// the game and say what happened (went up, went down, changed, didn't change, is now N), with the match count after every step, until
// (4) the one left goes to My table with A. Every scan type and comparison of the scanner is here, behind "More types" / "More ways".
namespace {
namespace th = theme;

std::string group_digits(uint64_t n) {
    std::string d = std::to_string(n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

struct KindCard { int kind; th::Icon icon; const char* title; const char* body; };
const KindCard kCards[] = {
    {2, th::Icon::Number, "Whole number", "Coins, lives, ammo, items"},
    {4, th::Icon::Decimal, "Decimal number", "Health bars, timers, positions"},
    {6, th::Icon::Question, "Not sure", "Tries every kind of number (slower)"},
    {7, th::Icon::Text, "Text", "Names and words"},
};
const char* kMoreKinds[] = {"Byte (0 to 255)", "2 bytes (up to 65,535)", "8 bytes (very large numbers)", "Double (a precise decimal)",
                            "Text in UTF-16", "Byte pattern (hex, ?? for any byte)"};
const int kMoreKindIdx[] = {0, 1, 3, 5, 8, 9};
// the first-scan comparisons behind "a range" (kFirstNames 2..6), and every next-scan comparison in plain words
const char* kRangeNames[] = {"Between two numbers", "Bigger than", "Smaller than", "At least", "At most"};
const char* kNextPlain[] = {"Is now exactly", "Changed", "Didn't change", "Went up", "Went down", "Went up by", "Went down by", "Is between",
                            "Is bigger than", "Is smaller than", "Is the same as at the first search"};
bool next_needs_value(int i) { return i == 0 || i == 5 || i == 6 || i == 7 || i == 8 || i == 9; }

bool is_pattern_kind(int k) { return k >= 7; }

// The Search options: which memory, aligned values, pausing: in plain words, the scanner's names in the tooltips.
void scan_options() {
    State& s = st();
    const th::Palette& p = th::pal();
    ImGui::PushStyleColor(ImGuiCol_Header, p.card);
    const bool open = ImGui::CollapsingHeader("Search options");
    ImGui::PopStyleColor();
    if (!open) return;
    ImGui::Indent(th::sp(th::S2));
    auto cb = [](const char* label, bool* v, const char* tip) {
        ImGui::Checkbox(label, v);
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
    };
    th::text(th::Type::Small, p.text2, "Where to look");
    cb("The game's heap (most values live here)", &s.filter.heap, "heap");
    cb("The game's own variables", &s.filter.main, "main data");
    cb("Shared and mirrored memory", &s.filter.alias, "alias");
    cb("Thread stacks", &s.filter.stack, "stacks");
    cb("Code", &s.filter.code, "code");
    cb("Everything else", &s.filter.other, "other");
    cb("Only memory the game can change", &s.filter.writable_only, "writable only");
    th::text(th::Type::Small, p.text2, "How");
    if (is_pattern_kind(s.kind_i)) cb("Ignore upper and lower case", &s.ignore_case, nullptr);
    else cb("Fast: only values at even addresses (finds almost everything)", &s.fast, "aligned");
    cb("Pause the game while searching", &s.pause_scan, nullptr);
    ImGui::Unindent(th::sp(th::S2));
}

// A row of equal pills, one of them chosen (a segmented control). Returns true when the choice changed.
bool segmented(const char* id, const char* const* labels, int n, int* sel) {
    bool changed = false;
    ImGui::PushID(id);
    const float gap = th::sp(th::S2);
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, gap);
        if (th::button(labels[i], *sel == i ? th::Icon::Check : th::Icon::None, *sel == i ? th::Btn::Primary : th::Btn::Secondary)) {
            changed = *sel != i;
            *sel = i;
        }
    }
    ImGui::PopID();
    return changed;
}

void add_result(const ScanResult& r) {
    State& s = st();
    add_entry(r.addr, r.type, "Value " + std::to_string(++s.find_added));
}

// The matches: their value now, before, and where; A / click adds one to My table, X / right-click has the rest.
void results_list(bool pattern) {
    State& s = st();
    const th::Palette& p = th::pal();
    const size_t count = s.scanner->result_count();
    th::text(th::Type::Strong, p.text, "Matches");
    if (count > 5000) {
        ImGui::SameLine();
        th::text(th::Type::Small, p.text3, "  too many to look through: narrow them down first");
    }
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("results", 3, fl, ImVec2(0, 0))) return;
    ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn(pattern ? "" : "Before", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupScrollFreeze(0, 1);
    th::push(th::Type::Small);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text3);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    th::pop();
    ImGuiListClipper clip;
    const size_t n = std::min<size_t>(count, 1000000);
    clip.Begin(static_cast<int>(n));
    const float row_h = std::round(th::px(36.f));
    while (clip.Step()) {
        const auto res = s.scanner->results(static_cast<size_t>(clip.DisplayStart), static_cast<size_t>(clip.DisplayEnd - clip.DisplayStart));
        for (size_t k = 0; k < res.size(); ++k) {
            const ScanResult& r = res[k];
            const size_t i = static_cast<size_t>(clip.DisplayStart) + k;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(i));
            const bool changed = !pattern && r.last.bits != r.first.bits;
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##r", s.result_sel == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h))) {
                s.result_sel = i;
                add_result(r);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Add it to My table");
            if (ImGui::IsItemFocused() && pad_mode() && pad_pressed(hid::btn(hid::kBtnX))) ImGui::OpenPopup("res_ctx");
            if (ImGui::BeginPopupContextItem("res_ctx")) {
                th::text(th::Type::Small, p.text3, "%s", gmem::describe(r.addr).c_str());
                if (ImGui::MenuItem("Add to My table")) add_result(r);
                if (ImGui::MenuItem("Find what writes it")) { std::snprintf(s.aw_addr, sizeof s.aw_addr, "%llX", static_cast<unsigned long long>(r.addr)); s.aw_mode = 0; open_window("Find what accesses"); }
                if (ImGui::MenuItem("Find what reads it")) { std::snprintf(s.aw_addr, sizeof s.aw_addr, "%llX", static_cast<unsigned long long>(r.addr)); s.aw_mode = 1; open_window("Find what accesses"); }
                if (ImGui::MenuItem("Find a pointer path to it (survives a restart)")) { std::snprintf(s.ptr_target, sizeof s.ptr_target, "%llX", static_cast<unsigned long long>(r.addr)); open_window("Pointer scanner"); }
                if (ImGui::MenuItem("Show it in the memory viewer")) goto_viewer(r.addr);
                if (ImGui::MenuItem("Disassemble here")) goto_disasm(r.addr);
                ImGui::EndPopup();
            }
            // the value now (marked when it moved since the first search), before, where
            ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + (row_h - ImGui::GetFontSize()) * 0.5f));
            th::push(th::Type::Strong);
            if (pattern) ImGui::TextUnformatted(bytes_text(r.addr, r.len, s.kind_i != 9).c_str());
            else if (changed) {
                const bool up = r.last.as_double() > r.first.as_double();
                th::icon_item(up ? th::Icon::Up : th::Icon::Down, ImGui::GetFontSize() * 0.9f, p.accent);
                ImGui::SameLine(0, th::sp(th::S1));
                ImGui::TextUnformatted(r.last.text().c_str());
            } else ImGui::TextUnformatted(r.last.text().c_str());
            th::pop();
            ImGui::TableSetColumnIndex(1);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - ImGui::GetFontSize()) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
            ImGui::TextUnformatted(pattern ? (s.kind_i == 9 ? "bytes" : "text") : r.first.text().c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - ImGui::GetFontSize()) * 0.5f);
            th::text(th::Type::Small, p.text3, "%s  \xC2\xB7  %s", gmem::describe(r.addr).c_str(), pattern ? (s.kind_i == 9 ? "bytes" : "text") : type_name(r.type));
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void find_choose() {
    State& s = st();
    const th::Palette& p = th::pal();
    th::text_wrapped(th::Type::Body, p.text2, "Find a number you see in the game, like your coins, then narrow the search down until one is left.");
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
    th::text(th::Type::Strong, p.text, "1   What kind of value is it?");
    const float gap = th::sp(th::S3);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > th::px(760.f) ? 4 : 2;
    const float cw = (avail - gap * (cols - 1)) / cols;
    for (int i = 0; i < 4; ++i) {
        if (i % cols) ImGui::SameLine(0, gap);
        const KindCard& c = kCards[i];
        if (th::choice_card(c.title, c.icon, c.title, c.body, s.kind_i == c.kind, ImVec2(cw, th::px(cols == 4 ? 84.f : 72.f)))) s.kind_i = c.kind;
    }
    // the other scan kinds
    int more = -1;
    for (int i = 0; i < 6; ++i)
        if (kMoreKindIdx[i] == s.kind_i) more = i;
    ImGui::SetNextItemWidth(th::px(320.f));
    if (ImGui::BeginCombo("##more", more >= 0 ? kMoreKinds[more] : "More types...")) {
        for (int i = 0; i < 6; ++i)
            if (ImGui::Selectable(kMoreKinds[i], more == i)) s.kind_i = kMoreKindIdx[i];
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, th::sp(th::S2)));
    const bool pattern = is_pattern_kind(s.kind_i);
    bool go = false;
    if (pattern) {
        th::text(th::Type::Strong, p.text, s.kind_i == 9 ? "2   Which bytes?" : "2   Which text?");
        if (s.kind_i == 9) go |= field("##v1", s.val1, sizeof s.val1, Keys::Text, "48 8B ?? 4? ...", 420);
        else go |= field("##v1", s.val1, sizeof s.val1, Keys::Text, "the text you see", 420);
    } else {
        th::text(th::Type::Strong, p.text, "2   What is it now?");
        const char* now[] = {"I know the number", "I don't know it", "It's in a range"};
        if (segmented("now", now, 3, &s.find_now)) s.first_i = s.find_now == 0 ? 0 : s.find_now == 1 ? 1 : std::max(2, s.first_i);
        if (s.find_now == 0) s.first_i = 0;
        if (s.find_now == 1) s.first_i = 1;
        if (s.find_now == 2) {
            int r = std::clamp(s.first_i - 2, 0, 4);
            ImGui::SetNextItemWidth(th::px(240.f));
            if (ImGui::Combo("##range", &r, kRangeNames, 5)) s.first_i = r + 2;
            s.first_i = r + 2;
            ImGui::SameLine();
        }
        if (s.first_i != 1) {
            go |= field("##v1", s.val1, sizeof s.val1, Keys::Number, "e.g. 100", 200);
            if (s.first_i == 2) {
                ImGui::SameLine();
                ImGui::TextUnformatted("and");
                ImGui::SameLine();
                go |= field("##v2", s.val2, sizeof s.val2, Keys::Number, "e.g. 200", 200);
            }
        }
    }
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
    if (th::button("Search", th::Icon::Search, th::Btn::Primary, 200.f, th::kControlLg)) go = true;
    if (go) start_first_scan();
    if (!s.scan_msg.empty()) th::text_wrapped(th::Type::Body, p.warn, "%s", s.scan_msg.c_str());
    const char* tip = pattern ? "Tip: names are usually stored as UTF-8 text; try UTF-16 under More types if nothing is found."
                    : s.first_i == 1 ? "Good for health bars and timers: next you change the value in the game and say whether it went up or down."
                    : s.kind_i == 6 ? "Not sure tries bytes, whole numbers and decimals at once: more matches, so expect a few more steps."
                    : "Tip: if nothing is found, the game may store it differently: try Decimal number, or Not sure.";
    th::text_wrapped(th::Type::Small, p.text3, "%s", tip);
    ImGui::Dummy(ImVec2(0, th::sp(th::S2)));
    scan_options();
}

void find_progress() {
    State& s = st();
    const th::Palette& p = th::pal();
    const uint64_t tot = s.prog.bytes_total.load(), don = s.prog.bytes_done.load();
    const float f = tot ? static_cast<float>(static_cast<double>(don) / static_cast<double>(tot)) : 0.f;
    ImGui::Dummy(ImVec2(0, th::sp(th::S6)));
    th::text(th::Type::Title, p.text, "Searching...");
    char b[96];
    std::snprintf(b, sizeof b, "%.0f%%", f * 100.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, th::px(6.f));
    ImGui::ProgressBar(f, ImVec2(-1, th::px(12.f)), "");
    ImGui::PopStyleVar();
    th::text(th::Type::Body, p.text2, "%s   %.0f of %.0f MiB   %s found so far", b, static_cast<double>(don) / 1048576.0, static_cast<double>(tot) / 1048576.0,
             group_digits(s.prog.results.load()).c_str());
    if (th::button("Stop", th::Icon::Close)) s.prog.cancel = true;
}

void find_narrow(float col_w) {
    State& s = st();
    const th::Palette& p = th::pal();
    const size_t count = s.last_count;
    const bool pattern = s.scanner->kind() == ScanKind::Utf8 || s.scanner->kind() == ScanKind::Utf16 || s.scanner->kind() == ScanKind::Aob;
    th::stat(group_digits(count).c_str(), count == 1 ? "match" : s.scanner->capped() ? "matches (the first ones: narrow it down)" : "matches",
             count == 0 ? p.warn : count <= 10 ? p.accent : 0);
    // the steps so far
    const size_t from = s.history.size() > 3 ? s.history.size() - 3 : 0;
    for (size_t i = from; i < s.history.size(); ++i) th::text_wrapped(th::Type::Small, p.text3, "%s", s.history[i].c_str());
    const float gap = th::sp(th::S2);
    const float bw = (col_w - gap) * 0.5f / th::k();
    if (count) {
        ImGui::BeginDisabled(!s.scanner->can_undo());
        if (th::button("Undo last step", th::Icon::Undo, th::Btn::Quiet, 0.f, th::kControlSm)) {
            s.scanner->undo();
            s.last_count = s.scanner->result_count();
            s.first_done = s.last_count > 0;
            if (!s.history.empty()) s.history.push_back("Undone: back to " + group_digits(s.last_count) + " found");
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        if (th::button("New search", th::Icon::Refresh, th::Btn::Quiet, 0.f, th::kControlSm)) {
            confirm("Start a new search?",
                    "The " + group_digits(count) + (count == 1 ? " match" : " matches") + " of this search are dropped. My table keeps what you added.",
                    "Start over", [] {
                        State& s2 = st();
                        s2.scanner->clear();
                        s2.first_done = false;
                        s2.last_count = 0;
                        s2.result_sel = SIZE_MAX;
                    });
        }
    }
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
    if (count == 0) {
        th::text_wrapped(th::Type::Body, p.text2,
                         "Nothing matched. The game may store it differently (try Decimal number or Not sure), or it changed while searching.");
        if (th::button("Start over", th::Icon::Refresh, th::Btn::Primary, col_w / th::k())) { s.scanner->clear(); s.first_done = false; s.last_count = 0; }
    } else {
        if (count <= 10) {
            const ImVec2 o = ImGui::GetCursorScreenPos();
            th::push(th::Type::Small);
            const float h = ImGui::GetTextLineHeight() + th::sp(th::S3) * 2.f;
            ImGui::GetWindowDrawList()->AddRectFilled(o, ImVec2(o.x + col_w, o.y + h), ImGui::GetColorU32(p.accent_tint), th::px(th::kRadiusSm));
            th::icon(ImGui::GetWindowDrawList(), th::Icon::Star, ImVec2(o.x + th::sp(th::S3) + ImGui::GetFontSize() * 0.5f, o.y + h * 0.5f), ImGui::GetFontSize(),
                     ImGui::GetColorU32(p.accent));
            ImGui::GetWindowDrawList()->AddText(ImVec2(o.x + th::sp(th::S3) + ImGui::GetFontSize() + th::sp(th::S2), o.y + th::sp(th::S3)), ImGui::GetColorU32(p.text),
                                                count == 1 ? "Found it: select it to add it to My table" : "Nearly: pick the one that moves with the game");
            th::pop();
            ImGui::Dummy(ImVec2(col_w, h));
        }
        th::text_wrapped(th::Type::Strong, p.text, pattern ? "Search these again" : "Change it in the game, then tell me what happened:");
        auto next = [&](int i) { s.next_i = i; start_next_scan(); };
        if (!pattern) {
            if (th::button("It went up", th::Icon::Up, th::Btn::Secondary, bw)) next(3);
            ImGui::SameLine(0, gap);
            if (th::button("It went down", th::Icon::Down, th::Btn::Secondary, bw)) next(4);
        }
        if (th::button("It changed", th::Icon::Changed, th::Btn::Secondary, bw)) next(1);
        ImGui::SameLine(0, gap);
        if (th::button("It didn't change", th::Icon::Same, th::Btn::Secondary, bw)) next(2);
        if (!pattern) {
            ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
            const float sbw = 120.f;
            bool go = field("##now", s.next_val, sizeof s.next_val, Keys::Number, "It's now exactly...", (col_w - gap) / th::k() - sbw);
            ImGui::SameLine(0, gap);
            go |= th::button("Search", th::Icon::Search, th::Btn::Primary, sbw);
            if (go && s.next_val[0]) {
                std::snprintf(s.val1, sizeof s.val1, "%s", s.next_val);
                next(0);
            }
            // every other comparison
            ImGui::SetNextItemWidth(col_w);
            int more = s.next_i >= 5 || s.next_i == 10 ? s.next_i : -1;
            if (ImGui::BeginCombo("##ways", more >= 0 ? kNextPlain[more] : "More ways to compare...")) {
                for (int i = 5; i < 11; ++i)
                    if (ImGui::Selectable(kNextPlain[i], more == i)) s.next_i = i;
                ImGui::EndCombo();
            }
            if (more >= 0) {
                if (next_needs_value(more)) {
                    field("##w1", s.val1, sizeof s.val1, Keys::Number, "value", more == 7 ? 120.f : 200.f);
                    if (more == 7) {
                        ImGui::SameLine();
                        field("##w2", s.val2, sizeof s.val2, Keys::Number, "and", 120.f);
                    }
                    ImGui::SameLine(0, gap);
                }
                if (th::button("Compare", th::Icon::Search, th::Btn::Secondary)) start_next_scan();
            }
        }
        if (!s.scan_msg.empty()) th::text_wrapped(th::Type::Body, p.warn, "%s", s.scan_msg.c_str());
    }
}
}  // namespace

void page_find() {
    State& s = st();
    const bool busy = scanning();
    if (busy) { find_progress(); return; }
    if (!s.first_done) {
        ImGui::BeginChild("##find_choose", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        find_choose();
        ImGui::EndChild();
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool wide = avail > th::px(760.f);
    const float col_w = wide ? std::min(avail * 0.42f, th::px(420.f)) : avail;
    ImGui::BeginChild("##find_left", ImVec2(wide ? col_w : 0.f, wide ? 0.f : ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_NavFlattened);
    find_narrow(ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    if (wide) ImGui::SameLine(0, th::sp(th::S5));
    ImGui::BeginChild("##find_right", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    const bool pattern = s.scanner->kind() == ScanKind::Utf8 || s.scanner->kind() == ScanKind::Utf16 || s.scanner->kind() == ScanKind::Aob;
    if (s.last_count) results_list(pattern);
    else th::empty_state(th::Icon::Search, "No matches", "Start over with another kind of value.");
    ImGui::EndChild();
}

// ================================================================================================================ My table
namespace {
const char* kFreezeName[] = {"frozen", "frozen, can go up", "frozen, can go down"};

void remove_entry(size_t i) {
    State& s = st();
    Entry e = s.list[i];
    const bool was_frozen = e.freeze_id != 0, was_on = e.is_script && e.script_on;
    set_freeze(s.list[i], false);
    if (s.list[i].is_script && s.list[i].script_on) { s.list[i].script_on = false; sync_script(s.list[i]); }
    s.watches.erase(std::remove_if(s.watches.begin(), s.watches.end(), [&](const Watch& w) { return w.uid == e.uid; }), s.watches.end());
    s.list.erase(s.list.begin() + static_cast<long>(i));
    mark_dirty();
    e.freeze_id = 0;
    e.watch = false;
    e.script_run = aa::RunResult();
    e.script_on = false;
    toast_undo("Removed \"" + e.desc + "\"", [e, i, was_frozen, was_on]() mutable {
        State& s2 = st();
        std::lock_guard<std::recursive_mutex> lk(s2.mu);
        const size_t at = std::min(i, s2.list.size());
        s2.list.insert(s2.list.begin() + static_cast<long>(at), e);
        Entry& back = s2.list[at];
        if (was_frozen) set_freeze(back, true);
        if (was_on) back.script_on = true;     // sync_scripts runs its [ENABLE] again
        mark_dirty();
    });
}
}  // namespace

void page_table() {
    State& s = st();
    const th::Palette& p = th::pal();
    sync_scripts();
    // the toolbar
    const bool add_addr = th::button("Add address", th::Icon::Plus);
    nav_focus_here("Add address");
    if (add_addr) {
        Entry e;
        e.desc = "New entry";
        e.uid = s.next_uid++;
        s.list.push_back(e);
        mark_dirty();
    }
    ImGui::SameLine(0, th::sp(th::S2));
    if (th::button("Add script", th::Icon::Code)) {
        Entry e;
        e.desc = "New script";
        e.is_script = true;
        e.script = "[ENABLE]\naobscanmodule(here, main, 00 00 00 00)\nhere:\n  nop\nregistersymbol(here)\n[DISABLE]\nhere:\n  db 00 00 00 00\nunregistersymbol(here)\n";
        e.uid = s.next_uid++;
        s.list.push_back(e);
        s.script_edit = static_cast<int>(s.list.size() - 1);
        s.script_text = e.script;
        mark_dirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("An AOB script: code changes that find their place by a byte pattern");
    ImGui::SameLine(0, th::sp(th::S2));
    ImGui::BeginDisabled(s.list.empty());
    if (th::button("Save as cheat codes", th::Icon::Export)) {
        std::string msg;
        const size_t n = export_list_to_cheat_file(&msg);
        if (n) toast(msg);
        else toast_warn(msg);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the values and pointer paths as Atmosphère cheat codes for this game: they then run on a console, Ryujinx or yuzu too");
    if (!s.list.empty()) {
        // beside the buttons when it fits, else on a line of its own (a docked panel)
        const float fw = th::px(200.f);
        ImGui::SameLine(0, th::sp(th::S2));
        const float room = ImGui::GetContentRegionAvail().x;
        if (room < fw) ImGui::NewLine();
        else ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - fw);
        field("##grp", s.group_filter, sizeof s.group_filter, Keys::Text, "Filter by group", 200);
    }
    if (s.hotkey_capture >= 0 && s.hotkey_capture < static_cast<int>(s.list.size())) {
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = th::px(44.f);
        ImGui::GetWindowDrawList()->AddRectFilled(o, ImVec2(o.x + w, o.y + h), ImGui::GetColorU32(p.accent_tint), th::px(th::kRadiusSm));
        ImGui::SetCursorScreenPos(ImVec2(o.x + th::sp(th::S3), o.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
        th::text(th::Type::Body, p.text, "Press a key or a controller button combination for \"%s\"   (Esc: no hotkey)",
                 s.list[s.hotkey_capture].desc.c_str());
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + th::sp(th::S2)));
    }
    if (s.list.empty()) {
        const float bh = th::px(th::kControlH) + th::sp(th::S4);
        th::empty_state(th::Icon::Table, "Your table is empty",
                        "Values you find with Find a value land here. Freeze them, change them, or give them a hotkey.",
                        ImGui::GetContentRegionAvail().y - bh);
        const float bw = th::px(200.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw) * 0.5f);
        if (th::button("Find a value", th::Icon::Search, th::Btn::Primary, 200.f)) set_page(PageFind);
        return;
    }

    int remove = -1;
    const ImGuiTableFlags tfl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("entries", 5, tfl)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, th::px(64.f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthFixed, th::px(120.f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, th::px(th::kControlH));
        ImGui::TableSetupScrollFreeze(0, 1);
        th::push(th::Type::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text3);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        th::pop();
        const float row_h = std::round(th::px(60.f));
        std::string last_group = "\x01";
        for (size_t i = 0; i < s.list.size(); ++i) {
            Entry& e = s.list[i];
            if (s.group_filter[0] && e.group.find(s.group_filter) == std::string::npos) continue;
            if (e.group != last_group && !e.group.empty()) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, th::px(36.f));
                ImGui::TableSetColumnIndex(0);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + th::sp(th::S2));
                th::icon_item(th::Icon::Layers, ImGui::GetFontSize(), p.accent);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + th::sp(th::S2));
                th::text(th::Type::Strong, p.accent, "%s", e.group.c_str());
            }
            last_group = e.group;
            ImGui::PushID(static_cast<int>(e.uid));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
            bool focus = false;
            const float fh = ImGui::GetFrameHeight();
            // on: freeze (a value) or run (a script)
            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - fh) * 0.5f);
            bool on = e.is_script ? e.script_on : e.freeze_id != 0;
            if (th::toggle("##on", &on)) {
                if (e.is_script) e.script_on = on;
                else set_freeze(e, on);
                mark_dirty();
            }
            focus |= ImGui::IsItemFocused();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip(e.is_script ? "Run the script" : "Freeze: keep the value as it is (the game can't change it)");
            // name, and where it is (its status when something is wrong)
            ImGui::TableSetColumnIndex(1);
            const uint64_t a = e.is_script ? 0 : entry_address(e);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(th::sp(th::S1), th::px(4.f)));
            th::push(th::Type::Strong);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + th::px(4.f));
            if (field("##d", &e.desc, Keys::Text, "name", -1)) mark_dirty();
            focus |= ImGui::IsItemFocused();
            th::pop();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("ent_ctx");
            std::string where;
            if (e.is_script) where = "AOB script";
            else if (e.is_pointer) where = "pointer " + e.path.text() + ", now at " + gmem::describe(a);
            else if (!e.signature.empty()) where = "found by signature at " + gmem::describe(a);
            else {
                const std::string lab = label_at(a);
                where = lab.empty() ? gmem::describe(a) : lab + "  (" + gmem::describe(a) + ")";
            }
            if (!e.is_script) where += std::string("  \xC2\xB7  ") + type_name(e.type);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + th::sp(th::S1));
            if (e.pinned) {
                th::icon_item(th::Icon::Pin, ImGui::GetFontSize() * 0.85f, p.accent);
                ImGui::SameLine(0, th::sp(th::S1));
            }
            if (!e.status.empty() && !(e.is_script && e.status.rfind("on (", 0) == 0) && e.status != "off") {
                th::icon_item(th::Icon::Warn, ImGui::GetFontSize() * 0.85f, p.warn);
                ImGui::SameLine(0, th::sp(th::S1));
                th::text(th::Type::Small, p.warn, "%s", e.status.c_str());
            } else {
                th::text(th::Type::Small, p.text3, "%s", where.c_str());
            }
            // value: live while not frozen; Enter / the keypad writes it (and moves the freeze)
            ImGui::TableSetColumnIndex(2);
            if (e.is_script) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - ImGui::GetFontSize()) * 0.5f);
                th::text(th::Type::Body, e.script_on ? p.accent : p.text3, "%s", e.script_on ? "running" : "off");
            } else {
                static char buf[64];
                const bool editing = ImGui::GetActiveID() == ImGui::GetID("##v");
                if (!editing && !e.freeze_id) e.value = value_text(a, e.type);
                std::snprintf(buf, sizeof buf, "%s", e.value.c_str());
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + th::px(4.f));
                th::push(th::Type::Strong);
                if (field("##v", buf, sizeof buf, Keys::Number, "value", -1)) {
                    Value v{};
                    if (parse_value(e.type, buf, &v)) {
                        e.value = buf;
                        if (a) gmem::write(a, &v.bits, type_size(e.type));
                        if (e.freeze_id) Engine::instance().freeze_set_value(e.freeze_id, v.bits);
                        mark_dirty();
                    } else {
                        toast_warn(std::string("\"") + buf + "\" is not a " + type_name(e.type) + " value");
                    }
                }
                focus |= ImGui::IsItemFocused();
                th::pop();
                th::text(th::Type::Small, e.freeze_id ? p.accent : p.text3, "%s", e.freeze_id ? kFreezeName[static_cast<int>(e.mode)] : "live");
            }
            // hotkey
            ImGui::TableSetColumnIndex(3);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - th::px(th::kControlSm)) * 0.5f);
            const bool capturing = s.hotkey_capture == static_cast<int>(i);
            if (th::button(capturing ? "Press..." : (e.hotkey.empty() ? "Set" : e.hotkey.c_str()), e.hotkey.empty() && !capturing ? th::Icon::Keyboard : th::Icon::None,
                           e.hotkey.empty() ? th::Btn::Quiet : th::Btn::Secondary, 0.f, th::kControlSm))
                s.hotkey_capture = static_cast<int>(i);
            focus |= ImGui::IsItemFocused();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("A key or controller combination that switches it while you play");
            // the row's actions: right-click / X / the dots
            ImGui::TableSetColumnIndex(4);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - th::px(th::kControlH)) * 0.5f);
            if (th::icon_button("##more", th::Icon::Dots, "More: freeze mode, hotkey action, group, find what writes it, delete ...")) ImGui::OpenPopup("ent_ctx");
            focus |= ImGui::IsItemFocused();
            if (focus && pad_mode() && pad_pressed(hid::btn(hid::kBtnX))) ImGui::OpenPopup("ent_ctx");
            if (focus && !pad_mode() && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) remove = static_cast<int>(i);
            if (ImGui::BeginPopup("ent_ctx")) {
                th::text(th::Type::Strong, p.text, "%s", e.desc.c_str());
                if (e.is_script) {
                    if (ImGui::MenuItem("Edit the script")) { s.script_edit = static_cast<int>(i); s.script_text = e.script; }
                } else {
                    th::text(th::Type::Small, p.text3, "When frozen");
                    auto mode = [&](const char* label, FreezeMode m) {
                        if (ImGui::RadioButton(label, e.mode == m)) {
                            e.mode = m;
                            if (e.freeze_id) Engine::instance().freeze_set_mode(e.freeze_id, e.mode);
                            mark_dirty();
                        }
                    };
                    mode("Keep it exactly", FreezeMode::Normal);
                    mode("Let it go up, never down", FreezeMode::AllowIncrease);
                    mode("Let it go down, never up", FreezeMode::AllowDecrease);
                    if (ImGui::BeginMenu("Value type")) {
                        for (int k = 0; k < 6; ++k)
                            if (ImGui::MenuItem(kind_names()[k], nullptr, kind_type(k) == e.type)) {
                                set_freeze(e, false);
                                e.type = kind_type(k);
                                mark_dirty();
                            }
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Find what writes it")) { std::snprintf(s.aw_addr, sizeof s.aw_addr, "%llX", static_cast<unsigned long long>(a)); s.aw_mode = 0; open_window("Find what accesses"); }
                    if (ImGui::MenuItem("Find what reads it")) { std::snprintf(s.aw_addr, sizeof s.aw_addr, "%llX", static_cast<unsigned long long>(a)); s.aw_mode = 1; open_window("Find what accesses"); }
                    if (ImGui::MenuItem("Find a pointer path to it (survives a restart)")) { std::snprintf(s.ptr_target, sizeof s.ptr_target, "%llX", static_cast<unsigned long long>(a)); open_window("Pointer scanner"); }
                    if (ImGui::MenuItem("Show it in the memory viewer")) goto_viewer(a);
                    if (ImGui::MenuItem("Read the memory around it as a structure")) { s.struct_addr = a; open_window("Structure"); }
                    if (ImGui::MenuItem(e.watch ? "Stop graphing it" : "Graph it over time", nullptr, e.watch)) {
                        e.watch = !e.watch;
                        if (e.watch) { Watch w; w.uid = e.uid; s.watches.push_back(w); open_window("Value graphs"); }
                        else s.watches.erase(std::remove_if(s.watches.begin(), s.watches.end(), [&](const Watch& w) { return w.uid == e.uid; }), s.watches.end());
                    }
                    if (ImGui::MenuItem("Name this address (a label)")) { s.labels.push_back({a, e.desc}); mark_dirty(); toast("Label \"" + e.desc + "\" added"); }
                }
                ImGui::Separator();
                th::text(th::Type::Small, p.text3, "Hotkey");
                if (ImGui::MenuItem("Set the hotkey (press a key or pad buttons)")) s.hotkey_capture = static_cast<int>(i);
                if (!e.hotkey.empty() && ImGui::MenuItem("Remove the hotkey")) { e.hotkey.clear(); mark_dirty(); }
                if (!e.is_script) {
                    const char* acts[] = {"Freeze / unfreeze", "Write the value", "Add 1", "Subtract 1"};
                    ImGui::SetNextItemWidth(th::px(220.f));
                    if (ImGui::Combo("does", &e.hotkey_action, acts, 4)) mark_dirty();
                }
                ImGui::SetNextItemWidth(th::px(220.f));
                if (field("group", &e.group, Keys::Text, "group", 220)) mark_dirty();
                ImGui::Separator();
                if (ImGui::MenuItem(e.pinned ? "Unpin from the HUD" : "Pin to the HUD (shown over the game)", nullptr, e.pinned)) {
                    e.pinned = !e.pinned;
                    mark_dirty();
                    toast(e.pinned ? "\"" + e.desc + "\" is pinned: it shows over the game while the panel is closed" : "\"" + e.desc + "\" unpinned");
                }
                if (!e.is_script && ImGui::MenuItem("Copy as an Atmosphère cheat code")) {
                    const std::string t = atmosphere_text(e);
                    ImGui::SetClipboardText(t.c_str());
                    NXLOG_I("cheats", "Atmosphere cheat for %s:\n%s", e.desc.c_str(), t.c_str());
                    toast("Cheat code copied (Save as cheat codes writes the whole table to the game's file)");
                }
                if (ImGui::MenuItem("Delete")) remove = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0 && remove < static_cast<int>(s.list.size())) remove_entry(static_cast<size_t>(remove));
}

// The AOB script editor (one entry at a time).
void win_script_editor() {
    State& s = st();
    if (s.script_edit < 0 || s.script_edit >= static_cast<int>(s.list.size())) { s.script_edit = -1; return; }
    Entry& e = s.list[s.script_edit];
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(theme::px(620.f), theme::px(440.f)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(("AOB script: " + e.desc + "###cheats_script").c_str(), &open)) {
        field("Name", &e.desc, Keys::Text, "name");
        editor("##script", &s.script_text, ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
        if (ImGui::Button("Save")) {
            if (e.script_on) { e.script_on = false; sync_script(e); }
            e.script = s.script_text;
            mark_dirty();
            toast("script saved");
        }
        ImGui::SameLine();
        if (ImGui::Button(e.script_on ? "Save and turn off" : "Save and turn on")) {
            if (e.script_on) { e.script_on = false; sync_script(e); e.script = s.script_text; }
            else { e.script = s.script_text; e.script_on = true; sync_script(e); }
            mark_dirty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) open = false;
        if (!e.status.empty()) ImGui::TextWrapped("%s", e.status.c_str());
        ImGui::TextDisabled("aobscan / aobscanmodule / define / assert / registersymbol, <address>:, db dw dd dq, any A64 instruction");
    }
    ImGui::End();
    if (!open) s.script_edit = -1;
}

// ================================================================================================================ Pointer scanner
void win_pointer() {
    State& s = st();
    bool& open = s.wins[2].open;
    if (!tool_begin("Pointer scanner", &open)) { tool_end(); return; }
    const bool busy = s.ptr_running.load();
    ImGui::BeginDisabled(busy);
    field("Target", s.ptr_target, sizeof s.ptr_target, Keys::Hex, "address (hex)", 180);
    ImGui::SameLine();
    int depth = static_cast<int>(s.ptr_opt.max_depth);
    ImGui::SetNextItemWidth(theme::px(90.f));
    if (ImGui::InputInt("levels", &depth)) s.ptr_opt.max_depth = static_cast<unsigned>(std::clamp(depth, 1, 7));
    int maxoff = static_cast<int>(s.ptr_opt.max_offset);
    ImGui::SetNextItemWidth(theme::px(120.f));
    if (ImGui::InputInt("max offset", &maxoff, 0x100, 0x1000, ImGuiInputTextFlags_CharsHexadecimal)) s.ptr_opt.max_offset = static_cast<uint64_t>(std::clamp(maxoff, 8, 0x100000));
    ImGui::SameLine();
    if (ImGui::Button("Scan", ImVec2(theme::px(90.f), 0))) {
        uint64_t target = 0;
        if (!parse_addr(s.ptr_target, &target)) s.ptr_msg = "enter the target address";
        else {
            if (s.ptr_thread && s.ptr_thread->joinable()) s.ptr_thread->join();
            s.ptr_running = true;
            s.ptr_msg = "building the pointer map...";
            s.ptr_thread = std::make_unique<std::thread>([target] {
                State& s = st();
                PointerScanOptions o = s.ptr_opt;
                o.scan.heap = o.scan.alias = o.scan.main = o.scan.other = true;
                RegionFilter all;
                all.main = all.heap = all.alias = all.stack = all.code = all.other = true;
                auto in = s.src->regions(all);
                PointerMap map = pointer_map_build(*s.src, o, in, &s.ptr_prog);
                std::vector<StaticRange> roots;
                for (Module* m : modules())
                    roots.push_back({m->base(), m->base() + m->size(), m->base(), m->name(), m->name() == "main" ? 0u : 0u});
                auto paths = pointer_find_paths(map, roots, target, o);
                // only paths rooted in main export as Atmosphère cheats (mem type 0); keep the others for this run
                std::stable_sort(paths.begin(), paths.end(), [](const PointerPath& a, const PointerPath& b) {
                    if ((a.module == "main") != (b.module == "main")) return a.module == "main";
                    return a.depth() < b.depth();
                });
                {
                    std::lock_guard<std::recursive_mutex> lk(s.mu);
                    s.ptr_paths = std::move(paths);
                    char b[128];
                    std::snprintf(b, sizeof b, "%zu paths (%zu pointers mapped)", s.ptr_paths.size(), map.slots());
                    s.ptr_msg = b;
                }
                NXLOG_I("cheats", "pointer scan for %llx: %s", static_cast<unsigned long long>(target), s.ptr_msg.c_str());
                s.ptr_running = false;
                (void)in;
            });
        }
    }
    ImGui::EndDisabled();
    ImGui::TextUnformatted(s.ptr_msg.c_str());
    // the restart rescan: after a restart the value lives elsewhere; keep the paths that lead to its new address
    ImGui::BeginDisabled(busy || s.ptr_paths.empty());
    field("##new", s.ptr_new, sizeof s.ptr_new, Keys::Hex, "the value's address now", 180);
    ImGui::SameLine();
    if (ImGui::Button("Rescan: keep paths that reach it")) {
        uint64_t t = 0;
        if (parse_addr(s.ptr_new, &t)) {
            const size_t before = s.ptr_paths.size();
            s.ptr_paths = pointer_rescan(*s.src, s.ptr_paths, t, [](const std::string& n) -> uint64_t {
                Module* m = module_by_name(n.c_str());
                return m ? m->base() : 0;
            });
            char b[96];
            std::snprintf(b, sizeof b, "%zu of %zu paths still lead to it", s.ptr_paths.size(), before);
            s.ptr_msg = b;
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::BeginTable("paths", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, theme::px(150.f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(50.f));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(s.ptr_paths.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const PointerPath& p = s.ptr_paths[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(p.text().c_str());
                ImGui::TableSetColumnIndex(1);
                const uint64_t now = pointer_resolve(*s.src, p, [](const std::string& n) -> uint64_t {
                    Module* m = module_by_name(n.c_str());
                    return m ? m->base() : 0;
                });
                ImGui::TextDisabled("%s", now ? gmem::describe(now).c_str() : "-");
                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("Add")) {
                    Entry e;
                    e.is_pointer = true;
                    e.path = p;
                    e.desc = "pointer " + p.text();
                    e.type = ScanType::U32;
                    e.uid = s.next_uid++;
                    s.list.push_back(e);
                    mark_dirty();
                    toast("pointer path added to the address list");
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    tool_end();
}

}  // namespace nxrt::cheats::ui
