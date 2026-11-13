// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/MultiTapBiquadFilterAndMixCommand.cs (Ryubing fork, MIT).
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BiquadFilterHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class MultiTapBiquadFilterAndMixCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;

  private:
    BiquadFilterParameter2 _parameter0{};
    BiquadFilterParameter2 _parameter1{};

  public:
    Span<BiquadFilterState> BiquadFilterState0;
    Span<BiquadFilterState> BiquadFilterState1;
    Span<BiquadFilterState> PreviousBiquadFilterState0;
    Span<BiquadFilterState> PreviousBiquadFilterState1;

    Span<VoiceState> State;

    int32_t LastSampleIndex = 0;

    float Volume0 = 0.0f;
    float Volume1 = 0.0f;

    bool NeedInitialization0 = false;
    bool NeedInitialization1 = false;
    bool HasVolumeRamp = false;
    bool IsFirstMixBuffer = false;

    MultiTapBiquadFilterAndMixCommand() = default;

    MultiTapBiquadFilterAndMixCommand& Initialize(float volume0, float volume1, uint32_t inputBufferIndex, uint32_t outputBufferIndex,
                                                  int32_t lastSampleIndex, Span<VoiceState> state, BiquadFilterParameter2& filter0,
                                                  BiquadFilterParameter2& filter1, Span<BiquadFilterState> biquadFilterState0,
                                                  Span<BiquadFilterState> biquadFilterState1, Span<BiquadFilterState> previousBiquadFilterState0,
                                                  Span<BiquadFilterState> previousBiquadFilterState1, bool needInitialization0,
                                                  bool needInitialization1, bool hasVolumeRamp, bool isFirstMixBuffer, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;

        InputBufferIndex = static_cast<uint16_t>(inputBufferIndex);
        OutputBufferIndex = static_cast<uint16_t>(outputBufferIndex);

        _parameter0 = filter0;
        _parameter1 = filter1;
        BiquadFilterState0 = biquadFilterState0;
        BiquadFilterState1 = biquadFilterState1;
        PreviousBiquadFilterState0 = previousBiquadFilterState0;
        PreviousBiquadFilterState1 = previousBiquadFilterState1;

        State = state;
        LastSampleIndex = lastSampleIndex;

        Volume0 = volume0;
        Volume1 = volume1;

        NeedInitialization0 = needInitialization0;
        NeedInitialization1 = needInitialization1;
        HasVolumeRamp = hasVolumeRamp;
        IsFirstMixBuffer = isFirstMixBuffer;

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::MultiTapBiquadFilterAndMix; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);

        UpdateState(BiquadFilterState0, PreviousBiquadFilterState0, NeedInitialization0);
        UpdateState(BiquadFilterState1, PreviousBiquadFilterState1, NeedInitialization1);

        if (HasVolumeRamp) {
            float volume = Volume0;
            float ramp = (Volume1 - Volume0) / static_cast<float>(context.SampleCount);

            State[0].last_samples[LastSampleIndex] = BiquadFilterHelper::ProcessDoubleBiquadFilterAndMixRamp(
                _parameter0, _parameter1, BiquadFilterState0[0], BiquadFilterState1[0], outputBuffer, inputBuffer, context.SampleCount, volume, ramp);
        } else {
            BiquadFilterHelper::ProcessDoubleBiquadFilterAndMix(_parameter0, _parameter1, BiquadFilterState0[0], BiquadFilterState1[0], outputBuffer,
                                                                inputBuffer, context.SampleCount, Volume1);
        }
    }

  private:
    void UpdateState(Span<BiquadFilterState> state, Span<BiquadFilterState> previousState, bool needInitialization) {
        if (needInitialization) {
            // If there is no previous state, initialize to zero.
            state[0] = BiquadFilterState{};
        } else if (IsFirstMixBuffer) {
            // This is the first buffer, set previous state to current state.
            previousState[0] = state[0];
        } else {
            // Rewind the current state by copying back the previous state.
            state[0] = previousState[0];
        }
    }
};

}  // namespace nxrt::audren
