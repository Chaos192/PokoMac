// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/PcmFloatDataSourceCommandVersion1.cs (Ryubing fork, MIT).
// Initialize takes the server VoiceInfo as a template argument (members as in PcmInt16DataSourceCommandVersion1.h).
// Output: the float samples are converted by PcmHelper.ConvertSampleToPcmInt16 (x * 32767, saturated and truncated) and then written as
// raw int16 values (as float).
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "../DataSourceHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class PcmFloatDataSourceCommandVersion1 : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t OutputBufferIndex = 0;
    uint32_t SampleRate = 0;
    uint32_t ChannelIndex = 0;
    uint32_t ChannelCount = 0;
    float Pitch = 0.0f;
    std::vector<WaveBuffer> WaveBuffers = std::vector<WaveBuffer>(static_cast<size_t>(Constants::VoiceWaveBufferCount));
    Span<VoiceState> State;
    DecodingBehaviour DecodingBehaviour_ = DecodingBehaviour::Default;

    template <class VoiceInfoT>
    PcmFloatDataSourceCommandVersion1& Initialize(VoiceInfoT& serverInfo, Span<VoiceState> state, uint16_t outputBufferIndex, uint16_t channelIndex,
                                                  int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        OutputBufferIndex = static_cast<uint16_t>(channelIndex + outputBufferIndex);
        SampleRate = serverInfo.SampleRate;
        ChannelIndex = channelIndex;
        ChannelCount = serverInfo.ChannelsCount;
        Pitch = serverInfo.Pitch;
        for (size_t i = 0; i < WaveBuffers.size(); i++) {
            WaveBuffers[i] = serverInfo.WaveBuffers[i].ToCommon(1);
        }
        State = state;
        DecodingBehaviour_ = serverInfo.DecodingBehaviour;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::PcmFloatDataSourceVersion1; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        DataSourceHelper::WaveBufferInformation info{};
        info.SourceSampleRate = SampleRate;
        info.SampleFormat = SampleFormat::PcmFloat;
        info.Pitch = Pitch;
        info.DecodingBehaviour = DecodingBehaviour_;
        info.ExtraParameter = 0;
        info.ExtraParameterSize = 0;
        info.ChannelIndex = static_cast<int32_t>(ChannelIndex);
        info.ChannelCount = static_cast<int32_t>(ChannelCount);
        info.SrcQuality = SampleRateConversionQuality::Default;
        DataSourceHelper::ProcessWaveBuffers(outputBuffer, info, WaveBuffers, State[0], context.SampleRate, static_cast<int32_t>(context.SampleCount));
    }
};

}  // namespace nxrt::audren
