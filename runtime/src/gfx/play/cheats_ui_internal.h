// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Shared state and helpers of the cheats overlay's windows (cheats_ui*.cpp). Not a public header: cheats_ui.h is the overlay's interface.
#pragma once
#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../../cheats/accesswatch.h"
#include "../../cheats/aobscript.h"
#include "../../cheats/cheattable.h"
#include "../../cheats/engine.h"
#include "../../cheats/luavm.h"
#include "../../cheats/pointerscan.h"
#include "../../cheats/scanner.h"
#include "cheats_theme.h"

namespace nxrt::cheats::ui {

// ---- the address list -----------------------------------------------------------------------------------------------------------
struct Entry {
    std::string desc;
    bool is_pointer = false;
    uint64_t addr = 0;                    // static address (or the last resolved one)
    PointerPath path;                     // when is_pointer
    ScanType type = ScanType::U32;
    std::string value;                    // shown / edited value text
    uint64_t freeze_id = 0;               // 0 = not frozen
    FreezeMode mode = FreezeMode::Normal;
    std::string hotkey;                   // "F5", "L+R+A", "" = none
    int hotkey_action = 0;                // 0 toggle, 1 set value, 2 +1, 3 -1
    std::string group;
    std::string signature;                // AOB-anchored: the address is the match + sig_offset
    int64_t sig_offset = 0;
    bool sig_resolved = false;
    bool is_script = false;               // an AOB script entry
    std::string script;
    bool script_on = false;
    aa::RunResult script_run;             // what the enable did (for the disable)
    std::string status;                   // "pattern not found: turned off", ...
    bool watch = false;                   // sampled into the value graph
    bool hk_down = true;                  // hotkey edge detection (true: no fire for a key already down)
    bool pinned = false;                  // shown in the HUD over the game
    uint64_t uid = 0;                     // stable id for ImGui and graphs
};

// ---- value watches (graphs and triggers) ----------------------------------------------------------------------------------------
struct Watch {
    uint64_t uid = 0;                     // the entry it follows
    std::vector<float> samples;           // ring buffer
    size_t head = 0;
    bool full = false;
    int trig = 0;                         // 0 none, 1 changed, 2 >, 3 <, 4 ==
    double trig_value = 0;
    bool trig_pause = true;               // pause the game when it fires (else only log)
    bool fired = false;
    double last = 0;
    bool have_last = false;
};

struct Snapshot {
    std::string name;
    uint64_t taken_frame = 0;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> chunks;   // address, bytes
    uint64_t bytes = 0;
};

// One tool of the Advanced page: its title (also the key in the layout file), the group it is listed under, its icon and one line that
// says what it is for. It shows inside the Advanced page when selected; `open` puts it in a window of its own (several can be open).
struct Win {
    const char* title;
    const char* group;                    // "Search", "Memory", "Code", "Game"
    theme::Icon icon;
    const char* blurb;                    // what it is for, in plain words
    void (*draw)();
    bool open = false;                    // popped out into its own window (and shown)
    int rect[4] = {0, 0, 0, 0};           // its window's saved position and size (width 0: none)
};

// The overlay's pages (the tabs of the main panel), simple first: L / R step through them.
enum Page : int { PageCheats = 0, PageFind, PageTable, PageAdvanced, PageCount };

// A short message at the foot of the panel; with `undo` it offers to take the action back (- on the pad, Ctrl+Z, or its button).
struct Toast {
    std::string text;
    int kind = 0;                         // 0 done, 1 warning, 2 error
    double until = 0;
    std::function<void()> undo;
};
// A question before a destructive action (a modal: A / Enter confirms, B / Esc cancels).
struct Confirm {
    std::string title, body, ok;
    std::function<void()> fn;
    bool pending = false;
};

struct State {
    bool inited = false;
    bool look_inited = false;             // init_look ran (the F1 settings menu shares the look without the cheats being on)
    std::recursive_mutex mu;              // the entries and everything the frame callbacks touch (render thread vs presenting thread)
    // settings
    // the look ([cheats] theme, accent, style, opacity, blur, tint, scale; the Settings tool): -1 / "theme" = the theme's own value
    std::string theme_name;               // a built-in theme or a theme file
    std::string theme_error;              // why that theme was not used (the default theme was), logged once
    std::string accent = "theme";
    int style = -1;                       // -1 the theme's, 0 solid, 1 glass
    float opacity = -1.f, blur = -1.f, tint = -1.f;
    float scale = 1.0f;
    bool pause_while_open = false;
    bool animations = true;               // [cheats] animations: a short fade when the overlay opens
    double shown_at = 0;                  // when it last opened (the fade; cheats_ui.h on_shown)
    unsigned char frame_rgba[4] = {0, 0, 0, 0};
    bool have_frame_rgba = false;
    // scanner
    std::shared_ptr<MemSource> src;
    std::unique_ptr<Scanner> scanner;
    ScanProgress prog;
    std::unique_ptr<std::thread> scan_thread;
    int kind_i = 2;                       // index into the scan kind list (u8, u16, u32, u64, f32, f64, all, utf8, utf16, aob)
    int first_i = 0, next_i = 1;
    char val1[128] = "", val2[64] = "";
    bool ignore_case = true;
    RegionFilter filter;
    bool fast = true;
    bool pause_scan = false;
    bool first_done = false;
    size_t last_count = 0;
    size_t result_sel = SIZE_MAX;
    std::string scan_msg;
    std::vector<std::string> history;     // the scans of this session, newest last ("first: 4 bytes exact 100 -> 214 results")
    // the list
    std::vector<Entry> list;
    uint64_t next_uid = 1;
    char group_filter[48] = "";
    bool dirty = false;
    double last_save = 0;
    // viewer / disassembler
    uint64_t view_addr = 0, view_sel = 0;
    uint64_t dis_addr = 0, dis_sel = 0;
    std::vector<uint64_t> dis_history;
    char view_goto[32] = "", dis_goto[32] = "";
    // pointer scan
    char ptr_target[32] = "", ptr_new[32] = "";
    std::vector<PointerPath> ptr_paths;
    PointerScanOptions ptr_opt;
    std::atomic<bool> ptr_running{false};
    std::unique_ptr<std::thread> ptr_thread;
    ScanProgress ptr_prog;
    std::string ptr_msg;
    // structure dissect
    char dis_struct_addr[32] = "";
    uint64_t struct_addr = 0;
    int struct_size = 0x100;
    // snapshots
    std::vector<Snapshot> snaps;
    int snap_a = -1, snap_b = -1;          // -1 = memory now
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> diff;
    char snap_lo[32] = "", snap_hi[32] = "";
    // find what writes / reads
    char aw_addr[32] = "";
    int aw_mode = 0;
    std::string aw_msg;
    // breakpoints
    char bp_addr[32] = "";
    std::string bp_msg;
    // labels
    std::vector<table::Label> labels;
    // value watches
    std::vector<Watch> watches;
    // game control
    float speed = 1.0f;
    int advance_n = 1;
    // Lua
    std::unique_ptr<lua::Vm> lua;
    std::unique_ptr<lua::Host> lua_host;
    std::vector<table::LuaScript> lua_scripts;
    std::map<std::string, uint32_t> lua_ids;     // script name -> running id
    int lua_sel = -1;
    std::vector<std::string> console;
    char console_in[256] = "";
    std::string lua_edit;
    // cheat files
    std::string files_msg;
    char import_path[260] = "";
    // code patches
    std::string patch_msg;
    // the script editor (an AOB script entry)
    int script_edit = -1;
    std::string script_text;
    // hotkey capture: the entry whose hotkey the next key / pad press sets
    int hotkey_capture = -1;
    // windows: the main panel's page, its rectangle, the tool the Advanced page shows; the tools' own windows
    int page = PageCheats;
    std::string tool = "Memory viewer";
    int main_rect[4] = {0, 0, 0, 0};      // the main panel's saved position and size (width 0: the default)
    bool main_rect_applied = false;
    std::string close_key = "F3";         // [cheats] key, for the hints
    // an overlay first: the panel docks left, centre (where it was put) or right; Peek fades it almost out while held (ZR, the overlay
    // key, the eye button); the HUD shows what is pinned over the game while the panel is closed
    int dock = 1;                         // 0 left, 1 centre, 2 right
    bool peek_ui = false;                 // the eye button held
    bool peek_key = false;                // the overlay key held (overlay.cpp)
    double zr_since = 0;                  // ZR held since (a short press pages down, a long one peeks)
    bool zr_peek = false;
    float hud_opacity = 0.85f;
    bool hud_mini = false;                // [cheats] mini: the HUD also lists every active cheat and hotkey
    int hud_pos[2] = {-1, -1};            // the HUD's top left in UI units (-1: the default corner)
    bool hud_shown = true;                // the HUD's own toggle ([cheats] hud_key / hud_pad): hidden keeps the pins, shows nothing
    std::set<std::string> pinned_cheats;  // cheat codes of the Cheats page pinned to the HUD, by name
    // glass: the blurred frame to draw behind the panel (set_backdrop) and the panel's rectangle this frame (backdrop_rect)
    ImTextureRef backdrop;
    bool backdrop_on = false;
    int glass_rect[4] = {0, 0, 0, 0};
    bool glass_drawn = false;
    std::vector<Win> wins;
    // input
    std::mutex in_mu;
    std::set<int> keys_down;              // SDL scancodes held
    uint64_t pad_down = 0;                // pad buttons held (hid bits)
    std::atomic<bool> pad_last{false};    // the last input came from a pad (text fields open the on-screen keyboard)
    uint64_t pad_prev_ui = 0;             // pad bits at the previous UI frame (edges for X / Y / L / R / ZL / ZR)
    uint64_t pad_edge = 0;                // pressed since the previous UI frame
    uint32_t frame_cb = 0;
    // status
    std::string nav_focus;                // the scripted test's `focus`: the item to put the pad's focus on (nav_focus_here)
    Toast toast;
    Confirm confirm;
    // the scan wizard (Find a value): the "what is it now" choice (0 known, 1 unknown, 2 a comparison); the next-scan value field
    int find_now = 0;
    char next_val[64] = "";
    size_t find_added = 0;                // results added to the table from this search (names "Value 1", "Value 2" ...)
};
State& st();

// ---- helpers (cheats_ui.cpp) -------------------------------------------------------------------------------------------------------
double now_s();
void toast(const std::string& msg);                                       // a result ("added Coins to the table")
void toast_warn(const std::string& msg);                                  // something did not work, and why
void toast_undo(const std::string& msg, std::function<void()> undo);      // a result that can be taken back
void confirm(const std::string& title, const std::string& body, const std::string& ok, std::function<void()> fn);
void set_page(int page);
std::string hexs(uint64_t v);
bool parse_addr(const char* text, uint64_t* out);             // hex ("7100001230", "0x..."), or "main+0x1234" / a label
uint64_t entry_address(Entry& e);                             // resolves a pointer / AOB-anchored entry (0 when it does not resolve)
std::string label_at(uint64_t addr);                          // "" when none
void add_entry(uint64_t addr, ScanType t, const std::string& desc);
Entry* entry_by_uid(uint64_t uid);
void set_freeze(Entry& e, bool on);
void mark_dirty();
void goto_viewer(uint64_t addr);
void goto_disasm(uint64_t addr);
// Shows a tool: Scanner / Address list / Cheat files are pages of the panel; every other tool shows in the Advanced page (or in its own
// window when it is popped out).
void open_window(const char* title);
Win* find_win(const char* title);
// The tools draw through these, so a tool is the same code inside the Advanced page and in a window of its own.
bool tool_begin(const char* title, bool* open);
void tool_end();
// Called right after an item: puts the pad / keyboard navigation focus on it when the scripted test asked for `name` (as the D-pad
// would leave it there: focused, not activated).
void nav_focus_here(const char* name);
std::string make_nop_script_at(uint64_t addr, const std::string& name, std::string* msg);   // signature maker + NOP AOB script
void take_snapshot();                                         // Snapshots: a new snapshot (the window's range, or the filter's regions)
void compare_snapshots();                                     // Snapshots: snap_a against snap_b (-1: memory now) into diff
bool window_open(const char* title);
// Pad buttons pressed since the last UI frame (hid bits), so a window can give X / Y / L / R their context actions.
bool pad_pressed(uint64_t hid_bit);
bool pad_mode();                                              // the pad was the last device used
const char* const* kind_names();                              // the scan kind list
int kind_count();
ScanType kind_type(int i);
std::string value_text(uint64_t addr, ScanType t);

// ---- text entry with the on-screen keyboard (cheats_ui_osk.cpp) ------------------------------------------------------------------------
enum class Keys : uint8_t { Text, Hex, Number };
// An input field. With the pad (or on Y while it is focused) it opens the on-screen keyboard / hex keypad. Returns true when the text
// changed and was confirmed (Enter, the keypad's OK).
bool field(const char* id, char* buf, size_t n, Keys keys, const char* hint = nullptr, float width = 0.f);
bool field(const char* id, std::string* s, Keys keys, const char* hint = nullptr, float width = 0.f);
// A multi-line text editor (scripts) with the keyboard on the pad.
bool editor(const char* id, std::string* s, ImVec2 size);
void osk_draw();                                              // draws the keyboard when one is open (once per frame, last)
bool osk_open();
void osk_show(std::string* s, Keys keys, const char* title);   // the keyboard for a string (the screenshot test)
// The game's icon (the title folder's icon.png / icon.jpg) as a texture of the current ImGui context, or null when there is none
// (cheats_icon.cpp).
ImTextureData* game_icon();

// ---- pages and windows -------------------------------------------------------------------------------------------------------------
void page_cheats();                                           // Cheats: the game's cheat codes and mods' (cheats_ui_tools.cpp)
void page_find();                                             // Find a value: the scan wizard (cheats_ui_scan.cpp)
void page_table();                                            // My table (cheats_ui_scan.cpp)
void win_pointer();
void win_viewer();
void win_disasm();
void win_memmap();
void win_struct();
void win_snapshots();
void win_access();
void win_breakpoints();
void win_threads();
void win_watch();
void win_labels();
void win_control();
void win_lua();
void win_script_editor();
void win_patches();
void win_settings();
void win_help();

// AOB script entries: runs [ENABLE] / [DISABLE] where an entry's wanted state changed (a click, a hotkey, a table loaded at start).
// cheats_ui_scan.cpp; called by the list window and by the frame callback, so a saved table's scripts come on without the overlay open.
void sync_scripts();

// Cheat files: the address list as Atmosphere cheats appended to this build's file (cheats_ui_tools.cpp). Returns how many.
size_t export_list_to_cheat_file(std::string* msg);
size_t import_cheat_file(const std::string& path, std::string* msg);   // appends an Atmosphere file to the title's and reloads
// The scripted test of the overlay ([cheats] test = <file>, cheats_ui_test.cpp): runs its due steps at each frame boundary.
void test_init();
void test_tick();

// The signature maker for a code address: opens its popup (cheats_ui_mem.cpp).
void open_signature_maker(uint64_t addr);
void draw_signature_maker();

}  // namespace nxrt::cheats::ui
