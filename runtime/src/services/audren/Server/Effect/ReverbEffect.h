// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/ReverbEffect.cs (Ryubing fork, MIT).
//
// The C# non-generic Update overrides call `Update(out ..., in parameter, mapper)`, which resolves back to the same non-generic
// overload (a non-generic method beats the generic one). Here they call the generic body explicitly with Update<T>, which is
// the evident intent of the C# code.
#pragma once
#include "../../Dsp/State/ReverbState.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class ReverbEffect : public BaseEffect {
  public:
    // The reverberation parameter (C# ReverbParameter Parameter; default-initialised).
    ReverbParameter Parameter{};

    // The reverberation state (C# Memory<ReverbState>, one element).
    Span<ReverbState> State;

  private:
    EffectStateSlot<ReverbState> _state;

  public:
    ReverbEffect() {
        Type = TargetEffectType();
        State = _state.View();
    }

    EffectType TargetEffectType() const override { return EffectType::Reverb; }

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

        ReverbParameter reverbParameter = EffectParameterHelpers::SpecificDataAs<ReverbParameter>(parameter);

        updateErrorInfo = BehaviourParameter::ErrorInfo{};

        if (EffectParameterHelpers::IsChannelCountMaxValid(reverbParameter)) {
            UpdateParameterBase(parameter);

            ::nxrt::audren::UsageState oldParameterStatus = Parameter.status;

            Parameter = reverbParameter;

            if (EffectParameterHelpers::IsChannelCountValid(reverbParameter)) {
                IsEnabled = parameter.is_enabled;

                if (oldParameterStatus != ::nxrt::audren::UsageState::Enabled) {
                    Parameter.status = oldParameterStatus;
                }

                if (BufferUnmapped || parameter.is_new) {
                    UsageState = ::nxrt::audren::UsageState::New;
                    Parameter.status = ::nxrt::audren::UsageState::Invalid;

                    BufferUnmapped = !mapper.TryAttachBuffer(updateErrorInfo, WorkBuffers[0], parameter.buffer_base, parameter.buffer_size);
                }
            }
        }
    }

    void UpdateForCommandGeneration() override {
        UpdateUsageStateForCommandGeneration();

        Parameter.status = ::nxrt::audren::UsageState::Enabled;
    }
};

}  // namespace nxrt::audren
