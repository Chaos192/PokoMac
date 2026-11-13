// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Constants.cs (Ryubing fork, MIT). The C# static class is kept as struct Constants with the same member names.
// Six of these values already exist at namespace scope in audren_types.h; they are forwarded here, not defined twice.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audren_common.h"
#include "audren_types.h"

namespace nxrt::audren {

struct Constants {
    // The default device output name.
    static constexpr const char* DefaultDeviceOutputName = "DeviceOut";
    // The default device input name.
    static constexpr const char* DefaultDeviceInputName = "BuiltInHeadset";
    // The maximum number of channels supported. (6 channels for 5.1 surround)
    static constexpr int32_t ChannelCountMax = static_cast<int32_t>(::nxrt::audren::ChannelCountMax);
    // The maximum number of channels supported per voice.
    static constexpr int32_t VoiceChannelCountMax = static_cast<int32_t>(::nxrt::audren::VoiceChannelCountMax);
    // The maximum count of mix buffer supported per operations (volumes, mix effect, ...)
    static constexpr int32_t MixBufferCountMax = static_cast<int32_t>(::nxrt::audren::MixBufferCountMax);
    // The maximum count of wavebuffer per voice.
    static constexpr int32_t VoiceWaveBufferCount = static_cast<int32_t>(::nxrt::audren::VoiceWaveBufferCount);
    // The maximum count of biquad filter per voice.
    static constexpr int32_t VoiceBiquadFilterCount = static_cast<int32_t>(::nxrt::audren::VoiceBiquadFilterCount);
    // The lowest priority that a voice can have.
    static constexpr int32_t VoiceLowestPriority = 0xFF;
    // The highest priority that a voice can have.
    static constexpr int32_t VoiceHighestPriority = 0;
    // Maximum ErrorInfo that can be returned by BehaviourErrorInfoOutStatus.
    static constexpr int32_t MaxErrorInfos = static_cast<int32_t>(::nxrt::audren::MaxErrorInfos);
    // Default alignment for buffers.
    static constexpr int32_t BufferAlignment = 0x40;
    // Alignment required for the work buffer.
    static constexpr int32_t WorkBufferAlignment = 0x1000;
    // Alignment required for every performance metrics frame.
    static constexpr int32_t PerformanceMetricsPerFramesSizeAlignment = 0x100;
    // The id of the final mix.
    static constexpr int32_t FinalMixId = 0;
    // The id defining an unused mix id.
    static constexpr int32_t UnusedMixId = 0x7FFFFFFF;  // int.MaxValue
    // The id defining an unused splitter id as a signed integer.
    static constexpr int32_t UnusedSplitterIdInt = -1;
    // The id defining an unused splitter id.
    static constexpr uint32_t UnusedSplitterId = 0xFFFFFFFFu;  // uint.MaxValue
    // The id of invalid/unused node id.
    static constexpr int32_t InvalidNodeId = -268435456;
    // The indice considered invalid for processing order.
    static constexpr int32_t InvalidProcessingOrder = -1;
    // The maximum number of audio renderer sessions allowed to be created system wide.
    static constexpr int32_t AudioRendererSessionCountMax = 2;
    // The maximum number of audio output sessions allowed to be created system wide.
    static constexpr int32_t AudioOutSessionCountMax = 12;
    // The maximum number of audio input sessions allowed to be created system wide.
    static constexpr int32_t AudioInSessionCountMax = 4;
    // Maximum buffers supported by one audio device session.
    static constexpr int32_t AudioDeviceBufferCountMax = 32;
    // The target sample rate of the audio renderer. (48kHz)
    static constexpr uint32_t TargetSampleRate = 48000;
    // The target sample size of the audio renderer. (PCM16)
    static constexpr int32_t TargetSampleSize = static_cast<int32_t>(sizeof(uint16_t));
    // The target sample count per audio renderer update.
    static constexpr int32_t TargetSampleCount = 240;
    // The size of an upsampler entry to process upsampling to TargetSampleRate.
    static constexpr int32_t UpSampleEntrySize = TargetSampleCount * VoiceChannelCountMax;
    // The target audio latency computed from TargetSampleRate and TargetSampleCount. (5.00 ms)
    static constexpr int32_t AudioProcessorMaxUpdateTimeTarget =
        1000000000 / (static_cast<int32_t>(TargetSampleRate) / TargetSampleCount);
    // The maximum update time of the DSP on original hardware. (5.76 ms)
    static constexpr int32_t AudioProcessorMaxUpdateTime = 5760000;
    // The maximum update time per audio renderer session.
    static constexpr int32_t AudioProcessorMaxUpdateTimePerSessions =
        AudioProcessorMaxUpdateTime / AudioRendererSessionCountMax;
    // Guest timer frequency used for system ticks.
    static constexpr int32_t TargetTimerFrequency = 19200000;
    // The default coefficients used for standard 5.1 surround to stereo downmixing.
    // C# static readonly float[]: allocated once, so a const std::vector.
    static inline const std::vector<float> DefaultSurroundToStereoCoefficients = {1.0f, 0.707f, 0.251f, 0.707f};
};

}  // namespace nxrt::audren
