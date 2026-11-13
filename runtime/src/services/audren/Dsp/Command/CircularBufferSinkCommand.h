// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/CircularBufferSinkCommand.cs (Ryubing fork, MIT).
// Initialize takes the server AddressInfo as a template argument (the MemoryPool port is not in the tree yet); it must provide
// GetReference(bool) returning the DSP/guest address.
//
// Behaviour kept from the C#: the samples of each input are written one input after another (each input has a stride of 2 bytes, so
// the channels are not interleaved), and the write position is advanced only in a local variable. CurrentOffset is not updated.
#pragma once
#include <cstdint>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "../PcmHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class CircularBufferSinkCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    std::vector<uint16_t> Input;
    uint32_t InputCount = 0;
    uint64_t CircularBuffer = 0;
    uint64_t CircularBufferSize = 0;
    uint64_t CurrentOffset = 0;

    CircularBufferSinkCommand() { Input.assign(static_cast<size_t>(Constants::ChannelCountMax), uint16_t{0}); }

    template <class AddressInfoT>
    CircularBufferSinkCommand& Initialize(uint32_t bufferOffset, CircularBufferParameter& parameter, AddressInfoT& circularBufferAddressInfo,
                                          uint32_t currentOffset, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputCount = parameter.input_count;
        NXAUDREN_ASSERT(InputCount <= static_cast<uint32_t>(Constants::ChannelCountMax));
        for (uint32_t i = 0; i < InputCount; i++) {
            Input[i] = static_cast<uint16_t>(bufferOffset + parameter.input[i]);
        }
        CircularBuffer = circularBufferAddressInfo.GetReference(true);
        CircularBufferSize = parameter.buffer_size;
        CurrentOffset = currentOffset;
        NXAUDREN_ASSERT(CircularBuffer != 0);
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::CircularBufferSink; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        constexpr uint64_t TargetChannelCount = 2;
        uint64_t currentOffset = CurrentOffset;

        if (CircularBufferSize > 0) {
            for (uint32_t i = 0; i < InputCount; i++) {
                const float* inputBuffer = context.GetBufferPointer(static_cast<int32_t>(Input[i]));
                uint64_t targetOffset = CircularBuffer + currentOffset;
                for (uint32_t y = 0; y < context.SampleCount; y++) {
                    *GuestPtr<int16_t>(targetOffset + static_cast<uint64_t>(y) * TargetChannelCount) = PcmHelper::Saturate(inputBuffer[y]);
                }

                currentOffset += static_cast<uint64_t>(context.SampleCount) * TargetChannelCount;
                if (currentOffset >= CircularBufferSize) {
                    currentOffset = 0;
                }
            }
        }
    }
};

}  // namespace nxrt::audren
