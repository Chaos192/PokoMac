// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/MixRampGroupedCommand.cs (Ryubing fork, MIT). The C# ushort[] / float[] fields are sized
// Constants.MixBufferCountMax as in the C# Initialize.
#pragma once
#include <iterator>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class MixRampGroupedCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint32_t MixBufferCount = 0;
    std::vector<uint16_t> InputBufferIndices;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<float> Volume0;
    std::vector<float> Volume1;
    Span<VoiceState> State;

    MixRampGroupedCommand& Initialize(uint32_t mixBufferCount, uint32_t inputBufferIndex, uint32_t outputBufferIndex, Span<const float> volume0,
                                      Span<const float> volume1, Span<VoiceState> state, int32_t nodeId) {
        Enabled = true;
        MixBufferCount = mixBufferCount;
        NodeId = nodeId;
        const size_t max = static_cast<size_t>(Constants::MixBufferCountMax);
        InputBufferIndices.assign(max, uint16_t{0});
        OutputBufferIndices.assign(max, uint16_t{0});
        Volume0.assign(max, 0.0f);
        Volume1.assign(max, 0.0f);
        for (uint32_t i = 0; i < mixBufferCount; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(inputBufferIndex);
            OutputBufferIndices[i] = static_cast<uint16_t>(outputBufferIndex + i);
            Volume0[i] = volume0[i];
            Volume1[i] = volume1[i];
        }
        State = state;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::MixRampGrouped; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        VoiceState& state = State[0];
        Span<float> lastSamplesSpan(state.last_samples, std::size(state.last_samples));

        for (uint32_t i = 0; i < MixBufferCount; i++) {
            Span<const float> inputBuffer = context.GetBuffer(InputBufferIndices[i]);
            Span<float> outputBuffer = context.GetBuffer(OutputBufferIndices[i]);
            float volume0 = Volume0[i];
            float volume1 = Volume1[i];
            if (volume0 != 0 || volume1 != 0) {
                lastSamplesSpan[i] = ProcessMixRampGrouped(outputBuffer, inputBuffer, volume0, volume1, static_cast<int32_t>(context.SampleCount));
            } else {
                lastSamplesSpan[i] = 0;
            }
        }
    }

  private:
    static float ProcessMixRampGrouped(Span<float> outputBuffer, Span<const float> inputBuffer, float volume0, float volume1, int32_t sampleCount) {
        float ramp = (volume1 - volume0) / static_cast<float>(sampleCount);
        float volume = volume0;
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
