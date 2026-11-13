// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/MixCommand.cs (Ryubing fork, MIT). Only the scalar path (C# ProcessMixSlowPath) is ported;
// the SIMD variants are skipped (audren_common.h).
#pragma once
#include "../../audren_common.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class MixCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;
    float Volume = 0.0f;

    MixCommand& Initialize(uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t nodeId, float volume) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = static_cast<uint16_t>(inputBufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(outputBufferIndex);
        Volume = volume;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Mix; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        ProcessMixSlowPath(outputBuffer, inputBuffer);
    }

  private:
    void ProcessMixSlowPath(Span<float> outputMix, Span<const float> inputMix) const {
        for (size_t i = 0; i < inputMix.Length; i++) {
            outputMix[i] += FloatingPointHelper::MultiplyRoundUp(inputMix[i], Volume);
        }
    }
};

}  // namespace nxrt::audren
