// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/BiquadFilterCommand.cs (Ryubing fork, MIT).
// The C# property `Memory<BiquadFilterState> BiquadFilterState` shares its name with the type; the type is written qualified
// inside this class so the member name does not change the meaning of the type name.
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BiquadFilterHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class BiquadFilterCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    Span<::nxrt::audren::BiquadFilterState> BiquadFilterState;
    int32_t InputBufferIndex = 0;
    int32_t OutputBufferIndex = 0;
    bool NeedInitialization = false;

  private:
    BiquadFilterParameter2 _parameter{};

  public:
    BiquadFilterCommand() = default;

    BiquadFilterCommand& Initialize(int32_t baseIndex, BiquadFilterParameter2& filter, Span<::nxrt::audren::BiquadFilterState> biquadFilterStateMemory,
                                    int32_t inputBufferOffset, int32_t outputBufferOffset, bool needInitialization, int32_t nodeId) {
        _parameter = filter;
        BiquadFilterState = biquadFilterStateMemory;
        InputBufferIndex = baseIndex + inputBufferOffset;
        OutputBufferIndex = baseIndex + outputBufferOffset;
        NeedInitialization = needInitialization;

        Enabled = true;
        NodeId = nodeId;

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::BiquadFilter; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        ::nxrt::audren::BiquadFilterState& state = BiquadFilterState[0];

        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);

        if (NeedInitialization) {
            state = ::nxrt::audren::BiquadFilterState{};
        }

        BiquadFilterHelper::ProcessBiquadFilter(_parameter, state, outputBuffer, inputBuffer, context.SampleCount);
    }
};

}  // namespace nxrt::audren
