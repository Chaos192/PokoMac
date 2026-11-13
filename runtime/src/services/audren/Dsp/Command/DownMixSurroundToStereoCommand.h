// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/DownMixSurroundToStereoCommand.cs (Ryubing fork, MIT). The C# Coefficients array is copied
// into the command (the C# keeps a reference to the server's array).
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "../../Constants.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class DownMixSurroundToStereoCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    std::vector<uint16_t> InputBufferIndices;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<float> Coefficients;

    DownMixSurroundToStereoCommand() {
        InputBufferIndices.assign(static_cast<size_t>(Constants::VoiceChannelCountMax), uint16_t{0});
        OutputBufferIndices.assign(static_cast<size_t>(Constants::VoiceChannelCountMax), uint16_t{0});
    }

    DownMixSurroundToStereoCommand& Initialize(uint32_t bufferOffset, Span<const uint8_t> inputBufferOffset, Span<const uint8_t> outputBufferOffset,
                                               Span<const float> downMixParameter, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        for (int32_t i = 0; i < Constants::VoiceChannelCountMax; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + inputBufferOffset[i]);
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + outputBufferOffset[i]);
        }
        Coefficients.assign(downMixParameter.begin(), downMixParameter.end());
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::DownMixSurroundToStereo; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> frontLeft = context.GetBuffer(InputBufferIndices[0]);
        Span<const float> frontRight = context.GetBuffer(InputBufferIndices[1]);
        Span<const float> frontCenter = context.GetBuffer(InputBufferIndices[2]);
        Span<const float> lowFrequency = context.GetBuffer(InputBufferIndices[3]);
        Span<const float> backLeft = context.GetBuffer(InputBufferIndices[4]);
        Span<const float> backRight = context.GetBuffer(InputBufferIndices[5]);

        Span<float> stereoLeft = context.GetBuffer(OutputBufferIndices[0]);
        Span<float> stereoRight = context.GetBuffer(OutputBufferIndices[1]);

        Span<const float> coefficients(Coefficients.data(), Coefficients.size());
        for (int32_t i = 0; i < static_cast<int32_t>(context.SampleCount); i++) {
            stereoLeft[i] = DownMixSurroundToStereo(coefficients, backLeft[i], lowFrequency[i], frontCenter[i], frontLeft[i]);
            stereoRight[i] = DownMixSurroundToStereo(coefficients, backRight[i], lowFrequency[i], frontCenter[i], frontRight[i]);
        }

        context.ClearBuffer(OutputBufferIndices[2]);
        context.ClearBuffer(OutputBufferIndices[3]);
        context.ClearBuffer(OutputBufferIndices[4]);
        context.ClearBuffer(OutputBufferIndices[5]);
    }

  private:
    static float DownMixSurroundToStereo(Span<const float> coefficients, float back, float lfe, float center, float front) {
        return FloatingPointHelper::RoundUp(coefficients[3] * back + coefficients[2] * lfe + coefficients[1] * center + coefficients[0] * front);
    }
};

}  // namespace nxrt::audren
