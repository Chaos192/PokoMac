// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/DeviceSinkCommand.cs (Ryubing fork, MIT).
// Initialize takes the server DeviceSink as a template argument (the sink port is not in the tree yet). It must provide `Parameter`
// (a DeviceParameter of audren_types.h) and `UpsamplerInfo` (a pointer, nullptr when no upsampling; its OutputBuffer is a Span<float>).
//
// Output: the C# writes PcmHelper.Saturate(sample) with no scale factor. Saturate truncates toward zero ((short) cast), so the device
// receives trunc(clamp(x)), not a rounded value scaled by 32768.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "../../Integration/IHardwareDevice.h"
#include "../PcmHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class DeviceSinkCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    std::string DeviceName;  // C#: never assigned (the C# name is commented out in Initialize)
    int32_t SessionId = 0;
    uint32_t InputCount = 0;
    std::vector<uint16_t> InputBufferIndices;
    Span<float> Buffers;

    template <class DeviceSinkT>
    DeviceSinkCommand& Initialize(uint32_t bufferOffset, DeviceSinkT& sink, int32_t sessionId, Span<float> buffers, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        SessionId = sessionId;
        InputCount = sink.Parameter.input_count;
        InputBufferIndices.assign(static_cast<size_t>(InputCount), uint16_t{0});
        const uint32_t count = std::min(InputCount, static_cast<uint32_t>(Constants::ChannelCountMax));
        for (uint32_t i = 0; i < count; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + sink.Parameter.input[i]);
        }
        if (sink.UpsamplerInfo != nullptr) {
            Buffers = sink.UpsamplerInfo->OutputBuffer;
        } else {
            Buffers = buffers;
        }
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::DeviceSink; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        IHardwareDevice* device = context.OutputDevice;
        if (device->GetSampleRate() == Constants::TargetSampleRate) {
            const uint32_t channelCount = device->GetChannelCount();
            const uint32_t bufferCount = std::min(channelCount, InputCount);
            constexpr int32_t SampleCount = Constants::TargetSampleCount;

            // In case of upmixing to 5.1, we allocate the right amount.
            uint32_t inputCount;
            if (bufferCount != channelCount && channelCount == static_cast<uint32_t>(Constants::ChannelCountMax)) {
                inputCount = channelCount;
            } else {
                inputCount = bufferCount;
            }

            const size_t outputSize = static_cast<size_t>(inputCount) * SampleCount;
            std::vector<int16_t> outputBuffer(outputSize, int16_t{0});
            for (uint32_t i = 0; i < bufferCount; i++) {
                Span<float> inputBuffer = GetBuffer(static_cast<int32_t>(InputBufferIndices[i]), SampleCount);
                for (int32_t j = 0; j < SampleCount; j++) {
                    const size_t index = i + static_cast<size_t>(j) * channelCount;
                    NXAUDREN_ASSERT(index < outputSize);
                    outputBuffer[index] = PcmHelper::Saturate(inputBuffer[j]);
                }
            }

            device->AppendBuffer(Span<const int16_t>(outputBuffer.data(), outputSize), inputCount);
        } else {
            // TODO (C#): support resampling for a device that only supports another sample rate.
            NXAUDREN_FATAL("DeviceSinkCommand: resampling to a device with another sample rate is not implemented");
        }
    }

  private:
    Span<float> GetBuffer(int32_t index, int32_t sampleCount) const {
        return Buffers.Slice(static_cast<size_t>(index) * static_cast<size_t>(sampleCount), static_cast<size_t>(sampleCount));
    }
};

}  // namespace nxrt::audren
