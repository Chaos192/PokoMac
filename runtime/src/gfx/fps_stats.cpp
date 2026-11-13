// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "fps_stats.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace nxrt::gfx {
namespace {
constexpr double kShortWindowS = 0.5;   // fps, average and worst frame
constexpr double kLongWindowS = 5.0;    // the 1 % low
}  // namespace

void FpsStats::reset() {
    head_ = 0;
    count_ = 0;
}

void FpsStats::push(double now_s, double dt_ms) {
    if (count_ == kMaxFrames) {          // full: drop the oldest frame
        head_ = (head_ + 1) % kMaxFrames;
        --count_;
    }
    ring_[(head_ + count_) % kMaxFrames] = {now_s, dt_ms};
    ++count_;
    // frames are in time order: drop the ones older than the long window from the front
    while (count_ > 0 && ring_[head_].t < now_s - kLongWindowS) {
        head_ = (head_ + 1) % kMaxFrames;
        --count_;
    }
}

FpsStats::Snapshot FpsStats::snapshot(double now_s) const {
    Snapshot s;
    std::vector<double> dts;   // frame times of the last 5 s
    dts.reserve(count_);
    double sum_short = 0, max_short = 0;
    std::size_t n_short = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const Frame& f = ring_[(head_ + i) % kMaxFrames];
        if (f.t < now_s - kLongWindowS) continue;
        dts.push_back(f.dt_ms);
        if (f.t >= now_s - kShortWindowS) {
            sum_short += f.dt_ms;
            max_short = std::max(max_short, f.dt_ms);
            ++n_short;
        }
    }
    if (n_short < 2) return s;   // not valid yet

    s.valid = true;
    s.avg_ms = sum_short / static_cast<double>(n_short);
    s.fps = s.avg_ms > 0 ? 1000.0 / s.avg_ms : 0.0;
    s.max_ms = max_short;

    // the slowest 1 % (at least one frame): their mean frame time gives the low
    std::sort(dts.begin(), dts.end(), std::greater<double>());
    const std::size_t k = std::max<std::size_t>(1, dts.size() / 100);
    double sum_slow = 0;
    for (std::size_t i = 0; i < k; ++i) sum_slow += dts[i];
    const double mean_slow = sum_slow / static_cast<double>(k);
    s.low1_fps = mean_slow > 0 ? 1000.0 / mean_slow : 0.0;
    return s;
}

}  // namespace nxrt::gfx
