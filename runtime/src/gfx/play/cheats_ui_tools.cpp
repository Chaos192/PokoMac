// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's code and tool windows: Find what accesses (writes / reads), Breakpoints (registers, continue / step), Code patches
// (IPS for the next build), Threads, Value graphs (triggers), Game control (pause, frame advance, speed), Lua (scripts, console), Cheat files
// (Atmosphère import / export), Settings, Help.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../../cheats/accesswatch.h"
#include "../../cheats/cheat_vm.h"
#include "../../cheats/codepatch.h"
#include "../../cheats/control.h"
#include "../../cheats/disasm.h"
#include "../../cheats/export.h"
#include "../../cheats/gmem.h"
#include "cheats_ui_internal.h"
#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::cheats::ui {
namespace fs = std::filesystem;
namespace {
Win& win(const char* title) {
    for (auto& w : st().wins)
        if (std::strcmp(w.title, title) == 0) return w;
    return st().wins[0];
}
std::string read_all(const std::string& p) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void regs_table(const cp::Regs& r, bool editable) {
    if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < 31; ++i) {
            ImGui::TableNextColumn();
            ImGui::Text("x%-2d %016llX", i, static_cast<unsigned long long>(r.x[i]));
            if (editable && ImGui::IsItemClicked()) {}
        }
        ImGui::TableNextColumn();
        ImGui::Text("sp  %016llX", static_cast<unsigned long long>(r.sp));
        ImGui::EndTable();
    }
    ImGui::Text("pc %s   fn %s   N%dZ%dC%dV%d", gmem::describe(r.pc).c_str(), gmem::describe(r.fn).c_str(), (r.nzcv >> 3) & 1, (r.nzcv >> 2) & 1,
                (r.nzcv >> 1) & 1, r.nzcv & 1);
    ImGui::Text("d0 %g  d1 %g  d2 %g  d3 %g", r.d[0], r.d[1], r.d[2], r.d[3]);
}
}  // namespace

// ================================================================================================================ Find what accesses
void win_access() {
    State& s = st();
    Win& w = win("Find what accesses");
    if (!tool_begin("Find what accesses", &w.open)) { tool_end(); return; }
    ImGui::TextWrapped("The page that holds the address is protected; every guest instruction that writes (or reads) it is caught once per "
                       "pass, named by its function, counted and its registers kept. The game keeps running.");
    const bool on = aw::watching();
    ImGui::BeginDisabled(on);
    field("##aw", s.aw_addr, sizeof s.aw_addr, Keys::Text, "address", 190);
    ImGui::SameLine();
    ImGui::RadioButton("writes", &s.aw_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("reads and writes", &s.aw_mode, 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!on) {
        if (ImGui::Button("Start", ImVec2(theme::px(80.f), 0))) {
            uint64_t a = 0;
            std::string err;
            if (!parse_addr(s.aw_addr, &a)) s.aw_msg = "enter an address";
            else if (!aw::watch_start(a, 4, s.aw_mode ? aw::Access::Read : aw::Access::Write, &err)) s.aw_msg = "cannot watch it: " + err;
            else {
                s.aw_msg = "watching " + gmem::describe(a);
                Engine& e = Engine::instance();
                if (!e.enabled()) e.enable(true);           // the engine's pass re-arms the page
            }
        }
    } else if (ImGui::Button("Stop", ImVec2(theme::px(80.f), 0))) {
        aw::watch_stop();
        s.aw_msg = "stopped";
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) aw::clear();
    ImGui::Text("%s   (%llu accesses caught)", s.aw_msg.c_str(), static_cast<unsigned long long>(aw::total_faults()));
    if (aw::changed_unseen())
        ImGui::TextWrapped("The value changed, but no game code wrote it at this address: the system updates it (a service such as audio "
                           "or input), or the game writes the same memory through another address. Freezing such a value can stop the game.");
    const auto acc = aw::accessors();
    static int sel = -1;
    if (ImGui::BeginTable("acc", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f))) {
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, theme::px(70.f));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, theme::px(50.f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(130.f));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < acc.size(); ++i) {
            const auto& a = acc[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(a.guest_fn ? cp::where(a.guest_fn).c_str() : a.where.c_str(), sel == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns)) sel = static_cast<int>(i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(a.hits));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(a.is_write ? "write" : "read");
            ImGui::TableSetColumnIndex(3);
            if (a.guest_fn && ImGui::SmallButton("disassemble")) goto_disasm(a.guest_fn);
            ImGui::SameLine();
            if (a.guest_fn && ImGui::SmallButton("label")) { s.labels.push_back({a.guest_fn, "writes_" + hexs(a.guest_fn)}); mark_dirty(); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (sel >= 0 && sel < static_cast<int>(acc.size())) {
        // the instruction that did it: a store (or load) in the function whose base register, as the registers were at the access, plus its
        // offset lands on the address (lifted code keeps no exact pc, so this names the likely ones)
        uint64_t target = 0;
        parse_addr(s.aw_addr, &target);
        std::string mod;
        uint64_t fs = 0, fe = 0;
        if (acc[sel].guest_fn && cp::function_at(acc[sel].guest_fn, &mod, &fs, &fe)) {
            ImGui::SeparatorText("The likely instruction");
            int shown = 0;
            for (uint64_t pc = fs; pc < fe && pc < fs + 0x4000 && shown < 6; pc += 4) {
                uint32_t w = 0;
                if (!gmem::read(pc, &w, 4)) break;
                const a64::Insn in = a64::decode(w, pc);
                const bool st = in.mnemonic.rfind("st", 0) == 0, ld = in.mnemonic.rfind("ld", 0) == 0;
                if (!(acc[sel].is_write ? st : (st || ld))) continue;
                const size_t lb = in.text.find('[');
                if (lb == std::string::npos) continue;
                const std::string inner = in.text.substr(lb + 1);
                const size_t comma = inner.find_first_of(",]");
                const std::string base = inner.substr(0, comma);
                int64_t off = 0;
                if (const size_t h = inner.find('#'); h != std::string::npos && h < inner.find(']')) off = std::strtoll(inner.c_str() + h + 1, nullptr, 0);
                uint64_t bv = 0;
                if (base == "sp") bv = acc[sel].sp;
                else if (base.size() > 1 && base[0] == 'x') { const int r = std::atoi(base.c_str() + 1); if (r >= 0 && r < 31) bv = acc[sel].regs[r]; }
                if (!bv || bv + static_cast<uint64_t>(off) > target + 7 || bv + static_cast<uint64_t>(off) + 8 <= target) continue;
                ImGui::PushID(static_cast<int>(pc));
                std::string t = in.text;
                for (char& c : t) if (c == '\t') c = ' ';
                ImGui::Text("%s  %s", gmem::describe(pc).c_str(), t.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("NOP it")) {
                    std::vector<uint8_t> b(4);
                    const uint32_t nop = a64::nop_word();
                    std::memcpy(b.data(), &nop, 4);
                    std::string err;
                    if (cp::apply(pc, b, &err)) toast("NOP at " + gmem::describe(pc) + ": recompiling the function");
                    else toast("cannot patch: " + err);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("show")) goto_disasm(pc);
                ImGui::SameLine();
                if (ImGui::SmallButton("signature")) open_signature_maker(pc);
                ImGui::PopID();
                ++shown;
            }
            if (!shown) ImGui::TextDisabled("no store in the function adds up to the address with these registers (see the disassembly)");
        }
        ImGui::SeparatorText("Registers at the last access");
        cp::Regs r;
        for (int i = 0; i < 31; ++i) r.x[i] = acc[sel].regs[i];
        r.sp = acc[sel].sp;
        r.pc = acc[sel].last_pc;
        r.fn = acc[sel].guest_fn;
        regs_table(r, false);
    }
    tool_end();
}

// ================================================================================================================ Breakpoints
void win_breakpoints() {
    State& s = st();
    Win& w = win("Breakpoints");
    if (!tool_begin("Breakpoints", &w.open)) { tool_end(); return; }
    field("##bpa", s.bp_addr, sizeof s.bp_addr, Keys::Text, "code address", 190);
    ImGui::SameLine();
    auto add = [&](bool stop) {
        uint64_t a = 0;
        std::string err;
        if (!parse_addr(s.bp_addr, &a)) s.bp_msg = "enter a code address";
        else if (!cp::bp_add(a, stop, &err)) s.bp_msg = err;
        else s.bp_msg = std::string(stop ? "breakpoint" : "trace point") + " at " + cp::where(a) + ": recompiling its function";
    };
    if (ImGui::Button("Break here")) add(true);
    ImGui::SameLine();
    if (ImGui::Button("Trace here")) add(false);
    ImGui::TextWrapped("%s", s.bp_msg.c_str());
    // a held thread
    cp::Regs r;
    if (cp::held(&r)) {
        ImGui::SeparatorText(("Held: thread " + r.thread + " at " + cp::where(r.pc)).c_str());
        regs_table(r, true);
        if (ImGui::Button("Continue (A)") || (pad_mode() && pad_pressed(hid::btn(hid::kBtnA)) && ImGui::IsWindowFocused())) cp::resume(cp::Resume::Continue);
        ImGui::SameLine();
        if (ImGui::Button("Step")) cp::resume(cp::Resume::Step);
        ImGui::SameLine();
        if (ImGui::Button("Run to return")) cp::resume(cp::Resume::ToReturn);
        ImGui::SameLine();
        if (ImGui::Button("Show the code")) goto_disasm(r.pc);
    }
    ImGui::SeparatorText("Breakpoints and trace points");
    if (ImGui::BeginTable("bps", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, theme::px(34.f));
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, theme::px(60.f));
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(110.f));
        ImGui::TableHeadersRow();
        for (const auto& b : cp::breakpoints()) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(b.addr));
            ImGui::TableSetColumnIndex(0);
            bool en = b.enabled;
            if (ImGui::Checkbox("##en", &en)) cp::bp_enable(b.addr, en);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s %s", b.stop ? "break" : "trace", cp::where(b.addr).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(b.hits));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(cp::state_name(b.state));
            if (ImGui::IsItemHovered() && !b.note.empty()) ImGui::SetTooltip("%s", b.note.c_str());
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("code")) goto_disasm(b.addr);
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) { std::string e; cp::bp_remove(b.addr, &e); }
            ImGui::PopID();
            if (b.hits && !b.stop && ImGui::TreeNode(reinterpret_cast<void*>(b.addr), "last hit: thread %s", b.last.thread.c_str())) {
                regs_table(b.last, false);
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }
    tool_end();
}

// ================================================================================================================ Code patches
void win_patches() {
    State& s = st();
    Win& w = win("Code patches");
    if (!tool_begin("Code patches", &w.open)) { tool_end(); return; }
    ImGui::TextWrapped("A code patch changes the bytes and recompiles the function from them, so the running game uses the new code. For the "
                       "next build it is an IPS patch: write it to the title's code patches folder and set the title up again in the launcher.");
    const auto ps = cp::patches();
    if (ImGui::BeginTable("patches", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.f))) {
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, theme::px(150.f));
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, theme::px(140.f));
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(110.f));
        ImGui::TableHeadersRow();
        for (const auto& p : ps) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(p.addr));
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(cp::where(p.addr).c_str());
            ImGui::TableSetColumnIndex(1);
            std::string b;
            char h[4];
            for (uint8_t x : p.bytes) { std::snprintf(h, sizeof h, "%02X ", x); b += h; }
            ImGui::TextUnformatted(b.c_str());
            ImGui::TableSetColumnIndex(2);
            const ImVec4 c = p.state == cp::State::Live ? ImVec4(0.5f, 0.95f, 0.6f, 1.f) : p.state == cp::State::Reverted ? ImVec4(0.6f, 0.6f, 0.65f, 1.f) : ImVec4(1.f, 0.8f, 0.4f, 1.f);
            ImGui::TextColored(c, "%s", cp::state_name(p.state));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextWrapped("%s", p.note.c_str());
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("code")) goto_disasm(p.addr);
            ImGui::SameLine();
            if (p.state != cp::State::Reverted && ImGui::SmallButton("revert")) { std::string e; if (!cp::revert(p.addr, &e)) toast(e); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Write the IPS patches for the next build")) {
        std::string err;
        const std::string dir = Engine::instance().folder() + "/exefs_patches";
        const auto files = cp::write_ips(dir, &err);
        if (files.empty()) s.patch_msg = err.empty() ? "no active patch to write" : err;
        else {
            s.patch_msg = "written: ";
            for (const auto& f : files) s.patch_msg += f + "  ";
            s.patch_msg += " (a rebuild compiles them in: the launcher's Code patches folder, then Set up again)";
        }
    }
    ImGui::TextWrapped("%s", s.patch_msg.c_str());
    tool_end();
}

// ================================================================================================================ Threads
void win_threads() {
    State& s = st();
    Win& w = win("Threads");
    if (!tool_begin("Threads", &w.open)) { tool_end(); return; }
    const auto ts = kernel::threads();
    ImGui::Text("%zu guest threads", ts.size());
    if (ImGui::BeginTable("thr", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, theme::px(40.f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Prio / core", ImGuiTableColumnFlags_WidthFixed, theme::px(80.f));
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, theme::px(80.f));
        ImGui::TableSetupColumn("In function", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(90.f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& t : ts) {
            if (!t) continue;
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(t->id));
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(t->id));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(t->name.empty() ? "(unnamed)" : t->name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d / %d", t->prio, t->ideal_core);
            ImGui::TableSetColumnIndex(3);
            const char* state = t->exited.load() ? "exited" : !t->started ? "created" : t->parked ? "paused" : t->in_runtime.load() > 0 ? "in kernel" : "running";
            ImGui::TextUnformatted(state);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(t->ctx.fn ? cp::where(t->ctx.fn).c_str() : "-");
            ImGui::TableSetColumnIndex(5);
            if (t->ctx.fn && ImGui::SmallButton("code")) goto_disasm(t->ctx.fn);
            ImGui::SameLine();
            if (ImGui::SmallButton("stack")) goto_viewer(t->ctx.x[31]);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Registers are exact while a thread is paused (Game control: Pause) or held at a breakpoint.");
    (void)s;
    tool_end();
}

// ================================================================================================================ Value graphs
void win_watch() {
    State& s = st();
    Win& w = win("Value graphs");
    if (!tool_begin("Value graphs", &w.open)) { tool_end(); return; }
    if (s.watches.empty()) ImGui::TextWrapped("Graph a value from the address list (its actions: Graph this value). One sample per frame, the last 600.");
    int drop = -1;
    for (size_t i = 0; i < s.watches.size(); ++i) {
        Watch& wt = s.watches[i];
        Entry* e = entry_by_uid(wt.uid);
        if (!e) { drop = static_cast<int>(i); continue; }
        ImGui::PushID(static_cast<int>(wt.uid));
        // unroll the ring buffer for the plot
        std::vector<float> v;
        if (!wt.samples.empty()) {
            const size_t n = wt.full ? wt.samples.size() : wt.head;
            v.reserve(n);
            for (size_t k = 0; k < n; ++k) v.push_back(wt.samples[(wt.full ? wt.head + k : k) % wt.samples.size()]);
        }
        float lo = 0, hi = 1;
        if (!v.empty()) { lo = *std::min_element(v.begin(), v.end()); hi = *std::max_element(v.begin(), v.end()); if (hi == lo) { hi += 1; lo -= 1; } }
        char over[96];
        std::snprintf(over, sizeof over, "%s = %g  (%g .. %g)", e->desc.c_str(), v.empty() ? 0.0 : static_cast<double>(v.back()), static_cast<double>(lo), static_cast<double>(hi));
        ImGui::PlotLines("##p", v.empty() ? nullptr : v.data(), static_cast<int>(v.size()), 0, over, lo, hi, ImVec2(-1, theme::px(70.f)));
        const char* trig[] = {"no trigger", "on any change", "when above", "when below", "when equal to"};
        ImGui::SetNextItemWidth(theme::px(150.f));
        ImGui::Combo("##trig", &wt.trig, trig, 5);
        if (wt.trig >= 2) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(theme::px(110.f));
            ImGui::InputDouble("##tv", &wt.trig_value);
        }
        ImGui::SameLine();
        ImGui::Checkbox("pause the game", &wt.trig_pause);
        ImGui::SameLine();
        if (wt.fired) {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "fired");
            ImGui::SameLine();
            if (ImGui::SmallButton("re-arm")) wt.fired = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("remove")) drop = static_cast<int>(i);
        ImGui::PopID();
        ImGui::Separator();
    }
    if (drop >= 0) {
        if (Entry* e = entry_by_uid(s.watches[drop].uid)) e->watch = false;
        s.watches.erase(s.watches.begin() + drop);
    }
    tool_end();
}

// ================================================================================================================ Game control
void win_control() {
    State& s = st();
    Win& w = win("Game control");
    if (!tool_begin("Game control", &w.open)) { tool_end(); return; }
    const bool p = ctl::paused();
    if (ImGui::Button(p ? "Resume" : "Pause", ImVec2(theme::px(120.f), 0))) ctl::pause(!p);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(theme::px(90.f));
    ImGui::InputInt("##n", &s.advance_n);
    s.advance_n = std::clamp(s.advance_n, 1, 600);
    ImGui::SameLine();
    if (ImGui::Button("Advance frames")) ctl::frame_advance(s.advance_n);
    ImGui::Text("frame %llu   %s", static_cast<unsigned long long>(ctl::frames()), ctl::status_text().c_str());
    ImGui::SeparatorText("Speed");
    ImGui::SetNextItemWidth(theme::px(260.f));
    if (ImGui::SliderFloat("##speed", &s.speed, 0.1f, 8.f, "x%.2f", ImGuiSliderFlags_Logarithmic)) ctl::set_speed(s.speed);
    for (float sp : {0.25f, 0.5f, 1.f, 2.f, 4.f}) {
        ImGui::SameLine();
        char b[16];
        std::snprintf(b, sizeof b, "x%g", sp);
        if (ImGui::SmallButton(b)) { s.speed = sp; ctl::set_speed(sp); }
    }
    ImGui::TextDisabled("The console clock, sleeps, timed waits, vsync and the frame limiter run at this speed; sound stays real time.");
    ImGui::Checkbox("pause the game while the overlay is open", &s.pause_while_open);
    tool_end();
}

// ================================================================================================================ Lua
void win_lua() {
    State& s = st();
    Win& w = win("Lua");
    if (!tool_begin("Lua", &w.open)) { tool_end(); return; }
    if (ImGui::BeginTable("lua", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("scripts", ImGuiTableColumnFlags_WidthFixed, theme::px(180.f));
        ImGui::TableSetupColumn("edit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button("New script")) {
            table::LuaScript l;
            l.name = "script" + std::to_string(s.lua_scripts.size() + 1);
            l.source = "-- runs at every frame boundary\nnx.onframe(function()\nend)\n";
            s.lua_scripts.push_back(l);
            s.lua_sel = static_cast<int>(s.lua_scripts.size() - 1);
            s.lua_edit = l.source;
            mark_dirty();
        }
        const auto infos = s.lua->scripts();
        for (size_t i = 0; i < s.lua_scripts.size(); ++i) {
            const auto& l = s.lua_scripts[i];
            const bool running = s.lua_ids.count(l.name) != 0;
            std::string err;
            for (const auto& in : infos)
                if (running && in.id == s.lua_ids[l.name] && !in.running) err = in.error;
            std::string label = std::string(running ? (err.empty() ? "> " : "! ") : "  ") + l.name;
            if (ImGui::Selectable(label.c_str(), s.lua_sel == static_cast<int>(i))) { s.lua_sel = static_cast<int>(i); s.lua_edit = l.source; }
            if (!err.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", err.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        if (s.lua_sel >= 0 && s.lua_sel < static_cast<int>(s.lua_scripts.size())) {
            table::LuaScript& l = s.lua_scripts[s.lua_sel];
            if (field("name", &l.name, Keys::Text, "name")) mark_dirty();
            editor("##src", &s.lua_edit, ImVec2(-1, theme::px(220.f)));
            const bool running = s.lua_ids.count(l.name) != 0;
            if (ImGui::Button(running ? "Save and restart" : "Save and run")) {
                if (running) { s.lua->stop(s.lua_ids[l.name]); s.lua_ids.erase(l.name); }
                l.source = s.lua_edit;
                std::string err;
                const uint32_t id = s.lua->load(l.name, l.source, &err);
                if (id) s.lua_ids[l.name] = id;
                toast(err.empty() ? "Lua " + l.name + " running" : "Lua " + l.name + ": " + err);
                mark_dirty();
            }
            ImGui::SameLine();
            if (running && ImGui::Button("Stop")) { s.lua->stop(s.lua_ids[l.name]); s.lua_ids.erase(l.name); mark_dirty(); }
            ImGui::SameLine();
            if (ImGui::Button("Save")) { l.source = s.lua_edit; mark_dirty(); }
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                if (running) { s.lua->stop(s.lua_ids[l.name]); s.lua_ids.erase(l.name); }
                s.lua_scripts.erase(s.lua_scripts.begin() + s.lua_sel);
                s.lua_sel = -1;
                mark_dirty();
            }
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Console");
    ImGui::BeginChild("##out", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_FrameStyle);
    for (const auto& l : s.console) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    if (field("##in", s.console_in, sizeof s.console_in, Keys::Text, "Lua: nx.read32(0x7100001230)   nx.speed(2)", -1)) {
        const std::string line = s.console_in;
        s.console.push_back("> " + line);
        const std::string out = s.lua->eval(line);
        if (!out.empty()) s.console.push_back(out);
        s.console_in[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    tool_end();
}

// Every value entry in main or the heap and every pointer path rooted in main, as Atmosphere cheats appended to this build's cheat file
// (created with an empty {Master} when there is none); the engine reloads it. Returns how many were written; *msg says what happened.
// Appends an Atmosphere cheat file to the title's own (so it runs as on a console) and reloads it. Returns the cheats imported (0 with *msg).
size_t import_cheat_file(const std::string& path, std::string* msg) {
    Engine& e = Engine::instance();
    const EngineStats es = e.stats();
    const std::string text = read_all(path);
    CheatFile f = ParseFile(text);
    if (text.empty()) { *msg = "cannot read that file"; return 0; }
    if (!f.ok) { *msg = "not an Atmosphere cheat file: " + f.error; return 0; }
    if (es.file_path.empty()) { *msg = "no title loaded"; return 0; }
    std::error_code ec;
    fs::create_directories(fs::u8path(es.file_path).parent_path(), ec);
    {
        std::ofstream out(fs::u8path(es.file_path), std::ios::binary | std::ios::app);
        out << "\n" << text << "\n";
    }
    e.reload();
    *msg = "imported " + std::to_string(f.count()) + " cheats into " + es.file_path;
    return f.count();
}

size_t export_list_to_cheat_file(std::string* msg) {
    State& s = st();
    Engine& e = Engine::instance();
    const EngineStats es = e.stats();
    std::string text;
    size_t n = 0;
    for (Entry& en : s.list) {
        if (en.is_script || (!en.is_pointer && !en.signature.empty())) continue;
        Value v{};
        if (!parse_value(en.type, en.value, &v)) continue;
        std::string body;
        if (en.is_pointer) {
            if (en.path.module != "main") continue;          // only main-rooted paths have a console equivalent
            body = export_pointer_body(0, en.path.base_offset, en.path.offsets, v);
        } else {
            const uint64_t a = entry_address(en);
            const uint64_t mb = gmem::main_base(), hb = gmem::region_base(1);
            if (a >= mb && a < mb + gmem::main_size()) body = export_static_body(0, a - mb, v);
            else if (hb && a >= hb) body = export_static_body(1, a - hb, v);
            else continue;
        }
        text += export_cheat(en.desc, body) + "\n";
        ++n;
    }
    if (!n) *msg = "nothing to export (value entries in main / heap, pointer paths rooted in main)";
    else if (es.file_path.empty()) *msg = "no title loaded";
    else {
        std::error_code ec;
        fs::create_directories(fs::u8path(es.file_path).parent_path(), ec);
        const bool exists = fs::exists(fs::u8path(es.file_path), ec);
        std::ofstream out(fs::u8path(es.file_path), std::ios::binary | std::ios::app);
        if (!exists) out << "{Master}\n\n";
        out << text;
        out.close();
        e.reload();
        *msg = "exported " + std::to_string(n) + " cheats to " + es.file_path;
        NXLOG_I("cheats", "exported %zu Atmosphere cheats to %s:\n%s", n, es.file_path.c_str(), text.c_str());
    }
    return n;
}

// ================================================================================================================ Cheats (page)
// The game's cheat codes (its Atmosphère cheat file and the enabled mods' cheat files, [cheats] mod_files) with a switch each; turning one
// on starts the cheat engine. Empty state: where the file goes. Import a file, save My table as cheat codes, reload.
void page_cheats() {
    State& s = st();
    namespace th = theme;
    const th::Palette& p = th::pal();
    Engine& e = Engine::instance();
    const EngineStats es = e.stats();
    const auto rows = e.cheats();
    size_t total = 0, on = 0;
    for (const auto& r : rows)
        if (!r.master) { ++total; on += r.enabled; }
    // heading: the count, and the engine's switch on the right
    {
        ImGui::BeginGroup();
        th::push(th::Type::Title);
        ImGui::TextUnformatted("Cheat codes");
        th::pop();
        if (total) th::text(th::Type::Small, p.text2, "%zu of %zu on%s", on, total, config_get_str("cheats", "mod_files", "").empty() ? "" : ", your mods' included");
        else th::text(th::Type::Small, p.text2, "Atmosphère cheat files for this game");
        ImGui::EndGroup();
        bool run = e.enabled();
        const char* run_label = run ? "Cheats running" : "Cheats stopped";
        const float tw = th::px(44.f) + th::sp(th::S2) + ImGui::CalcTextSize(run_label).x + th::px(2.f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - tw));
        ImGui::BeginGroup();
        if (th::toggle("##engine", &run)) e.enable(run);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Cheat codes and frozen values are written 12 times a second while this is on");
        ImGui::SameLine(0, th::sp(th::S2));
        ImGui::AlignTextToFramePadding();
        th::text(th::Type::Body, run ? p.text : p.text2, "%s", run_label);
        ImGui::EndGroup();
    }
    if (!es.last_stop.empty()) {
        th::icon_item(th::Icon::Warn, 0.f, p.warn);
        ImGui::SameLine(0, th::sp(th::S2));
        th::text_wrapped(th::Type::Body, p.warn, "The last pass stopped early: %s", es.last_stop.c_str());
    }
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
    const float foot_h = ImGui::GetFrameHeightWithSpacing() * 2.f + th::sp(th::S7) + ImGui::GetTextLineHeightWithSpacing() * (s.files_msg.empty() ? 1.f : 2.f);
    ImGui::BeginChild("##cheat_rows", ImVec2(0, -foot_h), ImGuiChildFlags_NavFlattened);
    if (rows.empty()) {
        std::string body;
        if (es.program_id.empty() || es.build_id.empty()) body = "No game is loaded, so there is no cheat file to read.";
        else
            body = "Put an Atmosphère cheat file for this version of the game in\n" + (es.file_path.empty() ? e.folder() : es.file_path) +
                   "\nor import one below. You can also make your own: find a value, then save My table as cheat codes.";
        th::empty_state(th::Icon::Cheats, "No cheat codes for this game yet", body.c_str());
    } else {
        const float row_h = std::round(th::px(52.f));
        for (const auto& r : rows) {
            ImGui::PushID(r.entry);
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddRectFilled(o, ImVec2(o.x + w, o.y + row_h), ImGui::GetColorU32(r.enabled || r.master ? p.card : 0), th::px(th::kRadiusSm));
            ImGui::SetCursorScreenPos(ImVec2(o.x + th::sp(th::S3), o.y + (row_h - ImGui::GetFrameHeight()) * 0.5f));
            if (r.master) {
                th::chip("always on", p.surface2, p.text2, th::Icon::Check);
            } else {
                bool en = r.enabled;
                if (th::toggle("##on", &en)) {
                    e.set_cheat_enabled(r.entry, en);
                    if (en && !e.enabled()) e.enable(true);      // a cheat turned on runs: the engine comes on with it
                    toast(std::string(en ? "On: " : "Off: ") + r.name);
                }
            }
            ImGui::SameLine(0, th::sp(th::S3));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
            th::text(r.master ? th::Type::Body : th::Type::Strong, r.master ? p.text2 : p.text, "%s", r.master ? "Master code (sets the others up)" : r.name.c_str());
            ImGui::SameLine(0, th::sp(th::S3));
            th::text(th::Type::Small, p.text3, "%zu code words", r.words);
            if (!r.master) {   // pin it to the HUD over the game
                const bool pinned = s.pinned_cheats.count(r.name) != 0;
                ImGui::SetCursorScreenPos(ImVec2(o.x + w - th::px(th::kControlH) - th::sp(th::S2), o.y + (row_h - th::px(th::kControlH)) * 0.5f));
                if (th::icon_button("##pin", th::Icon::Pin, pinned ? "Unpin from the HUD" : "Pin to the HUD (shown over the game)",
                                    pinned ? th::Btn::Secondary : th::Btn::Quiet)) {
                    if (pinned) s.pinned_cheats.erase(r.name);
                    else s.pinned_cheats.insert(r.name);
                    mark_dirty();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + row_h + th::sp(th::S1)));
            ImGui::Dummy(ImVec2(w, 0));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    // import / export
    ImGui::Dummy(ImVec2(0, th::sp(th::S1)));
    th::text(th::Type::Small, p.text2, "Import a cheat file, or save My table as cheat codes for this game");
    const float bw = 120.f;
    field("##imp", s.import_path, sizeof s.import_path, Keys::Text, "the full path of an Atmosphère .txt",
          (ImGui::GetContentRegionAvail().x - th::sp(th::S2)) / th::k() - bw);
    ImGui::SameLine(0, th::sp(th::S2));
    if (th::button("Import", th::Icon::Import, th::Btn::Secondary, bw)) {
        const size_t n = import_cheat_file(s.import_path, &s.files_msg);
        if (n) toast("Imported " + std::to_string(n) + " cheat codes");
        else toast_warn(s.files_msg);
    }
    ImGui::BeginDisabled(s.list.empty());
    if (th::button("Save My table as cheat codes", th::Icon::Export)) {
        const size_t n = export_list_to_cheat_file(&s.files_msg);
        if (n) toast("Saved " + std::to_string(n) + " cheat codes");
        else toast_warn(s.files_msg);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, th::sp(th::S2));
    if (th::button("Reload the file", th::Icon::Refresh, th::Btn::Quiet)) {
        const auto r = e.reload();
        toast(r.found ? "Reloaded: " + std::to_string(r.cheats) + " cheat codes" : "No cheat file for this game yet");
    }
    if (!s.files_msg.empty()) th::text_wrapped(th::Type::Small, p.text3, "%s", s.files_msg.c_str());
}

// ================================================================================================================ Settings
void win_settings() {
    State& s = st();
    Win& w = win("Settings");
    if (!tool_begin("Settings", &w.open)) { tool_end(); return; }
    namespace th = theme;
    const th::Palette& p = th::pal();
    const th::LookSpec& spec = th::look_spec();
    // the theme: a built-in one, or the file [cheats] theme names (the AnySwitch app's theme picker sets it)
    th::text(th::Type::Strong, p.text, "Theme");
    static const char* builtins[] = {"sunrise", "dusk", "midnight", "daylight", "paper", "frost"};
    static const char* builtin_names[] = {"Sunrise", "Dusk", "Midnight", "Daylight (light)", "Paper (light)", "Frost (glass)"};
    const std::string cur = th::theme().source;
    int ti = -1;
    for (int i = 0; i < 6; ++i)
        if (cur == std::string("builtin:") + builtins[i]) ti = i;
    ImGui::SetNextItemWidth(theme::px(300.f));
    const std::string label = ti >= 0 ? builtin_names[ti] : th::theme().name + " (a theme file)";
    if (ImGui::BeginCombo("##theme", label.c_str())) {
        for (int i = 0; i < 6; ++i)
            if (ImGui::Selectable(builtin_names[i], ti == i)) {
                s.theme_name = builtins[i];
                std::string err;
                th::set_theme(th::load_theme(s.theme_name, &err));
                s.theme_error = err;
            }
        ImGui::EndCombo();
    }
    if (!s.theme_error.empty()) {
        th::icon_item(th::Icon::Warn, 0.f, p.warn);
        ImGui::SameLine(0, th::sp(th::S2));
        th::text_wrapped(th::Type::Small, p.warn, "Your theme was not used: %s", s.theme_error.c_str());
    }
    // the panel: solid or glass (the game blurred behind it), its opacity, the glass's blur and tint
    th::text(th::Type::Strong, p.text, "Panel");
    const char* styles[] = {"The theme's", "Solid", "Glass"};
    int si = s.style + 1;
    ImGui::SetNextItemWidth(theme::px(300.f));
    if (ImGui::Combo("Style", &si, styles, 3)) s.style = si - 1;
    auto pct = [&](const char* name, float* v, const th::Range& r, float theirs) {
        bool own = *v >= 0.f;
        ImGui::PushID(name);
        if (ImGui::Checkbox("##own", &own)) *v = own ? theirs : -1.f;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Off: the theme's own value");
        ImGui::SameLine();
        ImGui::BeginDisabled(!own);
        int x = static_cast<int>(std::lround((own ? *v : theirs) * 100.f));
        ImGui::SetNextItemWidth(theme::px(260.f));
        if (ImGui::SliderInt(name, &x, r.min, r.max, "%d%%")) *v = static_cast<float>(x) / 100.f;
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    pct("Opacity", &s.opacity, spec.opacity, th::theme().opacity);
    pct("Blur (glass)", &s.blur, spec.blur, th::theme().blur);
    pct("Tint (glass)", &s.tint, spec.tint, th::theme().tint);
    if (th::glass() == false && (s.style == 1 || (s.style < 0 && th::theme().glass)))
        th::text_wrapped(th::Type::Small, p.text3, "Glass needs the backdrop blur, which this display does not have (HDR output): the panel is solid.");
    // the accent: the theme's, the game's picture's, a preset, or any colour
    th::text(th::Type::Strong, p.text, "Accent colour");
    const char* accents[] = {"theme", "auto", "blue", "green", "amber", "violet", "red", "teal", "slate"};
    const char* accent_names[] = {"The theme's", "From the game's picture", "Blue", "Green", "Amber", "Violet", "Red", "Teal", "Slate",
                                  "Your own colour"};
    int ai = 9;
    for (int i = 0; i < 9; ++i)
        if (s.accent == accents[i]) ai = i;
    ImGui::SetNextItemWidth(theme::px(300.f));
    if (ImGui::Combo("##accent", &ai, accent_names, 10)) {
        if (ai < 9) s.accent = accents[ai];
        else {
            char hex[16];
            const ImU32 c = th::pal().accent;
            std::snprintf(hex, sizeof hex, "#%02x%02x%02x", static_cast<unsigned>(c & 0xFF), static_cast<unsigned>((c >> 8) & 0xFF),
                          static_cast<unsigned>((c >> 16) & 0xFF));
            s.accent = hex;
        }
        if (s.accent == "auto") s.have_frame_rgba = false;
    }
    if (ai == 9) {
        ImU32 c = 0;
        th::parse_colour(s.accent, &c);
        float rgbf[3] = {(c & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, ((c >> 16) & 0xFF) / 255.f};
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##own_accent", rgbf, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayHex)) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "#%02x%02x%02x", static_cast<int>(rgbf[0] * 255.f + 0.5f), static_cast<int>(rgbf[1] * 255.f + 0.5f),
                          static_cast<int>(rgbf[2] * 255.f + 0.5f));
            s.accent = hex;
        }
        ImGui::SameLine();
        static char hexbuf[16];
        std::snprintf(hexbuf, sizeof hexbuf, "%s", s.accent.c_str());
        if (field("##hex", hexbuf, sizeof hexbuf, Keys::Text, "#rrggbb", 120.f)) {
            ImU32 t = 0;
            if (th::parse_colour(hexbuf, &t)) s.accent = hexbuf;
            else toast_warn(std::string("\"") + hexbuf + "\" is not a colour (like #ffc23d)");
        }
    }
    if (s.accent == "auto") {
        ImGui::SameLine();
        ImGui::ColorButton("##frame", ImVec4(s.frame_rgba[0] / 255.f, s.frame_rgba[1] / 255.f, s.frame_rgba[2] / 255.f, 1.f));
        ImGui::SameLine();
        ImGui::TextDisabled(s.have_frame_rgba ? "from the game frame" : "(no frame sampled yet)");
    }
    th::text(th::Type::Strong, p.text, "HUD over the game");
    int ho = static_cast<int>(std::lround(s.hud_opacity * 100.f));
    ImGui::SetNextItemWidth(theme::px(300.f));
    if (ImGui::SliderInt("Opacity##hud", &ho, spec.hud_opacity.min, spec.hud_opacity.max, "%d%%")) s.hud_opacity = static_cast<float>(ho) / 100.f;
    ImGui::Checkbox("Also list every active cheat and hotkey", &s.hud_mini);
    th::text_wrapped(th::Type::Small, p.text3, "Pin values in My table (their actions) or cheat codes on the Cheats page (the pin): they show over the game while the panel is closed. Drag the HUD to move it while the panel is open.");
    th::text(th::Type::Strong, p.text, "Size");
    int sc = static_cast<int>(std::lround(s.scale * 100.f));
    ImGui::SetNextItemWidth(theme::px(300.f));
    if (ImGui::SliderInt("##scale", &sc, spec.scale.min, spec.scale.max, "%d%%")) s.scale = static_cast<float>(sc) / 100.f;
    th::text_wrapped(th::Type::Small, p.text3,
                     "For this session; the AnySwitch app's Settings > Cheats set them for every game (pkp.ini [cheats]: theme, style, "
                     "opacity, blur, tint, accent, scale). A theme's own font needs a restart.");
    tool_end();
}

// ================================================================================================================ Help
void win_help() {
    State& s = st();
    Win& w = win("Help");
    if (!tool_begin("Help", &w.open)) { tool_end(); return; }
    namespace th = theme;
    const th::Palette& p = th::pal();
    th::text(th::Type::Strong, p.text, "The pages");
    th::text_wrapped(th::Type::Body, p.text2,
        "Cheats: the game's cheat codes (and your mods'), each with a switch. Find a value: say what kind of value it is and what it is now, "
        "then change it in the game and say what happened until one match is left; select it to keep it. My table: what you kept: freeze "
        "it, type a new value, give it a hotkey, put it in a group, or save the table as cheat codes. Advanced: the memory viewer, the "
        "disassembler, find what writes a value, code patches, breakpoints, pointer paths, Lua and more; each can open in a window of its own.");
    th::text(th::Type::Strong, p.text, "Controller");
    th::text_wrapped(th::Type::Body, p.text2,
        "D-pad or left stick: move. A: select. B: back. X: a row's actions. Y: type with the on-screen keyboard. L / R: pages. ZL / ZR: page "
        "up and down. -: undo (while the message offers it); hold - to move between windows. +: close.");
    th::text(th::Type::Strong, p.text, "Keyboard and mouse");
    th::text_wrapped(th::Type::Body, p.text2,
        "Tab and the arrows move, Enter selects, Esc backs out, Ctrl+PgUp / PgDn switch pages, Ctrl+Z undoes, Delete removes a row of My "
        "table. Right-click a row for its actions. %s closes the overlay. Everything is saved for this game, next to it.", s.close_key.c_str());
    (void)s;
    tool_end();
}

}  // namespace nxrt::cheats::ui
