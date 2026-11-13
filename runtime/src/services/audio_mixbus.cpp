// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The mix bus (audio_mixbus.h).
#include "audio_mixbus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"

namespace nxrt::audio::mixbus {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kTargetFrames = kRate * 60 / 1000;   // 60 ms ahead at most
constexpr uint32_t kNoClockBlock = kRate * 5 / 1000;     // a sink without a clock takes a block at a time
std::atomic<uint32_t> g_idle_ms{50};   // a voice plays this long after its last push (or its open); the renderer too
Clock::duration idle() { return std::chrono::milliseconds(g_idle_ms.load(std::memory_order_relaxed)); }

struct Voice {
    std::deque<int16_t> pcm;   // interleaved stereo
    Clock::time_point seen{};  // the last push (or the open)
    std::string who;
};

struct Bus {
    std::mutex mu;
    std::map<uint32_t, Voice> voices;
    uint32_t next = 1;
    Clock::time_point renderer_seen{};           // the renderer's last mix_into
    std::shared_ptr<AudioSink> direct;           // the sink the bus opened for itself (no renderer), and its opening
    uint64_t direct_opens = 0;
    bool direct_logged = false;
};
Bus& bus() {
    static Bus b;
    return b;
}

bool renderer_active_locked(const Bus& b) {
    return b.renderer_seen != Clock::time_point{} && Clock::now() - b.renderer_seen < std::chrono::milliseconds(50);
}
bool playing(const Voice& v, Clock::time_point now) { return v.seen != Clock::time_point{} && now - v.seen < idle(); }
uint32_t held(const Voice& v) { return static_cast<uint32_t>(v.pcm.size() / 2); }

// The sink the bus writes itself, open at the bus format (opened again when someone else opened it since: audio_sink.h open_counted).
std::shared_ptr<AudioSink> direct_sink_locked(Bus& b) {
    std::shared_ptr<AudioSink> s = sink();
    if (b.direct == s && s->opens() == b.direct_opens) return s;
    if (!s->open_counted(kRate, 2)) {
        NXLOG_W("audio", "mix bus: host sink %s would not open at %u Hz stereo", s->name(), kRate);
        b.direct.reset();
        return nullptr;
    }
    b.direct = s;
    b.direct_opens = s->opens();
    if (!b.direct_logged) {
        b.direct_logged = true;
        NXLOG_I("audio", "mix bus: no audio renderer running, the bus mixes its voices into host sink %s", s->name());
    }
    return s;
}

// No renderer: as many frames as every playing voice holds (a voice not playing gives what it has), mixed, within the sink's room.
void pump_locked(Bus& b) {
    const Clock::time_point now = Clock::now();
    uint32_t n = UINT32_MAX;
    bool any = false;
    for (const auto& [id, v] : b.voices) {
        if (!playing(v, now)) continue;
        any = true;
        n = std::min(n, held(v));
    }
    if (!any) {
        n = 0;
        for (const auto& [id, v] : b.voices) n = std::max(n, held(v));
    }
    if (!n) return;
    std::shared_ptr<AudioSink> s = direct_sink_locked(b);
    if (!s) {
        for (auto& [id, v] : b.voices) v.pcm.clear();
        return;
    }
    if (s->has_device_clock()) {
        const uint32_t q = s->queued_frames();
        n = std::min(n, q >= kTargetFrames ? 0 : kTargetFrames - q);
        if (!n) return;
    }
    std::vector<int32_t> mix(size_t{n} * 2, 0);
    for (auto& [id, v] : b.voices) {
        const uint32_t k = std::min(n, held(v));
        for (size_t i = 0; i < size_t{k} * 2; ++i) mix[i] += v.pcm[i];
        v.pcm.erase(v.pcm.begin(), v.pcm.begin() + static_cast<std::ptrdiff_t>(size_t{k} * 2));
    }
    std::vector<int16_t> out(mix.size());
    for (size_t i = 0; i < mix.size(); ++i) out[i] = static_cast<int16_t>(std::clamp(mix[i], -32768, 32767));
    s->write(out.data(), n);
}

}  // namespace

uint32_t open_voice(const char* who) {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    const uint32_t id = b.next++;
    Voice& v = b.voices[id];
    v.who = who ? who : "?";
    v.seen = Clock::now();   // playing from now: the bus waits for its first frames before it plays the others alone
    return id;
}

void close_voice(uint32_t voice) {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    b.voices.erase(voice);
    if (!renderer_active_locked(b)) pump_locked(b);   // the others no longer wait for it
}

uint32_t room(uint32_t voice) {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    auto it = b.voices.find(voice);
    const uint32_t mine = it == b.voices.end() ? 0 : held(it->second);
    uint32_t ahead = mine;
    if (!renderer_active_locked(b)) {
        std::shared_ptr<AudioSink> s = sink();
        if (!s->has_device_clock()) return mine >= kNoClockBlock ? 0 : kNoClockBlock - mine;
        ahead += b.direct == s && s->opens() == b.direct_opens ? s->queued_frames() : 0;
    }
    return ahead >= kTargetFrames ? 0 : kTargetFrames - ahead;
}

void push(uint32_t voice, const int16_t* stereo, uint32_t frames) {
    if (!frames) return;
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    auto it = b.voices.find(voice);
    if (it == b.voices.end()) return;
    Voice& v = it->second;
    v.seen = Clock::now();
    const bool had_room = held(v) < kTargetFrames;   // a producer that waited for room() keeps its whole block
    v.pcm.insert(v.pcm.end(), stereo, stereo + size_t{frames} * 2);
    const size_t cap = size_t{kTargetFrames} * 2 * 2;   // else never more than twice the target: the oldest frames go
    if (!had_room && v.pcm.size() > cap) v.pcm.erase(v.pcm.begin(), v.pcm.begin() + static_cast<std::ptrdiff_t>(v.pcm.size() - cap));
    if (!renderer_active_locked(b)) pump_locked(b);
}

uint32_t mix_into(int16_t* pcm, uint32_t frames, uint32_t channels, uint32_t rate) {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    b.renderer_seen = Clock::now();
    b.direct.reset();   // the renderer owns the sink now
    if (rate != kRate || channels < 2) return 0;
    uint32_t most = 0;
    for (auto& [id, v] : b.voices) {
        const uint32_t n = std::min(frames, held(v));
        for (uint32_t f = 0; f < n; ++f)
            for (uint32_t c = 0; c < 2; ++c) {
                const int32_t x = int32_t{pcm[size_t{f} * channels + c]} + v.pcm[size_t{f} * 2 + c];
                pcm[size_t{f} * channels + c] = static_cast<int16_t>(std::clamp(x, -32768, 32767));
            }
        v.pcm.erase(v.pcm.begin(), v.pcm.begin() + static_cast<std::ptrdiff_t>(size_t{n} * 2));
        most = std::max(most, n);
    }
    return most;
}

bool renderer_active() {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    return renderer_active_locked(b);
}

uint32_t others_playing(uint32_t voice) {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    const Clock::time_point now = Clock::now();
    uint32_t n = 0;
    for (const auto& [id, v] : b.voices)
        if (id != voice && playing(v, now)) ++n;
    return n;
}

void set_idle_ms_for_tests(uint32_t ms) { g_idle_ms.store(ms, std::memory_order_relaxed); }

void reset() {
    Bus& b = bus();
    std::lock_guard<std::mutex> lk(b.mu);
    b.voices.clear();
    b.renderer_seen = Clock::time_point{};
    b.direct.reset();
    b.direct_opens = 0;
}

}  // namespace nxrt::audio::mixbus
