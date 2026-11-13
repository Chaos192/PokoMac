// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/AdpcmDataSourceCommandVersion1.cs (Ryubing fork, MIT).
// Initialize takes the server VoiceInfo as a template argument (members as in PcmInt16DataSourceCommandVersion1.h, plus
// DataSourceStateAddressInfo with GetReference(bool) and Size). The C# AddressInfo of the coefficients is read through that interface.
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../../Constants.h"
#include "../DataSourceHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class AdpcmDataSourceCommandVersion1 : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint16_t OutputBufferIndex = 0;
    uint32_t SampleRate = 0;
    float Pitch = 0.0f;
    std::vector<WaveBuffer> WaveBuffers = std::vector<WaveBuffer>(static_cast<size_t>(Constants::VoiceWaveBufferCount));
    Span<VoiceState> State;
    uint64_t AdpcmParameter = 0;
    uint64_t AdpcmParameterSize = 0;
    DecodingBehaviour DecodingBehaviour_ = DecodingBehaviour::Default;

    template <class VoiceInfoT>
    AdpcmDataSourceCommandVersion1& Initialize(VoiceInfoT& serverInfo, Span<VoiceState> state, uint16_t outputBufferIndex, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        OutputBufferIndex = outputBufferIndex;
        SampleRate = serverInfo.SampleRate;
        Pitch = serverInfo.Pitch;
        for (size_t i = 0; i < WaveBuffers.size(); i++) {
            WaveBuffers[i] = serverInfo.WaveBuffers[i].ToCommon(1);
        }
        AdpcmParameter = serverInfo.DataSourceStateAddressInfo.GetReference(true);
        AdpcmParameterSize = serverInfo.DataSourceStateAddressInfo.Size;
        State = state;
        DecodingBehaviour_ = serverInfo.DecodingBehaviour;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::AdpcmDataSourceVersion1; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);
        DataSourceHelper::WaveBufferInformation info{};
        info.SourceSampleRate = SampleRate;
        info.SampleFormat = SampleFormat::Adpcm;
        info.Pitch = Pitch;
        info.DecodingBehaviour = DecodingBehaviour_;
        info.ExtraParameter = AdpcmParameter;
        info.ExtraParameterSize = AdpcmParameterSize;
        info.ChannelIndex = 0;
        info.ChannelCount = 1;
        info.SrcQuality = SampleRateConversionQuality::Default;
        DataSourceHelper::ProcessWaveBuffers(outputBuffer, info, WaveBuffers, State[0], context.SampleRate, static_cast<int32_t>(context.SampleCount));
    }
};

}  // namespace nxrt::audren
