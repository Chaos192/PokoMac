// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/FillBufferCommand.cs (Ryubing fork, MIT).
// Initialize takes the server splitter destination as a template argument (the splitter port is not in the tree yet). It must provide
// GetV1RefOrNull() / GetV2RefOrNull() returning a pointer to a destination (nullptr when absent), and each destination's
// PreviousMixBufferVolume as a std::vector<float>. Process only writes that array, so the command keeps a view of it instead of the
// C# Destination1 / Destination2 references.
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class FillBufferCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    bool IsV2 = false;
    int32_t Length = 0;
    float Value = 0.0f;
    Span<float> PreviousMixBufferVolume;

    template <class SplitterDestinationT>
    FillBufferCommand& Initialize(SplitterDestinationT& destination, int32_t length, float value, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        // The C# property PreviousMixBufferVolume is a method of the server SplitterDestinationVersion1/2 (a plain vector member in test doubles).
        auto volumes = [](auto* v) {
            if constexpr (requires { v->PreviousMixBufferVolume(); }) return Span<float>(v->PreviousMixBufferVolume());
            else return Span<float>(v->PreviousMixBufferVolume);
        };
        if (destination.GetV2RefOrNull() == nullptr) {
            PreviousMixBufferVolume = volumes(destination.GetV1RefOrNull());
            IsV2 = false;
        } else {
            PreviousMixBufferVolume = volumes(destination.GetV2RefOrNull());
            IsV2 = true;
        }
        Length = length;
        Value = value;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::FillBuffer; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        (void)context;
        for (int32_t i = 0; i < Length; i++) {
            PreviousMixBufferVolume[i] = Value;
        }
    }
};

}  // namespace nxrt::audren
