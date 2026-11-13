// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Port of Ryujinx.Audio/Renderer/Dsp/Command/LimiterCommandVersion1.cs (Ryubing fork, MIT). Scalar path (no SIMD in the C# file).
#pragma once
#include <cmath>
#include <vector>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/ExponentialMovingAverage.h"
#include "../State/LimiterState.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class LimiterCommandVersion1 : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    Span<LimiterState> State;
    CpuAddress WorkBuffer = 0;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<uint16_t> InputBufferIndices;
    bool IsEffectEnabled = false;

  private:
    LimiterParameter _parameter{};

  public:
    LimiterCommandVersion1()
        : OutputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0), InputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0) {}

    // C# `public LimiterParameter Parameter => _parameter;`
    LimiterParameter Parameter() const { return _parameter; }

    LimiterCommandVersion1& Initialize(uint32_t bufferOffset, LimiterParameter parameter, Span<LimiterState> state, bool isEnabled, CpuAddress workBuffer,
                                       int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        _parameter = parameter;
        State = state;
        WorkBuffer = workBuffer;

        IsEffectEnabled = isEnabled;

        // C# Array6 indexing: a channel count above 6 throws in the C#.
        if (static_cast<size_t>(_parameter.channel_count) > sizeof(_parameter.input)) {
            NXAUDREN_FATAL("LimiterCommandVersion1::Initialize: channel count above 6");
        }

        for (size_t i = 0; i < _parameter.channel_count; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.input[i]);
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.output[i]);
        }

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::LimiterVersion1; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        LimiterState& state = State[0];

        if (IsEffectEnabled) {
            if (_parameter.status == UsageState::Invalid) {
                ReplaceState(state, _parameter, WorkBuffer);
            } else if (_parameter.status == UsageState::New) {
                LimiterState::UpdateParameter(_parameter);
            }
        }

        ProcessLimiter(context, state);
    }

  private:
    static bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }

    void ProcessLimiter(CommandList& context, LimiterState& state) {
        NXAUDREN_ASSERT(IsChannelCountValid(_parameter.channel_count));

        if (IsEffectEnabled && IsChannelCountValid(_parameter.channel_count)) {
            float* inputBuffers[Constants::ChannelCountMax] = {};
            float* outputBuffers[Constants::ChannelCountMax] = {};

            for (size_t i = 0; i < _parameter.channel_count; i++) {
                inputBuffers[i] = context.GetBufferPointer(InputBufferIndices[i]);
                outputBuffers[i] = context.GetBufferPointer(OutputBufferIndices[i]);
            }

            for (size_t channelIndex = 0; channelIndex < _parameter.channel_count; channelIndex++) {
                for (uint32_t sampleIndex = 0; sampleIndex < context.SampleCount; sampleIndex++) {
                    float rawInputSample = inputBuffers[channelIndex][sampleIndex];

                    float inputSample = (rawInputSample / 32767.0f) * _parameter.input_gain;

                    float sampleInputMax = std::fabs(inputSample);

                    float inputCoefficient = _parameter.release_coefficient;

                    if (sampleInputMax > state.DetectorAverage[channelIndex].Read()) {
                        inputCoefficient = _parameter.attack_coefficient;
                    }

                    float detectorValue = state.DetectorAverage[channelIndex].Update(sampleInputMax, inputCoefficient);
                    float attenuation = 1.0f;

                    if (detectorValue > _parameter.threshold) {
                        attenuation = _parameter.threshold / detectorValue;
                    }

                    float outputCoefficient = _parameter.release_coefficient;

                    if (state.CompressionGainAverage[channelIndex].Read() > attenuation) {
                        outputCoefficient = _parameter.attack_coefficient;
                    }

                    float compressionGain = state.CompressionGainAverage[channelIndex].Update(attenuation, outputCoefficient);

                    float& delayedSample =
                        state.DelayedSampleBuffer[channelIndex * static_cast<size_t>(_parameter.delay_buffer_sample_count_max) +
                                                  static_cast<size_t>(state.DelayedSampleBufferPosition[channelIndex])];

                    float outputSample = delayedSample * compressionGain * _parameter.output_gain;

                    outputBuffers[channelIndex][sampleIndex] = outputSample * 32767.0f;

                    delayedSample = inputSample;

                    state.DelayedSampleBufferPosition[channelIndex]++;

                    while (state.DelayedSampleBufferPosition[channelIndex] >= _parameter.delay_buffer_sample_count_min) {
                        state.DelayedSampleBufferPosition[channelIndex] -= _parameter.delay_buffer_sample_count_min;
                    }
                }
            }
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
