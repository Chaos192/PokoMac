// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The controller applet waits for players (services_api.h ctrlui; nxnative services.md "The controller applet waits for players", Mario
// Kart World): a game that asks for more players than are connected gets the prompt over its picture until enough controllers are
// connected (each connected controller is a player of its own in play mode, pad_players.h) or the player closes it (B / Escape:
// canceled), as a Switch's applet stays open. With no prompt (no window, scripted input, a test) the applet answers at once as before.
// Our own implementation from nxnative's finding; no code of theirs.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "nxrt/log.h"
#include "nxrt/services_api.h"

namespace nxrt::services::ctrlui {

namespace {

struct State {
    std::mutex mu;
    std::condition_variable cv;
    uint64_t next_id = 1;
    uint64_t open_id = 0;          // the prompt up now (0: none)
    Prompt open;
    bool canceled = false;         // the open prompt was closed by the player
};

State& st() {
    static State s;
    return s;
}

std::atomic<bool>& enabled() {
    static std::atomic<bool> on{false};
    return on;
}

std::atomic<void (*)()>& wait_hook() {
    static std::atomic<void (*)()> fn{nullptr};
    return fn;
}

}  // namespace

void enable_prompt(bool on) { enabled().store(on); }
void set_wait_hook(void (*fn)()) { wait_hook().store(fn); }
bool prompt_enabled() { return enabled().load(); }

bool enough(int players, int lo) { return players >= (lo < 1 ? 1 : lo); }

bool wait_for_players(int lo, int hi, const std::function<int()>& players, const std::function<bool()>& abandoned) {
    int n = players();
    if (enough(n, lo)) return true;
    if (!prompt_enabled()) return false;
    State& s = st();
    std::unique_lock<std::mutex> lk(s.mu);
    const uint64_t id = s.next_id++;
    s.open_id = id;
    s.open = Prompt{lo, hi, n};
    s.canceled = false;
    NXLOG_I("am", "controller prompt open: %d to %d players wanted, %d connected: waiting for controllers", lo, hi, n);
    bool ok = false;
    for (;;) {
        if (s.canceled) break;
        lk.unlock();
        if (void (*hook)() = wait_hook().load()) hook();
        n = players();
        const bool gone = abandoned && abandoned();
        lk.lock();
        s.open.players = n;
        if (enough(n, lo)) {
            ok = true;
            break;
        }
        if (gone || s.canceled) break;
        s.cv.wait_for(lk, std::chrono::milliseconds(wait_hook().load() ? 20 : 100));
    }
    s.open_id = 0;
    NXLOG_I("am", "controller prompt closed: %d players (%s)", n, ok ? "done" : "canceled");
    return ok;
}

uint64_t pending(Prompt* p) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.open_id && p) *p = s.open;
    return s.open_id;
}

void cancel(uint64_t id) {
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!id || id != s.open_id) return;
        s.canceled = true;
    }
    s.cv.notify_all();
}

}  // namespace nxrt::services::ctrlui
