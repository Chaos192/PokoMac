// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/UpsampleCommand.cs (Ryubing fork, MIT).
//
// UpsamplerInfo is the server class (Server/Upsampler/UpsamplerInfo.h); the command holds a pointer to the persistent object.
#pragma once
#include <algorithm>
#include <vector>

#include "../../audren_common.h"
#include "../../Constants.h"
#include "../../Server/Upsampler/UpsamplerInfo.h"
#include "../UpsamplerHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class UpsampleCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint32_t BufferCount = 0;
    uint32_t InputBufferIndex = 0;
    uint32_t InputSampleCount = 0;
    uint32_t InputSampleRate = 0;
    UpsamplerInfo* UpsamplerInfo_ = nullptr;
    Span<float> OutBuffer;

    UpsampleCommand& Initialize(uint32_t bufferOffset, UpsamplerInfo& info, uint32_t inputCount, Span<const uint8_t> inputBufferOffset,
                                uint32_t bufferCount, uint32_t sampleCount, uint32_t sampleRate, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = 0;
        OutBuffer = info.OutputBuffer;
        BufferCount = bufferCount;
        InputSampleCount = sampleCount;
        InputSampleRate = sampleRate;

        info.SourceSampleCount = inputCount;
        info.InputBufferIndices.assign(static_cast<size_t>(inputCount), uint16_t{0});
        for (uint32_t i = 0; i < inputCount; i++) {
            info.InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + inputBufferOffset[i]);
        }

        // C#: a new state array is allocated only when the length changes ("Keep state if possible").
        if (info.BufferStates.size() != static_cast<size_t>(inputCount)) {
            info.BufferStates.assign(static_cast<size_t>(inputCount), UpsamplerBufferState{});
        }

        UpsamplerInfo_ = &info;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Upsample; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        uint32_t bufferCount = std::min(BufferCount, UpsamplerInfo_->SourceSampleCount);
        for (uint32_t i = 0; i < bufferCount; i++) {
            const int32_t inputIndex = static_cast<int32_t>(UpsamplerInfo_->InputBufferIndices[i]);
            Span<const float> inputBuffer = context.GetBuffer(inputIndex);
            Span<float> outputBuffer = GetBuffer(inputIndex, static_cast<int32_t>(UpsamplerInfo_->SampleCount));
            UpsamplerHelper::Upsample(outputBuffer, inputBuffer, static_cast<int32_t>(UpsamplerInfo_->SampleCount),
                                      static_cast<int32_t>(InputSampleCount), UpsamplerInfo_->BufferStates[i]);
        }
    }

  private:
    Span<float> GetBuffer(int32_t index, int32_t sampleCount) const {
        return UpsamplerInfo_->OutputBuffer.Slice(static_cast<size_t>(index) * static_cast<size_t>(sampleCount), static_cast<size_t>(sampleCount));
    }
};

}  // namespace nxrt::audren
