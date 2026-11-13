// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Effect/DelayLineReverb3d.cs (Ryubing fork, MIT); the C# class is DelayLine3d.
// Auto-properties CurrentSampleCount and SampleCountMax are methods (see IDelayLine.h).
#pragma once
#include <cmath>
#include <vector>
#include "IDelayLine.h"

namespace nxrt::audren {

class DelayLine3d : public IDelayLine {
  private:
    std::vector<float> _workBuffer;
    uint32_t _sampleRate;
    uint32_t _currentSampleIndex = 0;
    uint32_t _lastSampleIndex = 0;

    uint32_t _currentSampleCount = 0;
    uint32_t _sampleCountMax = 0;

    void ConfigureDelay(uint32_t targetSampleCount) {
        if (_sampleCountMax >= targetSampleCount) {
            _currentSampleCount = targetSampleCount;
            _lastSampleIndex = (_currentSampleIndex + targetSampleCount) % (_sampleCountMax + 1);
        }
    }

  public:
    DelayLine3d(uint32_t sampleRate, float delayTimeMax) : _sampleRate(sampleRate) {
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
        NXAUDREN_ASSERT(std::isfinite(value));

        _workBuffer[_lastSampleIndex++] = value;

        float output = Read();

        _currentSampleIndex++;

        if (_currentSampleIndex >= _sampleCountMax) {
            _currentSampleIndex = 0;
        }

        if (_lastSampleIndex >= _sampleCountMax) {
            _lastSampleIndex = 0;
        }

        return output;
    }

    float TapUnsafe(uint32_t sampleIndex, int32_t offset) override {
        return IDelayLine::Tap(Span<float>(_workBuffer), static_cast<int32_t>(_lastSampleIndex),
                               static_cast<int32_t>(sampleIndex) + offset, static_cast<int32_t>(_sampleCountMax) + 1);
    }

    float Tap(uint32_t sampleIndex) override { return TapUnsafe(sampleIndex, -1); }
};

}  // namespace nxrt::audren
