// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Game control: pause, frame advance, speed (control.h).
#include "control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/cheats.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::cheats::ctl {
namespace {
constexpr const char* kTag = "cheats";

struct Callback { uint32_t id; FrameFn fn; void* user; };

struct State {
    std::atomic<bool> want_pause{false};
    std::atomic<int> advance{0};             // frames to run before holding again
    std::atomic<bool> holding{false};        // the frame hook is holding the presenting thread
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> held_threads{0};
    std::atomic<double> speed{1.0};
    void (*service)() = nullptr;
    std::mutex cb_mu;
    std::vector<Callback> cbs;
    uint32_t next_cb = 1;
    // threads this pause parked (not the ones the game paused itself)
    std::vector<std::shared_ptr<kernel::KThread>> parked;
    std::mutex held_mu;
    std::vector<std::function<void()>> held_jobs;
    std::atomic<std::thread::id> frame_thread{};
    std::atomic<int64_t> last_hook_ns{0};    // the last frame boundary (steady clock)
    std::mutex service_mu;                   // one thread at a time runs the pause service and the frame callbacks
};
thread_local bool tl_serving_hold = false;   // this thread runs them for a breakpoint that stopped the frames
int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
State& S() {
    static State s;
    return s;
}

// Asks every other started guest thread to park (as svcSetThreadActivity(Paused) does) and waits briefly for each; one that is deep in
// lifted code parks at its next call out.
void park_others() {
    State& s = S();
    kernel::KThread* self = kernel::current_thread();
    s.parked.clear();
    for (auto& t : kernel::threads()) {
        if (!t || t.get() == self || !t->started || t->exited.load()) continue;
        if (t->pause_req.exchange(true)) continue;       // the game paused it itself: not ours to resume
        s.parked.push_back(t);
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    for (auto& t : s.parked) {
        for (;;) {
            if (t->exited.load() || t->in_runtime.load() > 0) break;
            std::unique_lock<std::mutex> lk(t->pause_mu);
            if (t->parked) break;
            if (std::chrono::steady_clock::now() > until) break;
            t->pause_cv.wait_for(lk, std::chrono::milliseconds(1));
        }
    }
    s.held_threads = s.parked.size();
}

void resume_others() {
    State& s = S();
    for (auto& t : s.parked) {
        {
            std::lock_guard<std::mutex> lk(t->pause_mu);
            t->pause_req.store(false);
        }
        t->pause_cv.notify_all();
    }
    s.parked.clear();
    s.held_threads = 0;
}

void run_callbacks() {
    State& s = S();
    std::vector<Callback> cbs;
    {
        std::lock_guard<std::mutex> lk(s.cb_mu);
        cbs = s.cbs;
    }
    for (const Callback& c : cbs) c.fn(c.user);
}
}  // namespace

void set_service(void (*fn)()) { S().service = fn; }

void frame_hook() {
    State& s = S();
    s.frames.fetch_add(1, std::memory_order_relaxed);
    s.frame_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    s.last_hook_ns.store(steady_ns(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(s.service_mu);
        run_callbacks();
    }
    // work that needs the game held (code swaps): park the others, run it, let them go
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard<std::mutex> lk(s.held_mu);
        jobs.swap(s.held_jobs);
    }
    if (!jobs.empty() && kernel::current_thread()) {
        park_others();
        for (auto& j : jobs) j();
        resume_others();
    } else {
        for (auto& j : jobs) j();      // no guest thread to hold (a test): run it as is
    }
    if (!s.want_pause.load(std::memory_order_relaxed)) return;
    if (s.advance.load() > 0) {                     // frame advance: let this frame run
        s.advance.fetch_sub(1);
        return;
    }
    // hold the presenting thread here; the others park
    if (kernel::current_thread() == nullptr) return;   // not a guest thread (a test, a host-side present): nothing to hold
    s.holding = true;
    park_others();
    NXLOG_I(kTag, "game paused at frame %llu (%llu threads parked)", static_cast<unsigned long long>(s.frames.load()),
            static_cast<unsigned long long>(s.held_threads.load()));
    while (s.want_pause.load() && s.advance.load() == 0) {
        {
            std::lock_guard<std::mutex> sl(s.service_mu);
            s.last_hook_ns.store(steady_ns(), std::memory_order_relaxed);
            if (s.service) s.service();
            run_callbacks();                        // hotkeys and scripts still answer (held() is true: no frame passed)
            std::vector<std::function<void()>> now; // code swaps asked for meanwhile: the others are parked already
            {
                std::lock_guard<std::mutex> lk(s.held_mu);
                now.swap(s.held_jobs);
            }
            for (auto& j : now) j();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    resume_others();
    s.holding = false;
    if (s.advance.load() > 0) s.advance.fetch_sub(1);   // the frame about to run is the first advanced one
    NXLOG_I(kTag, "game %s", s.want_pause.load() ? "advances one frame" : "resumed");
}

void pause(bool on) {
    State& s = S();
    if (!on) s.advance = 0;
    s.want_pause = on;
    NXLOG_I(kTag, "pause %s", on ? "requested" : "released");
}
bool paused() { return S().want_pause.load(); }
bool held() { return S().holding.load() || tl_serving_hold; }
void frame_advance(int n) {
    State& s = S();
    if (n < 1) n = 1;
    if (!s.want_pause.load()) { pause(true); return; }
    s.advance.store(n);
}
uint64_t frames() { return S().frames.load(); }
uint64_t paused_threads() { return S().held_threads.load(); }

void set_speed(double v) {
    v = std::clamp(v, 0.05, 16.0);
    S().speed = v;
    kernel::set_time_scale(v);
    NXLOG_I(kTag, "game speed x%.2f", v);
}
double speed() { return S().speed.load(); }

void run_held(std::function<void()> fn) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.held_mu);
    s.held_jobs.push_back(std::move(fn));
}
bool on_frame_thread() { return S().frame_thread.load(std::memory_order_relaxed) == std::this_thread::get_id(); }
void service_once() {
    State& s = S();
    if (on_frame_thread()) {
        if (s.service) s.service();
        return;
    }
    // A breakpoint holds another thread. When the presenting thread waits for it, no frame passes and nothing would draw the overlay or
    // run the callbacks (its Continue button, hotkeys, the scripted test): measured on Wonder, a thread held in the title's counter
    // function stopped the frames ("no frame presented in the last 10.0 s"). Then the held thread serves them itself, one thread at a time.
    if (steady_ns() - s.last_hook_ns.load(std::memory_order_relaxed) < 250'000'000) return;
    std::unique_lock<std::mutex> lk(s.service_mu, std::try_to_lock);
    if (!lk.owns_lock()) return;
    tl_serving_hold = true;
    if (s.service) s.service();
    run_callbacks();
    tl_serving_hold = false;
}

uint32_t add_frame_callback(FrameFn fn, void* user) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.cb_mu);
    const uint32_t id = s.next_cb++;
    s.cbs.push_back({id, fn, user});
    return id;
}
void remove_frame_callback(uint32_t id) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.cb_mu);
    s.cbs.erase(std::remove_if(s.cbs.begin(), s.cbs.end(), [id](const Callback& c) { return c.id == id; }), s.cbs.end());
}

std::string status_text() {
    State& s = S();
    char b[96];
    std::string out;
    if (s.holding.load()) {
        std::snprintf(b, sizeof b, "paused (%llu threads)", static_cast<unsigned long long>(s.held_threads.load()));
        out = b;
    } else if (s.want_pause.load()) {
        out = "pausing...";
    }
    if (s.speed.load() != 1.0) {
        std::snprintf(b, sizeof b, "%sx%.2g", out.empty() ? "" : "  ", s.speed.load());
        out += b;
    }
    return out;
}

}  // namespace nxrt::cheats::ctl

namespace nxrt::cheats {
void frame_hook() { ctl::frame_hook(); }
void set_pause_service(void (*fn)()) { ctl::set_service(fn); }
}  // namespace nxrt::cheats
