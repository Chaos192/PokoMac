// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/ClearMixBufferCommand.cs (Ryubing fork, MIT).
#pragma once
#include "../../audren_common.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class ClearMixBufferCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    ClearMixBufferCommand& Initialize(int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::ClearMixBuffer; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }
    void Process(CommandList& context) override { context.ClearBuffers(); }
};

}  // namespace nxrt::audren
