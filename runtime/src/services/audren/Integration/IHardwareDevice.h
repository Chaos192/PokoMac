// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Integration/IHardwareDevice.cs (Ryubing fork, MIT): the output device a DeviceSinkCommand writes PCM16 to.
#pragma once
#include "../Constants.h"
#include "../audren_common.h"

namespace nxrt::audren {

class IHardwareDevice {
  public:
    virtual ~IHardwareDevice() = default;
    virtual void SetVolume(float volume) = 0;
    virtual float GetVolume() = 0;
    virtual uint32_t GetSampleRate() = 0;
    virtual uint32_t GetChannelCount() = 0;
    // Appends new PCM16 samples (interleaved, `channelCount` channels).
    virtual void AppendBuffer(Span<const int16_t> data, uint32_t channelCount) = 0;
    // Nanoseconds of audio the device holds queued, measured on the device's own clock; -1 when the device has no clock (the renderer
    // then paces itself by the host clock). Not in the Ryujinx interface: the C# devices block in AppendBuffer instead.
    virtual int64_t GetQueuedNanoseconds() { return -1; }
    // True if the audio renderer must downmix to this device's channel count (anything but 6 channels).
    bool NeedDownmixing() {
        const uint32_t channelCount = GetChannelCount();
        NXAUDREN_ASSERT(channelCount > 0 && channelCount <= static_cast<uint32_t>(Constants::ChannelCountMax));
        return channelCount != static_cast<uint32_t>(Constants::ChannelCountMax);
    }
};

}  // namespace nxrt::audren
