// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Round-trip test for the saved cheat table + layout (src/cheats/cheattable.cpp). No runtime dependency.
#include "cheattable.h"
#include "cheattable.cpp"

#include <cassert>
#include <cstdio>

using namespace nxrt::cheats::table;

int main() {
    std::vector<TableEntry> in;
    {
        TableEntry a;
        a.description = "coins \"money\"";   // quotes must survive
        a.address = 0x7100abcdef;
        a.type = 2;
        a.value = "999";
        a.freeze = true;
        a.freeze_mode = 1;
        a.hotkey = 58;
        a.group = "player";
        in.push_back(a);
        TableEntry b;
        b.description = "lives (pointer)";
        b.is_pointer = true;
        b.path.module = "main";
        b.path.mem_type = 0;
        b.path.base_offset = 0x123456;
        b.path.offsets = {0x10, 0x8, 0x20};
        b.type = 2;
        b.value = "5";
        in.push_back(b);
        TableEntry c;
        c.description = "aob NOP decrement";
        c.address = 0x7100112233;
        c.signature = "48 8B ?? 4?";
        c.sig_offset = -4;
        in.push_back(c);
    }
    const std::string js = dump_table(in);
    std::vector<TableEntry> out;
    if (!parse_table(js, &out)) { std::printf("FAIL: parse_table rejected our own output\n%s\n", js.c_str()); return 1; }
    assert(out.size() == 3);
    assert(out[0].description == "coins \"money\"");
    assert(out[0].address == 0x7100abcdefull);
    assert(out[0].freeze && out[0].freeze_mode == 1 && out[0].hotkey == 58 && out[0].group == "player");
    assert(out[1].is_pointer && out[1].path.module == "main" && out[1].path.base_offset == 0x123456ull);
    assert(out[1].path.offsets.size() == 3 && out[1].path.offsets[2] == 0x20);
    assert(out[2].signature == "48 8B ?? 4?" && out[2].sig_offset == -4);

    Layout l;
    l.viewer = true; l.watch = true; l.disasm = true; l.viewer_addr = 0x7100aabbcc;
    Layout l2;
    if (!parse_layout(dump_layout(l), &l2)) { std::printf("FAIL: parse_layout\n"); return 1; }
    assert(l2.viewer && l2.watch && l2.disasm && !l2.pointer && l2.viewer_addr == 0x7100aabbccull);

    // hotkey names, AOB script entries, labels, Lua scripts and the per-window map round-trip
    {
        std::vector<TableEntry> t;
        TableEntry s;
        s.description = "no coin loss";
        s.is_script = true;
        s.script = "[ENABLE]\naobscanmodule(x, main, 08 05 00 51)\nx:\n nop\n[DISABLE]\n";
        s.freeze = true;
        s.hotkey_name = "L+R+A";
        s.hotkey_action = 2;
        s.pinned = true;
        t.push_back(s);
        Extras ex;
        ex.labels.push_back({0x7100001230ull, "coin_spend"});
        ex.lua.push_back({"auto", "nx.onframe(function() end)\nprint(\"hi\")", true});
        std::vector<TableEntry> t2;
        Extras ex2;
        const std::string j = dump_table(t, ex);
        assert(parse_table(j, &t2, &ex2));
        assert(t2.size() == 1 && t2[0].is_script && t2[0].script == s.script && t2[0].hotkey_name == "L+R+A" && t2[0].hotkey_action == 2);
        assert(t2[0].pinned);
        assert(dump_table(t2, ex2).find("\"pinned\": true") != std::string::npos);
        assert(ex2.labels.size() == 1 && ex2.labels[0].address == 0x7100001230ull && ex2.labels[0].name == "coin_spend");
        assert(ex2.lua.size() == 1 && ex2.lua[0].run && ex2.lua[0].source == ex.lua[0].source);
        Layout lo;
        lo.open["Threads"] = true;
        lo.open["Memory map"] = false;
        Layout lo2;
        lo.rect["Main"] = {40, 30, 900, 600};
        lo.ui["page"] = "Find a value";
        lo.ui["tool"] = "Memory \"viewer\"";
        assert(parse_layout(dump_layout(lo), &lo2) && lo2.open.size() == 2 && lo2.open["Threads"] && !lo2.open["Memory map"]);
        assert(lo2.rect["Main"].size() == 4 && lo2.rect["Main"][2] == 900);
        assert(lo2.ui.size() == 2 && lo2.ui["page"] == "Find a value" && lo2.ui["tool"] == "Memory \"viewer\"");
    }

    // tolerant of trailing junk / missing optional fields
    std::vector<TableEntry> o3;
    assert(parse_table("{\"format\":1,\"entries\":[{\"address\":16}]}", &o3) && o3.size() == 1 && o3[0].address == 16);
    assert(!parse_table("not json", &o3));

    std::printf("cheattable_test: all passed\n");
    return 0;
}
