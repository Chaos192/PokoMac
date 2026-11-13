// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The audio renderer's frame pacing on a device with a clock of its own (audren/Dsp/AudioProcessor.h Wait): the device here drains in 10 ms
// quanta, as XAudio2 does. Every frame signals the game's system event and the game's RequestUpdate clears it, so two frames started back
// to back count as one for the game, which then feeds one frame of audio for two (Ocarina of Time on a PC: ~140 updates a second
// instead of 200, its voices running dry: "it would play and stop"). Frames must start at least 4 ms apart (a frame that keeps the device
// from running dry excepted: at most 2% of them), 200 a second on average, with the device never running dry; and a game that answers
// each signal in time, beside a busy main thread, makes an update for nearly every frame (the old pacing: 235 of 440).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audren/Dsp/AudioProcessor.h"
#include "svc_test.h"

using namespace svctest;
namespace ar = nxrt::audren;

namespace {

using Clock = std::chrono::steady_clock;

int64_t ns_since(Clock::time_point t0) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count(); }

// A device whose clock plays 480 frames (10 ms at 48 kHz) at each 10 ms tick, as XAudio2's processing passes do.
struct QuantumDevice final : ar::IHardwareDevice {
    Clock::time_point t0 = Clock::now();
    int64_t appended = 0, played = 0, ticks = 0, underruns = 0;
    std::mutex mu;
    void SetVolume(float) override {}
    float GetVolume() override { return 1.0f; }
    uint32_t GetSampleRate() override { return 48000; }
    uint32_t GetChannelCount() override { return 2; }
    void AppendBuffer(ar::Span<const int16_t> data, uint32_t channels) override { add(static_cast<int64_t>(data.Length / channels)); }
    void add(int64_t frames) {
        std::lock_guard<std::mutex> lk(mu);
        advance();
        appended += frames;
    }
    void advance() {   // mu held
        const int64_t now_ticks = ns_since(t0) / 10000000;
        for (; ticks < now_ticks; ++ticks) {
            if (appended - played >= 480) {
                played += 480;
            } else {
                if (appended > 0) ++underruns;   // (before the first frame: nothing to play yet)
                played = appended;
            }
        }
    }
    int64_t GetQueuedNanoseconds() override {
        std::lock_guard<std::mutex> lk(mu);
        advance();
        return (appended - played) * 1000000000 / 48000;
    }
};

struct Driver final : ar::IHardwareDeviceDriver {
    QuantumDevice* first = nullptr;
    std::unique_ptr<ar::IHardwareDevice> OpenHardwareDevice(uint32_t, uint32_t) override {
        auto d = std::make_unique<QuantumDevice>();
        if (!first) first = d.get();
        return d;
    }
};

}  // namespace

SVC_TEST(audio_renderer, frames_stay_spread_out_on_a_device_clock) {
    Driver driver;
    ar::AudioProcessor proc;
    proc.Start(&driver);
    QuantumDevice* dev = driver.first;
    CHECK(dev != nullptr);
    if (!dev) return;
    proc.Wait();   // the processor's first RenderEnd (the manager does the same before its loop)

    // The game: its audio thread waits for the system event, and its update clears it (RequestUpdate), on a thread of its own.
    std::mutex ev_mu;
    std::condition_variable ev_cv;
    bool signalled = false, stop = false;
    std::atomic<int> updates{0};
    std::thread game([&] {
        std::unique_lock<std::mutex> lk(ev_mu);
        while (!stop) {
            ev_cv.wait(lk, [&] { return signalled || stop; });
            if (stop) break;
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(300));   // the game's own work for the frame
            lk.lock();
            signalled = false;                                             // RequestUpdate clears the event
            ++updates;
        }
    });

    // A slow main thread beside it (the 9 fps run: the main thread busy for the whole frame): it must not change the audio's pace.
    std::atomic<bool> busy{true};
    std::thread main_thread([&] {
        while (busy.load(std::memory_order_relaxed)) {
        }
    });

    const Clock::time_point t0 = Clock::now();
    std::vector<int64_t> starts;
    int64_t under_before = 0;
    for (int frame = 0; frame < 440; ++frame) {
        if (frame == 40) {                     // after the queue has filled: what is measured from here
            std::lock_guard<std::mutex> lk(dev->mu);
            under_before = dev->underruns;
        }
        starts.push_back(ns_since(t0));
        {
            std::lock_guard<std::mutex> lk(ev_mu);   // the frame's signal (SendCommands)
            signalled = true;
        }
        ev_cv.notify_one();
        dev->add(240);                          // the frame's output (the device sink command)
        proc.Signal();
        proc.Wait();
    }
    const int updates_seen = updates.load();
    {
        std::lock_guard<std::mutex> lk(ev_mu);
        stop = true;
    }
    ev_cv.notify_one();
    game.join();
    busy = false;
    main_thread.join();
    int64_t under_after;
    {
        std::lock_guard<std::mutex> lk(dev->mu);
        under_after = dev->underruns;
    }
    proc.Stop();   // (the devices go with it)

    int64_t min_gap = INT64_MAX;
    int short_gaps = 0, last_short = -1;   // frames that started less than 3.5 ms after the one before, and the last of them
    for (size_t i = 41; i < starts.size(); ++i) {
        min_gap = std::min(min_gap, starts[i] - starts[i - 1]);
        if (starts[i] - starts[i - 1] < 3500000) {
            ++short_gaps;
            last_short = static_cast<int>(i);
        }
    }
    const double secs = static_cast<double>(starts.back() - starts[40]) / 1e9;
    const double rate = 399.0 / secs;
    std::printf("  frames 40..439: %.1f a second, %d gaps under 3.5 ms (shortest %.2f ms, the last at frame %d), underruns %lld; game updates %d "
                "of 440\n", rate, short_gaps, static_cast<double>(min_gap) / 1e6, last_short, static_cast<long long>(under_after - under_before),
                updates_seen);
    CHECK(short_gaps <= 8);                          // back to back only to keep the device from running dry (the old pacing: every other)
    CHECK(rate > 185.0 && rate < 215.0);             // the device's rate, 200 a second
    CHECK_EQ(under_after - under_before, 0);         // and it never runs dry
    CHECK(updates_seen >= 420);                      // the game sees each frame's signal on its own
}
