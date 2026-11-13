// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The scripted test of the cheats overlay ([cheats] test = <file>): steps that drive the same state the windows show (scans, the list,
// freezes, find what writes, code patches, pointer scans, breakpoints, game control, exports), one at a time at the frame boundaries, each
// logged as "cheats test: ...". With the test runner's screenshots and scripted input (tt.h) it gives a headless run's evidence that the
// tools work on a real game; the windows show the results while it runs. Off unless the key is set. With [cheats] test_live = true the
// file is read again for lines appended while the game runs (a live session, beside the test runner's [test] live script).
//
// One step per line, lines starting with `#` are comments, an entry is its index in the list or `last`; a step waits until the previous one finished (a scan, a pointer scan) and its delay has passed:
//   wait <seconds> | at <seconds since the runtime started> | log <text> | open <window title> | close <window title> | save
//   scan <u8|u16|u32|u64|f32|f64|all> <exact|unknown|between|bigger|smaller|atleast|atmost> [v1] [v2] | scan <utf8|utf16> [icase] <text>
//   scan aob <pattern> | next <exact|changed|unchanged|increased|decreased|incby|decby|between|bigger|smaller|samefirst> [v1] [v2]
//   results <n>
//   add <result> [description] | addr <address> <type> [description] | list (every entry: where, value, frozen, hotkey) | graph <entry>
//   view | disasm | struct <address> (opens that window there) | label <address> <name> | freeze <entry> [value] | unfreeze <entry> | value <entry> | hotkey <entry> <name> [action]
//   writes <entry> | accessors | nop_writer | aob_writer [description] | patches | ips | revert_all | trace <writer|hex address> [stop] | bps | continue | step | ret
//   unbp <address>   (trace / unbp take any address the list takes: hex, main+0x.., a label)
//   set <entry> <value> | poke <code|data|heap|alias|stack|tls|shared|gpu> [module] [type] (an entry near the end of such a block) | restore <entry>
//   inject <address> <A64 line>[; <line> ...]   (a code cave, as the overlay's Inject code)
//   blcallers (the BL / B to the last writer's function; the first value whose writer has a BL caller becomes entry `chosen`) | nop_chosen
//   trace_cands (trace points at every BL-called writer blcallers found) | pick_entered (`chosen`: the first whose writer ran twice since;
//   `loopy`: the first whose writer's start did not run at all, a loop already running) | nop_loopy
//   trace_loops <thread name> [frames] (trace points at the loops a thread's frames are inside, and at those functions' starts) | loop_hits
//   focus <item>   (the pad's focus put on an item, as the D-pad leaves it: "Add address", the Address list's first button)
//   ptrscan <entry> [levels] | paths <n> | ptradd <path> | export | import <file> | reload | cheat <n|all> on|off | codewords
//   pause | resume | advance <n> | speed <x> | snapshot | diff
//   cpu   (the process's CPU time and the frame number now: two of them give the CPU time per frame of what ran between)
//   lua <file> | script <file> [description]   (an AOB script entry, switched on) | enable <entry> on|off   (a script entry)
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "../../cheats/codepatch.h"
#include "../../cheats/control.h"
#include "../../cheats/disasm.h"
#include "../../cheats/gmem.h"
#include "../nvn/nvn_core.h"
#include "cheats_ui.h"
#include "cheats_ui_internal.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/kernel.h"
#include "nxrt/nxmodule.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace nxrt::cheats::ui {
namespace {
constexpr const char* kTag = "cheats";

struct Step { int line = 0; std::string text; };
const double g_loaded = now_s();       // the runtime came up (its static init): `at <s>` counts from here, as a test script's clock does
struct T {
    bool on = false;
    std::vector<Step> steps;
    size_t next = 0;
    double due = 0;
    // [cheats] test_live = true: lines appended to the file while the game runs become more steps (with the test runner's live script
    // a session drives the tools step by step, as that script drives the pad)
    bool live = false;
    std::string path;
    uint64_t read_to = 0;              // bytes of the file taken so far (whole lines)
    int lines = 0;
    double next_look = 0;
    std::unique_ptr<std::thread> worker;
    std::atomic<bool> busy{false};
    uint64_t writer_fn = 0;           // the first accessor of the last "writes"
    // `blcallers`: the first value whose writer the game calls by BL (its entry, the accessor and the watched address), for `nop_chosen`
    int chosen_entry = -1;
    aw::Accessor chosen_acc;
    uint64_t chosen_target = 0;
    // every value whose writer has a BL caller (one per writer function), for `trace_cands` / `pick_entered`
    struct Cand { int entry; aw::Accessor acc; uint64_t target; uint64_t fn; };
    std::vector<Cand> cands;
    int loopy = -1;
    uint64_t want_fn = 0;             // `want_writer`: blcallers chooses only a value this function writes
    std::vector<std::pair<uint64_t, uint64_t>> loop_traces;   // `trace_loops`: (function start, loop head) traced                   // `pick_entered`: the first candidate whose writer's start did not run (a loop already running), for nop_loopy
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> snap;
    std::map<uint64_t, uint64_t> poked;  // entry uid -> the 8 bytes a poke found there
} g_t;

// CPU time this process has used so far (user + kernel), in seconds.
double process_cpu_s() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return static_cast<double>((static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                               (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime)) / 1e7;
#else
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
    return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) + static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
#endif
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    std::istringstream is(s);
    std::string x;
    while (is >> x) w.push_back(x);
    return w;
}
std::string rest(const std::string& s, size_t from_word) {
    std::istringstream is(s);
    std::string x, out;
    for (size_t i = 0; is >> x; ++i)
        if (i >= from_word) out += (out.empty() ? "" : " ") + x;
    return out;
}
bool type_of(const std::string& n, ScanType* t) {
    static const struct { const char* n; ScanType t; } k[] = {{"u8", ScanType::U8}, {"u16", ScanType::U16}, {"u32", ScanType::U32},
                                                               {"u64", ScanType::U64}, {"f32", ScanType::F32}, {"f64", ScanType::F64}};
    for (const auto& e : k) if (n == e.n) { *t = e.t; return true; }
    return false;
}
Entry* entry(const std::string& i) {
    if (i == "last") return st().list.empty() ? nullptr : &st().list.back();
    if (i == "loopy") return g_t.loopy >= 0 && static_cast<size_t>(g_t.cands[g_t.loopy].entry) < st().list.size() ? &st().list[g_t.cands[g_t.loopy].entry] : nullptr;
    if (i == "chosen") return g_t.chosen_entry >= 0 && static_cast<size_t>(g_t.chosen_entry) < st().list.size() ? &st().list[g_t.chosen_entry] : nullptr;
    const size_t k = static_cast<size_t>(std::atoi(i.c_str()));
    return k < st().list.size() ? &st().list[k] : nullptr;
}
void run_bg(std::function<void()> fn) {
    if (g_t.worker && g_t.worker->joinable()) g_t.worker->join();
    g_t.busy = true;
    g_t.worker = std::make_unique<std::thread>([fn] {
        fn();
        g_t.busy = false;
    });
}
Win* window(const std::string& title) {
    for (auto& w : st().wins)
        if (title == w.title) return &w;
    return nullptr;
}

// The store in the writer function whose base register plus offset, with the registers of its last write, lands on `target`.
uint64_t likely_store(const aw::Accessor& a, uint64_t target) {
    std::string mod;
    uint64_t fs = 0, fe = 0;
    if (!a.guest_fn || !cp::function_at(a.guest_fn, &mod, &fs, &fe)) return 0;
    for (uint64_t pc = fs; pc < fe && pc < fs + 0x4000; pc += 4) {
        uint32_t w = 0;
        if (!gmem::read(pc, &w, 4)) break;
        const a64::Insn in = a64::decode(w, pc);
        if (in.mnemonic.rfind("st", 0) != 0) continue;
        const size_t lb = in.text.find('[');
        if (lb == std::string::npos) continue;
        const std::string inner = in.text.substr(lb + 1);
        const std::string base = inner.substr(0, inner.find_first_of(",]"));
        int64_t off = 0;
        if (const size_t h = inner.find('#'); h != std::string::npos && h < inner.find(']')) off = std::strtoll(inner.c_str() + h + 1, nullptr, 0);
        uint64_t bv = 0;
        if (base == "sp") bv = a.sp;
        else if (base.size() > 1 && base[0] == 'x') { const int r = std::atoi(base.c_str() + 1); if (r >= 0 && r < 31) bv = a.regs[r]; }
        const uint64_t ea = bv + static_cast<uint64_t>(off);
        if (bv && ea <= target && target < ea + 8) return pc;
    }
    return 0;
}

void step(const Step& sp) {
    State& s = st();
    const auto w = words(sp.text);
    if (w.empty()) return;
    const std::string& op = w[0];
    auto say = [&](const char* fmt, auto... a) {
        char b[1024];
        std::snprintf(b, sizeof b, fmt, a...);
        NXLOG_I(kTag, "test %d: %s", sp.line, b);
    };
    if (op == "wait") { g_t.due = now_s() + std::atof(w.size() > 1 ? w[1].c_str() : "1"); return; }
    if (op == "at") { g_t.due = g_loaded + std::atof(w.size() > 1 ? w[1].c_str() : "0"); return; }   // the run's clock, as a test script's
    if (op == "log") { say("%s", rest(sp.text, 1).c_str()); return; }
    if (op == "open" || op == "close") {   // open: the page or the Advanced tool shows (as the D-pad would get there); close: its own window shuts
        const std::string t = rest(sp.text, 1);
        if (op == "open") { open_window(t.c_str()); say("open %s", t.c_str()); }
        else if (Win* x = window(t)) { x->open = false; say("close %s", x->title); }
        return;
    }
    if (op == "save") { save(); say("table and layout saved"); return; }
    if (op == "scan" && w.size() >= 3) {
        // scan <u8|u16|u32|u64|f32|f64|all> <exact|unknown|between|bigger|smaller|atleast|atmost> [v1] [v2]
        // scan <utf8|utf16> [icase] <text...> | scan aob <pattern...>
        ScanOptions o;
        FirstScan cmp = FirstScan::Exact;
        Value v1{}, v2{};
        std::string label;
        if (w[1] == "utf8" || w[1] == "utf16" || w[1] == "aob") {
            o.kind = w[1] == "utf8" ? ScanKind::Utf8 : w[1] == "utf16" ? ScanKind::Utf16 : ScanKind::Aob;
            size_t from = 2;
            if (o.kind != ScanKind::Aob && w[2] == "icase") { o.ignore_case = true; from = 3; }
            o.text = rest(sp.text, from);
            o.fast = false;
            o.filter.code = o.filter.other = true;   // patterns and texts live in code and read-only data too (the window's filter boxes)
            label = std::string(kind_name(o.kind)) + (o.ignore_case ? " (ignore case)" : "") + " \"" + o.text + "\"";
        } else {
            if (w[1] == "all") { o.kind = ScanKind::AllNumeric; o.all_numeric = true; o.type = ScanType::U32; }
            else if (!type_of(w[1], &o.type)) return say("unknown type %s", w[1].c_str());
            static const struct { const char* n; FirstScan c; } k[] = {{"exact", FirstScan::Exact}, {"unknown", FirstScan::Unknown},
                {"between", FirstScan::Between}, {"bigger", FirstScan::Greater}, {"smaller", FirstScan::Less},
                {"atleast", FirstScan::GreaterOrEqual}, {"atmost", FirstScan::LessOrEqual}};
            bool known = false;
            for (const auto& e : k) if (w[2] == e.n) { cmp = e.c; known = true; }
            if (!known) return say("unknown first-scan mode %s", w[2].c_str());
            const ScanType vt = o.kind == ScanKind::AllNumeric ? ScanType::F64 : o.type;   // all types: the value as a number
            if (w.size() > 3) parse_value(vt, w[3], &v1);
            if (w.size() > 4) parse_value(vt, w[4], &v2);
            if (o.kind == ScanKind::AllNumeric && w.size() > 3) o.text = w[3];   // parsed as each type ("100" also finds 100.0f)
            label = w[1] + " " + w[2] + (w.size() > 3 ? " " + w[3] : "") + (w.size() > 4 ? " " + w[4] : "");
        }
        const ScanType t = o.type;
        run_bg([o, cmp, v1, v2, t, label] {
            State& s = st();
            s.prog.cancel = false;
            s.scanner->first_scan(o, cmp, v1, v2, s.prog);
            s.first_done = true;
            s.last_count = s.scanner->result_count();
            s.kind_i = static_cast<int>(t);
            NXLOG_I(kTag, "test: first scan (%s): %zu results, %.1f MiB scanned", label.c_str(), s.last_count,
                    static_cast<double>(s.prog.bytes_total.load()) / 1048576.0);
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            s.history.push_back("first: " + label + " -> " + std::to_string(s.last_count) + " results");
        });
        return;
    }
    if (op == "next" && w.size() >= 2) {
        // next <exact|changed|unchanged|increased|decreased|incby|decby|between|bigger|smaller|samefirst> [v1] [v2]
        static const struct { const char* n; NextScan c; } k[] = {{"changed", NextScan::Changed}, {"unchanged", NextScan::Unchanged},
            {"increased", NextScan::Increased}, {"decreased", NextScan::Decreased}, {"exact", NextScan::Exact},
            {"incby", NextScan::IncreasedBy}, {"decby", NextScan::DecreasedBy}, {"between", NextScan::Between},
            {"bigger", NextScan::Greater}, {"smaller", NextScan::Less}, {"samefirst", NextScan::SameAsFirst}};
        bool known = false;
        NextScan c = NextScan::Changed;
        for (const auto& e : k) if (w[1] == e.n) { c = e.c; known = true; }
        if (!known) return say("unknown next-scan mode %s", w[1].c_str());
        Value v1{}, v2{};
        if (w.size() > 2) parse_value(s.scanner->type(), w[2], &v1);
        if (w.size() > 3) parse_value(s.scanner->type(), w[3], &v2);
        const std::string name = rest(sp.text, 1);
        run_bg([c, v1, v2, name] {
            State& s = st();
            s.scanner->next_scan(c, v1, v2, s.prog);
            s.last_count = s.scanner->result_count();
            NXLOG_I(kTag, "test: next scan (%s): %zu results", name.c_str(), s.last_count);
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            s.history.push_back("next: " + name + " -> " + std::to_string(s.last_count) + " results");
        });
        return;
    }
    if (op == "results") {
        const size_t n = w.size() > 1 ? static_cast<size_t>(std::atoi(w[1].c_str())) : 10;
        const auto r = s.scanner->results(0, n);
        say("%zu results; the first %zu:", s.scanner->result_count(), r.size());
        for (size_t i = 0; i < r.size(); ++i)
            say("  #%zu %s = %s (first %s, %s, %zu bytes)", i, gmem::describe(r[i].addr).c_str(), r[i].last.text().c_str(), r[i].first.text().c_str(),
                type_name(r[i].type), r[i].len);
        return;
    }
    if (op == "add" && w.size() >= 2) {
        const auto r = s.scanner->results(static_cast<size_t>(std::atoi(w[1].c_str())), 1);
        if (r.empty()) return say("no result %s", w[1].c_str());
        add_entry(r[0].addr, r[0].type, w.size() > 2 ? rest(sp.text, 2) : "");
        say("entry %zu: %s", s.list.size() - 1, gmem::describe(r[0].addr).c_str());
        return;
    }
    if (op == "addr" && w.size() >= 3) {
        uint64_t a = 0;
        ScanType t;
        if (!parse_addr(w[1].c_str(), &a) || !type_of(w[2], &t)) return say("addr <address> <type> [description]");
        add_entry(a, t, w.size() > 3 ? rest(sp.text, 3) : "");
        say("entry %zu: %s = %s", s.list.size() - 1, gmem::describe(a).c_str(), value_text(a, t).c_str());
        return;
    }
    if (op == "freeze" || op == "unfreeze") {
        Entry* e = w.size() > 1 ? entry(w[1]) : nullptr;
        if (!e) return say("no entry");
        if (op == "freeze" && w.size() > 2) e->value = w[2];
        else if (op == "freeze") e->value = value_text(entry_address(*e), e->type);
        set_freeze(*e, op == "freeze");
        say("%s %s at %s (value %s)", op.c_str(), e->desc.c_str(), gmem::describe(entry_address(*e)).c_str(), e->value.c_str());
        return;
    }
    if (op == "list") {
        for (size_t i = 0; i < s.list.size(); ++i) {
            Entry& e = s.list[i];
            if (e.is_script) say("%zu: %s (AOB script, %s): %s", i, e.desc.c_str(), e.script_on ? "on" : "off", e.status.c_str());
            else {
                const uint64_t a = entry_address(e);
                say("%zu: %s at %s = %s%s%s", i, e.desc.c_str(), a ? gmem::describe(a).c_str() : "(unresolved)", value_text(a, e.type).c_str(),
                    e.freeze_id ? ", frozen" : "", e.hotkey.empty() ? "" : (", hotkey " + e.hotkey).c_str());
            }
        }
        return;
    }
    if (op == "enable" && w.size() >= 3) {         // enable <entry> on|off: an AOB script entry's box (its [ENABLE] / [DISABLE] runs)
        Entry* e = entry(w[1]);
        if (!e || !e->is_script) return say("no script entry");
        e->script_on = w[2] == "on";
        sync_scripts();
        say("script %s: %s (%s)", e->desc.c_str(), e->script_on ? "on" : "off", e->status.c_str());
        return;
    }
    if ((op == "view" || op == "disasm" || op == "struct") && w.size() >= 2) {   // a viewer at an address (main+0x.., a label, hex)
        uint64_t a = 0;
        if (!parse_addr(w[1].c_str(), &a)) return say("%s <address>", op.c_str());
        if (op == "view") { s.view_addr = s.view_sel = a; open_window("Memory viewer"); }
        else if (op == "disasm") { s.dis_addr = s.dis_sel = a; open_window("Disassembler"); }
        else { s.struct_addr = a; open_window("Structure"); }
        say("%s at %s", op.c_str(), gmem::describe(a).c_str());
        return;
    }
    if (op == "label" && w.size() >= 3) {           // label <address> <name>
        uint64_t a = 0;
        if (!parse_addr(w[1].c_str(), &a)) return say("label <address> <name>");
        s.labels.push_back({a, w[2]});
        mark_dirty();
        say("label %s = %s", w[2].c_str(), gmem::describe(a).c_str());
        return;
    }
    if (op == "graph" && w.size() >= 2) {          // graph <entry>: a value graph of it (one sample a frame)
        Entry* e = entry(w[1]);
        if (!e) return say("no entry");
        Watch wt;
        wt.uid = e->uid;
        s.watches.push_back(wt);
        open_window("Value graphs");
        say("graph of %s", e->desc.c_str());
        return;
    }
    if (op == "value") {
        Entry* e = w.size() > 1 ? entry(w[1]) : nullptr;
        if (!e) return say("no entry");
        say("%s = %s", e->desc.c_str(), value_text(entry_address(*e), e->type).c_str());
        return;
    }
    if (op == "hotkey" && w.size() >= 3) {
        Entry* e = entry(w[1]);
        if (!e) return say("no entry");
        e->hotkey = w[2] == "none" ? std::string() : w[2];   // none: no hotkey
        e->hotkey_action = w.size() > 3 ? std::atoi(w[3].c_str()) : 0;
        e->hk_down = true;
        say("hotkey %s on %s (action %d)", e->hotkey.c_str(), e->desc.c_str(), e->hotkey_action);
        return;
    }
    if (op == "writes") {
        Entry* e = w.size() > 1 ? entry(w[1]) : nullptr;
        if (!e) return say("no entry");
        std::string err;
        const uint64_t a = entry_address(*e);
        std::snprintf(s.aw_addr, sizeof s.aw_addr, "%llX", static_cast<unsigned long long>(a));
        g_t.writer_fn = 0;                               // a later `blcallers` must not see an earlier watch's writer
        if (!aw::watch_start(a, 4, aw::Access::Write, &err)) return say("watch failed: %s", err.c_str());
        if (!Engine::instance().enabled()) Engine::instance().enable(true);
        say("find what writes %s", gmem::describe(a).c_str());
        return;
    }
    if (op == "accessors") {
        const auto acc = aw::accessors();
        say("%zu accessors, %llu faults%s", acc.size(), static_cast<unsigned long long>(aw::total_faults()),
            aw::changed_unseen() ? "; the value changed with no access at this address (the system writes it)" : "");
        say("  %s", aw::page_report().c_str());
        for (const auto& a : acc) say("  %s: %llu hits (%s)", a.guest_fn ? cp::where(a.guest_fn).c_str() : a.where.c_str(), static_cast<unsigned long long>(a.hits), a.is_write ? "write" : "read");
        for (const auto& a : acc)            // the first guest function (a frozen entry's writes come from the host: the engine)
            if (a.guest_fn) { g_t.writer_fn = a.guest_fn; break; }
        return;
    }
    if (op == "blcallers") {                        // how many BL / B in the writer's module go to the last writer's function (a direct caller)
        const uint64_t fn = g_t.writer_fn;
        const NxModule* nx = fn ? nxmodule_at(fn) : nullptr;
        if (!nx) return say("no writer in lifted code");
        uint64_t bl = 0, b = 0;
        std::vector<uint32_t> buf(1 << 16);
        for (uint64_t a = nx->base; a < nx->text_end; a += buf.size() * 4) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), (nx->text_end - a) / 4));
            if (!gmem::read(a, buf.data(), n * 4)) continue;
            for (size_t k = 0; k < n; ++k) {
                const uint32_t w = buf[k], op6 = w >> 26;
                if (op6 != 0x25 && op6 != 0x05) continue;
                const int64_t imm = static_cast<int64_t>(static_cast<int32_t>(w << 6) >> 6) * 4;
                if (a + k * 4 + static_cast<uint64_t>(imm) == fn) (op6 == 0x25 ? bl : b)++;
            }
        }
        if (bl > 0 && !s.list.empty() &&
            std::none_of(g_t.cands.begin(), g_t.cands.end(), [&](const T::Cand& c) { return c.fn == fn; })) {
            T::Cand c{static_cast<int>(s.list.size()) - 1, {}, 0, fn};
            for (const auto& x : aw::accessors())
                if (x.guest_fn == fn) { c.acc = x; break; }
            parse_addr(s.aw_addr, &c.target);
            g_t.cands.push_back(c);
        }
        const bool pick = bl > 0 && g_t.chosen_entry < 0 && !s.list.empty() && (!g_t.want_fn || g_t.want_fn == fn);
        if (pick) {
            const auto acc = aw::accessors();
            for (const auto& x : acc)
                if (x.guest_fn == fn) { g_t.chosen_acc = x; break; }
            parse_addr(s.aw_addr, &g_t.chosen_target);
            g_t.chosen_entry = static_cast<int>(s.list.size()) - 1;
        }
        say("writer %s: %llu BL and %llu B to it in %s%s", cp::where(fn).c_str(), static_cast<unsigned long long>(bl), static_cast<unsigned long long>(b),
            nx->name ? nx->name : "?", pick ? "; chosen (entry `chosen`)" : "");
        return;
    }
    if (op == "want_writer" && w.size() >= 2) {    // want_writer <address>: blcallers chooses only a value written by this function
        uint64_t a = 0;
        if (!parse_addr(w[1].c_str(), &a)) return say("want_writer <address>");
        g_t.want_fn = a;
        say("blcallers chooses only a value written by %s", cp::where(a).c_str());
        return;
    }
    if (op == "trace_cands") {                      // a trace point at the start of every candidate writer (are they entered again and again?)
        for (const auto& c : g_t.cands) {
            std::string err;
            say("trace %s: %s", cp::where(c.fn).c_str(), cp::bp_add(c.fn, false, &err) ? "recompiling" : err.c_str());
        }
        return;
    }
    if (op == "pick_entered") {                     // the chosen value: the first candidate whose writer was entered at least twice since
        const auto bps = cp::breakpoints();
        g_t.chosen_entry = -1;
        g_t.loopy = -1;
        for (size_t ci = 0; ci < g_t.cands.size(); ++ci) {
            const auto& c = g_t.cands[ci];
            uint64_t hits = 0;
            for (const auto& b : bps) if (b.addr == c.fn) hits = b.hits;
            const bool take = g_t.chosen_entry < 0 && hits >= 2;
            say("candidate %s (writes %s): its start ran %llu times%s", cp::where(c.fn).c_str(), gmem::describe(c.target).c_str(),
                static_cast<unsigned long long>(hits), take ? "; chosen" : hits < 2 ? " (a function that does not return: a patch reaches it at its next entry)" : "");
            if (take) { g_t.chosen_entry = c.entry; g_t.chosen_acc = c.acc; g_t.chosen_target = c.target; }
            if (hits == 0 && g_t.loopy < 0) g_t.loopy = static_cast<int>(ci);
        }
        for (const auto& c : g_t.cands) { std::string err; cp::bp_remove(c.fn, &err); }
        return;
    }
    if (op == "trace_loops" && w.size() >= 2) {     // trace_loops <thread name> [frames]: the loops a thread is inside right now
        // The thread's guest frame chain (x29: [fp, lr] records): for each return address from the outermost frame in, the function that
        // holds it and the innermost loop around the call (a backward branch at or after it to a head before it): a trace point at that
        // loop head and one at the function's start. A frame that never returns shows hits at its loop head and none at its start, which
        // only the loop-head check (NX_OVL) can give: the code running there is the build's, entered before the trace existed.
        const size_t want = w.size() > 2 ? static_cast<size_t>(std::atoi(w[2].c_str())) : 4;
        std::vector<uint64_t> lrs;
        for (const auto& t : kernel::threads()) {
            if (!t || t->name != w[1]) continue;
            uint64_t fp = t->ctx.x[29];
            for (int d = 0; d < 64 && fp; ++d) {
                uint64_t rec[2] = {};
                if (!gmem::read(fp, rec, sizeof rec) || rec[0] <= fp) { if (rec[1]) lrs.push_back(rec[1]); break; }
                if (rec[1]) lrs.push_back(rec[1]);
                fp = rec[0];
            }
            break;
        }
        if (lrs.empty()) return say("no frames of thread %s", w[1].c_str());
        size_t taken = 0;
        for (auto it = lrs.rbegin(); it != lrs.rend() && taken < want; ++it) {
            const uint64_t lr = *it;
            std::string mod;
            uint64_t fs = 0, fe = 0;
            if (!cp::function_at(lr - 4, &mod, &fs, &fe) || fe - fs > 0x40000) continue;
            std::vector<uint32_t> code(static_cast<size_t>((fe - fs) / 4));
            if (!gmem::read(fs, code.data(), code.size() * 4)) continue;
            uint64_t head = 0;
            for (size_t k = 0; k < code.size(); ++k) {
                const uint64_t pc = fs + k * 4;
                const uint32_t x = code[k];
                int64_t off = 0;
                bool br = true;
                if ((x >> 26) == 0x05) off = static_cast<int64_t>(static_cast<int32_t>(x << 6) >> 6) * 4;
                else if ((x & 0xFF000010u) == 0x54000000u || (x & 0x7E000000u) == 0x34000000u) off = static_cast<int64_t>(static_cast<int32_t>((x >> 5) << 13) >> 13) * 4;
                else if ((x & 0x7E000000u) == 0x36000000u) off = static_cast<int64_t>(static_cast<int32_t>((x >> 5) << 18) >> 18) * 4;
                else br = false;
                if (!br) continue;
                const uint64_t t = pc + static_cast<uint64_t>(off);
                if (pc >= lr && t >= fs && t <= lr - 4 && t > head) head = t;    // the innermost loop around the call
            }
            if (!head) continue;
            std::string e1, e2;
            const bool a = cp::bp_add(head, false, &e1);
            const bool b = head == fs || cp::bp_add(fs, false, &e2);
            g_t.loop_traces.push_back({fs, head});
            say("thread %s, frame returning to %s: loop head %s traced (%s), start %s (%s)", w[1].c_str(), cp::where(lr).c_str(), cp::where(head).c_str(),
                a ? "recompiling" : e1.c_str(), cp::where(fs).c_str(), b ? "traced" : e2.c_str());
            ++taken;
        }
        return;
    }
    if (op == "loop_hits") {                        // the hits of trace_loops' points: loop head against function start
        const auto bps = cp::breakpoints();
        auto hits = [&](uint64_t a) { for (const auto& b : bps) if (b.addr == a) return b.hits; return uint64_t{0}; };
        for (const auto& [fs, head] : g_t.loop_traces)
            say("loop head %s: %llu hits; its function's start %s: %llu hits", cp::where(head).c_str(), static_cast<unsigned long long>(hits(head)),
                cp::where(fs).c_str(), static_cast<unsigned long long>(hits(fs)));
        return;
    }
    if (op == "nop_loopy") {                        // NOP the store of the first candidate whose writer is a loop already running (pick_entered)
        if (g_t.loopy < 0) return say("no candidate whose writer's start did not run");
        const auto& c = g_t.cands[g_t.loopy];
        const uint64_t pc = likely_store(c.acc, c.target);
        if (!pc) return say("no store in %s adds up to %llx", cp::where(c.fn).c_str(), static_cast<unsigned long long>(c.target));
        uint32_t old = 0;
        gmem::read(pc, &old, 4);
        std::vector<uint8_t> nb(4);
        const uint32_t nop = a64::nop_word();
        std::memcpy(nb.data(), &nop, 4);
        std::string err;
        const bool ok = cp::apply(pc, nb, &err);
        say("NOP the running loop's store at %s (was %s), the value at %s: %s", cp::where(pc).c_str(), a64::decode(old, pc).text.c_str(),
            gmem::describe(c.target).c_str(), ok ? "recompiling" : err.c_str());
        return;
    }
    if (op == "nop_chosen") {                       // NOP the store of the chosen value's writer (the first one `blcallers` found called by BL)
        if (g_t.chosen_entry < 0) return say("no value whose writer is called by BL");
        aw::watch_stop();
        const uint64_t pc = likely_store(g_t.chosen_acc, g_t.chosen_target);
        if (!pc) return say("no store in %s adds up to %llx", cp::where(g_t.chosen_acc.guest_fn).c_str(), static_cast<unsigned long long>(g_t.chosen_target));
        uint32_t old = 0;
        gmem::read(pc, &old, 4);
        std::vector<uint8_t> nb(4);
        const uint32_t nop = a64::nop_word();
        std::memcpy(nb.data(), &nop, 4);
        std::string err;
        const bool ok = cp::apply(pc, nb, &err);
        say("NOP the chosen writer's store at %s (was %s), the value at %s: %s", cp::where(pc).c_str(), a64::decode(old, pc).text.c_str(),
            gmem::describe(g_t.chosen_target).c_str(), ok ? "recompiling" : err.c_str());
        return;
    }
    if (op == "nop_writer") {
        const auto acc = aw::accessors();
        uint64_t target = 0;
        parse_addr(s.aw_addr, &target);
        size_t k = 0;
        while (k < acc.size() && !acc[k].guest_fn) ++k;
        if (k == acc.size()) return say("no accessor in guest code");
        const uint64_t pc = likely_store(acc[k], target);
        if (!pc) return say("no store in %s adds up to %llx", cp::where(acc[k].guest_fn).c_str(), static_cast<unsigned long long>(target));
        uint32_t old = 0;
        gmem::read(pc, &old, 4);
        aw::watch_stop();
        std::vector<uint8_t> b(4);
        const uint32_t nop = a64::nop_word();
        std::memcpy(b.data(), &nop, 4);
        std::string err;
        const bool ok = cp::apply(pc, b, &err);
        say("NOP the writer at %s (was %s): %s", cp::where(pc).c_str(), a64::decode(old, pc).text.c_str(), ok ? "recompiling" : err.c_str());
        return;
    }
    if (op == "aob_writer") {                        // aob_writer [description]: the writer's store as a NOP AOB script entry (signature maker), on
        const auto acc = aw::accessors();
        uint64_t target = 0;
        parse_addr(s.aw_addr, &target);
        size_t k = 0;
        while (k < acc.size() && !acc[k].guest_fn) ++k;
        if (k == acc.size()) return say("no accessor in guest code");
        const uint64_t pc = likely_store(acc[k], target);
        if (!pc) return say("no store found");
        aw::watch_stop();
        std::string msg;
        Entry e;
        e.desc = w.size() > 1 ? rest(sp.text, 1) : "NOP " + gmem::describe(pc);
        e.script = make_nop_script_at(pc, e.desc, &msg);
        say("signature of %s: %s", cp::where(pc).c_str(), msg.c_str());
        if (e.script.empty()) return;
        e.is_script = true;
        e.script_on = true;
        e.uid = s.next_uid++;
        s.list.push_back(e);
        sync_scripts();
        say("script %s: %s", s.list.back().desc.c_str(), s.list.back().status.c_str());
        return;
    }
    if (op == "focus" && w.size() >= 2) {           // focus <item>: the pad's focus on that item ("Add address" in the Address list)
        s.nav_focus = rest(sp.text, 1);
        say("focus on %s", s.nav_focus.c_str());
        return;
    }
    if (op == "set" && w.size() >= 3) {             // set <entry> <value>: one write of the value (the list's value field)
        Entry* e = entry(w[1]);
        if (!e) return say("no entry");
        const uint64_t a = entry_address(*e);
        Value v{};
        if (!a || !parse_value(e->type, w[2], &v)) return say("set <entry> <value>");
        const bool ok = gmem::write(a, &v.bits, type_size(e->type));
        say("set %s at %s to %s: %s, reads %s", e->desc.c_str(), gmem::describe(a).c_str(), w[2].c_str(), ok ? "written" : "REFUSED",
            value_text(a, e->type).c_str());
        return;
    }
    if (op == "poke" && w.size() >= 2) {            // poke <code|data|heap|alias|stack|tls|shared|gpu> [module] [type]: an entry in such memory
        uint64_t a = 0, lo = 0, hi = 0;
        std::string what, mod;
        ScanType pt = ScanType::U32;
        for (size_t k = 2; k < w.size(); ++k)
            if (!type_of(w[k], &pt)) mod = w[k];
        if (w[1] == "gpu") {                        // PokoMac: the NVN backend has no pool snapshot (AnySwitch's nvn::be::pool_snapshot)
            return say("poke gpu: not in this build");
        } else {                                    // the last block of that kind (of the module, if named): its last word
            for (const auto& r : gmem::regions())
                if (w[1] == r.kind && (mod.empty() || r.module == mod) && r.size >= 0x1000) {
                    lo = r.addr;
                    hi = r.addr + r.size;
                    what = std::string(r.kind) + " block" + (r.module.empty() ? "" : " of " + r.module) + " (perm " + std::to_string(r.perm) + ")";
                }
        }
        if (!hi) return say("no %s memory", w[1].c_str());
        a = (hi - 0x40) & ~uint64_t{7};
        if (w[1] == "stack") a = lo + 0x40;        // the deepest end of a stack (it grows down): unused but for a very deep call
        add_entry(a, pt, "poke " + w[1] + " " + type_name(pt));
        uint64_t orig = 0;
        gmem::read(a, &orig, 8);
        g_t.poked[s.list.back().uid] = orig;
        say("entry %zu: %s in a %s [%llx, %llx) = %s", s.list.size() - 1, gmem::describe(a).c_str(), what.c_str(), static_cast<unsigned long long>(lo),
            static_cast<unsigned long long>(hi), value_text(a, pt).c_str());
        return;
    }
    if (op == "restore" && w.size() >= 2) {         // restore <entry>: the value a poke found there, written back
        Entry* e = entry(w[1]);
        if (!e || !g_t.poked.count(e->uid)) return say("no poked entry");
        const uint64_t v = g_t.poked[e->uid];
        const uint64_t a = entry_address(*e);
        const bool ok = gmem::write(a, &v, 8);
        say("restore %s at %s: %s, reads %s", e->desc.c_str(), gmem::describe(a).c_str(), ok ? "written" : "REFUSED", value_text(a, e->type).c_str());
        return;
    }
    if (op == "inject" && w.size() >= 3) {          // inject <address> <A64 line>[; <line> ...]: a code cave (the overlay's Inject code)
        uint64_t a = 0;
        if (!parse_addr(w[1].c_str(), &a)) return say("inject <address> <lines separated by ;>");
        std::vector<std::string> lines;
        std::string all = rest(sp.text, 2), cur;
        for (char ch : all + ";") {
            if (ch == ';') { if (!cur.empty()) lines.push_back(cur); cur.clear(); }
            else if (!(cur.empty() && ch == ' ')) cur += ch;
        }
        std::string err;
        uint32_t was = 0;
        gmem::read(a, &was, 4);
        const bool ok = cp::inject(a, lines, &err);
        say("inject %zu lines at %s (was %s): %s", lines.size(), cp::where(a).c_str(), a64::decode(was, a).text.c_str(), ok ? "recompiling" : err.c_str());
        return;
    }
    if (op == "patches") {
        for (const auto& p : cp::patches()) say("patch %s: %s (%s)", cp::where(p.addr).c_str(), cp::state_name(p.state), p.note.c_str());
        return;
    }
    if (op == "ips") {
        std::string err;
        const auto files = cp::write_ips(Engine::instance().folder() + "/exefs_patches", &err);
        for (const auto& f : files) say("IPS written: %s", f.c_str());
        if (files.empty()) say("no IPS written: %s", err.c_str());
        return;
    }
    if (op == "revert_all") {
        for (const auto& p : cp::patches()) { std::string e; cp::revert(p.addr, &e); }
        say("patches reverted");
        return;
    }
    if (op == "trace" && w.size() >= 2) {
        uint64_t a = 0;
        if (w[1] == "writer") a = g_t.writer_fn;
        else if (!parse_addr(w[1].c_str(), &a)) return say("trace <writer|address>");
        std::string err;
        say("trace point at %s: %s", cp::where(a).c_str(), cp::bp_add(a, w.size() > 2 && w[2] == "stop", &err) ? "recompiling" : err.c_str());
        return;
    }
    if (op == "bps") {
        for (const auto& b : cp::breakpoints())
            say("%s %s: %llu hits, %s; last x0 %llx sp %llx thread %s", b.stop ? "break" : "trace", cp::where(b.addr).c_str(), static_cast<unsigned long long>(b.hits),
                cp::state_name(b.state), static_cast<unsigned long long>(b.last.x[0]), static_cast<unsigned long long>(b.last.sp), b.last.thread.c_str());
        cp::Regs r;
        if (cp::held(&r)) say("a thread is held at %s", cp::where(r.pc).c_str());
        return;
    }
    if (op == "continue") { cp::resume(cp::Resume::Continue); say("continue"); return; }
    if (op == "step") { cp::resume(cp::Resume::Step); say("step"); return; }
    if (op == "ret") { cp::resume(cp::Resume::ToReturn); say("run to return"); return; }
    if (op == "unbp" && w.size() >= 2) {           // unbp <hex address>: removes a breakpoint or trace point (its function is rebuilt)
        std::string err;
        uint64_t a = 0;
        if (!parse_addr(w[1].c_str(), &a)) return say("unbp <address>");
        say("remove %s: %s", cp::where(a).c_str(), cp::bp_remove(a, &err) ? "done" : err.c_str());
        return;
    }
    if (op == "ptrscan" && w.size() >= 2) {
        Entry* e = entry(w[1]);
        if (!e) return say("no entry");
        const uint64_t target = entry_address(*e);
        const unsigned depth = w.size() > 2 ? static_cast<unsigned>(std::atoi(w[2].c_str())) : 3;
        std::snprintf(s.ptr_target, sizeof s.ptr_target, "%llX", static_cast<unsigned long long>(target));
        run_bg([target, depth] {
            State& s = st();
            PointerScanOptions o = s.ptr_opt;
            o.max_depth = depth;
            o.scan.heap = o.scan.alias = o.scan.main = o.scan.other = true;
            RegionFilter all;
            all.main = all.heap = all.alias = all.stack = all.code = all.other = true;
            PointerMap map = pointer_map_build(*s.src, o, s.src->regions(all), &s.ptr_prog);
            std::vector<StaticRange> roots;
            for (Module* m : modules()) roots.push_back({m->base(), m->base() + m->size(), m->base(), m->name(), 0});
            auto paths = pointer_find_paths(map, roots, target, o);
            std::stable_sort(paths.begin(), paths.end(), [](const PointerPath& a, const PointerPath& b) {
                if ((a.module == "main") != (b.module == "main")) return a.module == "main";
                return a.depth() < b.depth();
            });
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            s.ptr_paths = std::move(paths);
            NXLOG_I(kTag, "test: pointer scan for %llx: %zu paths from %zu pointers", static_cast<unsigned long long>(target), s.ptr_paths.size(), map.slots());
        });
        return;
    }
    if (op == "paths") {
        const size_t n = w.size() > 1 ? static_cast<size_t>(std::atoi(w[1].c_str())) : 5;
        for (size_t i = 0; i < s.ptr_paths.size() && i < n; ++i) say("  path %zu: %s", i, s.ptr_paths[i].text().c_str());
        return;
    }
    if (op == "ptradd" && w.size() >= 2) {
        const size_t i = static_cast<size_t>(std::atoi(w[1].c_str()));
        if (i >= s.ptr_paths.size()) return say("no path %zu", i);
        Entry e;
        e.is_pointer = true;
        e.path = s.ptr_paths[i];
        e.desc = w.size() > 2 ? rest(sp.text, 2) : "pointer " + e.path.text();
        e.uid = s.next_uid++;
        s.list.push_back(e);
        mark_dirty();
        say("entry %zu: %s -> %s", s.list.size() - 1, e.path.text().c_str(), gmem::describe(entry_address(s.list.back())).c_str());
        return;
    }
    if (op == "export") {
        std::string msg;
        export_list_to_cheat_file(&msg);
        say("%s", msg.c_str());
        return;
    }
    if (op == "reload") {
        const auto r = Engine::instance().reload();
        const EngineStats es = Engine::instance().stats();
        say("cheat file: found %d ok %d, %zu cheats (%s); passes %llu writes %llu", r.found, r.ok, r.cheats, r.error.c_str(),
            static_cast<unsigned long long>(es.passes), static_cast<unsigned long long>(es.writes));
        return;
    }
    if (op == "engine") {
        const EngineStats es = Engine::instance().stats();
        say("engine: %zu cheats, %zu on, %llu passes, %llu writes, %llu freeze writes", es.cheat_count, es.enabled_cheats,
            static_cast<unsigned long long>(es.passes), static_cast<unsigned long long>(es.writes), static_cast<unsigned long long>(es.freeze_writes));
        return;
    }
    if (op == "import" && w.size() >= 2) {         // import <Atmosphere .txt>: Cheat files' Import
        std::string msg;
        import_cheat_file(rest(sp.text, 1), &msg);
        say("%s", msg.c_str());
        return;
    }
    if (op == "cheat" && w.size() >= 3) {        // cheat <entry|all> on|off: the title's cheat file (Atmosphere), as its list's boxes
        const bool on = w[2] == "on";
        for (const auto& c : Engine::instance().cheats()) {
            if (c.master || (w[1] != "all" && std::atoi(w[1].c_str()) != c.entry)) continue;
            Engine::instance().set_cheat_enabled(c.entry, on);
            say("cheat %d (%s): %s", c.entry, c.name.c_str(), on ? "on" : "off");
        }
        return;
    }
    if (op == "codewords") {                       // the cheats' code and cave words held now (vmcode.h) and the router's counters
        const auto c = Engine::instance().stats().code;
        say("code writes %llu, cave writes %llu, passes with code %llu, words patched %llu, reverted %llu, refused %llu, cave words %llu "
            "written %llu restored",
            (unsigned long long)c.code_writes, (unsigned long long)c.cave_writes, (unsigned long long)c.passes_with_code,
            (unsigned long long)c.words_patched, (unsigned long long)c.words_reverted, (unsigned long long)c.patch_failures,
            (unsigned long long)c.cave_words_written, (unsigned long long)c.cave_words_restored);
        for (const auto& h : Engine::instance().code_held()) {
            std::string owners;
            for (int e : h.owners) owners += (owners.empty() ? "" : ",") + std::to_string(e);
            say("held %s %s: %08x -> %08x (cheats %s)%s", h.place.kind == vmcode::Kind::Code ? "code" : "cave", cp::where(h.addr).c_str(),
                h.original, h.now, owners.c_str(), h.plain ? " plain bytes" : "");
        }
        return;
    }
    if (op == "cpu") {                              // a measurement point: CPU time per frame between two of them
        static double cpu0 = 0, t0 = 0;
        static uint64_t f0 = 0;
        const double cpu = process_cpu_s(), t = now_s();
        const uint64_t f = ctl::frames();
        if (f0 && f > f0)
            say("cpu: process %.3f s at frame %llu; since the last point: %llu frames in %.2f s (%.2f fps), %.3f CPU s = %.3f CPU ms per frame",
                cpu, static_cast<unsigned long long>(f), static_cast<unsigned long long>(f - f0), t - t0, static_cast<double>(f - f0) / (t - t0),
                cpu - cpu0, (cpu - cpu0) * 1000.0 / static_cast<double>(f - f0));
        else
            say("cpu: process %.3f s at frame %llu", cpu, static_cast<unsigned long long>(f));
        cpu0 = cpu; t0 = t; f0 = f;
        return;
    }
    if (op == "pause") { ctl::pause(true); say("pause"); return; }
    if (op == "resume") { ctl::pause(false); say("resume"); return; }
    if (op == "advance") { ctl::frame_advance(w.size() > 1 ? std::atoi(w[1].c_str()) : 1); say("advance"); return; }
    if (op == "speed" && w.size() > 1) { ctl::set_speed(std::atof(w[1].c_str())); s.speed = static_cast<float>(ctl::speed()); say("speed x%g", ctl::speed()); return; }
    if (op == "snapshot") {                         // the Snapshots window's own snapshot and compare (with memory now)
        take_snapshot();
        say("snapshot %s: %.1f MiB", s.snaps.back().name.c_str(), static_cast<double>(s.snaps.back().bytes) / 1048576.0);
        return;
    }
    if (op == "diff") {
        s.snap_a = static_cast<int>(s.snaps.size()) - 1;
        s.snap_b = -1;
        compare_snapshots();
        say("diff of %s against memory now: %zu words changed", s.snaps.empty() ? "-" : s.snaps.back().name.c_str(), s.diff.size());
        for (size_t i = 0; i < s.diff.size() && i < 4; ++i)
            say("  %s: %u -> %u", gmem::describe(s.diff[i].first).c_str(), s.diff[i].second.first, s.diff[i].second.second);
        return;
    }
    if (op == "lua" && w.size() >= 2) {
        std::ifstream f(std::filesystem::u8path(rest(sp.text, 1)), std::ios::binary);
        std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        const std::string name = std::filesystem::u8path(rest(sp.text, 1)).stem().string();
        const uint32_t id = s.lua->load(name, src, &err);
        if (id && err.empty()) s.lua_ids[name] = id;
        say("lua %s: %s", name.c_str(), err.empty() ? "running" : err.c_str());
        return;
    }
    if (op == "script" && w.size() >= 2) {
        std::ifstream f(std::filesystem::u8path(w[1]), std::ios::binary);
        Entry e;
        e.is_script = true;
        e.script.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        e.desc = w.size() > 2 ? rest(sp.text, 2) : "script";
        e.script_on = true;
        e.uid = s.next_uid++;
        s.list.push_back(e);
        sync_scripts();
        say("script %s: %s", s.list.back().desc.c_str(), s.list.back().status.c_str());
        return;
    }
    say("unknown step: %s", sp.text.c_str());
}
}  // namespace

namespace {
// The file's whole lines from read_to on become steps. False when the file cannot be read.
bool read_steps() {
    std::ifstream f(std::filesystem::u8path(g_t.path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(f.tellg());
    if (size <= g_t.read_to) return true;
    std::string text(static_cast<size_t>(size - g_t.read_to), '\0');
    f.seekg(static_cast<std::streamoff>(g_t.read_to));
    f.read(&text[0], static_cast<std::streamsize>(text.size()));
    const size_t nl = text.rfind('\n');
    if (nl == std::string::npos) return true;                 // a line still being written
    text.resize(nl + 1);
    g_t.read_to += text.size();
    size_t at = 0;
    while (at < text.size()) {
        const size_t e = text.find('\n', at);
        std::string line = text.substr(at, e - at);
        at = e + 1;
        ++g_t.lines;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        // a comment is a line that starts with # (A64 immediates in `inject` lines carry a # too)
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        g_t.steps.push_back({g_t.lines, line});
    }
    return true;
}
}  // namespace

void test_init() {
    g_t.path = config_get_str("cheats", "test", "");
    if (g_t.path.empty()) return;
    g_t.live = config_get_bool("cheats", "test_live", false);
    if (!read_steps()) {
        NXLOG_W(kTag, "test: cannot read %s", g_t.path.c_str());
        if (!g_t.live) return;
    }
    g_t.on = !g_t.steps.empty() || g_t.live;
    g_t.due = now_s();
    NXLOG_I(kTag, "test: %zu steps from %s%s", g_t.steps.size(), g_t.path.c_str(), g_t.live ? " (live: lines added later run too)" : "");
}

void test_tick() {
    if (!g_t.on || g_t.busy.load() || st().prog.running.load() || now_s() < g_t.due) return;
    if (g_t.next >= g_t.steps.size()) {
        if (g_t.live) {                                       // look for appended lines four times a second
            if (now_s() < g_t.next_look) return;
            g_t.next_look = now_s() + 0.25;
            read_steps();
            if (g_t.next >= g_t.steps.size()) return;
        } else {
            g_t.on = false;
            NXLOG_I(kTag, "test: all steps done");
            return;
        }
    }
    step(g_t.steps[g_t.next++]);
}

}  // namespace nxrt::cheats::ui
