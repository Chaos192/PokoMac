// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheat engine host: loads the running title's Atmosphère cheat file, runs the cheat VM every 1/12 s on its own thread (as dmnt does),
// and writes frozen addresses after every pass. It also holds the overlay's cheat table (address list) entries, whose freezes it drives.
// Off by default: nothing runs until enable(true) (the overlay or [cheats] enable = true).
//
// The file is <cheats dir>/<program id>/cheats/<build id>.txt (the console's layout) or a flat <cheats dir>/<build id>.txt. program id is
// game::info().program_id as 16 lowercase hex digits; build id is the first 8 bytes of main's module id as 16 lowercase hex digits. Toggles
// persist in toggles.txt beside the file, per program id, as on the console.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

#include "cheat_vm.h"

namespace nxrt::cheats {

enum class FreezeMode : uint8_t { Normal, AllowIncrease, AllowDecrease };   // never-change, never-decrease, never-increase

// A frozen address written after each VM pass. Width 1/2/4/8. The value is kept up to date for never-decrease / never-increase.
struct FrozenAddr {
    uint64_t id = 0;                   // stable handle for the table
    uint64_t addr = 0;
    unsigned width = 4;
    uint64_t value = 0;                // the value to hold (little-endian in the low bytes)
    FreezeMode mode = FreezeMode::Normal;
    bool is_float = false;             // compare as float/double for never-decrease / never-increase
    bool enabled = true;
};

struct EngineStats {
    uint64_t passes = 0;
    uint64_t writes = 0;
    uint64_t failed_reads = 0, failed_writes = 0;
    uint64_t freeze_writes = 0;
    bool file_loaded = false;
    std::string file_path;
    std::string program_id;
    std::string build_id;
    std::string last_stop;             // why the last pass stopped early, if it did
    size_t cheat_count = 0;
    size_t enabled_cheats = 0;
};

class Engine {
public:
    static Engine& instance();

    // Turns the 1/12 s thread on or off. Off by default. Idempotent.
    void enable(bool on);
    bool enabled() const { return enabled_.load(); }

    // The cheats folder (default: <exe dir>/cheats, or [cheats] dir). Call before the first enable to change it.
    void set_folder(const std::string& dir);
    std::string folder() const;

    // (Re)loads the cheat file for the running title. Returns the load result; stats().file_loaded says whether a file was found. Safe any time.
    struct LoadResult { bool found = false; bool ok = false; std::string error; size_t cheats = 0; };
    LoadResult reload();

    // The loaded cheats, for the overlay list.
    struct CheatRow { int entry; std::string name; bool master; bool enabled; size_t words; };
    std::vector<CheatRow> cheats() const;
    void set_cheat_enabled(int entry, bool on);     // persists to toggles.txt
    size_t master_entry() const { return 0; }

    // Frozen addresses (the address list's locks). add returns the entry id. Thread safe.
    uint64_t freeze_add(uint64_t addr, unsigned width, uint64_t value, FreezeMode mode, bool is_float);
    void freeze_remove(uint64_t id);
    void freeze_set_value(uint64_t id, uint64_t value);
    void freeze_set_mode(uint64_t id, FreezeMode mode);
    void freeze_set_enabled(uint64_t id, bool on);
    std::vector<FrozenAddr> frozen() const;
    void freeze_clear();

    // FF0/FF1 ask the game to pause/continue. The play loop installs a handler; without one it is a logged no-op.
    void set_pause_handler(std::function<void(bool)> fn);

    EngineStats stats() const;

    // Runs one VM pass + frozen writes synchronously (tests, and a single step from the overlay). Returns the pass report.
    PassReport step_once();

    // The program id / build id strings the file name uses (16 lowercase hex). Empty when no title is loaded.
    std::string program_id_hex() const;
    std::string build_id_hex() const;

private:
    Engine() = default;
    void thread_main();
    void write_frozen();
    std::string folder_unlocked() const;      // mu_ held
    size_t add_mod_cheats(CheatFile& f) const;   // [cheats] mod_files: the enabled mods' cheat files (docs/MODS.md)
    std::string file_path_for(const std::string& pid, const std::string& bid) const;

    mutable std::mutex mu_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> stop_{false};
    std::unique_ptr<std::thread> thread_;
    std::string folder_;
    CheatFile file_;
    Vm vm_;
    bool program_dirty_ = true;        // reload the VM program (a toggle changed)
    std::vector<FrozenAddr> frozen_;
    uint64_t next_id_ = 1;
    std::function<void(bool)> pause_fn_;
    mutable EngineStats stats_;
    std::unique_ptr<VmHost> host_;
};

}  // namespace nxrt::cheats
