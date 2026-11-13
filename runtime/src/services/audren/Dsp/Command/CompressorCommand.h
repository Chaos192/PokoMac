// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Port of Ryujinx.Audio/Renderer/Dsp/Command/CompressorCommand.cs (Ryubing fork, MIT). Scalar path only (no SIMD in the C# file).
// The C# `unsafe` pointer loops become float* arithmetic on the command buffers; channel arrays are fixed (at most 6 channels).
#pragma once
#include <cmath>
#include <vector>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/ExponentialMovingAverage.h"
#include "../FloatingPointHelper.h"
#include "../State/CompressorState.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class CompressorCommand : public ICommand {
  private:
    static constexpr int32_t FixedPointPrecision = 15;

  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    Span<CompressorState> State;
    Span<EffectResultState> ResultState;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<uint16_t> InputBufferIndices;
    bool IsEffectEnabled = false;

  private:
    CompressorParameter _parameter{};

  public:
    CompressorCommand()
        : OutputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0), InputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0) {}

    // C# `public CompressorParameter Parameter => _parameter;`
    CompressorParameter Parameter() const { return _parameter; }

    CompressorCommand& Initialize(uint32_t bufferOffset, CompressorParameter parameter, Span<CompressorState> state, Span<EffectResultState> resultState,
                                  bool isEnabled, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        _parameter = parameter;
        State = state;
        ResultState = resultState;

        IsEffectEnabled = isEnabled;

        // C# Array6 indexing: a channel count above 6 throws in the C#.
        if (static_cast<size_t>(_parameter.channel_count) > sizeof(_parameter.input)) {
            NXAUDREN_FATAL("CompressorCommand::Initialize: channel count above 6");
        }

        for (size_t i = 0; i < _parameter.channel_count; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.input[i]);
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.output[i]);
        }

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Compressor; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        CompressorState& state = State[0];

        if (IsEffectEnabled) {
            if (_parameter.status == UsageState::Invalid) {
                ReplaceState(state, _parameter);
            } else if (_parameter.status == UsageState::New) {
                state.UpdateParameter(_parameter);
            }
        }

        ProcessCompressor(context, state);
    }

  private:
    static bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }

    // C# CompressorStatistics.Reset(ushort channelCount)
    static void ResetStatistics(CompressorStatistics& statistics, uint16_t channelCount) {
        statistics.maximum_mean = 0.0f;
        statistics.minimum_gain = 1.0f;
        for (size_t i = 0; i < channelCount; i++) {
            statistics.last_samples[i] = 0.0f;
        }
    }

    void ProcessCompressor(CommandList& context, CompressorState& state) {
        NXAUDREN_ASSERT(IsChannelCountValid(_parameter.channel_count));

        if (IsEffectEnabled && IsChannelCountValid(_parameter.channel_count)) {
            const size_t channelCount = _parameter.channel_count;

            if (!ResultState.IsEmpty() && _parameter.statistics_reset) {
                ResetStatistics(*reinterpret_cast<CompressorStatistics*>(ResultState[0].specific_data), _parameter.channel_count);
            }

            float* inputBuffers[Constants::ChannelCountMax];
            float* outputBuffers[Constants::ChannelCountMax];
            float channelInput[Constants::ChannelCountMax] = {};
            ExponentialMovingAverage inputMovingAverage = state.InputMovingAverage;
            float unknown4 = state.Unknown4;
            ExponentialMovingAverage compressionGainAverage = state.CompressionGainAverage;
            float previousCompressionEmaAlpha = state.PreviousCompressionEmaAlpha;

            for (size_t i = 0; i < channelCount; i++) {
                inputBuffers[i] = context.GetBufferPointer(InputBufferIndices[i]);
                outputBuffers[i] = context.GetBufferPointer(OutputBufferIndices[i]);
            }

            for (uint32_t sampleIndex = 0; sampleIndex < context.SampleCount; sampleIndex++) {
                for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
                    channelInput[channelIndex] = inputBuffers[channelIndex][sampleIndex];
                }

                float mean = FloatingPointHelper::MeanSquare(Span<const float>(channelInput, channelCount));
                float newMean = inputMovingAverage.Update(mean, _parameter.input_gain);
                float y = FloatingPointHelper::Log10(newMean) * 10.0f;
                float z = 1.0f;

                bool unknown10OutOfRange = y >= state.Unknown10;

                if (newMean < 1.0e-10f) {
                    y = -100.0f;

                    unknown10OutOfRange = state.Unknown10 <= -100.0f;
                }

                if (unknown10OutOfRange) {
                    float tmpGain;

                    if (y >= state.Unknown14) {
                        tmpGain = ((1.0f / _parameter.ratio) - 1.0f) * (y - _parameter.threshold);
                    } else {
                        tmpGain = (y - state.Unknown10) * ((y - state.Unknown10) * -state.CompressorGainReduction);
                    }

                    z = FloatingPointHelper::DecibelToLinear(tmpGain);
                }

                float unknown4New = z;
                float compressionEmaAlpha;

                if ((unknown4 - z) <= 0.08f) {
                    compressionEmaAlpha = _parameter.release_coefficient;

                    if ((unknown4 - z) >= -0.08f) {
                        if (std::fabs(compressionGainAverage.Read() - z) >= 0.001f) {
                            unknown4New = unknown4;
                        }

                        compressionEmaAlpha = previousCompressionEmaAlpha;
                    }
                } else {
                    compressionEmaAlpha = _parameter.attack_coefficient;
                }

                float compressionGain = compressionGainAverage.Update(z, compressionEmaAlpha);

                for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
                    outputBuffers[channelIndex][sampleIndex] = channelInput[channelIndex] * compressionGain * state.OutputGain;
                }

                unknown4 = unknown4New;
                previousCompressionEmaAlpha = compressionEmaAlpha;

                if (!ResultState.IsEmpty()) {
                    CompressorStatistics& statistics = *reinterpret_cast<CompressorStatistics*>(ResultState[0].specific_data);

                    statistics.minimum_gain = std::min(statistics.minimum_gain, compressionGain * state.OutputGain);
                    statistics.maximum_mean = std::max(statistics.maximum_mean, mean);

                    for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
                        statistics.last_samples[channelIndex] = std::fabs(channelInput[channelIndex] * (1.0f / 32768.0f));
                    }
                }
            }

            state.InputMovingAverage = inputMovingAverage;
            state.Unknown4 = unknown4;
            state.CompressionGainAverage = compressionGainAverage;
            state.PreviousCompressionEmaAlpha = previousCompressionEmaAlpha;
        } else {
            for (size_t i = 0; i < _parameter.channel_count; i++) {
                if (InputBufferIndices[i] != OutputBufferIndices[i]) {
                    context.CopyBuffer(OutputBufferIndices[i], InputBufferIndices[i]);
                }
            }
        }
    }
};

}  // namespace nxrt::audren
