// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/BiquadFilterEffect.cs (Ryubing fork, MIT). Version 1 parameters (16-bit fixed
// point coefficients) are converted with BiquadFilterHelper::ToBiquadFilterEffectParameter2; version 2 parameters are used
// as they are. See ReverbEffect.h for the note on the non-generic Update overrides.
#pragma once
#include <cstddef>
#include <vector>

#include "../../Constants.h"
#include "../../Dsp/BiquadFilterHelper.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class BiquadFilterEffect : public BaseEffect {
  public:
    // The biquad filter parameter (C# BiquadFilterEffectParameter2 Parameter; default-initialised).
    BiquadFilterEffectParameter2 Parameter{};

    // The biquad filter state (C# Memory<BiquadFilterState>, Constants.ChannelCountMax elements).
    Span<BiquadFilterState> State;

    // The biquad filter effect version (1 or 2).
    int32_t BiquadFilterEffectVersion = 0;

  private:
    // POD state, so value-initialised storage is the C# default value.
    std::vector<BiquadFilterState> _state;

  public:
    explicit BiquadFilterEffect(int32_t version) {
        Type = TargetEffectType();
        BiquadFilterEffectVersion = version;
        _state.resize(static_cast<size_t>(Constants::ChannelCountMax));
        State = Span<BiquadFilterState>(_state.data(), _state.size());
    }

    EffectType TargetEffectType() const override { return EffectType::BiquadFilter; }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion1& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion1>(updateErrorInfo, parameter, mapper);
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion2& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion2>(updateErrorInfo, parameter, mapper);
    }

    template <IEffectInParameter T>
    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const T& parameter, [[maybe_unused]] PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        UpdateParameterBase(parameter);

        if (BiquadFilterEffectVersion == 2) {
            Parameter = EffectParameterHelpers::SpecificDataAs<BiquadFilterEffectParameter2>(parameter);
        } else {
            BiquadFilterEffectParameter1 oldParameter = EffectParameterHelpers::SpecificDataAs<BiquadFilterEffectParameter1>(parameter);
            Parameter = BiquadFilterHelper::ToBiquadFilterEffectParameter2(oldParameter);
        }

        IsEnabled = parameter.is_enabled;

        updateErrorInfo = BehaviourParameter::ErrorInfo{};
    }

    void UpdateForCommandGeneration() override {
        UpdateUsageStateForCommandGeneration();

        Parameter.status = ::nxrt::audren::UsageState::Enabled;
    }
};

}  // namespace nxrt::audren
