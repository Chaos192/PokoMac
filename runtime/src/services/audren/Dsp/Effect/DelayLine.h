// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Effect/DelayLine.cs (Ryubing fork, MIT). Auto-properties CurrentSampleCount and
// SampleCountMax are methods (see IDelayLine.h); their C# backing fields are _currentSampleCount and _sampleCountMax.
#pragma once
#include <algorithm>
#include <vector>
#include "IDelayLine.h"

namespace nxrt::audren {

class DelayLine : public IDelayLine {
  private:
    std::vector<float> _workBuffer;
    uint32_t _sampleRate;
    uint32_t _currentSampleIndex = 0;
    uint32_t _lastSampleIndex = 0;

    uint32_t _currentSampleCount = 0;
    uint32_t _sampleCountMax = 0;

    void ConfigureDelay(uint32_t targetSampleCount) {
        _currentSampleCount = std::min(_sampleCountMax, targetSampleCount);
        _currentSampleIndex = 0;

        if (_currentSampleCount == 0) {
            _lastSampleIndex = 0;
        } else {
            _lastSampleIndex = _currentSampleCount - 1;
        }
    }

  public:
    DelayLine(uint32_t sampleRate, float delayTimeMax) : _sampleRate(sampleRate) {
        _sampleCountMax = IDelayLine::GetSampleCount(_sampleRate, delayTimeMax);
        _workBuffer.assign(static_cast<size_t>(_sampleCountMax) + 1, 0.0f);

        SetDelay(delayTimeMax);
    }

    uint32_t CurrentSampleCount() const override { return _currentSampleCount; }
    uint32_t SampleCountMax() const override { return _sampleCountMax; }

    void SetDelay(float delayTime) override {
        ConfigureDelay(IDelayLine::GetSampleCount(_sampleRate, delayTime));
    }

    float Read() override { return _workBuffer[_currentSampleIndex]; }

    float Update(float value) override {
        float output = Read();

        _workBuffer[_currentSampleIndex++] = value;

        if (_currentSampleIndex >= _lastSampleIndex) {
            _currentSampleIndex = 0;
        }

        return output;
    }

    float TapUnsafe(uint32_t sampleIndex, int32_t offset) override {
        return IDelayLine::Tap(Span<float>(_workBuffer), static_cast<int32_t>(_currentSampleIndex),
                               static_cast<int32_t>(sampleIndex) + offset, static_cast<int32_t>(_currentSampleCount));
    }

    float Tap(uint32_t sampleIndex) override {
        if (sampleIndex >= _currentSampleCount) {
            sampleIndex = _currentSampleCount - 1;
        }

        return TapUnsafe(sampleIndex, -1);
    }
};

}  // namespace nxrt::audren
