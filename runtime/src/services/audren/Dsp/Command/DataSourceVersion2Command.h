// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/DataSourceVersion2Command.cs (Ryubing fork, MIT).
// The C# CommandType property has a private setter, so it is the data member CommandType_ (set by Initialize). Members that share a
// name with their C# type (SampleFormat, DecodingBehaviour) take the suffix _. Initialize takes the server VoiceInfo as a template
// argument (see PcmInt16DataSourceCommandVersion1.h for the members it must provide).
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "../DataSourceHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class DataSourceVersion2Command : public ICommand {
  public:
    int32_t NodeId = 0;
    CommandType CommandType_ = CommandType::Invalid;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t OutputBufferIndex = 0;
    uint32_t SampleRate = 0;
    float Pitch = 0.0f;
    std::vector<WaveBuffer> WaveBuffers = std::vector<WaveBuffer>(static_cast<size_t>(Constants::VoiceWaveBufferCount));
    Span<VoiceState> State;
    uint64_t ExtraParameter = 0;
    uint64_t ExtraParameterSize = 0;
    uint32_t ChannelIndex = 0;
    uint32_t ChannelCount = 0;
    DecodingBehaviour DecodingBehaviour_ = DecodingBehaviour::Default;
    SampleFormat SampleFormat_ = SampleFormat::Invalid;
    SampleRateConversionQuality SrcQuality = SampleRateConversionQuality::Default;

    template <class VoiceInfoT>
    DataSourceVersion2Command& Initialize(VoiceInfoT& serverInfo, Span<VoiceState> state, uint16_t outputBufferIndex, uint16_t channelIndex,
                                          int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        ChannelIndex = channelIndex;
        ChannelCount = serverInfo.ChannelsCount;
        SampleFormat_ = serverInfo.SampleFormat;
        SrcQuality = serverInfo.SrcQuality;
        CommandType_ = GetCommandTypeBySampleFormat(SampleFormat_);
        OutputBufferIndex = static_cast<uint16_t>(channelIndex + outputBufferIndex);
        SampleRate = serverInfo.SampleRate;
        Pitch = serverInfo.Pitch;
        for (size_t i = 0; i < WaveBuffers.size(); i++) {
            WaveBuffers[i] = serverInfo.WaveBuffers[i].ToCommon(2);
        }
        if (SampleFormat_ == SampleFormat::Adpcm) {
            ExtraParameter = serverInfo.DataSourceStateAddressInfo.GetReference(true);
            ExtraParameterSize = serverInfo.DataSourceStateAddressInfo.Size;
        }
        State = state;
        DecodingBehaviour_ = serverInfo.DecodingBehaviour;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType_; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        DataSourceHelper::WaveBufferInformation info{};
        info.SourceSampleRate = SampleRate;
        info.SampleFormat = SampleFormat_;
        info.Pitch = Pitch;
        info.DecodingBehaviour = DecodingBehaviour_;
        info.ExtraParameter = ExtraParameter;
        info.ExtraParameterSize = ExtraParameterSize;
        info.ChannelIndex = static_cast<int32_t>(ChannelIndex);
        info.ChannelCount = static_cast<int32_t>(ChannelCount);
        info.SrcQuality = SrcQuality;
        DataSourceHelper::ProcessWaveBuffers(outputBuffer, info, WaveBuffers, State[0], context.SampleRate, static_cast<int32_t>(context.SampleCount));
    }

  private:
    static CommandType GetCommandTypeBySampleFormat(SampleFormat sampleFormat) {
        switch (sampleFormat) {
            case SampleFormat::Adpcm:
                return CommandType::AdpcmDataSourceVersion2;
            case SampleFormat::PcmInt16:
                return CommandType::PcmInt16DataSourceVersion2;
            case SampleFormat::PcmFloat:
                return CommandType::PcmFloatDataSourceVersion2;
            default:
                NXAUDREN_FATAL("NotImplementedException: unsupported sample format for DataSourceVersion2Command");
        }
    }
};

}  // namespace nxrt::audren
