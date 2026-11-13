// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/AuxiliaryBufferEffect.cs (Ryubing fork, MIT). See ReverbEffect.h for the note on
// the non-generic Update overrides. The send and return buffers are resolved through the PoolMapper into WorkBuffers[0] and
// WorkBuffers[1]; the DSP-side addresses of the four buffer-info fields come from those.
#pragma once
#include "../../Dsp/State/AuxiliaryBufferHeader.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class AuxiliaryBufferEffect : public BaseEffect {
  public:
    // The auxiliary buffer parameter (C# AuxiliaryBufferParameter Parameter; default-initialised).
    AuxiliaryBufferParameter Parameter{};

    // Auxiliary buffer state (C# AuxiliaryBufferAddresses State, a plain value).
    AuxiliaryBufferAddresses State{};

    AuxiliaryBufferEffect() { Type = TargetEffectType(); }

    EffectType TargetEffectType() const override { return EffectType::AuxiliaryBuffer; }

    DspAddress GetWorkBuffer(int index) override {
        NXAUDREN_ASSERT(index >= 0 && static_cast<size_t>(index) < WorkBuffers.size());
        return WorkBuffers[static_cast<size_t>(index)].GetReference(true);
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion1& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion1>(updateErrorInfo, parameter, mapper);
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion2& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion2>(updateErrorInfo, parameter, mapper);
    }

    template <IEffectInParameter T>
    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const T& parameter, PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        UpdateParameterBase(parameter);

        Parameter = EffectParameterHelpers::SpecificDataAs<AuxiliaryBufferParameter>(parameter);
        IsEnabled = parameter.is_enabled;

        updateErrorInfo = BehaviourParameter::ErrorInfo{};

        if (BufferUnmapped || parameter.is_new) {
            uint64_t bufferSize = static_cast<uint64_t>(sizeof(int32_t)) * Parameter.buffer_storage_size + sizeof(AuxiliaryBufferHeader);

            // C# `out _`: the send buffer's error info is discarded.
            BehaviourParameter::ErrorInfo sendErrorInfo{};
            bool sendBufferUnmapped = !mapper.TryAttachBuffer(sendErrorInfo, WorkBuffers[0], Parameter.send_buffer_info_address, bufferSize);
            bool returnBufferUnmapped = !mapper.TryAttachBuffer(updateErrorInfo, WorkBuffers[1], Parameter.return_buffer_info_address, bufferSize);

            BufferUnmapped = sendBufferUnmapped && returnBufferUnmapped;

            if (!BufferUnmapped) {
                DspAddress sendDspAddress = WorkBuffers[0].GetReference(false);
                DspAddress returnDspAddress = WorkBuffers[1].GetReference(false);

                State.send_buffer_info = sendDspAddress + sizeof(AuxiliaryBufferHeader::AuxiliaryBufferInfo);
                State.send_buffer_info_base = sendDspAddress + sizeof(AuxiliaryBufferHeader);

                State.return_buffer_info = returnDspAddress + sizeof(AuxiliaryBufferHeader::AuxiliaryBufferInfo);
                State.return_buffer_info_base = returnDspAddress + sizeof(AuxiliaryBufferHeader);
            }
        }
    }

    void UpdateForCommandGeneration() override { UpdateUsageStateForCommandGeneration(); }
};

}  // namespace nxrt::audren
