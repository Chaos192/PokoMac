// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Implementation of HostAudioDevice / HostAudioDeviceDriver (see HostAudioDevice.h).
#include "HostAudioDevice.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

#include "../Dsp/PcmHelper.h"
#include "../../audio_device.h"
#include "../../audio_mixbus.h"
#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::audren {

namespace {

// The sink this device last opened, with the parameters it was opened with. All HostAudioDevice instances share the host sink, so the
// open state is process wide. Guarded by gSinkMutex.
std::mutex gSinkMutex;
std::shared_ptr<::nxrt::audio::AudioSink> gOpenSink;
uint64_t gOpenOpens = 0;   // gOpenSink->opens() after this device's open: another user (audout, the mix bus) opened it since when it moved
uint32_t gOpenRate = 0;
uint32_t gOpenChannels = 0;

// The device that owns the host sink (see HostAudioDevice.h), and when it last wrote. Guarded by gOwnerMutex.
std::mutex gOwnerMutex;
const HostAudioDevice* gOwner = nullptr;
std::chrono::steady_clock::time_point gOwnerSeen{};
constexpr auto kOwnerIdle = std::chrono::milliseconds(50);

// True when `self` may write the sink (it owns it, or takes it over); false when another device that owns it wrote in the last 50 ms.
bool own_sink(const HostAudioDevice* self) {
    std::lock_guard<std::mutex> lk(gOwnerMutex);
    const auto now = std::chrono::steady_clock::now();
    if (gOwner && gOwner != self && now - gOwnerSeen < kOwnerIdle) return false;
    if (gOwner != self && gOwner) NXLOG_I("AudioRenderer", "another audio renderer's output owns the host sink now");
    gOwner = self;
    gOwnerSeen = now;
    return true;
}

}  // namespace

HostAudioDevice::HostAudioDevice(uint32_t sampleRate, uint32_t channelCount) : _sampleRate(sampleRate), _channelCount(channelCount) {
    NXAUDREN_ASSERT(channelCount == 2 || channelCount == 6);
}

HostAudioDevice::~HostAudioDevice() {
    if (_voice) ::nxrt::audio::mixbus::close_voice(_voice);
    std::lock_guard<std::mutex> lk(gOwnerMutex);
    if (gOwner == this) gOwner = nullptr;
}

void HostAudioDevice::SetVolume(float volume) { _volume = volume; }

float HostAudioDevice::GetVolume() { return _volume; }

uint32_t HostAudioDevice::GetSampleRate() { return _sampleRate; }

uint32_t HostAudioDevice::GetChannelCount() { return _channelCount; }

void HostAudioDevice::AppendBuffer(Span<const int16_t> data, uint32_t channelCount) {
    NXAUDREN_ASSERT(channelCount > 0);

    const size_t frames = data.Length / channelCount;

    if (frames == 0) {
        return;
    }

    // Interleaved, scaled, at this device's channel count.
    std::vector<int16_t> out(frames * _channelCount, 0);

    const uint32_t copyChannels = channelCount < _channelCount ? channelCount : _channelCount;

    for (size_t frame = 0; frame < frames; frame++) {
        for (uint32_t c = 0; c < copyChannels; c++) {
            const int16_t sample = data[frame * channelCount + c];

            // C# PcmHelper.Saturate(sample * volume) for the scaled case; the unit volume is a copy.
            out[frame * _channelCount + c] = _volume == 1.0f ? sample : PcmHelper::Saturate(static_cast<float>(sample) * _volume);
        }
    }

    // Another renderer owns the host sink: these frames go to the mix bus, down to stereo (5.1: the centre and the rears at -3 dB into
    // the front pair; the LFE is left out), and the owner plays them inside its own blocks.
    if (!own_sink(this)) {
        std::vector<int16_t> stereo(frames * 2);
        for (size_t f = 0; f < frames; f++) {
            const int16_t* s = &out[f * _channelCount];
            if (_channelCount == 2) {
                stereo[f * 2] = s[0];
                stereo[f * 2 + 1] = s[1];
            } else {   // FL, FR, FC, LFE, RL, RR
                const float c = 0.7071f * static_cast<float>(s[2]);
                stereo[f * 2] = PcmHelper::Saturate(static_cast<float>(s[0]) + c + 0.7071f * static_cast<float>(s[4]));
                stereo[f * 2 + 1] = PcmHelper::Saturate(static_cast<float>(s[1]) + c + 0.7071f * static_cast<float>(s[5]));
            }
        }
        if (!_voice) _voice = ::nxrt::audio::mixbus::open_voice("audio renderer");
        ::nxrt::audio::mixbus::push(_voice, stereo.data(), static_cast<uint32_t>(frames));
        return;
    }

    // The other producers' PCM (another renderer's, the Switch 2 mixer's streams: audio_mixbus.h) is heard through this block: the host
    // sink plays one stream.
    ::nxrt::audio::mixbus::mix_into(out.data(), static_cast<uint32_t>(frames), _channelCount, _sampleRate);

    // The game's output device volume (IAudioDevice SetAudioDeviceOutputVolume) applies to everything it plays.
    if (const float dv = ::nxrt::services::audio_device::output_volume(); dv != 1.0f) {
        for (int16_t& s : out) {
            s = PcmHelper::Saturate(static_cast<float>(s) * dv);
        }
    }

    std::lock_guard<std::mutex> guard(gSinkMutex);

    std::shared_ptr<::nxrt::audio::AudioSink> sink = ::nxrt::audio::sink();

    if (sink != gOpenSink || sink->opens() != gOpenOpens || _sampleRate != gOpenRate || _channelCount != gOpenChannels) {
        // A new sink object was installed with set_sink (it closes the previous one), or the stream format changed: (re)open.
        if (gOpenSink != nullptr && sink == gOpenSink) {
            gOpenSink->close();
        }

        if (!sink->open_counted(_sampleRate, _channelCount)) {
            NXLOG_W("AudioRenderer", "host audio sink %s failed to open (%u Hz, %u channels)", sink->name(), _sampleRate, _channelCount);
            gOpenSink.reset();
            return;
        }

        gOpenSink = sink;
        gOpenOpens = sink->opens();
        gOpenRate = _sampleRate;
        gOpenChannels = _channelCount;
    }

    sink->write(out.data(), static_cast<uint32_t>(frames));
}

int64_t HostAudioDevice::GetQueuedNanoseconds() {
    // Under the mutex AppendBuffer holds while it opens, closes and writes the sink: the sink cannot be reopened under this call.
    std::lock_guard<std::mutex> guard(gSinkMutex);

    if (gOpenSink == nullptr || !gOpenSink->has_device_clock() || _sampleRate == 0) {
        return -1;   // nothing written yet, or a sink without a clock (the renderer paces itself by the host clock)
    }

    return static_cast<int64_t>(gOpenSink->queued_frames()) * 1000000000ll / static_cast<int64_t>(_sampleRate);
}

HostAudioDeviceDriver::HostAudioDeviceDriver()
    : _configuredChannels(::nxrt::config_get_int("audio", "channels", 2) == 6 ? 6u : 2u) {}

std::unique_ptr<IHardwareDevice> HostAudioDeviceDriver::OpenHardwareDevice(uint32_t sampleRate, uint32_t channelCount) {
    return std::make_unique<HostAudioDevice>(sampleRate, channelCount);
}

bool HostAudioDeviceDriver::SupportsChannelCount(uint32_t channelCount) const { return channelCount == _configuredChannels; }

}  // namespace nxrt::audren
