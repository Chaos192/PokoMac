// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode self-test driver, see play_selftest.h.
#include "play_selftest.h"

#include <windows.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/platform.h"

namespace nxrt::play {

namespace {

[[noreturn]] void finish(int code, const char* verdict) {
    if (code) NXLOG_E("play", "selftest FAIL: %s", verdict);
    else NXLOG_I("play", "selftest PASS%s%s", *verdict ? ": " : "", verdict);
    exit_process(code, code ? "fail" : "pass", std::string("selftest: ") + (*verdict ? verdict : "done"));
}

[[noreturn]] void fail(const std::string& why) { finish(1, why.c_str()); }

bool parse_wh(const std::string& s, int* w, int* h) { return std::sscanf(s.c_str(), "%dx%d", w, h) == 2; }

void push_key(SDL_Scancode sc, bool down, SDL_Keymod mod) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    e.key.mod = mod;
    e.key.down = down;
    e.key.repeat = false;
    SDL_PushEvent(&e);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

}  // namespace

bool SelfTest::parse(const std::string& script, std::string* err) {
    steps_.clear();
    next_ = 0;
    for (const std::string& tok : split(script, ',')) {
        if (tok.empty()) continue;
        const std::vector<std::string> p = split(tok, ':');
        Step s;
        s.verb = p[0];
        if (p.size() > 1) s.a = p[1];
        if (p.size() > 2) s.b = p[2];
        static const char* kVerbs[] = {"wait", "size", "mode", "altenter", "key", "close", "cap", "measure", "expect_mode", "expect_size", "expect_fps", "expect_late", "set", "expect_present", "expect_cap", "expect_ini", "pass", "gate", "report"};
        bool known = false;
        for (const char* v : kVerbs) known |= s.verb == v;
        if (!known) {
            if (err) *err = "unknown selftest step '" + tok + "'";
            return false;
        }
        steps_.push_back(s);
    }
    return true;
}

void SelfTest::tick(gfx::Renderer* renderer, FrameLimiter* limiter, SelfTestHost* host) {
    if (measuring_) {
        const uint64_t now = FrameLimiter::now_ns();
        if (now - m_start_ns_ < m_len_ns_) return;
        const uint64_t frames = renderer ? renderer->stats().frames : 0;
        last_fps_ = static_cast<double>(frames - m_frames0_) * 1e9 / static_cast<double>(now - m_start_ns_);
        measuring_ = false;
        NXLOG_I("play", "selftest: fps=%.2f over %.1f s (cap %.0f)", last_fps_, static_cast<double>(now - m_start_ns_) / 1e9, limiter ? limiter->target_fps() : 0.0);
        if (limiter) {
            const FrameLimiter::Stats ls = limiter->stats();
            NXLOG_I("play", "selftest: limiter: %llu frames, mean %.3f ms, p99 %.3f ms, max %.3f ms, waited %llu, overran %llu (%.1f %%), late p99 %.3f ms",
                    static_cast<unsigned long long>(ls.frames), ls.mean_ms, ls.p99_ms, ls.max_ms, static_cast<unsigned long long>(ls.waited),
                    static_cast<unsigned long long>(ls.overran), ls.overran_fraction * 100.0, ls.late_p99_ms);
        }
        return;
    }
    if (wait_ > 0) { --wait_; return; }
    if (next_ >= steps_.size()) return;
    const Step& s = steps_[next_];
    if (s.verb == "gate") {
        // gate:NAME holds the script until a file named selftest_NAME exists in the working folder, then deletes it and goes on. A person
        // or a driver script uses it to run the next steps at a moment of its choosing (for example once the game is in play).
        const std::string path = "selftest_" + s.a;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        std::filesystem::remove(path, ec);
    }
    NXLOG_I("play", "selftest: step %zu: %s %s %s", next_, s.verb.c_str(), s.a.c_str(), s.b.c_str());
    ++next_;
    if (s.verb == "wait") {
        wait_ = std::atoi(s.a.c_str());
    } else if (s.verb == "size") {
        int w, h;
        if (!parse_wh(s.a, &w, &h)) fail("bad size '" + s.a + "'");
        platform::set_window_size(w, h);
        wait_ = 5;
    } else if (s.verb == "mode") {
        const platform::FullscreenMode m = s.a == "borderless" ? platform::FullscreenMode::Borderless
                                          : s.a == "exclusive" ? platform::FullscreenMode::Exclusive
                                                               : platform::FullscreenMode::Windowed;
        if (!platform::set_fullscreen(m)) fail("set_fullscreen(" + s.a + ") failed");
        wait_ = 5;
    } else if (s.verb == "altenter") {
        push_key(SDL_SCANCODE_LALT, true, SDL_KMOD_LALT);
        push_key(SDL_SCANCODE_RETURN, true, SDL_KMOD_LALT);
        push_key(SDL_SCANCODE_RETURN, false, SDL_KMOD_LALT);
        push_key(SDL_SCANCODE_LALT, false, SDL_KMOD_NONE);
        wait_ = 8;
    } else if (s.verb == "key") {
        const SDL_Scancode sc = SDL_GetScancodeFromName(s.a.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) fail("unknown key '" + s.a + "'");
        push_key(sc, s.b == "down", SDL_KMOD_NONE);
    } else if (s.verb == "close") {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_QUIT;
        e.quit.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&e);
        wait_ = 600;     // the pump turns it into the process exit; if that does not happen the script goes on and fails below
        if (next_ >= steps_.size()) steps_.push_back({"fail_close", "", ""});
    } else if (s.verb == "fail_close") {
        fail("the window close did not end the process");
    } else if (s.verb == "cap") {
        if (limiter) limiter->set_target_fps(std::atof(s.a.c_str()));
        wait_ = 30;      // let the schedule settle
    } else if (s.verb == "measure") {
        measuring_ = true;
        if (limiter) limiter->reset_stats();
        m_start_ns_ = FrameLimiter::now_ns();
        m_len_ns_ = static_cast<uint64_t>(std::atof(s.a.c_str()) * 1e9);
        m_frames0_ = renderer ? renderer->stats().frames : 0;
    } else if (s.verb == "expect_mode") {
        const char* m = "windowed";
        switch (platform::fullscreen_mode()) {
            case platform::FullscreenMode::Borderless: m = "borderless"; break;
            case platform::FullscreenMode::Exclusive: m = "exclusive"; break;
            default: break;
        }
        if (s.a != m) fail("window mode is " + std::string(m) + ", expected " + s.a);
    } else if (s.verb == "expect_size") {
        int w, h, pw = 0, ph = 0;
        if (!parse_wh(s.a, &w, &h)) fail("bad size '" + s.a + "'");
        platform::pixel_size(&pw, &ph);
        if (pw != w || ph != h) fail("window size is " + std::to_string(pw) + "x" + std::to_string(ph) + ", expected " + s.a);
    } else if (s.verb == "expect_fps") {
        const double want = std::atof(s.a.c_str()), tol = std::atof(s.b.c_str());
        if (std::fabs(last_fps_ - want) > want * tol) fail("fps " + std::to_string(last_fps_) + " is not within " + s.b + " of " + s.a);
    } else if (s.verb == "expect_late") {
        // the limiter's own accuracy on the last measure: frames that had to wait must have started within MS of their schedule
        if (!limiter) fail("no limiter");
        const FrameLimiter::Stats ls = limiter->stats();
        if (ls.waited < 50) fail("only " + std::to_string(ls.waited) + " frames waited: the game was slower than the cap, nothing to judge");
        if (ls.late_p99_ms > std::atof(s.a.c_str())) fail("late p99 " + std::to_string(ls.late_p99_ms) + " ms exceeds " + s.a);
    } else if (s.verb == "set") {
        // "set:vsync=1": the value may itself contain ':' (not used today), so re-join the parts
        const std::string kv = s.a + (s.b.empty() ? "" : ":" + s.b);
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || !host || !host->set(kv.substr(0, eq), kv.substr(eq + 1))) fail("bad setting '" + kv + "'");
        wait_ = 30;      // the settings are applied at the next frame boundary and the present mode on the next presented frame
    } else if (s.verb == "expect_present") {
        const std::string m = host ? host->present_mode() : "";
        if (m != s.a) fail("present mode is '" + m + "', expected " + s.a);
    } else if (s.verb == "expect_cap") {
        const double cap = limiter ? limiter->target_fps() : -1;
        if (std::fabs(cap - std::atof(s.a.c_str())) > 1e-6) fail("frame cap is " + std::to_string(cap) + ", expected " + s.a);
    } else if (s.verb == "expect_ini") {
        const std::string t = host ? host->settings_text() : "";
        if (t.find(s.a + (s.b.empty() ? "" : ":" + s.b)) == std::string::npos) fail("the settings file does not contain '" + s.a + "'");
    } else if (s.verb == "pass") {
        finish(0, "");
    } else if (s.verb == "report") {
        // report[:label]: one log line with what the window looks like now
        int pw = 0, ph = 0;
        platform::pixel_size(&pw, &ph);
        const char* m = "windowed";
        switch (platform::fullscreen_mode()) {
            case platform::FullscreenMode::Borderless: m = "borderless"; break;
            case platform::FullscreenMode::Exclusive: m = "exclusive"; break;
            default: break;
        }
        NXLOG_I("play", "selftest: report %s: mode %s, pixels %dx%d, focused %d, minimised %d, display %.1f Hz, frames %llu", s.a.c_str(), m, pw, ph,
                platform::focused() ? 1 : 0, platform::minimized() ? 1 : 0, static_cast<double>(platform::display_refresh_hz()),
                static_cast<unsigned long long>(renderer ? renderer->stats().frames : 0));
    }
}

}  // namespace nxrt::play
