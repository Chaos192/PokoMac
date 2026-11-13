// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The host screen's vertical blank: when the screen the window presents on refreshes, on the FrameLimiter::now_ns() clock.
//
// The console's vi vsync event (nn::vi::GetDisplayVsyncEvent) and its window's present interval run on the display's 60 Hz refresh; a
// game that paces itself on them (Super Mario Galaxy 2 runs the Wii's VI retrace on the event) expects one frame per refresh of the
// screen it is shown on. On their own 60 Hz clock they drift against the host's screen (59.94 Hz, or a 60 Hz screen whose refresh is
// 60.02): two frames in one refresh and none in the next (nxnative docs/reference/frame-rate.md "Vulkan acquire and vsync event follow
// the display link", hle_sdk16.c / hle_vk.m on the Metal display link; our own implementation). Here the screen's refresh is read
// from the system (Windows: the desktop compositor's timing, DwmGetCompositionTimingInfo; macOS: a CVDisplayLink on the main display),
// and the event and the acquire holds tick on it (services/vi_nv.cpp vsync_main, nvn_present_pace.h; src/core/vblank.cpp). Without it they keep their own
// grid. [display] vsync_source = host (default) | clock.
//
// On a screen at a whole multiple of the event's rate (60, 120, 240 Hz for a 60 Hz event) a tick is every n-th refresh, so the event
// runs at the screen's real rate / n. On any other screen (75, 144, 165 Hz) a tick is the refresh nearest a steady 1 / rate clock (60
// a second on average; nxnative frame-rate.md "Virtual 60 Hz ticks on screens that are not a multiple of 60"), never two in one refresh.
#pragma once
#include <cmath>
#include <cstdint>

namespace nxrt::vblank {

struct Screen {
    uint64_t phase = 0;    // a refresh's time (now_ns clock); every refresh is phase + k * period
    uint64_t period = 0;   // ns; 0: unknown
};

// The host screen now (any thread; the first call starts the system's source). false: none known (another host, the source failed, or
// [display] vsync_source = clock).
bool screen(Screen* out);
const char* source_name();   // "dwm", "cvdisplaylink", "none" (for the log)
uint64_t now_ns();           // the clock the times are on: FrameLimiter::now_ns()'s (QueryPerformanceCounter, mach_absolute_time), for the core

// ---- the tick logic (pure: tests step it) ------------------------------------------------------------------------------------------
// The refresh nearest t (t may be before or after the phase).
inline uint64_t nearest(const Screen& s, uint64_t t) {
    const double k = std::floor((static_cast<double>(static_cast<int64_t>(t - s.phase)) / static_cast<double>(s.period)) + 0.5);
    return s.phase + static_cast<uint64_t>(static_cast<int64_t>(k * static_cast<double>(s.period)));
}
// The first refresh after t.
inline uint64_t after(const Screen& s, uint64_t t) {
    uint64_t v = nearest(s, t);
    while (v <= t) v += s.period;
    return v;
}
// How many refreshes one tick of an event at rate_hz spans when the screen runs at a whole multiple of it (within 2 %); 0 otherwise
// (virtual ticks).
inline uint32_t refreshes_per_tick(const Screen& s, double rate_hz) {
    if (!s.period || rate_hz <= 0) return 0;
    const double host_hz = 1e9 / static_cast<double>(s.period);
    const double n = std::round(host_hz / rate_hz);
    if (n < 1 || std::fabs(host_hz / n - rate_hz) > 0.02 * rate_hz) return 0;
    return static_cast<uint32_t>(n);
}

struct Ticks {
    uint64_t last = 0;    // the last tick's refresh
    double ideal = 0;     // virtual ticks: the steady clock's last slot
};
// The next tick of an event at rate_hz after now: on the screen's refreshes (see the top). A tick that would be in the past (the caller
// woke late, or the first call) is the first refresh after now: missed ticks are skipped, not caught up.
inline uint64_t next_tick(Ticks& k, const Screen& s, double rate_hz, uint64_t now) {
    uint64_t v;
    if (const uint32_t n = refreshes_per_tick(s, rate_hz)) {
        v = k.last ? nearest(s, k.last + static_cast<uint64_t>(n) * s.period) : after(s, now);
        k.ideal = 0;
    } else {
        const double step = 1e9 / rate_hz;
        k.ideal = k.ideal > 0 ? k.ideal + step : static_cast<double>(now);
        v = nearest(s, static_cast<uint64_t>(k.ideal));
        if (k.last && v <= k.last) v = k.last + s.period;   // never two ticks in one refresh
    }
    if (v <= now) {
        v = after(s, now);
        k.ideal = static_cast<double>(v);
    }
    k.last = v;
    return v;
}

}  // namespace nxrt::vblank
