// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/VolumeCommand.cs (Ryubing fork, MIT). Only the scalar path (C# ProcessVolumeSlowPath) is
// ported; the Avx / Sse41 / AdvSimd variants are skipped (audren_common.h). Note: the C# SIMD variants round with Ceiling while the scalar
// path rounds with MathF.Round (half to even), so the two can differ by one step on fractional products.
#pragma once
#include "../../audren_common.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class VolumeCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;
    float Volume = 0.0f;

    VolumeCommand& Initialize(float volume, uint32_t bufferIndex, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = static_cast<uint16_t>(bufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(bufferIndex);
        Volume = volume;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Volume; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        ProcessVolumeSlowPath(outputBuffer, inputBuffer);
    }

  private:
    void ProcessVolumeSlowPath(Span<float> outputBuffer, Span<const float> inputBuffer) const {
        for (size_t i = 0; i < outputBuffer.Length; i++) {
            outputBuffer[i] = FloatingPointHelper::MultiplyRoundUp(inputBuffer[i], Volume);
        }
    }
};

}  // namespace nxrt::audren
