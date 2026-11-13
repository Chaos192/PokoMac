// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/BufferMixEffect.cs (Ryubing fork, MIT). The buffer mix effect has no work
// buffers. See ReverbEffect.h for the note on the non-generic Update overrides.
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class BufferMixEffect : public BaseEffect {
  public:
    // The buffer mix parameter (C# BufferMixParameter Parameter; default-initialised).
    BufferMixParameter Parameter{};

    BufferMixEffect() { Type = TargetEffectType(); }

    EffectType TargetEffectType() const override { return EffectType::BufferMix; }

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

        Parameter = EffectParameterHelpers::SpecificDataAs<BufferMixParameter>(parameter);
        IsEnabled = parameter.is_enabled;

        updateErrorInfo = BehaviourParameter::ErrorInfo{};
    }

    void UpdateForCommandGeneration() override { UpdateUsageStateForCommandGeneration(); }
};

}  // namespace nxrt::audren
