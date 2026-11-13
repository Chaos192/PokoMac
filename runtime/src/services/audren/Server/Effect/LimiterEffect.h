// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/LimiterEffect.cs (Ryubing fork, MIT). See ReverbEffect.h for the note on the
// non-generic Update overrides. Unlike the reverb and delay effects, the limiter does not check the channel counts.
#pragma once
#include "../../Dsp/State/LimiterState.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class LimiterEffect : public BaseEffect {
  public:
    // The limiter parameter (C# LimiterParameter Parameter; default-initialised).
    LimiterParameter Parameter{};

    // The limiter state (C# Memory<LimiterState>, one element).
    Span<LimiterState> State;

  private:
    EffectStateSlot<LimiterState> _state;

  public:
    LimiterEffect() {
        Type = TargetEffectType();
        State = _state.View();
    }

    EffectType TargetEffectType() const override { return EffectType::Limiter; }

    DspAddress GetWorkBuffer([[maybe_unused]] int index) override { return GetSingleBuffer(); }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion1& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion1>(updateErrorInfo, parameter, mapper);
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion2& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion2>(updateErrorInfo, parameter, mapper);
    }

    template <IEffectInParameter T>
    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const T& parameter, PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        LimiterParameter limiterParameter = EffectParameterHelpers::SpecificDataAs<LimiterParameter>(parameter);

        updateErrorInfo = BehaviourParameter::ErrorInfo{};

        UpdateParameterBase(parameter);

        Parameter = limiterParameter;

        IsEnabled = parameter.is_enabled;

        if (BufferUnmapped || parameter.is_new) {
            UsageState = ::nxrt::audren::UsageState::New;
            Parameter.status = ::nxrt::audren::UsageState::Invalid;

            BufferUnmapped = !mapper.TryAttachBuffer(updateErrorInfo, WorkBuffers[0], parameter.buffer_base, parameter.buffer_size);
        }
    }

    void UpdateForCommandGeneration() override {
        UpdateUsageStateForCommandGeneration();

        Parameter.status = ::nxrt::audren::UsageState::Enabled;
        Parameter.statistics_reset = false;
    }

    void InitializeResultState(EffectResultState& state) override {
        LimiterStatistics statistics = EffectParameterHelpers::LoadBytes<LimiterStatistics>(state.specific_data);

        EffectParameterHelpers::Reset(statistics);

        EffectParameterHelpers::StoreBytes(state.specific_data, statistics);
    }

    void UpdateResultState(EffectResultState& destState, EffectResultState& srcState) override { destState = srcState; }
};

}  // namespace nxrt::audren
