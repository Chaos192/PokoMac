// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Game control for the cheats overlay (phase 6): pause / resume, frame advance and the speed hack, all native.
//
//   - Pause: at the next frame boundary (the play loop's per-frame hook, on the presenting guest thread) every other guest thread is asked to
//     park, the way svcSetThreadActivity pauses one (kernel/thread.cpp: a thread inside the runtime counts as paused and parks when it comes
//     out; a thread in lifted code parks at its next call out). The presenting thread then stays in the hook, where the play loop's service
//     function pumps the window, the pads and the overlay and shows the last frame again, so the overlay keeps working over a still game.
//     Threads the game itself had paused are left as they are.
//   - Frame advance: the threads run until the next frame boundary, then park again.
//   - Speed: the console clock (svcGetSystemTick, CNTVCT_EL0), the guest's sleeps and timed waits, the display's vsync and the frame limiter
//     all run at `speed` times real time (kernel::set_time_scale). A game that steps once per vsync or by the tick delta runs that much
//     faster or slower; the audio renderer keeps real time (a game that paces itself on audio buffers does not speed up).
// Everything here is idle until used: no pause is requested and the scale is 1.0 by default.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace nxrt::cheats::ctl {

// The play loop's per-frame hook calls this once per frame, on the thread that presents (play.cpp update_hook). It runs the frame
// callbacks (hotkeys, Lua on-frame), then holds the thread while the game is paused.
void frame_hook();

// While paused, the frame hook calls this (play.cpp installs it): pump window events, poll the pads, run the overlay and present the last
// frame again. Without one the paused thread only sleeps.
void set_service(void (*fn)());

void pause(bool on);
bool paused();                 // a pause is in force (or requested and about to take hold)
void frame_advance(int frames = 1);
uint64_t frames();             // frame boundaries seen
uint64_t paused_threads();     // guest threads the current pause holds
bool held();                   // no frame passed for this callback: called from inside a pause, or by a thread a breakpoint holds

// 0.05 .. 16; 1.0 is normal speed.
void set_speed(double s);
double speed();

// A function run at every frame boundary (on the presenting thread, before a pause holds it), and while a pause holds the game every
// few milliseconds as well, so hotkeys, scripts' hotkeys and the scripted test keep working then (held() is true in those calls: no
// frame passed, so per-frame work such as a value graph's sample is skipped). Returns an id for remove.
using FrameFn = void (*)(void* user);
uint32_t add_frame_callback(FrameFn fn, void* user);
void remove_frame_callback(uint32_t id);

// Runs fn at the next frame boundary with every other guest thread parked (the code patcher swaps compiled code there, so no thread is
// in the middle of the bytes it rewrites), then lets them go (unless the game is paused). Returns at once; fn runs on the presenting thread.
void run_held(std::function<void()> fn);
// True on the thread that runs the frame hook (the presenting thread), which alone may run the pause service.
bool on_frame_thread();
// One round of the pause service, for a thread the overlay holds outside the frame hook: a breakpoint on the presenting thread, or on
// another thread when the frames stopped (the presenting thread waits for it): then the frame callbacks run here too (held() is true).
void service_once();

// One line for the overlay's status bar: "paused (12 threads)", "x2.0", "".
std::string status_text();

}  // namespace nxrt::cheats::ctl
