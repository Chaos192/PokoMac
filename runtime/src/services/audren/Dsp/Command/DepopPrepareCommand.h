// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/DepopPrepareCommand.cs (Ryubing fork, MIT).
#pragma once
#include <iterator>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class DepopPrepareCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint32_t MixBufferCount = 0;
    std::vector<uint16_t> OutputBufferIndices;
    Span<VoiceState> State;
    Span<float> DepopBuffer;

    DepopPrepareCommand() { OutputBufferIndices.assign(static_cast<size_t>(Constants::MixBufferCountMax), uint16_t{0}); }

    DepopPrepareCommand& Initialize(Span<VoiceState> state, Span<float> depopBuffer, uint32_t mixBufferCount, uint32_t bufferOffset, int32_t nodeId,
                                    bool enabled) {
        Enabled = enabled;
        NodeId = nodeId;
        MixBufferCount = mixBufferCount;
        for (int32_t i = 0; i < Constants::MixBufferCountMax; i++) {
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + static_cast<uint32_t>(i));
        }
        State = state;
        DepopBuffer = depopBuffer;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::DepopPrepare; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        (void)context;
        VoiceState& state = State[0];
        Span<float> lastSamplesSpan(state.last_samples, std::size(state.last_samples));

        for (uint32_t i = 0; i < MixBufferCount; i++) {
            if (lastSamplesSpan[i] != 0) {
                DepopBuffer[OutputBufferIndices[i]] += lastSamplesSpan[i];
                lastSamplesSpan[i] = 0;
            }
        }
    }
};

}  // namespace nxrt::audren
