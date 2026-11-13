// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pieces of the XAudio2 sink's output-device recovery that need no device, so they can be tested: the silent stand-in clock and the
// retry delays. When the device is lost (headphones unplugged, Bluetooth gone, the HDMI monitor off) the sink keeps the audio renderer's
// pacing alive on a StandInClock (a queue that drains at the sample rate on the host clock and plays nothing) while a worker thread
// tries to open the output device again. The approach follows pokopia-native's silent sink (windows/src/nxrt/hle/audio_silent.c): the
// game's audio threads must pace exactly as they would with sound, or the game stalls when the device disappears.
// Time is passed in (seconds on any steady clock), never read here.
#pragma once
#include <algorithm>
#include <cstdint>

namespace nxrt::audio {

// Seconds to wait before the next reopen attempt, given how many attempts have failed so far (0: the first attempt). 1, 2, 4, then 5 s
// forever: a device that comes back (default endpoint switched) is found quickly, one that stays away costs one attempt every 5 s.
inline double reopen_delay_s(unsigned failures) {
    switch (failures) {
        case 0: return 1.0;
        case 1: return 2.0;
        case 2: return 4.0;
        default: return 5.0;
    }
}

// A simulated device queue. Playback starts with the first accepted frames (as a voice does) and the queue drains at `rate`. An empty
// queue stays empty until the next write. Not thread safe: the owner serialises calls.
class StandInClock {
public:
    static constexpr double kCapacitySeconds = 0.120;   // what the real device holds before write() drops

    void begin(uint32_t rate, double now) {
        rate_ = rate;
        written_ = played_ = 0.0;
        last_ = now;
        running_ = false;
    }

    // Queues up to `frames`; returns how many fit.
    uint32_t write(uint32_t frames, double now) {
        advance(now);
        const double room = std::max(0.0, rate_ * kCapacitySeconds - (written_ - played_));
        const uint32_t take = static_cast<uint32_t>(std::min<double>(frames, room));
        if (take && !running_) {
            running_ = true;
            last_ = now;
        }
        written_ += take;
        return take;
    }

    uint32_t queued(double now) {
        advance(now);
        return static_cast<uint32_t>(std::max(0.0, written_ - played_));
    }

private:
    void advance(double now) {
        if (!running_) return;
        const double dt = std::max(0.0, now - last_);   // a clock that steps back plays nothing
        last_ = now;
        played_ = std::min(written_, played_ + dt * rate_);
    }

    uint32_t rate_ = 0;
    double written_ = 0.0, played_ = 0.0, last_ = 0.0;
    bool running_ = false;
};

}  // namespace nxrt::audio
