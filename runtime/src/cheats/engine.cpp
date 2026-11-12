// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheat engine host (engine.h).
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING 1
#include "engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "accesswatch.h"
#include "gmem.h"
#include "nxrt/cheats.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"

namespace nxrt::cheats {
namespace {
constexpr const char* kTag = "cheats";
namespace fs = std::filesystem;

std::string exe_dir() {
    char b[4096];
    const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
    std::string s(b, n < sizeof b ? n : 0);
    const size_t k = s.find_last_of("/\\");
    return k == std::string::npos ? std::string(".") : s.substr(0, k);
}

// the title's folder: PokoMac's game program sits in its game folder, which is also the working directory (AnySwitch resolves this through
// nxrt::game_dir / title_path, which PokoMac does not need)
std::string title_dir() { return exe_dir(); }
std::string title_path(const std::string& rel) { return rel; }

std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A VmHost over guest memory and the live controller buttons.
class LiveHost : public VmHost {
public:
    std::function<void(bool)>* pause = nullptr;
    bool read(uint64_t addr, void* dst, size_t n) override { return gmem::read(addr, dst, n); }
    bool write(uint64_t addr, const void* src, size_t n) override { return gmem::write(addr, src, n); }
    uint64_t base(unsigned t) override { return gmem::region_base(t); }
    uint64_t keys_held() override { return hid::sampled_buttons(); }
    void debug_log(uint32_t id, uint64_t value) override {
        NXLOG_I(kTag, "cheat log %08x: %016llx", id, static_cast<unsigned long long>(value));
    }
    void pause_game(bool p) override {
        if (pause && *pause) (*pause)(p);
        else NXLOG_I(kTag, "a cheat asked to %s the game, but no pause handler is installed", p ? "pause" : "continue");
    }
};

}  // namespace

Engine& Engine::instance() {
    static Engine e;
    return e;
}

std::string Engine::program_id_hex() const {
    const uint64_t pid = game::info().program_id;
    if (!pid) return std::string();
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(pid));
    return b;
}

std::string Engine::build_id_hex() const {
    const std::string bid = gmem::module_build_id("main");
    return bid.size() >= 16 ? bid.substr(0, 16) : std::string();
}

void Engine::set_folder(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mu_);
    folder_ = dir;
}

std::string Engine::folder() const {
    std::lock_guard<std::mutex> lk(mu_);
    return folder_unlocked();
}

std::string Engine::folder_unlocked() const {   // mu_ held (or no concurrency)
    if (!folder_.empty()) return folder_;
    const std::string cfg = config_get_str("cheats", "dir", "");
    if (!cfg.empty()) return cfg;
    return title_dir() + "/cheats";
}

// [cheats] mod_files (docs/MODS.md): a list, one path per line, of the cheat files of the title's enabled mods (the mod manager mods.py
// writes it: <mod>/cheats/<build id>.txt for this build). Read only, after the title's own file: each file's cheats go into the free entries
// in order (a mod's master code only when the title's file has none); toggles.txt beside the title's file still decides which are on, by
// name. Returns the number of cheats added.
size_t Engine::add_mod_cheats(CheatFile& f) const {
    std::string list = config_get_str("cheats", "mod_files", "");
    if (list.empty()) return 0;
    std::error_code ec;
    if (fs::u8path(list).is_relative() && !fs::exists(fs::u8path(title_path(list)), ec)) list = title_dir() + "/" + list;
    else list = title_path(list);
    const std::string text = read_text(list);
    size_t added = 0, at = 1, line_start = 0;
    while (line_start < text.size()) {
        size_t eol = text.find('\n', line_start);
        if (eol == std::string::npos) eol = text.size();
        std::string p = text.substr(line_start, eol - line_start);
        line_start = eol + 1;
        while (!p.empty() && (p.back() == '\r' || p.back() == ' ')) p.pop_back();
        if (p.empty() || p[0] == '#') continue;
        CheatFile m = ParseFile(read_text(p));
        if (!m.ok) {
            NXLOG_W(kTag, "a mod's cheat file %s was refused: %s", p.c_str(), m.error.c_str());
            continue;
        }
        size_t n = 0;
        for (size_t i = 0; i < kMaxEntries; ++i) {
            CheatEntry& e = m.entries[i];
            if (!e.used) continue;
            if (i == 0) {
                if (!f.entries[0].used) f.entries[0] = std::move(e), n++;
                else NXLOG_W(kTag, "the master code of the mod cheat file %s is left out: the title's file has its own", p.c_str());
                continue;
            }
            while (at < kMaxEntries && f.entries[at].used) at++;
            if (at >= kMaxEntries) {
                NXLOG_W(kTag, "the cheat list is full (%zu entries): the rest of %s is left out", kMaxEntries, p.c_str());
                break;
            }
            f.entries[at++] = std::move(e);
            n++;
        }
        NXLOG_I(kTag, "%zu cheats from the mod cheat file %s", n, p.c_str());
        added += n;
    }
    return added;
}

std::string Engine::file_path_for(const std::string& pid, const std::string& bid) const {
    const std::string dir = folder_unlocked();
    // the console's tree first, then a flat folder (both are used by cheat sites)
    const std::string tree = dir + "/" + pid + "/cheats/" + bid + ".txt";
    std::error_code ec;
    if (fs::exists(fs::u8path(tree), ec)) return tree;
    const std::string flat = dir + "/" + bid + ".txt";
    if (fs::exists(fs::u8path(flat), ec)) return flat;
    return tree;   // the place we would read from (for the message), even if absent
}

Engine::LoadResult Engine::reload() {
    LoadResult res;
    const std::string pid = program_id_hex();
    const std::string bid = build_id_hex();
    std::lock_guard<std::mutex> lk(mu_);
    stats_.program_id = pid;
    stats_.build_id = bid;
    if (pid.empty() || bid.empty()) {
        res.error = "no title loaded (program id or build id is unknown)";
        stats_.file_loaded = false;
        return res;
    }
    const std::string path = file_path_for(pid, bid);
    stats_.file_path = path;
    std::error_code ec;
    const bool own = fs::exists(fs::u8path(path), ec);
    CheatFile f;
    f.ok = true;
    if (own) {
        f = ParseFile(read_text(path));
        if (!f.ok) {
            res.found = true;
            res.ok = false;
            res.error = f.error;
            stats_.file_loaded = false;
            NXLOG_W(kTag, "cheat file %s was refused: %s", path.c_str(), f.error.c_str());
            return res;
        }
    }
    const size_t from_mods = add_mod_cheats(f);
    if (!own && from_mods == 0) {
        stats_.file_loaded = false;
        file_ = CheatFile();
        program_dirty_ = true;
        res.found = false;
        res.error = "no cheat file for this build";
        return res;
    }
    res.found = true;
    // apply toggles.txt beside the file (per program id)
    const fs::path tog_path = fs::u8path(path).parent_path() / "toggles.txt";
    const std::string tog_str = tog_path.string();
    if (fs::exists(tog_path, ec)) {
        std::string terr;
        if (!ParseToggles(read_text(tog_str), f, &terr))
            NXLOG_W(kTag, "toggles.txt was refused (%s): the console would attach no cheats; using the file's defaults", terr.c_str());
    }
    file_ = std::move(f);
    program_dirty_ = true;
    stats_.file_loaded = true;
    stats_.cheat_count = file_.count();
    res.ok = true;
    res.cheats = file_.count();
    NXLOG_I(kTag, "loaded %zu cheats from %s (program %s, build %s)", file_.count(), path.c_str(), pid.c_str(), bid.c_str());
    return res;
}

std::vector<Engine::CheatRow> Engine::cheats() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CheatRow> out;
    for (size_t i = 0; i < kMaxEntries; ++i) {
        const CheatEntry& e = file_.entries[i];
        if (!e.used) continue;
        out.push_back({static_cast<int>(i), e.name, i == 0, e.enabled, e.words.size()});
    }
    return out;
}

void Engine::set_cheat_enabled(int entry, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (entry < 0 || entry >= static_cast<int>(kMaxEntries)) return;
    if (entry == 0) return;   // the master cannot be toggled
    CheatEntry& e = file_.entries[entry];
    if (!e.used) return;
    e.enabled = on;
    program_dirty_ = true;
    // persist toggles.txt beside the file
    const std::string path = stats_.file_path;
    if (!path.empty()) {
        const fs::path tog = fs::u8path(path).parent_path() / "toggles.txt";
        std::error_code ec;
        fs::create_directories(tog.parent_path(), ec);   // only mods' cheats were loaded: the title's own folder may not be there yet
        std::ofstream(tog, std::ios::binary) << WriteToggles(file_);
    }
}

uint64_t Engine::freeze_add(uint64_t addr, unsigned width, uint64_t value, FreezeMode mode, bool is_float) {
    std::lock_guard<std::mutex> lk(mu_);
    FrozenAddr f;
    f.id = next_id_++;
    f.addr = addr;
    f.width = width;
    f.value = value;
    f.mode = mode;
    f.is_float = is_float;
    frozen_.push_back(f);
    return f.id;
}
void Engine::freeze_remove(uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    frozen_.erase(std::remove_if(frozen_.begin(), frozen_.end(), [id](const FrozenAddr& f) { return f.id == id; }), frozen_.end());
}
void Engine::freeze_set_value(uint64_t id, uint64_t value) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& f : frozen_) if (f.id == id) f.value = value;
}
void Engine::freeze_set_mode(uint64_t id, FreezeMode mode) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& f : frozen_) if (f.id == id) f.mode = mode;
}
void Engine::freeze_set_enabled(uint64_t id, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& f : frozen_) if (f.id == id) f.enabled = on;
}
std::vector<FrozenAddr> Engine::frozen() const {
    std::lock_guard<std::mutex> lk(mu_);
    return frozen_;
}
void Engine::freeze_clear() {
    std::lock_guard<std::mutex> lk(mu_);
    frozen_.clear();
}

void Engine::set_pause_handler(std::function<void(bool)> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    pause_fn_ = std::move(fn);
}

namespace {
int64_t signed_of(uint64_t bits, unsigned width) {
    switch (width) {
        case 1: return static_cast<int8_t>(bits & 0xFF);
        case 2: return static_cast<int16_t>(bits & 0xFFFF);
        case 4: return static_cast<int32_t>(bits & 0xFFFFFFFF);
        default: return static_cast<int64_t>(bits);
    }
}
double float_of(uint64_t bits, unsigned width) {
    if (width == 4) { float f; std::memcpy(&f, &bits, 4); return f; }
    double d; std::memcpy(&d, &bits, 8); return d;
}
}  // namespace

void Engine::write_frozen() {
    std::vector<FrozenAddr> snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snapshot = frozen_;
    }
    for (auto& f : snapshot) {
        if (!f.enabled) continue;
        uint64_t cur = 0;
        const bool have = gmem::read(f.addr, &cur, f.width);
        uint64_t want = f.value;
        if (f.mode != FreezeMode::Normal && have) {
            bool keep_cur;
            if (f.is_float) {
                const double c = float_of(cur, f.width), v = float_of(f.value, f.width);
                keep_cur = f.mode == FreezeMode::AllowIncrease ? c > v : c < v;
            } else {
                const int64_t c = signed_of(cur, f.width), v = signed_of(f.value, f.width);
                keep_cur = f.mode == FreezeMode::AllowIncrease ? c > v : c < v;
            }
            if (keep_cur) {
                want = cur;
                std::lock_guard<std::mutex> lk(mu_);     // raise/lower the stored value toward the game's
                for (auto& g : frozen_) if (g.id == f.id) g.value = cur;
            }
        }
        if (gmem::write(f.addr, &want, f.width)) stats_.freeze_writes++;
    }
}

PassReport Engine::step_once() {
    if (!host_) {
        auto lh = std::make_unique<LiveHost>();
        lh->pause = &pause_fn_;
        host_ = std::move(lh);
    }
    PassReport rep;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (program_dirty_) {
            if (!vm_.load(file_))
                NXLOG_W(kTag, "the enabled cheats exceed 0x400 words: a console runs none until some are turned off");
            program_dirty_ = false;
        }
    }
    rep = vm_.run(*host_);
    write_frozen();
    aw::rearm();   // re-arm "find what writes/reads" pages unprotected by the last access
    {
        std::lock_guard<std::mutex> lk(mu_);
        stats_.passes++;
        stats_.writes += rep.writes;
        stats_.failed_reads += rep.failed_reads;
        stats_.failed_writes += rep.failed_writes;
        stats_.last_stop = rep.stopped ? rep.why : std::string();
        size_t en = 0;
        for (size_t i = 0; i < kMaxEntries; ++i) if (file_.entries[i].used && file_.entries[i].enabled) ++en;
        stats_.enabled_cheats = en;
    }
    return rep;
}

void Engine::thread_main() {
    using namespace std::chrono;
    // dmnt runs about 12 passes a second
    constexpr auto kDelay = nanoseconds(1000000000LL / 12);
    while (!stop_.load()) {
        const auto t0 = steady_clock::now();
        if (stats_.file_loaded || !frozen_.empty() || aw::watching()) step_once();
        std::this_thread::sleep_until(t0 + kDelay);
    }
}

void Engine::enable(bool on) {
    if (on == enabled_.load()) return;
    if (on) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto lh = std::make_unique<LiveHost>();
            lh->pause = &pause_fn_;
            host_ = std::move(lh);
        }
        stop_.store(false);
        enabled_.store(true);
        thread_ = std::make_unique<std::thread>([this] { thread_main(); });
        NXLOG_I(kTag, "cheat engine on (1/12 s)");
    } else {
        stop_.store(true);
        if (thread_ && thread_->joinable()) thread_->join();
        thread_.reset();
        enabled_.store(false);
        NXLOG_I(kTag, "cheat engine off");
    }
}

EngineStats Engine::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

// ---- public entry points (nxrt/cheats.h) ----
void engine_enable(bool on) { Engine::instance().enable(on); }
bool engine_enabled() { return Engine::instance().enabled(); }
void engine_reload() { Engine::instance().reload(); }
void engine_set_folder(const char* dir) { if (dir) Engine::instance().set_folder(dir); }
void engine_start_if_configured() {
    if (!config_get_bool("cheats", "enable", false)) return;
    if (const std::string d = config_get_str("cheats", "dir", ""); !d.empty()) Engine::instance().set_folder(d);
    const Engine::LoadResult r = Engine::instance().reload();
    Engine::instance().enable(true);
    NXLOG_I(kTag, "cheats enabled at start: %s", r.found ? (r.ok ? "cheat file loaded" : r.error.c_str()) : "no cheat file for this build (scanner and table still work)");
}

}  // namespace nxrt::cheats
