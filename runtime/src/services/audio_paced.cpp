// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The paced sink: a simulated output device. It keeps the frames the audio renderer writes in a queue that drains at the sample rate on
// the host clock (optionally a little fast or slow, [audio] paced_ppm), and counts how often the queue ran dry, how much was dropped
// when it was full, and the lowest and highest queue level. It plays nothing. It exists to measure the renderer's timing without a
// sound card: a real device behaves the same way (XAudio2 consumes the queue at its own clock and starves when it is empty).
// It also adds up the device time spent starved (the silences a listener hears inside the sound), and with [audio] paced_wav writes
// what the device played, the starved stretches as silence, so gaps can be measured in the sound itself (the [audio] wav_dump tee
// records what the renderer wrote, in which a late frame leaves no gap).
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::audio {
namespace {

using Clock = std::chrono::steady_clock;

class PacedSink final : public AudioSink {
public:
    explicit PacedSink(double skew_ppm) : skew_(1.0 + skew_ppm / 1e6), wav_path_(config_get_str("audio", "paced_wav", "")) {
        if (!wav_path_.empty()) wthread_ = std::thread([this] { wav_run(); });
    }
    ~PacedSink() override {
        close();
        {
            std::lock_guard<std::mutex> lk(wmu_);
            wstop_ = true;
        }
        wcv_.notify_all();
        if (wthread_.joinable()) wthread_.join();
    }

    bool open(uint32_t sample_rate, uint32_t channels) override {
        std::lock_guard<std::mutex> lk(mu_);
        rate_ = sample_rate;
        channels_ = channels;
        written_ = played_ = 0.0;
        running_ = starved_ = false;
        opened_ = true;
        samples_.clear();
        device_pos_ = 0.0;
        emitted_ = 0;
        return sample_rate > 0 && (channels == 2 || channels == 6);
    }

    void close() override {
        bool was_open;
        {
            std::lock_guard<std::mutex> lk(mu_);
            was_open = opened_;
            opened_ = false;
        }
        if (was_open) NXLOG_I("audio", "%s", format_stats(name(), stats()).c_str());
    }

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        std::lock_guard<std::mutex> lk(mu_);
        frames_written_ += frames;
        if (!opened_ || rate_ == 0) return 0;
        advance();
        const double cap = rate_ * kCapacitySeconds;
        const double room = std::max(0.0, cap - (written_ - played_));
        const uint32_t take = static_cast<uint32_t>(std::min<double>(frames, room));
        if (take < frames) frames_dropped_ += frames - take;
        frames_accepted_ += take;
        if (!running_) {
            running_ = true;           // playback starts with the first frames, as a voice does
            last_ = Clock::now();
            if (t_first_ == Clock::time_point{}) t_first_ = last_;
        }
        starved_ = false;
        longest_starve_ = std::max(longest_starve_, cur_starve_);
        cur_starve_ = 0.0;
        if (!wav_path_.empty() && pcm) samples_.insert(samples_.end(), pcm, pcm + static_cast<size_t>(take) * channels_);
        written_ += take;
        peak_ = std::max(peak_, written_ - played_);
        return take;
    }

    uint32_t queued_frames() override {
        std::lock_guard<std::mutex> lk(mu_);
        advance();
        return static_cast<uint32_t>(std::max(0.0, written_ - played_));
    }

    bool has_device_clock() const override { return true; }
    const char* name() const override { return "paced"; }

    SinkStats stats() override {
        std::lock_guard<std::mutex> lk(mu_);
        advance();
        SinkStats s;
        s.frames_written = frames_written_;
        s.frames_accepted = frames_accepted_;
        s.frames_dropped = frames_dropped_;
        s.underruns = underruns_;
        s.queued_peak_frames = static_cast<uint32_t>(peak_);
        s.queued_low_frames = low_ < 0 ? 0 : static_cast<uint32_t>(low_);
        s.queued_peak = s.queued_peak_frames;
        s.sample_rate = rate_;
        s.starved_frames = static_cast<uint64_t>(starved_frames_);
        s.longest_starve_frames = static_cast<uint32_t>(std::max(longest_starve_, cur_starve_));
        return s;
    }

private:
    static constexpr double kWarmupSeconds = 0.5;
    static constexpr double kCapacitySeconds = 0.120;   // the device queue: what XAudio2 would hold before write() drops

    // Moves the play position to now (mu_ held). A queue that empties is an underrun until the next write. The lowest level is sampled
    // by every call, so it is the level just before a refill when the renderer polls the queue each frame.
    void advance() {
        if (!running_) return;
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        const double step = dt * rate_ * skew_;
        played_ += step;
        const double q = written_ - played_;
        if (!wav_path_.empty()) emit(step);
        if (q <= 0.0) {
            starved_frames_ += -q;   // the device played what was left, then nothing for the rest of the step
            cur_starve_ += -q;
            played_ = written_;
            if (!starved_) {
                starved_ = true;
                underruns_++;
                NXLOG_W("audio", "paced sink: underrun #%llu, %.2f s after the first frame (%.0f frames written)", static_cast<unsigned long long>(underruns_),
                        std::chrono::duration<double>(now - t_first_).count(), written_);
            }
            low_ = 0;
        } else if (!starved_ && played_ > rate_ * kWarmupSeconds && (low_ < 0 || q < low_)) {
            low_ = q;   // the start-up fill is not a margin: the lowest level is taken once half a second has played
        }
    }

    // The device's output for [audio] paced_wav (mu_ held): `step` more frames of device time, taken from the queued samples while
    // there are any and silence after. Handed to the writer thread, so a slow disk never delays the renderer that called in.
    void emit(double step) {
        device_pos_ += step;
        const uint64_t want = static_cast<uint64_t>(device_pos_);
        if (want <= emitted_ || channels_ == 0) return;
        const size_t n = static_cast<size_t>(want - emitted_) * channels_;
        emitted_ = want;
        const size_t real = std::min(n, samples_.size());
        std::lock_guard<std::mutex> lk(wmu_);
        if (!wopened_ && !wopen_failed_) {
            wopen_failed_ = !wav_.open(wav_path_, rate_, channels_);
            wopened_ = !wopen_failed_;
            if (wopen_failed_) NXLOG_W("audio", "paced sink: cannot write %s", wav_path_.c_str());
        }
        wpend_.insert(wpend_.end(), samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(real));
        wpend_.insert(wpend_.end(), n - real, int16_t{0});
        samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(real));
        wcv_.notify_one();
    }

    void wav_run() {
        std::unique_lock<std::mutex> lk(wmu_);
        for (;;) {
            wcv_.wait_for(lk, std::chrono::milliseconds(250), [this] { return wstop_ || wpend_.size() >= 48000; });
            std::vector<int16_t> chunk;
            chunk.swap(wpend_);
            const bool stop = wstop_, opened = wopened_;
            lk.unlock();
            if (!chunk.empty() && opened) wav_.write(chunk.data(), static_cast<uint32_t>(chunk.size() / wav_.channels()));
            lk.lock();
            if (stop) {
                wav_.close();
                return;
            }
        }
    }

    std::mutex mu_;
    double skew_;
    double starved_frames_ = 0.0, cur_starve_ = 0.0, longest_starve_ = 0.0;
    std::string wav_path_;
    std::deque<int16_t> samples_;          // queued samples, kept for paced_wav only
    double device_pos_ = 0.0;
    uint64_t emitted_ = 0;
    std::mutex wmu_;                       // the writer thread's side
    std::condition_variable wcv_;
    std::vector<int16_t> wpend_;
    WavWriter wav_;
    bool wopened_ = false, wopen_failed_ = false, wstop_ = false;
    std::thread wthread_;
    uint32_t rate_ = 0, channels_ = 0;
    bool opened_ = false, running_ = false, starved_ = false;
    double written_ = 0.0, played_ = 0.0, peak_ = 0.0, low_ = -1.0;
    Clock::time_point last_{}, t_first_{};
    uint64_t frames_written_ = 0, frames_accepted_ = 0, frames_dropped_ = 0, underruns_ = 0;
};

}  // namespace

std::shared_ptr<AudioSink> make_paced_sink(double skew_ppm) { return std::make_shared<PacedSink>(skew_ppm); }

}  // namespace nxrt::audio
