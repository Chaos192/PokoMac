// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/DelayEffect.cs (Ryubing fork, MIT). See ReverbEffect.h for the note on the
// non-generic Update overrides.
#pragma once
#include "../../Dsp/State/DelayState.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class DelayEffect : public BaseEffect {
  public:
    // The delay parameter (C# DelayParameter Parameter; default-initialised).
    DelayParameter Parameter{};

    // The delay state (C# Memory<DelayState>, one element).
    Span<DelayState> State;

  private:
    EffectStateSlot<DelayState> _state;

  public:
    DelayEffect() {
        Type = TargetEffectType();
        State = _state.View();
    }

    EffectType TargetEffectType() const override { return EffectType::Delay; }

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

        DelayParameter delayParameter = EffectParameterHelpers::SpecificDataAs<DelayParameter>(parameter);

        updateErrorInfo = BehaviourParameter::ErrorInfo{};

        if (EffectParameterHelpers::IsChannelCountMaxValid(delayParameter)) {
            UpdateParameterBase(parameter);

            ::nxrt::audren::UsageState oldParameterStatus = Parameter.status;

            Parameter = delayParameter;

            if (EffectParameterHelpers::IsChannelCountValid(delayParameter)) {
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
