// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/Reverb3dEffect.cs (Ryubing fork, MIT). See ReverbEffect.h for the note on the
// non-generic Update overrides. The usage state of the parameter is the field parameter_status (C# ParameterStatus).
#pragma once
#include "../../Dsp/State/Reverb3dState.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class Reverb3dEffect : public BaseEffect {
  public:
    // The 3D reverberation parameter (C# Reverb3dParameter Parameter; default-initialised).
    Reverb3dParameter Parameter{};

    // The 3D reverberation state (C# Memory<Reverb3dState>, one element).
    Span<Reverb3dState> State;

  private:
    EffectStateSlot<Reverb3dState> _state;

  public:
    Reverb3dEffect() {
        Type = TargetEffectType();
        State = _state.View();
    }

    EffectType TargetEffectType() const override { return EffectType::Reverb3d; }

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

        Reverb3dParameter reverbParameter = EffectParameterHelpers::SpecificDataAs<Reverb3dParameter>(parameter);

        updateErrorInfo = BehaviourParameter::ErrorInfo{};

        if (EffectParameterHelpers::IsChannelCountMaxValid(reverbParameter)) {
            UpdateParameterBase(parameter);

            ::nxrt::audren::UsageState oldParameterStatus = Parameter.parameter_status;

            Parameter = reverbParameter;

            if (EffectParameterHelpers::IsChannelCountValid(reverbParameter)) {
                IsEnabled = parameter.is_enabled;

                if (oldParameterStatus != ::nxrt::audren::UsageState::Enabled) {
                    Parameter.parameter_status = oldParameterStatus;
                }

                if (BufferUnmapped || parameter.is_new) {
                    UsageState = ::nxrt::audren::UsageState::New;
                    Parameter.parameter_status = ::nxrt::audren::UsageState::Invalid;

                    BufferUnmapped = !mapper.TryAttachBuffer(updateErrorInfo, WorkBuffers[0], parameter.buffer_base, parameter.buffer_size);
                }
            }
        }
    }

    void UpdateForCommandGeneration() override {
        UpdateUsageStateForCommandGeneration();

        Parameter.parameter_status = ::nxrt::audren::UsageState::Enabled;
    }
};

}  // namespace nxrt::audren
