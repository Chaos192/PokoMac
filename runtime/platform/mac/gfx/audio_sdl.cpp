// SPDX-License-Identifier: GPL-3.0-or-later
// macOS host audio sink: SDL3 audio streams on Core Audio (the role of src/services/audio_xaudio2.cpp on Windows).
// [audio] device picks the output device (an exact device name, or a part of one; "default" or empty follows the system default, also when
// it changes). write() queues into the stream and drops what goes beyond kMaxQueuedMs, so it never blocks; queued_frames() is what the
// device has not pulled yet, which drains at the device's own clock. The process master volume is applied as the stream's gain.
#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <mutex>

namespace nxrt::audio {
namespace {

constexpr uint32_t kMaxQueuedMs = 250;
constexpr const char* kTag = "audio";

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool ensure_sdl_audio() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
        ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!ok) NXLOG_W(kTag, "SDL audio init failed: %s", SDL_GetError());
    });
    return ok;
}

SDL_AudioDeviceID pick_device(const std::string& want) {
    if (want.empty() || lower_ascii(want) == "default") return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    SDL_AudioDeviceID found = 0;
    const std::string needle = lower_ascii(want);
    for (int pass = 0; pass < 2 && !found; ++pass) {
        for (int i = 0; i < n && !found; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (!name) continue;
            if (pass == 0 ? want == name : lower_ascii(name).find(needle) != std::string::npos) found = ids[i];
        }
    }
    SDL_free(ids);
    if (!found) {
        NXLOG_W(kTag, "coreaudio: [audio] device = %s matches no output device; using the default device", want.c_str());
        return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }
    return found;
}

class SdlSink final : public AudioSink {
public:
    ~SdlSink() override { close(); }
    bool open(uint32_t sample_rate, uint32_t channels) override {
        close();
        if (!ensure_sdl_audio()) return false;
        SDL_AudioSpec spec{SDL_AUDIO_S16LE, static_cast<int>(channels), static_cast<int>(sample_rate)};
        stream_ = SDL_OpenAudioDeviceStream(pick_device(config_get_str("audio", "device", "default")), &spec, nullptr, nullptr);
        if (!stream_) {
            NXLOG_W(kTag, "coreaudio: no audio output device (%s)", SDL_GetError());
            return false;
        }
        rate_ = sample_rate;
        channels_ = channels;
        applied_volume_ = -1.0f;
        apply_volume();
        SDL_ResumeAudioStreamDevice(stream_);
        started_ = false;
        const char* dn = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
        NXLOG_I(kTag, "coreaudio: %u Hz, %u channels on %s", sample_rate, channels, dn ? dn : "the default device");
        return true;
    }
    void close() override {
        if (!stream_) return;
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        NXLOG_I(kTag, "%s", format_stats(name(), stats()).c_str());
    }
    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        written_ += frames;
        if (!stream_ || !frames) return 0;
        apply_volume();
        const uint32_t q = queued_frames();
        if (started_ && q == 0) {
            if (!starved_) { ++underruns_; starved_ = true; }
        } else {
            starved_ = false;
        }
        if (started_) low_ = std::min(low_, q);
        const uint32_t cap = rate_ * kMaxQueuedMs / 1000;
        const uint32_t take = q >= cap ? 0 : std::min(frames, cap - q);
        if (take && SDL_PutAudioStreamData(stream_, pcm, static_cast<int>(take * channels_ * sizeof(int16_t)))) {
            accepted_ += take;
            started_ = true;
            peak_ = std::max(peak_, q + take);
        }
        dropped_ += frames - take;
        return take;
    }
    uint32_t queued_frames() override {
        if (!stream_) return 0;
        const int b = SDL_GetAudioStreamQueued(stream_);
        return b > 0 ? static_cast<uint32_t>(b) / (channels_ * static_cast<uint32_t>(sizeof(int16_t))) : 0;
    }
    bool has_device_clock() const override { return true; }
    const char* name() const override { return "coreaudio"; }
    SinkStats stats() override {
        SinkStats s;
        s.frames_written = written_;
        s.frames_accepted = accepted_;
        s.frames_dropped = dropped_;
        s.underruns = underruns_;
        s.queued_peak_frames = peak_;
        s.queued_low_frames = low_ == 0xffffffffu ? 0 : low_;
        s.sample_rate = rate_;
        return s;
    }

private:
    void apply_volume() {
        const float v = effective_volume();
        if (v != applied_volume_) {
            SDL_SetAudioStreamGain(stream_, v);
            applied_volume_ = v;
        }
    }
    SDL_AudioStream* stream_ = nullptr;
    uint32_t rate_ = 0, channels_ = 0;
    float applied_volume_ = -1.0f;
    bool started_ = false, starved_ = false;
    uint64_t written_ = 0, accepted_ = 0, dropped_ = 0, underruns_ = 0;
    uint32_t peak_ = 0, low_ = 0xffffffffu;
};

}  // namespace

// [audio] sink = auto / xaudio2 (the Windows names) select this sink on macOS.
std::shared_ptr<AudioSink> make_xaudio2_sink() {
    auto s = std::make_shared<SdlSink>();
    if (!s->open(48000, 2)) return nullptr;
    s->close();
    return s;
}

std::vector<OutputDevice> list_output_devices() {
    std::vector<OutputDevice> out;
    if (!ensure_sdl_audio()) return out;
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        if (!name) continue;
        out.push_back({widen(name), name, false});
    }
    SDL_free(ids);
    return out;
}

}  // namespace nxrt::audio
