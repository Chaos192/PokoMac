// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode binding engine, see play_engine.h for the rules.
#include "play_engine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nxrt::play {

namespace {
constexpr uint64_t kMs = 1000000ull;

InputId key_id(int sc) { return InputId{InKind::Key, sc}; }
InputId mouse_id(int b) { return InputId{InKind::Mouse, b}; }
InputId wheel_id(int dir) { return InputId{InKind::Wheel, dir > 0 ? 1 : -1}; }

void clamp_stick(float& x, float& y) {
    x = std::max(-1.f, std::min(1.f, x));
    y = std::max(-1.f, std::min(1.f, y));
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.f) { x /= len; y /= len; }
}
}  // namespace

Engine::Engine() : Engine(default_settings()) {}

Engine::Engine(const Settings& s) : st_(s) {
    rt_.assign(st_.bindings.size(), BindRt{});
    rebuild();
}

bool Engine::is_select(Act a) {
    return (a >= Act::SetA1 && a <= Act::SetB4) || a == Act::SetANext || a == Act::SetAPrev;
}

// The bindings that apply to one input in context c: the context's own, else those of the `all` layer. In the menu contexts the left
// click is the menu pointer's click (when menu_cursor is on) and therefore drives no binding.
std::vector<int> Engine::effective(Ctx c, const InputId& id) const {
    if (st_.mouse.menu_cursor &&(c == Ctx::Menu || c == Ctx::Map) && id.kind == InKind::Mouse && id.code == 1) return {};
    std::vector<int> own, all;
    for (size_t i = 0; i < st_.bindings.size(); ++i) {
        const Binding& b = st_.bindings[i];
        if (!(b.in == id)) continue;
        if (b.ctx == c && c != Ctx::All) own.push_back(static_cast<int>(i));
        else if (b.ctx == Ctx::All) all.push_back(static_cast<int>(i));
    }
    std::vector<int>& r = own.empty() ? all : own;
    r.erase(std::remove_if(r.begin(), r.end(), [&](int i) { return st_.bindings[i].act == Act::None; }), r.end());
    return r;
}

void Engine::rebuild() {
    cur_.clear();
    std::vector<InputId> ids;
    for (const Binding& b : st_.bindings)
        if (std::find(ids.begin(), ids.end(), b.in) == ids.end()) ids.push_back(b.in);
    for (const InputId& id : ids) {
        const std::vector<int> e = effective(ctx_, id);
        cur_.insert(cur_.end(), e.begin(), e.end());
    }
    std::sort(cur_.begin(), cur_.end());
}

// What an input does in a context, by content (indices into the binding list move when the list is edited).
std::vector<std::array<int, 3>> Engine::effective_sig(Ctx c, const InputId& id) const {
    std::vector<std::array<int, 3>> r;
    for (int i : effective(c, id)) {
        const Binding& b = st_.bindings[i];
        r.push_back({static_cast<int>(b.act), static_cast<int>(b.trig), b.ms});
    }
    return r;
}

void Engine::set_settings(const Settings& s, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    // inputs whose effective bindings change are consumed until released
    std::map<InputId, std::vector<std::array<int, 3>>> before;
    for (auto& kv : in_)
        if (kv.second.held) before[kv.first] = effective_sig(ctx_, kv.first);
    st_ = s;
    rt_.assign(st_.bindings.size(), BindRt{});
    for (auto& kv : before)
        if (effective_sig(ctx_, kv.first) != kv.second) in_[kv.first].consumed = true;
    rebuild();
    (void)now;
}

Settings Engine::settings() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

void Engine::set_context(Ctx c, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (c == ctx_ || c == Ctx::All) return;
    step(now);   // edges up to now belong to the old context
    for (auto& kv : in_) {
        if (!kv.second.held) continue;
        if (effective_sig(ctx_, kv.first) != effective_sig(c, kv.first)) kv.second.consumed = true;
    }
    ctx_ = c;
    rebuild();
    for (BindRt& r : rt_) r.prev = false;   // the new context starts with a clean edge state
    for (int b : cur_) rt_[b].prev = binding_active(b, now);   // already-held inputs do not fire an edge on entry
}

Ctx Engine::context() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ctx_;
}

void Engine::press(const InputId& id, uint64_t now) {
    InState& s = in_[id];
    if (s.held) return;
    s.held = true;
    s.consumed = false;
    const bool quick_again = s.had_release;
    const uint64_t last_release = s.release_ns;
    s.press_ns = now;
    for (int b : effective(ctx_, id)) {
        const Binding& bind = st_.bindings[b];
        BindRt& r = rt_[b];
        switch (bind.trig) {
            case Trig::Toggle: r.toggled = !r.toggled; break;
            case Trig::DoubleTap: {
                const uint64_t window = static_cast<uint64_t>(bind.ms > 0 ? bind.ms : st_.timing.double_ms) * kMs;
                r.dt_on = quick_again && now >= last_release && now - last_release <= window;
                break;
            }
            default: break;
        }
    }
}

void Engine::release(const InputId& id, uint64_t now) {
    auto it = in_.find(id);
    if (it == in_.end() || !it->second.held) return;
    InState& s = it->second;
    s.held = false;
    s.release_ns = now;
    s.had_release = true;
    if (!s.consumed) {
        for (int b : effective(ctx_, id)) {
            const Binding& bind = st_.bindings[b];
            BindRt& r = rt_[b];
            if (bind.trig == Trig::Tap) {
                const uint64_t window = static_cast<uint64_t>(bind.ms > 0 ? bind.ms : st_.timing.tap_ms) * kMs;
                if (now - s.press_ns <= window) r.pulse_until = now + static_cast<uint64_t>(st_.timing.pulse_ms) * kMs;
            }
            if (bind.trig == Trig::DoubleTap) r.dt_on = false;
        }
    }
    // a double tap that ends with the key up must not stay on
    for (size_t i = 0; i < st_.bindings.size(); ++i)
        if (st_.bindings[i].in == id && st_.bindings[i].trig == Trig::DoubleTap) rt_[i].dt_on = false;
    s.consumed = false;
}

void Engine::key(int scancode, bool down, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    step(now);
    if (down) press(key_id(scancode), now); else release(key_id(scancode), now);
    step(now);
}

void Engine::mouse_button(int button, bool down, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    step(now);
    if (down) press(mouse_id(button), now); else release(mouse_id(button), now);
    step(now);
}

void Engine::wheel(int dir, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    step(now);
    const InputId id = wheel_id(dir);
    for (int b : effective(ctx_, id)) {
        const Binding& bind = st_.bindings[b];
        BindRt& r = rt_[b];
        if (bind.trig == Trig::Toggle) r.toggled = !r.toggled;
        else if (is_select(bind.act)) on_activate(b, now);     // every notch is an edge, however fast the wheel turns
        else r.pulse_until = now + static_cast<uint64_t>(st_.timing.pulse_ms) * kMs;
    }
    step(now);
}

void Engine::release_all(uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    step(now);
    for (auto& kv : in_) {
        InState& s = kv.second;
        if (!s.held) continue;
        s.held = false;
        s.release_ns = now;
        s.had_release = true;
        s.consumed = false;
    }
    for (BindRt& r : rt_) { r.dt_on = false; r.pulse_until = 0; }
    sels_.clear();
    step(now);
}

void Engine::set_cycle_hint(int current, unsigned mask, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool recent = !sels_.empty() || (last_select_ns_ && now >= last_select_ns_ && now - last_select_ns_ < 300 * kMs);
    if (current >= 0 && current < 4 && !recent) cycle_ = current;
    cycle_mask_ = mask & 0x0f ? mask & 0x0f : 0x0f;
}

int Engine::tracked_cycle() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cycle_;
}

bool Engine::binding_active(int b, uint64_t now) const {
    const Binding& bind = st_.bindings[b];
    const BindRt& r = rt_[b];
    if (bind.in.kind == InKind::Wheel) return bind.trig == Trig::Toggle ? r.toggled : (!is_select(bind.act) && now < r.pulse_until);
    auto it = in_.find(bind.in);
    const bool held = it != in_.end() && it->second.held && !it->second.consumed;
    switch (bind.trig) {
        case Trig::Press: return held;
        case Trig::Tap: return now < r.pulse_until;
        case Trig::Hold: {
            const uint64_t delay = static_cast<uint64_t>(bind.ms > 0 ? bind.ms : st_.timing.hold_ms) * kMs;
            return held && now >= it->second.press_ns && now - it->second.press_ns >= delay;
        }
        case Trig::Toggle: return r.toggled;
        case Trig::DoubleTap: return held && r.dt_on;
    }
    return false;
}

void Engine::set_aux_sticks(float lx, float ly, float rx, float ry) {
    std::lock_guard<std::mutex> lk(mu_);
    aux_[0] = lx; aux_[1] = ly; aux_[2] = rx; aux_[3] = ry;
}

void Engine::pulse(Act a, uint64_t now, int ms) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t p = static_cast<uint64_t>(ms > 0 ? ms : st_.timing.pulse_ms) * kMs;
    uint64_t start = now;
    for (const Sel& s : sels_) start = std::max(start, s.end + p);
    sels_.push_back({a, start, start + p});
    last_select_ns_ = start + p;
}

void Engine::select(Act a, uint64_t now) {
    if (a == Act::SetANext || a == Act::SetAPrev) {
        const int dir = a == Act::SetANext ? 1 : 3;
        int i = cycle_;
        for (int n = 0; n < 4; ++n) {
            i = (i + dir) % 4;
            if (cycle_mask_ & (1u << i)) break;
        }
        cycle_ = i;
        a = static_cast<Act>(static_cast<int>(Act::SetA1) + i);
    } else if (a >= Act::SetA1 && a <= Act::SetA4) {
        cycle_ = static_cast<int>(a) - static_cast<int>(Act::SetA1);
    }
    // One pulse per select, queued behind the previous one with a release gap of one pulse, so the game never sees two combinations at once.
    const uint64_t pulse = static_cast<uint64_t>(st_.timing.pulse_ms) * kMs;
    uint64_t start = now;
    for (const Sel& s : sels_) start = std::max(start, s.end + pulse);
    sels_.push_back({a, start, start + pulse});
    last_select_ns_ = start + pulse;
}

void Engine::on_activate(int b, uint64_t now) {
    const Binding& bind = st_.bindings[b];
    acts_[static_cast<int>(bind.act)]++;
    last_ = bind.act;
    if (!is_select(bind.act)) return;
    if (bind.in.kind == InKind::Wheel && (bind.act == Act::SetANext || bind.act == Act::SetAPrev) && !st_.mouse.wheel_cycles) return;
    select(bind.act, now);
}

void Engine::step(uint64_t now) {
    for (int b : cur_) {
        const bool a = binding_active(b, now);
        if (a && !rt_[b].prev) on_activate(b, now);
        rt_[b].prev = a;
    }
}

EngineOut Engine::output(uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    step(now);
    EngineOut o;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    auto add = [&](const Output& x) { o.buttons |= x.buttons; lx += x.lx; ly += x.ly; rx += x.rx; ry += x.ry; };
    bool on[kActCount] = {};
    for (int b : cur_)
        if (!is_select(st_.bindings[b].act) && binding_active(b, now)) on[static_cast<int>(st_.bindings[b].act)] = true;
    for (int a = 1; a < kActCount; ++a) {
        if (!is_select(static_cast<Act>(a)) && on[a]) add(st_.out[a]);
    }
    bool retired = false;
    for (auto it = sels_.begin(); it != sels_.end();) {
        if (it->seen && now >= it->end && (!frames_used_ || frame_ - it->seen_frame >= kPulseFrames)) { it = sels_.erase(it); retired = true; }
        else ++it;
    }
    if (!retired && !sels_.empty() && now >= sels_.front().start) {       // a poll that retired a pulse shows nothing: the release gap
        add(st_.out[static_cast<int>(sels_.front().act)]);
        if (!sels_.front().seen) sels_.front().seen_frame = frame_;
        sels_.front().seen = true;
    }
    lx += aux_[0]; ly += aux_[1]; rx += aux_[2]; ry += aux_[3];
    clamp_stick(lx, ly);
    clamp_stick(rx, ry);
    o.lx = lx; o.ly = ly; o.rx = rx; o.ry = ry;
    return o;
}

void Engine::frame_tick() {
    std::lock_guard<std::mutex> lk(mu_);
    frames_used_ = true;
    ++frame_;
}

bool Engine::input_held(const InputId& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = in_.find(id);
    return it != in_.end() && it->second.held;
}

bool Engine::act_active(Act a, uint64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    step(now);
    if (is_select(a)) {
        return !sels_.empty() && sels_.front().act == a && now >= sels_.front().start && (now < sels_.front().end || !sels_.front().seen || (frames_used_ && frame_ - sels_.front().seen_frame < kPulseFrames));
    }
    for (int b : cur_)
        if (st_.bindings[b].act == a && binding_active(b, now)) return true;
    return false;
}

uint64_t Engine::activations(Act a) const {
    std::lock_guard<std::mutex> lk(mu_);
    return acts_[static_cast<int>(a)];
}

Act Engine::last_activated() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_;
}

}  // namespace nxrt::play
