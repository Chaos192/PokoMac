// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Effect/IDelayLine.cs (Ryubing fork, MIT).
// The C# interface properties CurrentSampleCount and SampleCountMax become pure virtual methods: a base class cannot hold
// a data member that the implementations also provide as a C# auto-property, so implementers return them from methods.
#pragma once
#include <cmath>
#include "../../audren_common.h"

namespace nxrt::audren {

class IDelayLine {
  public:
    virtual ~IDelayLine() = default;

    virtual uint32_t CurrentSampleCount() const = 0;
    virtual uint32_t SampleCountMax() const = 0;

    virtual void SetDelay(float delayTime) = 0;
    virtual float Read() = 0;
    virtual float Update(float value) = 0;

    virtual float TapUnsafe(uint32_t sampleIndex, int32_t offset) = 0;
    virtual float Tap(uint32_t sampleIndex) = 0;

    static float Tap(Span<float> workBuffer, int32_t baseIndex, int32_t sampleIndex, int32_t delaySampleCount) {
        int32_t targetIndex = baseIndex - sampleIndex;

        if (targetIndex < 0) {
            targetIndex += delaySampleCount;
        }

        return workBuffer[static_cast<size_t>(targetIndex)];
    }

    // C# (uint)MathF.Round(sampleRate * delayTime): round half to even.
    static uint32_t GetSampleCount(uint32_t sampleRate, float delayTime) {
        return static_cast<uint32_t>(std::nearbyint(static_cast<float>(sampleRate) * delayTime));
    }
};

}  // namespace nxrt::audren
