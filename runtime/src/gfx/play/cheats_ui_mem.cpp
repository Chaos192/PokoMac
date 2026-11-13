// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's memory windows: Memory viewer (hex, value inspector, go to pointer), Disassembler (A64, function names, NOP /
// assemble as live code patches, breakpoints, the signature maker), Memory map, Structure dissect, Labels and Snapshots (diff).
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../cheats/aob.h"
#include "../../cheats/codepatch.h"
#include "../../cheats/control.h"
#include "../../cheats/disasm.h"
#include "../../cheats/gmem.h"
#include "cheats_ui_internal.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/module.h"

namespace nxrt::cheats::ui {
namespace {

Win& win(const char* title) {
    for (auto& w : st().wins)
        if (std::strcmp(w.title, title) == 0) return w;
    return st().wins[0];
}

// ---- the signature maker ----
struct SigMaker {
    bool open = false;
    uint64_t addr = 0;
    Signature sig;
    std::string module;
    uint64_t lo = 0, hi = 0;
    int len = 16;
    std::string msg;
} g_sig;

void make_signature() {
    SigMaker& m = g_sig;
    State& s = st();
    std::string mod;
    uint64_t fs = 0, fe = 0;
    m.module = "main";
    if (cp::function_at(m.addr, &mod, &fs, &fe)) m.module = mod;
    Module* md = module_by_name(m.module.c_str());
    m.lo = md ? md->base() : 0;
    m.hi = md ? md->base() + md->size() : 0;
    // wildcard the bytes the disassembler says move between builds (branch distances, adrp pages, immediates, offsets)
    std::vector<size_t> wild;
    for (size_t off = 0; off < 64; off += 4) {
        uint32_t w = 0;
        if (!gmem::read(m.addr + off, &w, 4)) break;
        for (int b : a64::signature_wildcards(w)) wild.push_back(off + static_cast<size_t>(b));
    }
    m.sig = aob_make_signature(*s.src, m.addr, m.lo, m.hi, wild, static_cast<size_t>(std::clamp(m.len, 8, 64)), 64);
    char b[160];
    std::snprintf(b, sizeof b, "%zu bytes, %zu fixed, matches %zu time%s in %s", m.sig.pattern.size(), m.sig.pattern.fixed_bytes(), m.sig.matches,
                  m.sig.matches == 1 ? "" : "s", m.module.c_str());
    m.msg = b;
}

// The AOB script that NOPs the instruction at the signature maker's address (found again by its signature): the window's button and
// the scripted test's aob_writer step.
std::string nop_script_text(const std::string& name) {
    SigMaker& m = g_sig;
    uint8_t old[4] = {};
    gmem::read(m.addr, old, 4);
    const uint32_t nop = a64::nop_word();
    std::vector<uint8_t> nb(4);
    std::memcpy(nb.data(), &nop, 4);
    return aa::make_patch_script(name, m.module, aob_text(m.sig.pattern), 0, nb, std::vector<uint8_t>(old, old + 4));
}

// The signature of the instruction at addr (as the window makes it); the AOB script that NOPs it. Empty with *msg when no unique one.
std::string nop_script_at(uint64_t addr, const std::string& name, std::string* msg) {
    g_sig.addr = addr;
    g_sig.len = 16;
    make_signature();
    *msg = aob_text(g_sig.sig.pattern) + ": " + g_sig.msg;
    if (!g_sig.sig.unique()) return {};
    return nop_script_text(name);
}

// ---- the disassembler's assemble prompt ----
struct AsmPrompt {
    bool open = false;
    uint64_t addr = 0;
    char text[96] = "";
    std::string msg;
} g_asm;

// the code injection prompt (a code cave)
struct InjectPrompt {
    bool open = false;
    uint64_t addr = 0;
    std::string text;
} g_inj;

void patch_word(uint64_t addr, uint32_t word) {
    std::vector<uint8_t> b(4);
    std::memcpy(b.data(), &word, 4);
    std::string err;
    if (cp::apply(addr, b, &err)) toast("code patch at " + gmem::describe(addr) + ": recompiling the function");
    else toast("cannot patch: " + err);
}

bool is_code(uint64_t addr) {
    std::string m;
    uint64_t s = 0, e = 0;
    return cp::function_at(addr, &m, &s, &e);
}
}  // namespace

std::string make_nop_script_at(uint64_t addr, const std::string& name, std::string* msg) { return nop_script_at(addr, name, msg); }

void open_signature_maker(uint64_t addr) {
    g_sig.open = true;
    g_sig.addr = addr & ~3ull;
    make_signature();
}

void draw_signature_maker() {
    SigMaker& m = g_sig;
    if (!m.open) return;
    State& s = st();
    ImGui::SetNextWindowSize(ImVec2(theme::px(620.f), 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Signature maker###cheats_sig", &m.open)) {
        ImGui::Text("Code at %s", cp::where(m.addr).c_str());
        ImGui::SetNextItemWidth(theme::px(120.f));
        if (ImGui::SliderInt("start length", &m.len, 8, 64)) make_signature();
        ImGui::SameLine();
        if (ImGui::Button("Make again")) make_signature();
        const std::string pat = aob_text(m.sig.pattern);
        ImGui::TextWrapped("%s", pat.c_str());
        ImGui::TextColored(m.sig.unique() ? ImVec4(0.5f, 0.95f, 0.6f, 1.f) : ImVec4(1.f, 0.65f, 0.4f, 1.f), "%s", m.msg.c_str());
        ImGui::TextDisabled("Branch distances, pages and immediates are wildcards (??), so the signature survives a rebuild.");
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(pat.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Table entry: NOP this instruction (AOB script)")) {
            Entry e;
            e.desc = "NOP " + gmem::describe(m.addr);
            e.is_script = true;
            e.script = nop_script_text(e.desc);
            e.uid = s.next_uid++;
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            s.list.push_back(e);
            mark_dirty();
            toast("AOB script added to the address list (switch it on there)");
        }
        ImGui::SameLine();
        if (ImGui::Button("Table entry: this address by signature")) {
            Entry e;
            e.desc = "AOB " + gmem::describe(m.addr);
            e.signature = pat;
            e.sig_offset = 0;
            e.addr = m.addr;
            e.type = ScanType::U32;
            e.uid = s.next_uid++;
            std::lock_guard<std::recursive_mutex> lk(s.mu);
            s.list.push_back(e);
            mark_dirty();
        }
    }
    ImGui::End();
}

// ================================================================================================================ Memory viewer
void win_viewer() {
    State& s = st();
    Win& w = win("Memory viewer");
    if (!tool_begin("Memory viewer", &w.open)) { tool_end(); return; }
    if (field("##goto", s.view_goto, sizeof s.view_goto, Keys::Text, "address, main+off or a label", 220)) {
        uint64_t a = 0;
        if (parse_addr(s.view_goto, &a)) { s.view_addr = a & ~0xfull; s.view_sel = a; }
        else toast("not an address: " + std::string(s.view_goto));
    }
    ImGui::SameLine();
    if (ImGui::Button("-1 page") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_PageUp) && !ImGui::GetIO().KeyCtrl)) s.view_addr -= 0x100;
    ImGui::SameLine();
    if (ImGui::Button("+1 page") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_PageDown) && !ImGui::GetIO().KeyCtrl)) s.view_addr += 0x100;
    ImGui::SameLine();
    if (ImGui::Button("Go to pointer")) {
        uint64_t p = 0;
        if (gmem::read(s.view_sel, &p, 8) && gmem::in_guest_range(p, 1)) { s.view_addr = p & ~0xfull; s.view_sel = p; }
        else toast("the 8 bytes there are not a guest pointer");
    }
    ImGui::TextDisabled("%s", cp::where(s.view_addr).c_str());
    const int rows = 16;
    theme::push(theme::Type::Mono);
    if (ImGui::BeginTable("hex", 18, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadInnerX)) {
        for (int r = 0; r < rows; ++r) {
            const uint64_t a = s.view_addr + static_cast<uint64_t>(r) * 16;
            uint8_t row[16];
            const bool ok = gmem::read(a, row, 16);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string lab = label_at(a);
            ImGui::TextDisabled("%010llX%s ", static_cast<unsigned long long>(a), lab.empty() ? "" : "*");
            for (int i = 0; i < 16; ++i) {
                ImGui::TableSetColumnIndex(1 + i);
                ImGui::PushID(r * 16 + i);
                char t[4];
                std::snprintf(t, sizeof t, ok ? "%02X" : "??", ok ? row[i] : 0);
                const uint64_t ba = a + static_cast<uint64_t>(i);
                const bool sel = ba == s.view_sel;
                if (ImGui::Selectable(t, sel, 0, ImVec2(ImGui::CalcTextSize("00").x + 4, 0))) s.view_sel = ba;
                if (ImGui::IsItemFocused() && pad_mode()) s.view_sel = ba;
                ImGui::PopID();
            }
            ImGui::TableSetColumnIndex(17);
            char asc[17];
            for (int i = 0; i < 16; ++i) asc[i] = ok && row[i] >= 32 && row[i] < 127 ? static_cast<char>(row[i]) : '.';
            asc[16] = 0;
            ImGui::TextUnformatted(asc);
        }
        ImGui::EndTable();
    }
    theme::pop();
    // the inspector: the selected address as every type, an edit, the actions
    ImGui::SeparatorText(("At " + gmem::describe(s.view_sel)).c_str());
    for (int k = 0; k < 6; ++k) {
        ImGui::Text("%-8s %s", kind_names()[k], value_text(s.view_sel, kind_type(k)).c_str());
        if (k % 3 != 2) ImGui::SameLine(0, 24);
    }
    {
        char text[48] = {};
        for (int i = 0; i < 40; ++i) {
            char c = 0;
            if (!gmem::read(s.view_sel + i, &c, 1) || c == 0) break;
            text[i] = (c >= 32 && c < 127) ? c : '.';
        }
        ImGui::Text("text     \"%s\"", text);
    }
    static char bytes_in[64] = "";
    if (field("##wr", bytes_in, sizeof bytes_in, Keys::Text, "write bytes here (hex: 01 02 FF)", 260)) {
        AobPattern p;
        std::string err;
        if (aob_parse(bytes_in, &p, &err) && p.fixed_bytes() == p.size()) {
            if (is_code(s.view_sel) && (s.view_sel & 3) == 0 && (p.size() & 3) == 0) {
                std::string e2;
                if (!cp::apply(s.view_sel, p.val, &e2)) toast(e2);
                else toast("code patch: recompiling the function");
            } else if (!gmem::write(s.view_sel, p.val.data(), p.val.size())) {
                toast("not writable");
            }
        } else {
            toast("bytes as hex pairs, no wildcards");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to list")) add_entry(s.view_sel, ScanType::U32, "");
    ImGui::SameLine();
    if (ImGui::Button("What writes it")) { std::snprintf(s.aw_addr, sizeof s.aw_addr, "%llX", static_cast<unsigned long long>(s.view_sel)); s.aw_mode = 0; open_window("Find what accesses"); }
    ImGui::SameLine();
    if (ImGui::Button("Disassemble")) goto_disasm(s.view_sel);
    ImGui::SameLine();
    if (ImGui::Button("Structure")) { s.struct_addr = s.view_sel; open_window("Structure"); }
    tool_end();
}

// ================================================================================================================ Disassembler
void win_disasm() {
    State& s = st();
    Win& w = win("Disassembler");
    if (!tool_begin("Disassembler", &w.open)) { tool_end(); return; }
    if (!s.dis_addr) s.dis_addr = gmem::main_base();
    if (field("##goto", s.dis_goto, sizeof s.dis_goto, Keys::Text, "address, main+off or a label", 220)) {
        uint64_t a = 0;
        if (parse_addr(s.dis_goto, &a)) goto_disasm(a);
        else toast("not an address: " + std::string(s.dis_goto));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.dis_history.empty());
    if (ImGui::Button("Back")) { s.dis_addr = s.dis_sel = s.dis_history.back(); s.dis_history.pop_back(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("-")) s.dis_addr -= 0x80;
    ImGui::SameLine();
    if (ImGui::Button("+")) s.dis_addr += 0x80;
    ImGui::SameLine();
    std::string fm;
    uint64_t fs = 0, fe = 0;
    if (cp::function_at(s.dis_sel ? s.dis_sel : s.dis_addr, &fm, &fs, &fe)) {
        if (ImGui::Button("Function start")) goto_disasm(fs);
        ImGui::SameLine();
        ImGui::TextDisabled("%s, %llu instructions", cp::where(fs).c_str(), static_cast<unsigned long long>((fe - fs) / 4));
    } else {
        ImGui::TextDisabled("not code the build lifted");
    }
    // patches and breakpoints by address, for the markers
    std::map<uint64_t, cp::Patch> patches;
    for (const auto& p : cp::patches()) if (p.state != cp::State::Reverted) patches[p.addr] = p;
    std::map<uint64_t, cp::Breakpoint> bps;
    for (const auto& b : cp::breakpoints()) bps[b.addr] = b;
    const int lines = 40;
    std::vector<uint8_t> code(static_cast<size_t>(lines) * 4);
    const bool ok = gmem::read(s.dis_addr, code.data(), code.size());
    theme::push(theme::Type::Mono);
    if (ImGui::BeginTable("dis", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, theme::px(200.f));
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, theme::px(90.f));
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < lines; ++i) {
            const uint64_t a = s.dis_addr + static_cast<uint64_t>(i) * 4;
            uint32_t word = 0;
            if (ok) std::memcpy(&word, code.data() + i * 4, 4);
            const a64::Insn in = a64::decode(word, a);
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            std::string lab = label_at(a);
            uint64_t f0 = 0, f1 = 0;
            std::string fmod;
            const bool fn_start = cp::function_at(a, &fmod, &f0, &f1) && f0 == a;
            const auto bp = bps.find(a);
            std::string addr_text = std::string(bp != bps.end() ? (bp->second.stop ? "(B) " : "(T) ") : "    ") + gmem::describe(a);
            if (ImGui::Selectable(addr_text.c_str(), s.dis_sel == a, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                s.dis_sel = a;
                if (ImGui::IsMouseDoubleClicked(0) && in.branch_imm && in.target) goto_disasm(in.target);
            }
            if (ImGui::IsItemFocused()) {
                s.dis_sel = a;
                if (pad_mode() && pad_pressed(hid::btn(hid::kBtnX))) ImGui::OpenPopup("dis_ctx");
                if (pad_mode() && pad_pressed(hid::btn(hid::kBtnY)) && in.branch_imm && in.target) goto_disasm(in.target);
                if (!pad_mode() && ImGui::IsKeyPressed(ImGuiKey_Enter) && in.branch_imm && in.target) goto_disasm(in.target);
            }
            if (ImGui::BeginPopupContextItem("dis_ctx")) {
                ImGui::TextDisabled("%s  %s", gmem::describe(a).c_str(), in.text.c_str());
                if (ImGui::MenuItem("NOP this instruction (live patch)")) patch_word(a, a64::nop_word());
                if (ImGui::MenuItem("Assemble a replacement...")) { g_asm.open = true; g_asm.addr = a; std::snprintf(g_asm.text, sizeof g_asm.text, "%s", in.modelled ? in.text.c_str() : ""); for (char* c = g_asm.text; *c; ++c) if (*c == '\t') *c = ' '; g_asm.msg.clear(); }
                if (ImGui::MenuItem("Inject code here (a code cave)...")) { g_inj.open = true; g_inj.addr = a; g_inj.text = "// runs before the instruction here, then it and the rest\n"; }
                const auto p = patches.find(a);
                if (p != patches.end() && ImGui::MenuItem("Revert the patch")) { std::string e; if (!cp::revert(a, &e)) toast(e); }
                ImGui::Separator();
                if (ImGui::MenuItem(bp != bps.end() ? "Remove the breakpoint" : "Breakpoint (stop here)")) {
                    std::string e;
                    if (bp != bps.end()) cp::bp_remove(a, &e);
                    else if (!cp::bp_add(a, true, &e)) toast(e);
                    else open_window("Breakpoints");
                }
                if (bp == bps.end() && ImGui::MenuItem("Trace point (count and record, no stop)")) {
                    std::string e;
                    if (!cp::bp_add(a, false, &e)) toast(e);
                    else open_window("Breakpoints");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Make a signature...")) open_signature_maker(a);
                if (in.branch_imm && in.target && ImGui::MenuItem("Follow the target")) goto_disasm(in.target);
                if (ImGui::MenuItem("Browse this memory")) goto_viewer(a);
                if (ImGui::MenuItem("Label this address")) { s.labels.push_back({a, "label_" + hexs(a)}); open_window("Labels"); mark_dirty(); }
                if (ImGui::MenuItem("Copy the address")) ImGui::SetClipboardText(hexs(a).c_str());
                ImGui::EndPopup();
            }
            if (fn_start) {
                ImGui::SameLine();
                ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "fn");
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%08X", word);
            ImGui::TableSetColumnIndex(2);
            std::string t = in.text;
            for (char& c : t) if (c == '\t') c = ' ';
            if (!in.modelled) ImGui::TextDisabled("%s", t.c_str());
            else if (in.is_branch) ImGui::TextColored(ImVec4(0.65f, 0.80f, 1.f, 1.f), "%s", t.c_str());
            else ImGui::TextUnformatted(t.c_str());
            ImGui::TableSetColumnIndex(3);
            std::string note;
            if (!lab.empty()) note = lab;
            if (in.branch_imm && in.target) {
                const std::string tl = label_at(in.target);
                note += (note.empty() ? "" : "  ") + std::string("-> ") + (tl.empty() ? gmem::describe(in.target) : tl);
            }
            const auto p = patches.find(a);
            if (p != patches.end()) {
                ImGui::TextColored(p->second.state == cp::State::Live ? ImVec4(0.5f, 0.95f, 0.6f, 1.f) : ImVec4(1.f, 0.8f, 0.4f, 1.f), "patched: %s", cp::state_name(p->second.state));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p->second.note.c_str());
                ImGui::SameLine();
            }
            if (bp != bps.end()) {
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.5f, 1.f), "%llu hits", static_cast<unsigned long long>(bp->second.hits));
                ImGui::SameLine();
            }
            ImGui::TextDisabled("%s", note.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    theme::pop();
    ImGui::TextDisabled(pad_mode() ? "(X) actions: NOP, assemble, breakpoint, signature   (Y) follow a branch" : "right-click: NOP, assemble, breakpoint, signature   double-click / Enter: follow");
    tool_end();

    // the injection prompt
    if (g_inj.open) {
        ImGui::SetNextWindowSize(ImVec2(theme::px(560.f), theme::px(320.f)), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Inject code###cheats_inject", &g_inj.open)) {
            ImGui::TextWrapped("At %s: these instructions run first, then the one that is there, then the code goes on. One A64 instruction per line.",
                               cp::where(g_inj.addr).c_str());
            editor("##inj", &g_inj.text, ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.5f));
            if (ImGui::Button("Inject (live)")) {
                std::vector<std::string> lines;
                size_t i = 0;
                while (i <= g_inj.text.size()) {
                    size_t j = g_inj.text.find('\n', i);
                    if (j == std::string::npos) j = g_inj.text.size();
                    std::string l = g_inj.text.substr(i, j - i);
                    if (const size_t c = l.find("//"); c != std::string::npos) l = l.substr(0, c);
                    if (l.find_first_not_of(" \t\r") != std::string::npos) lines.push_back(l);
                    i = j + 1;
                }
                std::string err;
                if (cp::inject(g_inj.addr, lines, &err)) { toast("code cave in place: recompiling the function"); g_inj.open = false; }
                else toast("cannot inject: " + err);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) g_inj.open = false;
        }
        ImGui::End();
    }
    // the assemble prompt
    if (g_asm.open) {
        ImGui::SetNextWindowSize(ImVec2(theme::px(520.f), 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Assemble###cheats_asm", &g_asm.open)) {
            ImGui::Text("Replace the instruction at %s", gmem::describe(g_asm.addr).c_str());
            const bool go = field("##asm", g_asm.text, sizeof g_asm.text, Keys::Text, "mov w8, #99   /  nop  /  b 0x7100001234", -1);
            uint32_t word = 0;
            std::string err;
            const bool valid = a64::assemble(g_asm.text, g_asm.addr, &word, &err);
            if (valid) ImGui::TextColored(ImVec4(0.5f, 0.95f, 0.6f, 1.f), "%08X  %s", word, a64::decode(word, g_asm.addr).text.c_str());
            else ImGui::TextColored(ImVec4(1.f, 0.6f, 0.45f, 1.f), "%s", err.c_str());
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("Patch (live)") || (go && valid)) {
                patch_word(g_asm.addr, word);
                g_asm.open = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) g_asm.open = false;
        }
        ImGui::End();
    }
}

// ================================================================================================================ Memory map
void win_memmap() {
    State& s = st();
    Win& w = win("Memory map");
    if (!tool_begin("Memory map", &w.open)) { tool_end(); return; }
    static std::vector<gmem::Region> regions;
    static double last = 0;
    if (ImGui::Button("Refresh") || now_s() - last > 2.0) { regions = gmem::regions(); last = now_s(); }
    ImGui::SameLine();
    uint64_t total = 0;
    for (const auto& r : regions) total += r.size;
    ImGui::Text("%zu blocks, %.1f MiB", regions.size(), static_cast<double>(total) / 1048576.0);
    if (ImGui::BeginTable("map", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, theme::px(130.f));
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, theme::px(100.f));
        ImGui::TableSetupColumn("Perm", ImGuiTableColumnFlags_WidthFixed, theme::px(45.f));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, theme::px(60.f));
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(regions.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto& r = regions[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                char a[24];
                std::snprintf(a, sizeof a, "%010llX", static_cast<unsigned long long>(r.addr));
                if (ImGui::Selectable(a, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (r.perm & 4) goto_disasm(r.addr);
                    else goto_viewer(r.addr);
                }
                ImGui::TableSetColumnIndex(1);
                if (r.size >= 1048576) ImGui::Text("%.1f MiB", static_cast<double>(r.size) / 1048576.0);
                else ImGui::Text("%.1f KiB", static_cast<double>(r.size) / 1024.0);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%c%c%c", (r.perm & 1) ? 'r' : '-', (r.perm & 2) ? 'w' : '-', (r.perm & 4) ? 'x' : '-');
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(r.kind);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(r.module.c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    (void)s;
    tool_end();
}

// ================================================================================================================ Structure dissect
void win_struct() {
    State& s = st();
    Win& w = win("Structure");
    if (!tool_begin("Structure", &w.open)) { tool_end(); return; }
    static char addr_in[32] = "";
    if (field("##sa", addr_in, sizeof addr_in, Keys::Text, "structure address", 200)) {
        uint64_t a = 0;
        if (parse_addr(addr_in, &a)) s.struct_addr = a;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(theme::px(110.f));
    ImGui::InputInt("size", &s.struct_size, 0x40, 0x100, ImGuiInputTextFlags_CharsHexadecimal);
    s.struct_size = std::clamp(s.struct_size, 0x10, 0x4000);
    ImGui::Text("at %s", cp::where(s.struct_addr).c_str());
    // each 8-byte slot is shown as what it most likely is: a pointer into guest memory, a float pair, small integers, or text
    if (ImGui::BeginTable("struct", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, theme::px(60.f));
        ImGui::TableSetupColumn("Guess", ImGuiTableColumnFlags_WidthFixed, theme::px(70.f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(90.f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int off = 0; off < s.struct_size; off += 8) {
            const uint64_t a = s.struct_addr + static_cast<uint64_t>(off);
            uint64_t v = 0;
            const bool ok = gmem::read(a, &v, 8);
            ImGui::TableNextRow();
            ImGui::PushID(off);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("+%03X", off);
            ImGui::TableSetColumnIndex(1);
            const char* guess = "?";
            char val[160] = "??";
            if (ok) {
                float f0, f1;
                std::memcpy(&f0, &v, 4);
                std::memcpy(&f1, reinterpret_cast<const char*>(&v) + 4, 4);
                auto plausible = [](float f) { const float a = std::fabs(f); return std::isfinite(f) && (a == 0.f || (a > 1e-4f && a < 1e7f)); };
                bool text = true;
                for (int i = 0; i < 8 && text; ++i) { const uint8_t c = static_cast<uint8_t>(v >> (8 * i)); text = (c >= 32 && c < 127) || (c == 0 && i > 1); }
                if (v && gmem::in_guest_range(v, 1) && (v & 3) == 0) {
                    guess = "pointer";
                    std::snprintf(val, sizeof val, "-> %s", cp::where(v).c_str());
                } else if (text && (v & 0xff)) {
                    guess = "text";
                    char t[9] = {};
                    std::memcpy(t, &v, 8);
                    std::snprintf(val, sizeof val, "\"%s\"", t);
                } else if (plausible(f0) && plausible(f1) && (f0 != 0.f || f1 != 0.f) && (v >> 32) != 0) {
                    guess = "2 floats";
                    std::snprintf(val, sizeof val, "%g, %g", f0, f1);
                } else if ((v >> 32) == 0 || (v >> 32) == 0xffffffffull) {
                    guess = "int";
                    std::snprintf(val, sizeof val, "%d  /  %d, %d", static_cast<int32_t>(v), static_cast<int16_t>(v), static_cast<int16_t>(v >> 16));
                } else {
                    guess = "8 bytes";
                    std::snprintf(val, sizeof val, "%016llX", static_cast<unsigned long long>(v));
                }
            }
            ImGui::TextUnformatted(guess);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(val);
            ImGui::TableSetColumnIndex(3);
            if (std::strcmp(guess, "pointer") == 0 && ImGui::SmallButton("open")) s.struct_addr = v;
            ImGui::SameLine();
            if (ImGui::SmallButton("add")) add_entry(a, std::strcmp(guess, "2 floats") == 0 ? ScanType::F32 : std::strcmp(guess, "pointer") == 0 ? ScanType::U64 : ScanType::U32, "");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    tool_end();
}

// ================================================================================================================ Labels
void win_labels() {
    State& s = st();
    Win& w = win("Labels");
    if (!tool_begin("Labels", &w.open)) { tool_end(); return; }
    static char la[32] = "", ln[64] = "";
    field("##la", la, sizeof la, Keys::Text, "address", 160);
    ImGui::SameLine();
    field("##ln", ln, sizeof ln, Keys::Text, "name", 200);
    ImGui::SameLine();
    if (ImGui::Button("Add label")) {
        uint64_t a = 0;
        if (parse_addr(la, &a) && ln[0]) { s.labels.push_back({a, ln}); ln[0] = 0; mark_dirty(); }
        else toast("an address and a name");
    }
    int del = -1;
    if (ImGui::BeginTable("labels", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(110.f));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.labels.size(); ++i) {
            auto& l = s.labels[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableSetColumnIndex(0);
            if (field("##n", &l.name, Keys::Text, "name", -1)) mark_dirty();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cp::where(l.address).c_str());
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("go")) { if (is_code(l.address)) goto_disasm(l.address); else goto_viewer(l.address); }
            ImGui::SameLine();
            if (ImGui::SmallButton("delete")) del = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (del >= 0) { s.labels.erase(s.labels.begin() + del); mark_dirty(); }
    tool_end();
}

// ================================================================================================================ Snapshots
// Copies the range in the window's fields (or the regions the scanner's filter selects), at most 512 MiB, as a new snapshot.
void take_snapshot() {
    State& s = st();
    Snapshot sn;
    char nm[48];
    std::snprintf(nm, sizeof nm, "#%zu at frame %llu", s.snaps.size() + 1, static_cast<unsigned long long>(ctl::frames()));
    sn.name = nm;
    sn.taken_frame = ctl::frames();
    uint64_t lo = 0, hi = 0;
    std::vector<ScanRegion> regs;
    if (parse_addr(s.snap_lo, &lo) && parse_addr(s.snap_hi, &hi) && hi > lo) regs.push_back({lo, hi - lo, 3, "range"});
    else regs = s.src->regions(s.filter);
    const uint64_t cap = 512ull << 20;
    for (const auto& r : regs) {
        for (uint64_t off = 0; off < r.size && sn.bytes < cap; off += 1 << 20) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(1 << 20, r.size - off));
            std::vector<uint8_t> b(n);
            if (!s.src->read(r.addr + off, b.data(), n)) continue;
            sn.bytes += n;
            sn.chunks.push_back({r.addr + off, std::move(b)});
        }
    }
    s.snaps.push_back(std::move(sn));
}

// Compares snapshot snap_a with snap_b (-1: memory now) into s.diff, every 4-byte word that differs (at most 200000).
void compare_snapshots() {
    State& s = st();
    s.diff.clear();
    if ((s.snap_a < 0 && s.snap_b < 0) || s.snap_a == s.snap_b) return;
    const Snapshot* A = s.snap_a >= 0 ? &s.snaps[s.snap_a] : nullptr;
    const Snapshot* B = s.snap_b >= 0 ? &s.snaps[s.snap_b] : nullptr;
    const Snapshot* ref = A ? A : B;
    for (size_t ci = 0; ci < ref->chunks.size() && s.diff.size() < 200000; ++ci) {
        const auto& rc = ref->chunks[ci];
        std::vector<uint8_t> now;
        auto bytes_of = [&](const Snapshot* sn) -> const std::vector<uint8_t>* {
            if (!sn) {
                now.resize(rc.second.size());
                if (!s.src->read(rc.first, now.data(), now.size())) return nullptr;
                return &now;
            }
            for (const auto& c : sn->chunks)
                if (c.first == rc.first && c.second.size() == rc.second.size()) return &c.second;
            return nullptr;
        };
        const std::vector<uint8_t>* a = bytes_of(A);
        const std::vector<uint8_t>* b = bytes_of(B);
        if (!a || !b) continue;
        for (size_t i = 0; i + 4 <= a->size() && s.diff.size() < 200000; i += 4) {
            uint32_t x, y;
            std::memcpy(&x, a->data() + i, 4);
            std::memcpy(&y, b->data() + i, 4);
            if (x != y) s.diff.push_back({rc.first + i, {x, y}});
        }
    }
}

void win_snapshots() {
    State& s = st();
    Win& w = win("Snapshots");
    if (!tool_begin("Snapshots", &w.open)) { tool_end(); return; }
    ImGui::TextWrapped("A snapshot copies guest memory (the regions the scanner's filter selects, or a range). Compare two snapshots, or "
                       "one with memory now, to see every 4-byte word that changed.");
    field("##lo", s.snap_lo, sizeof s.snap_lo, Keys::Text, "from (empty: the filter)", 170);
    ImGui::SameLine();
    field("##hi", s.snap_hi, sizeof s.snap_hi, Keys::Text, "to", 170);
    ImGui::SameLine();
    if (ImGui::Button("Take a snapshot")) {
        take_snapshot();
        toast("snapshot taken: " + std::to_string(s.snaps.back().bytes >> 20) + " MiB");
    }
    std::vector<std::string> names = {"memory now"};
    for (const auto& sn : s.snaps) names.push_back(sn.name + " (" + std::to_string(sn.bytes >> 20) + " MiB)");
    auto pick = [&](const char* id, int* sel) {
        ImGui::SetNextItemWidth(theme::px(230.f));
        if (ImGui::BeginCombo(id, names[static_cast<size_t>(*sel + 1)].c_str())) {
            for (int i = -1; i < static_cast<int>(s.snaps.size()); ++i)
                if (ImGui::Selectable(names[static_cast<size_t>(i + 1)].c_str(), *sel == i)) *sel = i;
            ImGui::EndCombo();
        }
    };
    if (s.snap_a >= static_cast<int>(s.snaps.size())) s.snap_a = -1;
    if (s.snap_b >= static_cast<int>(s.snaps.size())) s.snap_b = -1;
    pick("old", &s.snap_a);
    ImGui::SameLine();
    pick("new", &s.snap_b);
    ImGui::SameLine();
    if (ImGui::Button("Compare") && (s.snap_a >= 0 || s.snap_b >= 0) && s.snap_a != s.snap_b) {
        compare_snapshots();
        toast(std::to_string(s.diff.size()) + " words changed");
    }
    ImGui::SameLine();
    if (ImGui::Button("Drop all")) { s.snaps.clear(); s.diff.clear(); s.snap_a = s.snap_b = -1; }
    ImGui::Text("%zu changed words", s.diff.size());
    if (ImGui::BeginTable("diff", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Old", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("New", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme::px(50.f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(s.diff.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto& d = s.diff[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(gmem::describe(d.first).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", static_cast<int32_t>(d.second.first));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d %s", static_cast<int32_t>(d.second.second), static_cast<int32_t>(d.second.second) > static_cast<int32_t>(d.second.first) ? "^" : "v");
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("add")) add_entry(d.first, ScanType::U32, "");
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    tool_end();
}

}  // namespace nxrt::cheats::ui
