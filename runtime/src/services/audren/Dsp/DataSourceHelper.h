// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/DataSourceHelper.cs (Ryubing fork, MIT). Static class -> struct with static members.
//
// Not ported: ToFloatAvx/ToFloatSse2/ToFloatAdvSimd/ToIntAvx/ToIntSse2/ToIntAdvSimd (SIMD variants, see audren_common.h). ToFloat and
// ToInt call the scalar *Slow variants.
//
// Deviations:
//   * C# memoryManager parameter dropped; guest reads use GuestSpan / GuestPtr.
//   * VoiceState (audren_types.h) is a plain layout struct, so the C# method VoiceState.MarkEndOfBufferWaveBufferProcessing is
//     provided here as the static DataSourceHelper::MarkEndOfBufferWaveBufferProcessing. Move it if the VoiceState port adds methods.
//   * VoiceState / WaveBuffer / AdpcmLoopContext fields keep the snake_case names of audren_types.h (not the C# PascalCase).
//   * (int)float conversions use FloatToInt32 (NaN -> 0, out of range saturates), the .NET 9 behaviour, instead of C++ UB.
//   * ResamplerHelper.Resample is called but ResamplerHelper is a separate port (Dsp/ResamplerHelper.h). Not ported here.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../audren_common.h"
#include "../audren_types.h"
#include "AdpcmHelper.h"
#include "PcmHelper.h"
#include "ResamplerHelper.h"

namespace nxrt::audren {

struct DataSourceHelper {
    static constexpr int32_t FixedPointPrecision = 15;

    struct WaveBufferInformation {
        uint32_t SourceSampleRate;
        float Pitch;
        uint64_t ExtraParameter;
        uint64_t ExtraParameterSize;
        int32_t ChannelIndex;
        int32_t ChannelCount;
        ::nxrt::audren::DecodingBehaviour DecodingBehaviour;
        SampleRateConversionQuality SrcQuality;
        ::nxrt::audren::SampleFormat SampleFormat;
    };

    static int32_t GetPitchLimitBySrcQuality(SampleRateConversionQuality quality) {
        switch (quality) {
            case SampleRateConversionQuality::Default:
            case SampleRateConversionQuality::Low:
                return 4;
            case SampleRateConversionQuality::High:
                return 8;
        }
        NXAUDREN_FATAL("ArgumentException: unknown SampleRateConversionQuality");
    }

    // (int)MathF.Truncate(x) with NaN -> 0 and saturation, as .NET 9 does.
    static int32_t FloatToInt32(float value) {
        if (value != value) return 0;
        if (value >= 2147483648.0f) return INT32_MAX;
        if (value <= -2147483648.0f) return INT32_MIN;
        return static_cast<int32_t>(value);
    }

    static bool HasFlag(::nxrt::audren::DecodingBehaviour value, ::nxrt::audren::DecodingBehaviour flag) {
        uint16_t f = static_cast<uint16_t>(flag);
        return (static_cast<uint16_t>(value) & f) == f;
    }

    // VoiceState.MarkEndOfBufferWaveBufferProcessing (Renderer/Common/VoiceState.cs).
    static void MarkEndOfBufferWaveBufferProcessing(VoiceState& voiceState, WaveBuffer& waveBuffer, int32_t& waveBufferIndex, uint32_t& waveBufferConsumed, uint64_t& playedSampleCount) {
        voiceState.is_wave_buffer_valid[waveBufferIndex++] = false;
        voiceState.loop_count = 0;
        waveBufferConsumed++;

        if (waveBufferIndex >= static_cast<int32_t>(VoiceWaveBufferCount)) {
            waveBufferIndex = 0;
        }

        if (waveBuffer.is_end_of_stream) {
            playedSampleCount = 0;
        }
    }

    static void ProcessWaveBuffers(Span<float> outputBuffer, WaveBufferInformation& info, Span<WaveBuffer> wavebuffers, VoiceState& voiceState, uint32_t targetSampleRate, int32_t sampleCount) {
        constexpr int32_t TempBufferSize = 0x3F00;

        // C# stackalloc: zero-initialised.
        int16_t tempBuffer[TempBufferSize] = {};

        float sampleRateRatio = static_cast<float>(info.SourceSampleRate) / static_cast<float>(targetSampleRate) * info.Pitch;

        float fraction = voiceState.fraction;
        int32_t waveBufferIndex = static_cast<int32_t>(voiceState.wave_buffer_index);
        uint64_t playedSampleCount = voiceState.played_sample_count;
        int32_t offset = voiceState.offset;
        uint32_t waveBufferConsumed = voiceState.wave_buffer_consumed;

        int32_t pitchMaxLength = GetPitchLimitBySrcQuality(info.SrcQuality);

        int32_t totalNeededSize = FloatToInt32(std::trunc(fraction + sampleRateRatio * static_cast<float>(sampleCount)));

        if (totalNeededSize + pitchMaxLength <= TempBufferSize && totalNeededSize >= 0) {
            int32_t sourceSampleCountToProcess = sampleCount;

            int32_t maxSampleCountPerIteration = std::min(FloatToInt32(std::trunc((static_cast<float>(TempBufferSize) - fraction) / sampleRateRatio)), sampleCount);

            [[maybe_unused]] bool isStarving = false;  // only read by the C# Debug.Assert

            int32_t i = 0;

            while (i < sourceSampleCountToProcess) {
                int32_t tempBufferIndex = 0;

                if (!HasFlag(info.DecodingBehaviour, ::nxrt::audren::DecodingBehaviour::SkipPitchAndSampleRateConversion)) {
                    for (int32_t k = 0; k < pitchMaxLength; k++) {
                        tempBuffer[k] = voiceState.pitch[k];
                    }
                    tempBufferIndex += pitchMaxLength;
                }

                int32_t sampleCountToProcess = std::min(sourceSampleCountToProcess, maxSampleCountPerIteration);

                int32_t y = 0;

                int32_t sampleCountToDecode = FloatToInt32(std::trunc(fraction + sampleRateRatio * static_cast<float>(sampleCountToProcess)));

                while (y < sampleCountToDecode) {
                    if (waveBufferIndex >= static_cast<int32_t>(VoiceWaveBufferCount)) {
                        waveBufferIndex = 0;
                        playedSampleCount = 0;
                    }

                    if (!voiceState.is_wave_buffer_valid[waveBufferIndex]) {
                        isStarving = true;
                        break;
                    }

                    WaveBuffer& waveBuffer = wavebuffers[static_cast<size_t>(waveBufferIndex)];

                    if (offset == 0 && info.SampleFormat == SampleFormat::Adpcm && waveBuffer.context != 0) {
                        voiceState.loop_context = *GuestPtr<AdpcmLoopContext>(waveBuffer.context);
                    }

                    Span<int16_t> tempSpan(tempBuffer + tempBufferIndex + y, static_cast<size_t>(TempBufferSize - tempBufferIndex - y));

                    int32_t decodedSampleCount = -1;

                    int32_t targetSampleStartOffset;
                    int32_t targetSampleEndOffset;

                    if (voiceState.loop_count > 0 && waveBuffer.loop_start_sample_offset != 0 && waveBuffer.loop_end_sample_offset != 0 && waveBuffer.loop_start_sample_offset <= waveBuffer.loop_end_sample_offset) {
                        targetSampleStartOffset = static_cast<int32_t>(waveBuffer.loop_start_sample_offset);
                        targetSampleEndOffset = static_cast<int32_t>(waveBuffer.loop_end_sample_offset);
                    } else {
                        targetSampleStartOffset = static_cast<int32_t>(waveBuffer.start_sample_offset);
                        targetSampleEndOffset = static_cast<int32_t>(waveBuffer.end_sample_offset);
                    }

                    int32_t targetWaveBufferSampleCount = targetSampleEndOffset - targetSampleStartOffset;

                    switch (info.SampleFormat) {
                        case SampleFormat::Adpcm: {
                            Span<const uint8_t> waveBufferAdpcm;

                            if (waveBuffer.buffer != 0 && waveBuffer.buffer_size != 0) {
                                // TODO(C#): possibly copies more data than needed.
                                waveBufferAdpcm = GuestSpan<const uint8_t>(waveBuffer.buffer, waveBuffer.buffer_size);
                            }

                            Span<const int16_t> coefficients = GuestSpan<const uint8_t>(info.ExtraParameter, static_cast<size_t>(info.ExtraParameterSize)).Cast<const int16_t>();
                            decodedSampleCount = AdpcmHelper::Decode(tempSpan, waveBufferAdpcm, targetSampleStartOffset, targetSampleEndOffset, offset, sampleCountToDecode - y, coefficients, voiceState.loop_context);
                            break;
                        }
                        case SampleFormat::PcmInt16: {
                            Span<const int16_t> waveBufferPcm16;

                            if (waveBuffer.buffer != 0 && waveBuffer.buffer_size != 0) {
                                uint64_t bufferOffset = waveBuffer.buffer + PcmHelper::GetBufferOffset<int16_t>(targetSampleStartOffset, offset, info.ChannelCount);
                                int32_t bufferSize = PcmHelper::GetBufferSize<int16_t>(targetSampleStartOffset, targetSampleEndOffset, offset, sampleCountToDecode - y) * info.ChannelCount;

                                waveBufferPcm16 = GuestSpan<const uint8_t>(bufferOffset, static_cast<size_t>(bufferSize)).Cast<const int16_t>();
                            }

                            decodedSampleCount = PcmHelper::Decode(tempSpan, waveBufferPcm16, targetSampleStartOffset, targetSampleEndOffset, info.ChannelIndex, info.ChannelCount);
                            break;
                        }
                        case SampleFormat::PcmFloat: {
                            Span<const float> waveBufferPcmFloat;

                            if (waveBuffer.buffer != 0 && waveBuffer.buffer_size != 0) {
                                uint64_t bufferOffset = waveBuffer.buffer + PcmHelper::GetBufferOffset<float>(targetSampleStartOffset, offset, info.ChannelCount);
                                int32_t bufferSize = PcmHelper::GetBufferSize<float>(targetSampleStartOffset, targetSampleEndOffset, offset, sampleCountToDecode - y) * info.ChannelCount;

                                waveBufferPcmFloat = GuestSpan<const uint8_t>(bufferOffset, static_cast<size_t>(bufferSize)).Cast<const float>();
                            }

                            decodedSampleCount = PcmHelper::Decode(tempSpan, waveBufferPcmFloat, targetSampleStartOffset, targetSampleEndOffset, info.ChannelIndex, info.ChannelCount);
                            break;
                        }
                        default:
                            std::fprintf(stderr, "[audren] Unsupported sample format %d\n", static_cast<int>(info.SampleFormat));
                            break;
                    }

                    NXAUDREN_ASSERT(decodedSampleCount <= sampleCountToDecode);

                    if (decodedSampleCount < 0) {
                        std::fprintf(stderr, "[audren] Decoding failed, skipping WaveBuffer\n");

                        MarkEndOfBufferWaveBufferProcessing(voiceState, waveBuffer, waveBufferIndex, waveBufferConsumed, playedSampleCount);
                        decodedSampleCount = 0;
                    }

                    y += decodedSampleCount;
                    offset += decodedSampleCount;
                    playedSampleCount += static_cast<uint32_t>(decodedSampleCount);

                    if (offset >= targetWaveBufferSampleCount || decodedSampleCount == 0) {
                        offset = 0;

                        if (waveBuffer.looping) {
                            voiceState.loop_count++;

                            if (waveBuffer.loop_count >= 0) {
                                if (decodedSampleCount == 0 || voiceState.loop_count > waveBuffer.loop_count) {
                                    MarkEndOfBufferWaveBufferProcessing(voiceState, waveBuffer, waveBufferIndex, waveBufferConsumed, playedSampleCount);
                                }
                            }

                            if (decodedSampleCount == 0) {
                                isStarving = true;
                                break;
                            }

                            if (HasFlag(info.DecodingBehaviour, ::nxrt::audren::DecodingBehaviour::PlayedSampleCountResetWhenLooping)) {
                                playedSampleCount = 0;
                            }
                        } else {
                            MarkEndOfBufferWaveBufferProcessing(voiceState, waveBuffer, waveBufferIndex, waveBufferConsumed, playedSampleCount);
                        }
                    }
                }

                Span<int32_t> outputSpanInt = outputBuffer.Slice(static_cast<size_t>(i)).Cast<int32_t>();

                if (HasFlag(info.DecodingBehaviour, ::nxrt::audren::DecodingBehaviour::SkipPitchAndSampleRateConversion)) {
                    for (int32_t j = 0; j < y; j++) {
                        outputBuffer[static_cast<size_t>(j)] = static_cast<float>(tempBuffer[j]);
                    }
                } else {
                    Span<int16_t> tempSpanRest(tempBuffer + tempBufferIndex + y, static_cast<size_t>(TempBufferSize - tempBufferIndex - y));

                    tempSpanRest.Slice(0, static_cast<size_t>(sampleCountToDecode - y)).Clear();

                    ToFloat(outputBuffer, outputSpanInt, sampleCountToProcess);

                    ResamplerHelper::Resample(outputBuffer, Span<const int16_t>(tempBuffer, TempBufferSize), sampleRateRatio, fraction, sampleCountToProcess, info.SrcQuality, y != sourceSampleCountToProcess || info.Pitch != 1.0f);

                    Span<int16_t>(tempBuffer, TempBufferSize).Slice(static_cast<size_t>(sampleCountToDecode), static_cast<size_t>(pitchMaxLength)).CopyTo(Span<int16_t>(voiceState.pitch, 8));
                }

                i += sampleCountToProcess;
            }

            NXAUDREN_ASSERT(sourceSampleCountToProcess == i || !isStarving);

            voiceState.wave_buffer_consumed = waveBufferConsumed;
            voiceState.offset = offset;
            voiceState.played_sample_count = playedSampleCount;
            voiceState.wave_buffer_index = static_cast<uint32_t>(waveBufferIndex);
            voiceState.fraction = fraction;
        }
    }

    static void ToFloatSlow(Span<float> output, Span<const int32_t> input, int32_t sampleCount) {
        for (int32_t i = 0; i < sampleCount; i++) {
            output[static_cast<size_t>(i)] = static_cast<float>(input[static_cast<size_t>(i)]);
        }
    }

    // SIMD variants are not ported; the scalar one is used.
    static void ToFloat(Span<float> output, Span<const int32_t> input, int32_t sampleCount) { ToFloatSlow(output, input, sampleCount); }

    static void ToIntSlow(Span<int32_t> output, Span<const float> input, int32_t sampleCount) {
        for (int32_t i = 0; i < sampleCount; i++) {
            output[static_cast<size_t>(i)] = FloatToInt32(input[static_cast<size_t>(i)]);
        }
    }

    // SIMD variants are not ported; the scalar one is used.
    static void ToInt(Span<int32_t> output, Span<const float> input, int32_t sampleCount) { ToIntSlow(output, input, sampleCount); }

    static void RemapLegacyChannelEffectMappingToChannelResourceMapping(bool isSupported, Span<uint16_t> bufferIndices, uint32_t channelCount) {
        if (!isSupported && channelCount == 6) {
            uint16_t backLeft = bufferIndices[2];
            uint16_t backRight = bufferIndices[3];
            uint16_t frontCenter = bufferIndices[4];
            uint16_t lowFrequency = bufferIndices[5];

            bufferIndices[2] = frontCenter;
            bufferIndices[3] = lowFrequency;
            bufferIndices[4] = backLeft;
            bufferIndices[5] = backRight;
        }
    }

    static void RemapChannelResourceMappingToLegacy(bool isSupported, Span<uint16_t> bufferIndices, uint32_t channelCount) {
        if (isSupported && channelCount == 6) {
            uint16_t frontCenter = bufferIndices[2];
            uint16_t lowFrequency = bufferIndices[3];
            uint16_t backLeft = bufferIndices[4];
            uint16_t backRight = bufferIndices[5];

            bufferIndices[2] = backLeft;
            bufferIndices[3] = backRight;
            bufferIndices[4] = frontCenter;
            bufferIndices[5] = lowFrequency;
        }
    }
};

}  // namespace nxrt::audren
