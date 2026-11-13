// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// PCM output and capture without the audio renderer (S3c): audout:u and audin:u (nn::audio::detail::IAudioOutManager / IAudioInManager)
// and their IAudioOut / IAudioIn streams. Each started stream has a pacing thread that hands the appended buffers on (to the host sink,
// or to the mix bus while something else plays: audio_mixbus.h) and releases them when they have played.
// Command ids and the AudioOutBuffer / AudioOutParameterInternal layouts: https://switchbrew.org/wiki/Audio_services; result module 153:
// https://switchbrew.org/wiki/Error_codes. Device names DeviceOut / BuiltInHeadset, the 6-channel rule and the Flush answer as Ryujinx
// (MIT) src/Ryujinx.Audio and src/Ryujinx.Horizon/Sdk/Audio/Detail describe them.
//
// What differs from a console on purpose: any rate from 8 to 192 kHz plays as asked (other values: 48 kHz); up to 64 buffers wait at once
// (queued plus released and not yet taken); the played-sample count survives Stop; capture is silence. A buffer is released when the host
// device has played it ([audio] out_latency_ms ahead, so the device never runs dry), or, without a device clock, when real time at the
// stream's rate has passed over it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio_device.h"
#include "audio_mixbus.h"
#include "audren/Integration/ThreadBoost.h"
#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

using MonoClock = std::chrono::steady_clock;   // release pacing and the host position estimate

const uint32_t kNotFound = res(153, 1);
const uint32_t kOperationFailed = res(153, 2);
const uint32_t kTooManyBuffers = res(153, 8);
constexpr size_t kMaxBuffers = 64;          // queued + released and not yet taken
constexpr uint64_t kDescriptorBytes = 0x28;  // AudioOutBuffer
constexpr uint64_t kNameBytes = 0x100;       // AudioOutInfo / AudioDeviceName
constexpr uint64_t kReleasedMaxBytes = 0x200;
constexpr const char* kOutName = "DeviceOut";
constexpr const char* kInName = "BuiltInHeadset";

GaugeCounter g_queued("audout.buffers_queued");
GaugeCounter g_released("audout.buffers_released");
std::atomic<int> g_started_outs{0};

int latency_ms() {
    static const int v = static_cast<int>(std::clamp<int64_t>(config_get_int("audio", "out_latency_ms", 30), 0, 200));
    return v;
}

int16_t sat16(float v) {   // saturate, then truncate toward zero
    if (!(v < 32767.0f)) return v != v ? 0 : 32767;
    if (v <= -32768.0f) return -32768;
    return static_cast<int16_t>(v);
}

// ---- the host sink, shared by every stream ---------------------------------------------------------------------------------------
struct HostSink {
    std::mutex mu;
    std::shared_ptr<audio::AudioSink> sink;
    uint32_t rate = 0;
    uint32_t channels = 0;
    bool ok = false;
    uint64_t opens = 0, written = 0, generation = 0;
};
HostSink& host() {
    static HostSink h;
    return h;
}
// Opens the sink for (rate, channels) unless it is already open so (caller holds host().mu).
void open_if_needed(HostSink& hs, uint32_t rate, uint32_t channels) {
    std::shared_ptr<audio::AudioSink> s = audio::sink();
    if (s && s == hs.sink && rate == hs.rate && channels == hs.channels && s->opens() == hs.opens) return;
    hs.sink = s;
    hs.rate = rate;
    hs.channels = channels;
    hs.ok = s && s->open_counted(rate, channels);
    hs.opens = s ? s->opens() : 0;
    hs.written = 0;
    ++hs.generation;
    if (hs.ok) NXLOG_I("audout", "host sink %s opened: %u Hz, %u channels", s->name(), rate, channels);
    else NXLOG_W("audout", "host sink refused %u Hz, %u channels: playing without sound", rate, channels);
}
struct DeviceMark {
    bool device = false;
    uint64_t generation = 0, end = 0;
};
DeviceMark host_write(const int16_t* pcm, uint32_t frames, uint32_t rate, uint32_t channels) {
    HostSink& h = host();
    std::lock_guard<std::mutex> lk(h.mu);
    open_if_needed(h, rate, channels);
    if (!h.ok) return {};
    const uint64_t accepted = h.sink->write(pcm, frames);   // what the sink took of the frames
    h.written += accepted;
    return DeviceMark{h.sink->has_device_clock(), h.generation, h.written};
}
// The frames the device has played of the current opening, when `generation` is still that opening and the sink has a device clock.
bool device_position(uint64_t generation, uint64_t* pos) {
    HostSink& h = host();
    std::lock_guard<std::mutex> lk(h.mu);
    if (!h.ok || !h.sink || h.sink != audio::sink() || h.generation != generation || h.sink->opens() != h.opens ||
        !h.sink->has_device_clock())
        return false;
    const uint64_t pending = h.sink->queued_frames();   // written, not played yet
    *pos = pending < h.written ? h.written - pending : 0;
    return true;
}

// ---- a stream --------------------------------------------------------------------------------------------------------------------
struct Entry {
    uint64_t tag = 0, addr = 0, bytes = 0, frames = 0;
    bool usable = false, submitted = false;
    DeviceMark mark;
    uint64_t timeline_end = 0;   // submitted frames up to and including this buffer
};

class Stream {
public:
    Stream(bool out, uint32_t rate, uint32_t channels) : out_(out), rate_(rate), channels_(channels) {}
    ~Stream() {
        shutdown();
        std::lock_guard<std::mutex> lk(mu_);
        g_queued.sub(static_cast<int64_t>(queue_.size()));
        g_released.sub(static_cast<int64_t>(released_.size()));
    }
    bool out() const { return out_; }

    uint32_t start() {
        std::lock_guard<std::mutex> lk(ctl_);
        {
            std::lock_guard<std::mutex> l(mu_);
            if (started_) return kOperationFailed;
        }
        if (out_) {
            HostSink& h = host();
            std::lock_guard<std::mutex> l(h.mu);
            open_if_needed(h, rate_, channels_);
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            started_ = true;
            stop_ = false;
            restart_host_ = true;
            if (out_ && !voice_) voice_ = audio::mixbus::open_voice("audio out");
        }
        if (out_) ++g_started_outs;
        thread_ = std::thread([this] { pace(); });
        NXLOG_I(tag(), "Start (%u Hz, %u channels)", rate_, channels_);
        return 0;
    }
    void stop() {
        std::lock_guard<std::mutex> lk(ctl_);
        if (halt()) release_all();
    }
    void shutdown() {
        std::lock_guard<std::mutex> lk(ctl_);
        halt();
        release_all();
    }
    bool flush() {
        if (!started()) return false;   // a stopped stream has nothing to flush
        release_all();
        return true;
    }
    uint32_t append(uint64_t tag, const Entry& e) {
        std::lock_guard<std::mutex> l(mu_);
        if (queue_.size() + released_.size() >= kMaxBuffers) return kTooManyBuffers;
        Entry n = e;
        n.tag = tag;
        queue_.push_back(n);
        g_queued.add(1);
        cv_.notify_all();
        return 0;
    }
    // GetReleased*: up to `bytes` / 8 tags into the buffer, zeros after them.
    uint32_t take_released(const ipc::Buffer& b) {
        const uint64_t bytes = b.size < kReleasedMaxBytes ? b.size / 8 * 8 : kReleasedMaxBytes;
        if (bytes == 0 || !b.addr || !mem::is_mapped(b.addr, bytes)) return 0;
        std::vector<uint8_t> out(bytes, 0);
        uint32_t n = 0;
        std::lock_guard<std::mutex> l(mu_);
        for (; n < bytes / 8 && !released_.empty(); ++n) {
            std::memcpy(out.data() + 8 * n, &released_.front(), 8);
            released_.erase(released_.begin());
        }
        g_released.sub(n);
        buf_write(b, out.data(), bytes);
        event_rule();
        return n;
    }
    bool contains(uint64_t tag) {
        std::lock_guard<std::mutex> l(mu_);
        const bool in_queue = std::any_of(queue_.begin(), queue_.end(), [tag](const Entry& e) { return e.tag == tag; });
        return in_queue || std::find(released_.begin(), released_.end(), tag) != released_.end();
    }
    uint32_t queued() {
        std::lock_guard<std::mutex> l(mu_);
        const size_t waiting = queue_.size();
        return static_cast<uint32_t>(waiting);
    }
    uint64_t played() {
        std::lock_guard<std::mutex> l(mu_);
        return played_;
    }
    bool started() {
        std::lock_guard<std::mutex> l(mu_);
        return started_;
    }
    void set_volume(float v) {
        std::lock_guard<std::mutex> l(mu_);
        volume_ = v;
    }
    float volume() {
        std::lock_guard<std::mutex> l(mu_);
        return volume_;
    }
    uint32_t event_handle() { return event_.new_handle(); }
    uint32_t rate() const { return rate_; }
    uint32_t channels() const { return channels_; }
    const char* tag() const { return out_ ? "audout" : "audin"; }

private:
    // Stops a started stream (thread joined, voice closed); false when it was not started. Caller holds ctl_.
    bool halt() {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (!started_) return false;   // not running: nothing to join
            stop_ = true;
            cv_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
        uint32_t voice = 0;
        {
            std::lock_guard<std::mutex> l(mu_);
            started_ = false;
            voice = voice_;
            voice_ = 0;
            phase_ = 0.0;
            has_last_ = false;
            last_[0] = last_[1] = 0;
        }
        if (out_) --g_started_outs;
        if (voice) audio::mixbus::close_voice(voice);
        return true;
    }
    void release_all() {
        std::lock_guard<std::mutex> l(mu_);
        const size_t n = queue_.size();
        std::transform(queue_.begin(), queue_.end(), std::back_inserter(released_), [](const Entry& e) { return e.tag; });
        queue_.clear();
        g_queued.sub(static_cast<int64_t>(n));
        g_released.add(static_cast<int64_t>(n));
        event_rule();
        cv_.notify_all();
    }
    void event_rule() {   // caller holds mu_
        if (released_.empty()) event_.clear();
        else event_.signal();
    }
    void release_front() {   // caller holds mu_
        const Entry e = queue_.front();
        queue_.erase(queue_.begin());
        released_.push_back(e.tag);
        played_ += e.frames;
        g_queued.sub(1);
        g_released.add(1);
        event_rule();
    }

    // The buffer's samples, with the stream's volume (and the output device's) applied. A buffer that cannot be read plays as silence,
    // at most one second of it (its descriptor may give any length; the stream's timeline still counts every frame).
    std::vector<int16_t> samples(const Entry& e) {
        const uint64_t frames = e.usable ? e.frames : std::min<uint64_t>(e.frames, rate_);
        std::vector<int16_t> pcm(static_cast<size_t>(frames) * channels_, 0);
        if (e.usable) {
            std::memcpy(pcm.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(e.addr)), pcm.size() * 2);
            const float v = volume_ * audio_device::output_volume();
            if (v != 1.0f) {
                const float g = std::max(v, 0.0f);
                for (int16_t& s : pcm) s = sat16(static_cast<float>(s) * g);
            }
        }
        return pcm;
    }
    // 48 kHz stereo for the bus: 5.1 folded to stereo, other rates interpolated linearly across buffers.
    std::vector<int16_t> to_bus(const std::vector<int16_t>& pcm, uint64_t frames) {
        std::vector<float> st(static_cast<size_t>(frames) * 2);
        for (uint64_t i = 0; i < frames; ++i) {
            const int16_t* f = pcm.data() + i * channels_;
            if (channels_ == 6) {
                const float c = 0.7071f * f[2];
                st[2 * i] = (f[0] + c) + 0.7071f * f[4];
                st[2 * i + 1] = (f[1] + c) + 0.7071f * f[5];
            } else {
                st[2 * i] = f[0];
                st[2 * i + 1] = channels_ > 1 ? f[1] : f[0];
            }
        }
        std::vector<int16_t> out;
        if (rate_ == audio::mixbus::kRate) {
            out.reserve(st.size());
            for (const float x : st) out.push_back(sat16(x));
            return out;
        }
        if (frames == 0) return out;
        if (!has_last_) {
            last_[0] = st[0];
            last_[1] = st[1];
        }
        const double step = static_cast<double>(rate_) / audio::mixbus::kRate;
        auto c = [&](uint64_t k, int ch) -> float { return k == 0 ? last_[ch] : st[2 * (k - 1) + ch]; };
        double p = phase_;
        for (; p < static_cast<double>(frames); p += step) {
            const uint64_t i = static_cast<uint64_t>(std::floor(p));
            const float frac = static_cast<float>(p - static_cast<double>(i));   // the position between frame i and i + 1
            for (int ch = 0; ch < 2; ++ch) out.push_back(sat16(c(i, ch) * (1.0f - frac) + c(i + 1, ch) * frac));
        }
        phase_ = p - static_cast<double>(frames);
        last_[0] = st[2 * (frames - 1)];
        last_[1] = st[2 * (frames - 1) + 1];
        has_last_ = true;
        return out;
    }
    bool shared_output() const {   // caller holds mu_
        return g_started_outs.load() > 1 || audio::mixbus::renderer_active() || audio::mixbus::others_playing(voice_) > 0;
    }

    // One pass of submits; false when the bus had no room (retry after 1 ms). Caller holds mu_.
    bool submit_pending() {
        for (Entry& e : queue_) {
            if (e.submitted) continue;
            if (e.frames > 0) {
                if (out_) {
                    const std::vector<int16_t> pcm = samples(e);
                    if (!std::exchange(logged_first_, true)) {
                        NXLOG_I(tag(), "first buffer: %llu frames (%.1f ms), release margin %d ms", static_cast<unsigned long long>(e.frames),
                                1000.0 * static_cast<double>(e.frames) / rate_, latency_ms());
                    }
                    const uint64_t frames = pcm.size() / channels_;
                    if (shared_output()) {
                        if (audio::mixbus::room(voice_) == 0) return false;
                        const std::vector<int16_t> bus = to_bus(pcm, frames);
                        audio::mixbus::push(voice_, bus.data(), static_cast<uint32_t>(bus.size() / 2));
                        e.mark = {};
                    } else {
                        e.mark = host_write(pcm.data(), static_cast<uint32_t>(frames), rate_, channels_);
                    }
                } else {
                    const uint64_t n = e.frames * channels_ * 2;
                    if (e.addr && mem::is_mapped(e.addr, n)) std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(e.addr)), 0, n);
                }
            }
            if (restart_host_) {
                host_pos_ = static_cast<double>(submitted_);
                host_time_ = MonoClock::now();
                restart_host_ = false;
            }
            e.submitted = true;
            submitted_ += e.frames;
            e.timeline_end = submitted_;
        }
        return true;
    }
    bool due(const Entry& e) {   // caller holds mu_
        if (e.frames == 0) return true;
        if (out_ && e.mark.device) {
            uint64_t pos = 0;
            if (device_position(e.mark.generation, &pos)) {
                const uint64_t margin = static_cast<uint64_t>(rate_) * static_cast<uint64_t>(latency_ms()) / 1000;
                if (pos + margin >= e.mark.end) return true;
                const auto now = MonoClock::now();
                if (pos != dev_last_pos_) {
                    dev_last_pos_ = pos;
                    dev_last_move_ = now;
                }
                if (now - dev_last_move_ < std::chrono::milliseconds(200)) return false;
            }
        }
        return host_pos_ + 0.5 >= static_cast<double>(e.timeline_end);
    }
    void pace() {
        audren::BoostAudioThread();
        audren::TimerResolutionScope::Acquire();
        std::unique_lock<std::mutex> l(mu_);
        dev_last_move_ = MonoClock::now();
        while (!stop_) {
            const bool room = submit_pending();
            const auto now = MonoClock::now();
            host_pos_ = std::min(host_pos_ + std::chrono::duration<double>(now - host_time_).count() * rate_, static_cast<double>(submitted_));
            host_time_ = now;
            while (!queue_.empty() && queue_.front().submitted && due(queue_.front())) release_front();
            if (queue_.empty()) restart_host_ = true;
            const bool more = room && std::any_of(queue_.begin(), queue_.end(), [](const Entry& e) { return !e.submitted; });
            if (!more) cv_.wait_for(l, std::chrono::milliseconds(1));
        }
        l.unlock();
        audren::TimerResolutionScope::Release();
    }

    const bool out_;
    const uint32_t rate_, channels_;
    std::mutex ctl_;   // Start / Stop / shutdown, one at a time
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::deque<Entry> queue_;
    std::deque<uint64_t> released_;
    bool started_ = false, stop_ = false, restart_host_ = true, logged_first_ = false;
    uint64_t played_ = 0, submitted_ = 0;
    double host_pos_ = 0.0;
    MonoClock::time_point host_time_ = MonoClock::now();
    uint64_t dev_last_pos_ = 0;
    MonoClock::time_point dev_last_move_ = MonoClock::now();
    float volume_ = 1.0f;
    uint32_t voice_ = 0;
    double phase_ = 0.0;
    float last_[2] = {0, 0};
    bool has_last_ = false;
    ServiceEvent event_;
};

// Every stream made, for audio_out_reset.
struct Registry {
    std::mutex mu;
    std::vector<std::weak_ptr<Stream>> streams;
};
Registry& registry() {
    static Registry r;
    return r;
}

// The AudioOutBuffer descriptor of an Append: false when it cannot be read.
bool read_descriptor(const ipc::Buffer& b, uint32_t channels, Entry* e) {
    uint64_t d[5] = {};
    const uint64_t n = std::min<uint64_t>(b.size, kDescriptorBytes);
    if (!b.addr || (n > 0 && !mem::is_mapped(b.addr, n))) return false;
    if (n) std::memcpy(d, reinterpret_cast<const void*>(static_cast<uintptr_t>(b.addr)), static_cast<size_t>(n));
    const uint64_t addr = d[1], capacity = d[2], bytes = d[3], offset = d[4];
    e->frames = std::min<uint64_t>(bytes / (channels * 2ull), 0xFFFFFFFFull);
    e->bytes = bytes;
    e->addr = addr + offset;
    e->usable = offset <= capacity && bytes <= capacity - offset && bytes > 0 && mem::is_mapped(addr + offset, bytes);
    if (!e->usable && bytes > 0)
        NXLOG_W("audout", "buffer of %llu bytes at 0x%llx + 0x%llx (capacity 0x%llx) cannot be read: it plays as silence",
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(addr), static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(capacity));
    return true;
}

class StreamService final : public ipc::Service {
public:
    StreamService(std::shared_ptr<Stream> s) : ipc::Service(s->out() ? "IAudioOut" : "IAudioIn"), st_(std::move(s)) {
        const bool out = st_->out();
        auto st = st_;
        const auto state = [st](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(st->started() ? 0u : 1u);
            return 0;
        };
        const auto start = [st](ipc::Request&, ipc::Response&) -> uint32_t { return st->start(); };
        const auto stop = [st](ipc::Request&, ipc::Response&) -> uint32_t {
            st->stop();
            return 0;
        };
        const auto append = [st](ipc::Request& req, ipc::Response&) -> uint32_t {
            Entry e;
            if (!read_descriptor(req.in_buf(0), st->channels(), &e)) return kOperationFailed;
            return st->append(req.arg<uint64_t>(0), e);
        };
        const auto event = [st](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(st->event_handle());
            return 0;
        };
        const auto released = [st](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(st->take_released(req.out_buf(0)));
            return 0;
        };
        const auto contains = [st](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(st->contains(req.arg<uint64_t>(0)) ? 1 : 0);
            return 0;
        };
        const auto count = [st](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(st->queued());
            return 0;
        };
        const auto flush = [st](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(st->flush() ? 1 : 0);
            return 0;
        };
        if (out) {
            add_cmd(0, "GetAudioOutState", state);
            add_cmd(1, "Start", start);
            add_cmd(2, "Stop", stop);
            add_cmd(3, "AppendAudioOutBuffer", append);
            add_cmd(4, "RegisterBufferEvent", event);
            add_cmd(5, "GetReleasedAudioOutBuffers", released);
            add_cmd(6, "ContainsAudioOutBuffer", contains);
            add_cmd(7, "AppendAudioOutBufferAuto", append);
            add_cmd(8, "GetReleasedAudioOutBuffersAuto", released);
            add_cmd(9, "GetAudioOutBufferCount", count);
            add_cmd(10, "GetAudioOutPlayedSampleCount", [st](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push<uint64_t>(st->played());
                return 0;
            });
            add_cmd(11, "FlushAudioOutBuffers", flush);
            add_cmd(12, "SetAudioOutVolume", [st](ipc::Request& req, ipc::Response&) -> uint32_t {
                st->set_volume(req.arg<float>(0));
                return 0;
            });
            add_cmd(13, "GetAudioOutVolume", [st](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push<float>(st->volume());
                return 0;
            });
        } else {
            add_cmd(0, "GetAudioInState", state);
            add_cmd(1, "Start", start);
            add_cmd(2, "Stop", stop);
            add_cmd(3, "AppendAudioInBuffer", append);
            add_cmd(4, "RegisterBufferEvent", event);
            add_cmd(5, "GetReleasedAudioInBuffers", released);
            add_cmd(6, "ContainsAudioInBuffer", contains);
            add_cmd(8, "AppendAudioInBufferAuto", append);
            add_cmd(9, "GetReleasedAudioInBuffersAuto", released);
            add_cmd(11, "GetAudioInBufferCount", count);
            add_cmd(14, "FlushAudioInBuffers", flush);
        }
    }
    ~StreamService() override { st_->shutdown(); }
    void on_close() override {   // the session ended: the stream stops and gives its buffers back
        st_->shutdown();
    }

private:
    std::shared_ptr<Stream> st_;
};

// The device name into a List / Open reply buffer; false when nothing could be written.
bool write_name(const ipc::Buffer& b, const char* name) {
    const uint64_t n = std::min<uint64_t>(b.size, kNameBytes);
    if (n == 0 || !b.addr || !mem::is_mapped(b.addr, n)) return false;
    char padded[kNameBytes] = {};
    std::strncpy(padded, name, sizeof padded - 1);
    buf_write(b, padded, n);
    return true;
}
std::string read_name(const ipc::Buffer& b) {
    const uint64_t n = std::min<uint64_t>(b.size, kNameBytes);
    if (n == 0 || !b.addr || !mem::is_mapped(b.addr, n)) return {};
    const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(b.addr));
    return std::string(p, strnlen(p, static_cast<size_t>(n)));
}

uint32_t open_stream(ipc::Request& req, ipc::Response& rsp, bool out) {
    const char* device = out ? kOutName : kInName;
    const std::string wanted = read_name(req.in_buf(0));
    if (!wanted.empty() && wanted != device) {
        NXLOG_W(out ? "audout" : "audin", "Open: no device named \"%s\"", wanted.c_str());
        return kNotFound;
    }
    const uint32_t asked_rate = req.arg<uint32_t>(0);
    const uint32_t asked_channels = req.arg<uint16_t>(4);
    const uint32_t rate = asked_rate < 8000 || asked_rate > 192000 ? 48000 : asked_rate;
    const uint32_t channels = out ? (asked_channels == 6 ? 6u : 2u)
                                  : (asked_channels == 1 || asked_channels == 4 || asked_channels == 6 ? asked_channels : 2u);
    auto st = std::make_shared<Stream>(out, rate, channels);
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        r.streams.erase(std::remove_if(r.streams.begin(), r.streams.end(), [](const std::weak_ptr<Stream>& w) { return w.expired(); }),
                        r.streams.end());
        r.streams.push_back(st);
    }
    write_name(req.out_buf(0), device);
    NXLOG_I(out ? "audout" : "audin", "Open %s: %u Hz, %u channels", device, rate, channels);
    rsp.push<uint32_t>(rate);
    rsp.push<uint32_t>(channels);
    rsp.push<uint32_t>(2);   // PcmInt16
    rsp.push<uint32_t>(1);   // Stopped
    rsp.push_object(std::make_shared<StreamService>(st));
    return 0;
}

std::shared_ptr<ipc::Service> make_manager(bool out) {
    return make_iface(out ? "audout:u" : "audin:u", [out](ipc::Service& s) {
        const auto list = [out](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(write_name(req.out_buf(0), out ? kOutName : kInName) ? 1u : 0u);
            return 0;
        };
        const auto open = [out](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_stream(req, rsp, out); };
        if (out) {
            s.add_cmd(0, "ListAudioOuts", list);
            s.add_cmd(1, "OpenAudioOut", open);
            s.add_cmd(2, "ListAudioOutsAuto", list);
            s.add_cmd(3, "OpenAudioOutAuto", open);
        } else {
            s.add_cmd(0, "ListAudioIns", list);
            s.add_cmd(1, "OpenAudioIn", open);
            s.add_cmd(2, "ListAudioInsAuto", list);
            s.add_cmd(3, "OpenAudioInAuto", open);
            s.add_cmd(4, "ListAudioInsAutoFiltered", list);
        }
    });
}

}  // namespace

std::shared_ptr<ipc::Service> audio_out_manager() { return make_manager(true); }
std::shared_ptr<ipc::Service> audio_in_manager() { return make_manager(false); }

void register_audio_out() {
    add("audout:u", [] { return make_manager(true); });
    add("audin:u", [] { return make_manager(false); });
}

void audio_out_reset() {
    std::vector<std::shared_ptr<Stream>> live;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        const std::vector<std::weak_ptr<Stream>> refs = std::exchange(r.streams, {});   // the registry ends up empty
        for (const std::weak_ptr<Stream>& ref : refs) {
            std::shared_ptr<Stream> alive = ref.lock();
            if (alive) live.push_back(std::move(alive));
        }
    }
    for (const auto& s : live) s->shutdown();
    HostSink& h = host();
    std::lock_guard<std::mutex> lk(h.mu);
    h.sink.reset();
    h.rate = h.channels = 0;
    h.ok = false;
}

}  // namespace nxrt::services
