// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/CopyMixBufferCommand.cs (Ryubing fork, MIT).
#pragma once
#include "../../audren_common.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class CopyMixBufferCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;

    CopyMixBufferCommand& Initialize(uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = static_cast<uint16_t>(inputBufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(outputBufferIndex);
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::CopyMixBuffer; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }
    void Process(CommandList& context) override { context.CopyBuffer(OutputBufferIndex, InputBufferIndex); }
};

}  // namespace nxrt::audren
