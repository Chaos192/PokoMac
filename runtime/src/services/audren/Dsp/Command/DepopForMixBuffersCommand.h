// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/DepopForMixBuffersCommand.cs (Ryubing fork, MIT).
#pragma once
#include <algorithm>

#include "../../audren_common.h"
#include "../FloatingPointHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class DepopForMixBuffersCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint32_t MixBufferOffset = 0;
    uint32_t MixBufferCount = 0;
    float Decay = 0.0f;
    Span<float> DepopBuffer;

    DepopForMixBuffersCommand& Initialize(Span<float> depopBuffer, uint32_t bufferOffset, uint32_t mixBufferCount, int32_t nodeId, uint32_t sampleRate) {
        Enabled = true;
        NodeId = nodeId;
        MixBufferOffset = bufferOffset;
        MixBufferCount = mixBufferCount;
        DepopBuffer = depopBuffer;
        if (sampleRate == 48000) {
            Decay = 0.962189f;
        } else {  // if (sampleRate == 32000)
            Decay = 0.943695f;
        }
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::DepopForMixBuffers; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        uint32_t bufferCount = std::min(MixBufferOffset + MixBufferCount, context.BufferCount);
        for (uint32_t i = MixBufferOffset; i < bufferCount; i++) {
            float depopValue = DepopBuffer[i];
            if (depopValue != 0) {
                float* buffer = context.GetBufferPointer(static_cast<int32_t>(i));
                DepopBuffer[i] = ProcessDepopMix(buffer, depopValue, context.SampleCount);
            }
        }
    }

  private:
    float ProcessDepopMix(float* buffer, float depopValue, uint32_t sampleCount) const {
        if (depopValue < 0) {
            depopValue = -depopValue;
            for (uint32_t i = 0; i < sampleCount; i++) {
                depopValue = FloatingPointHelper::MultiplyRoundDown(Decay, depopValue);
                buffer[i] -= depopValue;
            }
            return -depopValue;
        }

        for (uint32_t i = 0; i < sampleCount; i++) {
            depopValue = FloatingPointHelper::MultiplyRoundDown(Decay, depopValue);
            buffer[i] += depopValue;
        }
        return depopValue;
    }
};

}  // namespace nxrt::audren
