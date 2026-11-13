// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/MixRampCommand.cs (Ryubing fork, MIT). State is the voice state block in the work buffer
// (C# Memory<VoiceState>, here Span<VoiceState>); the last mixed sample is written to its last_samples[LastSampleIndex].
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class MixRampCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;
    float Volume0 = 0.0f;
    float Volume1 = 0.0f;
    Span<VoiceState> State;
    int32_t LastSampleIndex = 0;

    MixRampCommand& Initialize(float volume0, float volume1, uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t lastSampleIndex,
                               Span<VoiceState> state, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = static_cast<uint16_t>(inputBufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(outputBufferIndex);
        Volume0 = volume0;
        Volume1 = volume1;
        State = state;
        LastSampleIndex = lastSampleIndex;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::MixRamp; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        State[0].last_samples[LastSampleIndex] = ProcessMixRamp(outputBuffer, inputBuffer, static_cast<int32_t>(context.SampleCount));
    }

  private:
    float ProcessMixRamp(Span<float> outputBuffer, Span<const float> inputBuffer, int32_t sampleCount) const {
        float ramp = (Volume1 - Volume0) / static_cast<float>(sampleCount);
        float volume = Volume0;
        float state = 0.0f;
        for (int32_t i = 0; i < sampleCount; i++) {
            state = FloatingPointHelper::MultiplyRoundUp(inputBuffer[i], volume);
            outputBuffer[i] += state;
            volume += ramp;
        }
        return state;
    }
};

}  // namespace nxrt::audren
