// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/PerformanceCommand.cs (Ryubing fork, MIT). The C# PerformanceEntryAddresses pool is dropped:
// the command keeps a copy of the entry addresses (a view of the performance entry memory).
#pragma once
#include "../../Server/Performance/PerformanceEntryAddresses.h"
#include "../../audren_common.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class PerformanceCommand : public ICommand {
  public:
    enum class Type {
        Invalid,
        Start,
        End,
    };

    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    ::nxrt::audren::PerformanceEntryAddresses PerformanceEntryAddresses_;
    Type PerformanceType = Type::Invalid;

    PerformanceCommand& Initialize(::nxrt::audren::PerformanceEntryAddresses& performanceEntryAddresses, Type performanceType, int32_t nodeId) {
        Enabled = true;
        PerformanceEntryAddresses_ = performanceEntryAddresses;
        PerformanceType = performanceType;
        NodeId = nodeId;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Performance; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        if (PerformanceType == Type::Start) {
            PerformanceEntryAddresses_.SetStartTime(context.GetTimeElapsedSinceDspStartedProcessing());
        } else if (PerformanceType == Type::End) {
            PerformanceEntryAddresses_.SetProcessingTime(context.GetTimeElapsedSinceDspStartedProcessing());
            PerformanceEntryAddresses_.IncrementEntryCount();
        }
    }
};

}  // namespace nxrt::audren
