// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The F1 menu's pins: performance and settings readouts (perf_pins.h).
#include "perf_pins.h"

#include <SDL3/SDL_scancode.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <time.h>
#else
#include <time.h>
#include <unistd.h>
#include <cstdio>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/log.h"
#include "play_types.h"

namespace nxrt::play::perf {

namespace {
constexpr const char* kTag = "pins";

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
std::string bytes_text(uint64_t b) {
    const double gb = static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0);
    return gb >= 1.0 ? fmt(gb >= 10.0 ? "%.0f GB" : "%.1f GB", gb) : fmt("%.0f MB", static_cast<double>(b) / (1024.0 * 1024.0));
}
}  // namespace

HudSettings default_hud() {
    HudSettings h;
    h.key = {SDL_SCANCODE_F7, 0};
    return h;
}

std::vector<pins::Group> configured_default_groups(std::string* err) {
    std::vector<pins::Group> gs;
    const std::string text = config_get_str("hud", "default", "");
    if (!pins::parse_groups(text, &gs, err)) return {};
    return gs;
}

const std::vector<ItemInfo>& catalogue() {
    static const std::vector<ItemInfo> c = {
        {"fps", "Frame rate", "Frames shown per second", false},
        {"frametime", "Frame time", "Milliseconds per frame, with a graph of the last two seconds", false},
        {"low1", "1% low", "The frame rate of the slowest 1% of the last five seconds", false},
        {"gpu", "GPU time", "The GPU's milliseconds per frame", false},
        {"cpu", "Render thread", "The render thread's CPU milliseconds per frame", false},
        {"upscaler", "Upscaler", "The upscaler and the scene size (when one runs)", false},
        {"shaders", "Shaders compiled in play", "Pipelines built after the game started showing frames: 0 after a bake", false},
        {"memory", "Memory", "What the game's process holds in memory", false},
        {"vram", "Video memory", "Video memory in use, and what the system grants", false},
        {"resolution", "Resolution", "The game's picture size and the size it is shown at", false},
        {"set.vsync", "Vsync", "The vsync setting", true},
        {"set.fps_limit", "Frame limit", "The frame limit setting", true},
        {"set.hdr", "HDR", "The HDR output setting", true},
        {"set.latency", "Low latency", "Reflex or Anti-Lag", true},
        {"set.volume", "Volume", "The master volume", true},
    };
    return c;
}

const ItemInfo* item_info(const std::string& id) {
    for (const ItemInfo& i : catalogue())
        if (id == i.id) return &i;
    return nullptr;
}

Sample& sample() {
    static Sample s;
    return s;
}

void push_frame_time(float ms) {
    Sample& s = sample();
    s.frame_times[s.ft_head] = ms;
    s.ft_head = (s.ft_head + 1) % Sample::kGraph;
}

namespace {
struct Readouts {
    std::mutex mu;
    std::string vsync, fps_limit, framegen, hdr, latency, volume;
    bool fresh = false;
};
Readouts& readouts() {
    static Readouts r;
    return r;
}
}  // namespace

void refresh_readouts() {
    Readouts& r = readouts();
    std::lock_guard<std::mutex> lk(r.mu);
    if (!r.fresh) return;
    r.fresh = false;
    Sample& s = sample();
    s.set_vsync = r.vsync;
    s.set_fps_limit = r.fps_limit;
    s.set_framegen = r.framegen;
    s.set_hdr = r.hdr;
    s.set_latency = r.latency;
    s.set_volume = r.volume;
}

uint64_t thread_cpu_ns() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    const uint64_t t = ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
    return t * 100;   // 100 ns units
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

uint64_t process_memory_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pm{};
    pm.cb = sizeof pm;
    return GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm) ? static_cast<uint64_t>(pm.WorkingSetSize) : 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) != KERN_SUCCESS) return 0;
    return info.resident_size;
#else
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    const int got = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    return got == 2 ? resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) : 0;
#endif
}

void fill_settings_readouts(const Settings& st) {
    Readouts& s = readouts();
    std::lock_guard<std::mutex> lk(s.mu);
    s.fresh = true;
    const DisplaySettings& d = st.display;
    s.vsync = d.vsync ? "on" : "off";
    s.fps_limit = d.fps_cap < 0 ? "refresh rate" : d.fps_cap == 0 ? "unlimited" : std::to_string(d.fps_cap) + " fps";
    s.framegen = "off";   // PokoMac has no frame generation
    s.hdr = st.extras.hdr.mode == extras::HdrMode::Off ? "off" : st.extras.hdr.mode == extras::HdrMode::On ? "on"
          : st.extras.hdr.mode == extras::HdrMode::Native ? "native" : "auto";
    s.latency = st.extras.latency.mode == extras::LatencyMode::Off ? "off" : st.extras.latency.mode == extras::LatencyMode::Boost ? "boost" : "on";
    s.volume = std::to_string(static_cast<int>(std::lround(st.audio.volume * 100.0))) + "%";
}

void item_lines(const std::string& id, std::vector<pins::Line>& out) {
    const Sample& s = sample();
    pins::Line l;
    if (id == "fps") {
        l.label = "fps";
        l.value = fmt("%.0f", s.fps);
        l.tone = 1;
    } else if (id == "frametime") {
        l.label = "frame";
        l.value = fmt("%.1f ms", s.frame_ms);
        l.graph = s.frame_times;
        l.graph_n = Sample::kGraph;
        l.graph_head = s.ft_head;
        l.graph_max = static_cast<float>(std::max(33.4, s.max_ms * 1.1));
    } else if (id == "low1") {
        l.label = "1% low";
        l.value = fmt("%.0f fps", s.low1_fps);
        l.tone = s.fps > 0 && s.low1_fps < s.fps * 0.6 ? 2 : 0;
    } else if (id == "gpu") {
        l.label = "GPU";
        l.value = s.gpu_ms >= 0 ? fmt("%.2f ms", s.gpu_ms) : "-";
    } else if (id == "cpu") {
        l.label = "render CPU";
        l.value = s.render_cpu_ms >= 0 ? fmt("%.2f ms", s.render_cpu_ms) : "-";
    } else if (id == "framegen") {
        if (!s.fg_on) return;   // only while it is on
        l.label = s.fg_backend.empty() ? "frame gen" : s.fg_backend;
        l.value = fmt("%.0f", s.fg_shown_fps) + " shown / " + fmt("%.0f", s.fg_real_fps) + " real";
        l.tone = 1;
    } else if (id == "upscaler") {
        if (s.upscaler.empty()) return;
        l.label = "upscaler";
        l.value = s.upscaler + (s.scene_pct < 100 ? " " + std::to_string(s.scene_pct) + "%" : "");
    } else if (id == "shaders") {
        l.label = "shaders in play";
        l.value = s.shaders_in_play >= 0 ? std::to_string(s.shaders_in_play) : "-";
        l.tone = s.shaders_in_play > 0 ? 2 : 3;
    } else if (id == "memory") {
        l.label = "memory";
        l.value = s.mem_bytes ? bytes_text(s.mem_bytes) : "-";
    } else if (id == "vram") {
        l.label = "VRAM";
        l.value = s.vram_used ? bytes_text(s.vram_used) + (s.vram_budget ? " / " + bytes_text(s.vram_budget) : "") : "-";
        l.tone = s.vram_budget && s.vram_used > s.vram_budget * 9 / 10 ? 2 : 0;
    } else if (id == "resolution") {
        l.label = "render";
        if (s.src_w > 0) {
            char b[64];
            if (s.src_w == s.out_w && s.src_h == s.out_h) std::snprintf(b, sizeof b, "%dx%d", s.src_w, s.src_h);
            else std::snprintf(b, sizeof b, "%dx%d > %dx%d", s.src_w, s.src_h, s.out_w, s.out_h);
            l.value = b;
        } else {
            l.value = "-";
        }
    } else if (id.compare(0, 4, "set.") == 0) {
        const ItemInfo* in = item_info(id);
        if (!in) return;
        l.label = in->name;
        l.value = id == "set.vsync" ? s.set_vsync : id == "set.fps_limit" ? s.set_fps_limit : id == "set.framegen" ? s.set_framegen
                : id == "set.hdr" ? s.set_hdr : id == "set.latency" ? s.set_latency : s.set_volume;
        for (char& c : l.label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
        return;
    }
    out.push_back(std::move(l));
}

// ---- the board in force -----------------------------------------------------------------------------------------------------------------

namespace {
struct Force {
    std::mutex mu;
    HudSettings hud = default_hud();
    std::vector<KeyToggle> keys;       // [0] hide-all, then one per group (same order)
    static constexpr int kSlots = 8;
    std::vector<PadToggle> pads;       // kSlots per toggle: one state per pad
    std::function<void(const std::string&)> on_toggle;
    pins::Board drawn;                 // the presenting thread's copy (draw_board)
    uint64_t gen = 0, drawn_gen = ~0ull;
};
Force& force() {
    static Force f;
    return f;
}
}  // namespace

void set_board(const HudSettings& h) {
    Force& f = force();
    std::lock_guard<std::mutex> lk(f.mu);
    const bool same_keys = f.keys.size() == h.groups.size() + 1;
    std::vector<KeyToggle> keys(h.groups.size() + 1);
    std::vector<PadToggle> pads((h.groups.size() + 1) * Force::kSlots);
    keys[0].chord = h.key;
    for (size_t i = 0; i < h.groups.size(); ++i) keys[i + 1].chord = h.groups[i].key;
    for (size_t i = 0; i < h.groups.size() + 1; ++i)
        for (int sl = 0; sl < Force::kSlots; ++sl) pads[i * Force::kSlots + sl].chord = i == 0 ? h.pad : h.groups[i - 1].pad;
    if (f.pads.size() == pads.size())   // a chord being held keeps its state across a settings change
        for (size_t i = 0; i < pads.size(); ++i)
            if (pads[i].chord == f.pads[i].chord) pads[i] = f.pads[i];
    // a key held across the change keeps its state (its release is still the toggle's)
    if (same_keys)
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i].chord == f.keys[i].chord && f.keys[i].held()) keys[i] = f.keys[i];
    f.keys = std::move(keys);
    f.pads = std::move(pads);
    f.hud = h;
    ++f.gen;
}

HudSettings board() {
    Force& f = force();
    std::lock_guard<std::mutex> lk(f.mu);
    return f.hud;
}

bool any_shown() {
    Force& f = force();
    std::lock_guard<std::mutex> lk(f.mu);
    if (!f.hud.all_shown) return false;
    for (const pins::Group& g : f.hud.groups)
        if (g.shown && !g.items.empty()) return true;
    return false;
}

void draw_board() {
    Force& f = force();
    {
        std::lock_guard<std::mutex> lk(f.mu);
        if (f.gen != f.drawn_gen) {
            f.drawn.groups = f.hud.groups;
            f.drawn.all_shown = f.hud.all_shown;
            f.drawn_gen = f.gen;
        }
    }
    pins::draw(f.drawn, item_lines, false, "menu");
}

void set_toggle_handler(std::function<void(const std::string&)> fn) {
    Force& f = force();
    std::lock_guard<std::mutex> lk(f.mu);
    f.on_toggle = std::move(fn);
}

bool flip(HudSettings& h, const std::string& group) {
    if (group.empty()) {
        h.all_shown = !h.all_shown;
        return true;
    }
    for (pins::Group& g : h.groups)
        if (g.id == group) {
            g.shown = !g.shown;
            return true;
        }
    return false;
}

bool key_event(int32_t sc, uint16_t mods, bool down, bool repeat) {
    Force& f = force();
    std::string fired;
    bool ours = false;
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lk(f.mu);
        for (size_t i = 0; i < f.keys.size(); ++i) {
            const KeyToggle::Hit h = f.keys[i].event(sc, mods, down, repeat);
            if (h == KeyToggle::NotOurs) continue;
            ours = true;
            if (h == KeyToggle::Fired) {
                const std::string id = i == 0 ? std::string() : f.hud.groups[i - 1].id;
                flip(f.hud, id);   // at once on screen; the handler saves it
                ++f.gen;
                fired = i == 0 ? std::string("*") : id;
                NXLOG_I(kTag, "%s %s", i == 0 ? "every pin" : ("pin " + id).c_str(),
                        (i == 0 ? f.hud.all_shown : f.hud.groups[i - 1].shown) ? "shown" : "hidden");
            }
            break;
        }
        cb = f.on_toggle;
    }
    if (!fired.empty() && cb) cb(fired == "*" ? std::string() : fired);
    return ours;
}

uint32_t pad_step(int slot, uint32_t held, uint32_t free_bits) {
    Force& f = force();
    uint32_t mask = 0;
    std::vector<std::string> fired;
    std::function<void(const std::string&)> cb;
    if (slot < 0 || slot >= Force::kSlots) return 0;
    {
        std::lock_guard<std::mutex> lk(f.mu);
        for (size_t i = 0; i * Force::kSlots < f.pads.size(); ++i) {
            PadToggle& t = f.pads[i * Force::kSlots + static_cast<size_t>(slot)];
            if (!t.chord) continue;
            t.free = free_bits;
            if (!pin_pad_problem(t.chord, free_bits).empty()) continue;   // a chord the game could see is never used
            if (t.step(held, &mask)) {
                const std::string id = i == 0 ? std::string() : f.hud.groups[i - 1].id;
                flip(f.hud, id);
                ++f.gen;
                fired.push_back(i == 0 ? std::string("*") : id);
            }
        }
        cb = f.on_toggle;
    }
    for (const std::string& id : fired)
        if (cb) cb(id == "*" ? std::string() : id);
    return mask;
}

}  // namespace nxrt::play::perf
