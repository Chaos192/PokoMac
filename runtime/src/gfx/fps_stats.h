// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Frame-time statistics for the corner frame-rate counter (overlay.cpp). Pure: the caller passes the times in, nothing here
// reads a clock and nothing depends on ImGui or Vulkan.
#pragma once
#include <array>
#include <cstddef>

namespace nxrt::gfx {

class FpsStats {
public:
    static constexpr std::size_t kMaxFrames = 2048;   // the ring keeps at most this many frames (oldest dropped)

    void reset();
    // Adds one frame. now_s is the time the frame was presented, in seconds (any fixed origin, increasing); dt_ms the frame time.
    void push(double now_s, double dt_ms);

    struct Snapshot {
        bool valid = false;      // at least 2 frames in the last 0.5 s
        double fps = 0;          // 1000 / avg_ms over the last 0.5 s
        double avg_ms = 0;       // mean frame time over the last 0.5 s
        double max_ms = 0;       // worst frame time over the last 0.5 s
        double low1_fps = 0;     // frame rate of the slowest 1 % of the frames of the last 5 s (at least one frame)
    };
    Snapshot snapshot(double now_s) const;

private:
    struct Frame {
        double t = 0;
        double dt_ms = 0;
    };
    std::array<Frame, kMaxFrames> ring_{};
    std::size_t head_ = 0;    // index of the oldest frame
    std::size_t count_ = 0;   // frames held
};

}  // namespace nxrt::gfx
