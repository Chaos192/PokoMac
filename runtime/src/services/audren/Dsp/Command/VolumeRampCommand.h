// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/VolumeRampCommand.cs (Ryubing fork, MIT). The ramp runs from Volume0 at the first sample and
// reaches Volume1 only after the last sample of the buffer (C# semantics).
#pragma once
#include "../../audren_common.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class VolumeRampCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;
    float Volume0 = 0.0f;
    float Volume1 = 0.0f;

    VolumeRampCommand& Initialize(float volume0, float volume1, uint32_t bufferIndex, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = static_cast<uint16_t>(bufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(bufferIndex);
        Volume0 = volume0;
        Volume1 = volume1;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::VolumeRamp; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        ProcessVolumeRamp(outputBuffer, inputBuffer, static_cast<int32_t>(context.SampleCount));
    }

  private:
    void ProcessVolumeRamp(Span<float> outputBuffer, Span<const float> inputBuffer, int32_t sampleCount) const {
        float ramp = (Volume1 - Volume0) / static_cast<float>(sampleCount);
        float volume = Volume0;
        for (int32_t i = 0; i < sampleCount; i++) {
            outputBuffer[i] = FloatingPointHelper::MultiplyRoundUp(inputBuffer[i], volume);
            volume += ramp;
        }
    }
};

}  // namespace nxrt::audren
