// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/BiquadFilterAndMixCommand.cs (Ryubing fork, MIT).
// The C# property `Memory<BiquadFilterState> BiquadFilterState` shares its name with the type; the type is written qualified
// inside this class.
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BiquadFilterHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class BiquadFilterAndMixCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;

  private:
    BiquadFilterParameter2 _parameter{};

  public:
    Span<::nxrt::audren::BiquadFilterState> BiquadFilterState;
    Span<::nxrt::audren::BiquadFilterState> PreviousBiquadFilterState;

    Span<VoiceState> State;

    int32_t LastSampleIndex = 0;

    float Volume0 = 0.0f;
    float Volume1 = 0.0f;

    bool NeedInitialization = false;
    bool HasVolumeRamp = false;
    bool IsFirstMixBuffer = false;

    BiquadFilterAndMixCommand() = default;

    BiquadFilterAndMixCommand& Initialize(float volume0, float volume1, uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t lastSampleIndex,
                                          Span<VoiceState> state, BiquadFilterParameter2& filter,
                                          Span<::nxrt::audren::BiquadFilterState> biquadFilterState,
                                          Span<::nxrt::audren::BiquadFilterState> previousBiquadFilterState, bool needInitialization, bool hasVolumeRamp,
                                          bool isFirstMixBuffer, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;

        InputBufferIndex = static_cast<uint16_t>(inputBufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(outputBufferIndex);

        _parameter = filter;
        BiquadFilterState = biquadFilterState;
        PreviousBiquadFilterState = previousBiquadFilterState;

        State = state;
        LastSampleIndex = lastSampleIndex;

        Volume0 = volume0;
        Volume1 = volume1;

        NeedInitialization = needInitialization;
        HasVolumeRamp = hasVolumeRamp;
        IsFirstMixBuffer = isFirstMixBuffer;

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::BiquadFilterAndMix; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);

        if (NeedInitialization) {
            // If there is no previous state, initialize to zero.
            BiquadFilterState[0] = ::nxrt::audren::BiquadFilterState{};
        } else if (IsFirstMixBuffer) {
            // This is the first buffer, set previous state to current state.
            PreviousBiquadFilterState[0] = BiquadFilterState[0];
        } else {
            // Rewind the current state by copying back the previous state.
            BiquadFilterState[0] = PreviousBiquadFilterState[0];
        }

        if (HasVolumeRamp) {
            float volume = Volume0;
            float ramp = (Volume1 - Volume0) / static_cast<float>(context.SampleCount);

            State[0].last_samples[LastSampleIndex] = BiquadFilterHelper::ProcessBiquadFilterAndMixRamp(
                _parameter, BiquadFilterState[0], outputBuffer, inputBuffer, context.SampleCount, volume, ramp);
        } else {
            BiquadFilterHelper::ProcessBiquadFilterAndMix(_parameter, BiquadFilterState[0], outputBuffer, inputBuffer, context.SampleCount, Volume1);
        }
    }
};

}  // namespace nxrt::audren
