// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Sink/CircularBufferSink.cs (Ryubing fork, MIT). Server state of a circular buffer sink.
#pragma once
#include <cstring>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/AddressInfo.h"
#include "BaseSink.h"

namespace nxrt::audren {

class CircularBufferSink : public BaseSink {
  public:
    // The circular buffer parameter.
    CircularBufferParameter Parameter{};
    // The current offset to write data on the circular buffer (C# `{ get; private set; }`).
    uint32_t CurrentWriteOffset = 0;
    // The AddressInfo of the circular buffer.
    AddressInfo CircularBufferAddressInfo;

  private:
    // The last written data offset on the circular buffer.
    uint32_t _lastWrittenOffset = 0;
    // The previous written offset of the circular buffer.
    uint32_t _oldWrittenOffset = 0;

  public:
    CircularBufferSink() {
        CircularBufferAddressInfo = AddressInfo::Create();
        // See BaseSink.h: C# runs the override here, C++ needs the explicit call.
        CleanUp();
    }

    SinkType TargetSinkType() const override { return SinkType::CircularBuffer; }

    void Update(BehaviourParameter::ErrorInfo& errorInfo, const SinkInParameter& parameter, SinkOutStatus& outStatus,
                PoolMapper& mapper) override {
        errorInfo = BehaviourParameter::ErrorInfo{};
        outStatus = SinkOutStatus{};

        NXAUDREN_ASSERT(IsTypeValid(parameter));

        // C# MemoryMarshal.Cast<byte, CircularBufferParameter>(SpecificData)[0], copied out because the bytes may be unaligned.
        CircularBufferParameter inputDeviceParameter{};
        std::memcpy(&inputDeviceParameter, parameter.specific_data, sizeof(CircularBufferParameter));

        if (parameter.is_used != IsUsed || ShouldSkip) {
            UpdateStandardParameter(parameter);

            if (parameter.is_used) {
                NXAUDREN_ASSERT(CircularBufferAddressInfo.CpuAddress == 0);
                NXAUDREN_ASSERT(CircularBufferAddressInfo.GetReference(false) == 0);

                ShouldSkip = !mapper.TryAttachBuffer(errorInfo, CircularBufferAddressInfo, inputDeviceParameter.buffer_address,
                                                     inputDeviceParameter.buffer_size);
            } else {
                NXAUDREN_ASSERT(CircularBufferAddressInfo.CpuAddress != 0);
                NXAUDREN_ASSERT(CircularBufferAddressInfo.GetReference(false) != 0);
            }

            Parameter = inputDeviceParameter;
        }

        outStatus.last_written_offset = _lastWrittenOffset;
    }

    void UpdateForCommandGeneration() override {
        NXAUDREN_ASSERT(Type == TargetSinkType());

        if (IsUsed) {
            // C# int * uint * uint is computed as uint here (the frame size is a uint).
            uint32_t frameSize = static_cast<uint32_t>(Constants::TargetSampleSize) * Parameter.sample_count * Parameter.input_count;

            _lastWrittenOffset = _oldWrittenOffset;

            _oldWrittenOffset = CurrentWriteOffset;

            CurrentWriteOffset += frameSize;

            if (Parameter.buffer_size > 0) {
                CurrentWriteOffset %= Parameter.buffer_size;
            }
        }
    }

    void CleanUp() override {
        CircularBufferAddressInfo = AddressInfo::Create();
        _lastWrittenOffset = 0;
        _oldWrittenOffset = 0;
        CurrentWriteOffset = 0;
        BaseSink::CleanUp();
    }
};

}  // namespace nxrt::audren
