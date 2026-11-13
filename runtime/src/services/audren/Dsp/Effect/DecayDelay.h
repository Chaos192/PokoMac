// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Effect/DecayDelay.cs (Ryubing fork, MIT). The wrapped delay line is held by reference,
// as the C# holds it by reference. Properties with a body (CurrentSampleCount, SampleCountMax) are methods.
#pragma once
#include <memory>

#include "IDelayLine.h"

namespace nxrt::audren {

class DecayDelay : public IDelayLine {
  private:
    std::unique_ptr<IDelayLine> _owned;   // set when the DecayDelay owns its delay line (C# relies on the GC for that)
    IDelayLine* _ptr;
    IDelayLine& _delayLine;

    float _decayRate = 0.0f;

  public:
    explicit DecayDelay(IDelayLine& delayLine) : _ptr(&delayLine), _delayLine(delayLine) {}
    // Owning form: `new DecayDelay(new DelayLine(...))` in the C#.
    explicit DecayDelay(std::unique_ptr<IDelayLine> delayLine) : _owned(std::move(delayLine)), _ptr(_owned.get()), _delayLine(*_ptr) {}
    DecayDelay(DecayDelay&& o) noexcept : _owned(std::move(o._owned)), _ptr(o._ptr), _delayLine(*o._ptr), _decayRate(o._decayRate) {}

    uint32_t CurrentSampleCount() const override { return _delayLine.CurrentSampleCount(); }

    uint32_t SampleCountMax() const override { return _delayLine.SampleCountMax(); }

    void SetDecayRate(float decayRate) { _decayRate = decayRate; }

    float Update(float value) override {
        float delayLineValue = _delayLine.Read();
        float processedValue = value - (_decayRate * delayLineValue);

        return _delayLine.Update(processedValue) + processedValue * _decayRate;
    }

    void SetDelay(float delayTime) override { _delayLine.SetDelay(delayTime); }

    float Read() override { return _delayLine.Read(); }

    float TapUnsafe(uint32_t sampleIndex, int32_t offset) override { return _delayLine.TapUnsafe(sampleIndex, offset); }

    float Tap(uint32_t sampleIndex) override { return _delayLine.Tap(sampleIndex); }
};

}  // namespace nxrt::audren
