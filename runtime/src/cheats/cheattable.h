// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The overlay's saved cheat table and window layout (phase 6, CHEATS.md). Saved per title and build id as JSON next to the title
// (<cheats dir>/<build id>.cheattable.json and <build id>.overlay.json), outside the repository. Loadable and editable in the overlay;
// an entry or pointer path also exports to an Atmosphère .txt (export.h). This module is the pure (de)serialisation, with no ImGui or
// runtime dependency, so it is unit-tested on its own (tests/cheats). The overlay (cheats_ui.cpp) maps its Entry to a TableEntry.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nxrt::cheats::table {

// One saved address-list entry. A pointer entry carries its module+offsets; a plain entry an absolute address.
struct Offsets {
    std::string module;            // the static root module ("main", ...)
    unsigned mem_type = 0;         // Atmosphère nybble for base()
    uint64_t base_offset = 0;      // slot0 = base + base_offset
    std::vector<uint64_t> offsets; // the chain (last added, not dereferenced)
};
struct TableEntry {
    std::string description;
    bool is_pointer = false;
    uint64_t address = 0;          // absolute (when not a pointer)
    Offsets path;                  // when is_pointer
    int type = 2;                  // ScanType index 0..5 (u8,u16,u32,u64,f32,f64)
    std::string value;             // the frozen / last value as text
    bool freeze = false;
    int freeze_mode = 0;           // 0 hold, 1 increase-only, 2 decrease-only
    int hotkey = 0;                // SDL scancode, 0 none
    std::string group;
    // AOB-anchored entry (optional): a signature and the offset from its match to the address (CHEATS.md).
    std::string signature;         // "48 8B ?? .." or empty
    int64_t sig_offset = 0;
    std::string hotkey_name;       // "F5", "L+R+A" (a key name or pad buttons); preferred over the scancode
    int hotkey_action = 0;         // 0 toggle freeze (or the script), 1 write the value once, 2 add 1, 3 subtract 1
    // an AOB script entry (aobscript.h): its [ENABLE]/[DISABLE] text; `freeze` holds whether it is enabled
    bool is_script = false;
    std::string script;
    bool pinned = false;           // shown in the overlay's HUD over the game while the panel is closed
};

// What else a table keeps per title: address labels (shown in the viewer and the disassembler) and Lua scripts.
struct Label { uint64_t address = 0; std::string name; };
struct LuaScript { std::string name, source; bool run = false; };
struct Extras {
    std::vector<Label> labels;
    std::vector<LuaScript> lua;
};

// Which tool windows are open and the overlay settings remembered per title.
struct Layout {
    bool scanner = true, list = true, viewer = false, pointer = false, files = false, scripts = false, help = false;
    bool disasm = false, watch = false, memmap = false;
    uint64_t viewer_addr = 0;
    std::map<std::string, bool> open;       // every tool window by name (the fields above are the first ones, kept for old files)
    std::map<std::string, std::vector<int>> rect;   // each window's x, y, width, height in frame pixels ("Main": the overlay's panel)
    std::map<std::string, std::string> ui;          // the overlay's own choices: "page" (the page last shown), "tool" (the Advanced tool)
};

// JSON of the table / layout (what is written to disk). Stable field order so a diff is readable.
std::string dump_table(const std::vector<TableEntry>& entries);
std::string dump_table(const std::vector<TableEntry>& entries, const Extras& extras);
std::string dump_layout(const Layout& l);
// Parse back. Tolerant of our own output and of hand edits; returns false only when the text is not the expected object.
bool parse_table(const std::string& json, std::vector<TableEntry>* out);
bool parse_table(const std::string& json, std::vector<TableEntry>* out, Extras* extras);
bool parse_layout(const std::string& json, Layout* out);

}  // namespace nxrt::cheats::table
