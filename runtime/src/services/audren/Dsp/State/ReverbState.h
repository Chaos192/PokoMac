// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/State/ReverbState.cs (Ryubing fork, MIT).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/DecayDelay.h"
#include "../Effect/DelayLine.h"
#include "../Effect/IDelayLine.h"
#include "../FixedPointHelper.h"
#include "../FloatingPointHelper.h"

namespace nxrt::audren {

struct ReverbState {
    static inline const std::vector<float> _fdnDelayTimes = {
        // Room
        53.953247f, 79.192566f, 116.238770f, 130.615295f,
        // Hall
        53.953247f, 79.192566f, 116.238770f, 170.615295f,
        // Plate
        5.0f, 10.0f, 5.0f, 10.0f,
        // Cathedral
        47.03f, 71.0f, 103.0f, 170.0f,
        // Max delay (Hall is the one with the highest values so identical to Hall)
        53.953247f, 79.192566f, 116.238770f, 170.615295f};

    static inline const std::vector<float> _decayDelayTimes = {
        // Room
        7.0f, 9.0f, 13.0f, 17.0f,
        // Hall
        7.0f, 9.0f, 13.0f, 17.0f,
        // Plate (no decay)
        1.0f, 1.0f, 1.0f, 1.0f,
        // Cathedral
        7.0f, 7.0f, 13.0f, 9.0f,
        // Max delay (Hall is the one with the highest values so identical to Hall)
        7.0f, 9.0f, 13.0f, 17.0f};

    static inline const std::vector<float> _earlyDelayTimes = {
        // Room
        0.0f, 3.5f, 2.8f, 3.9f, 2.7f, 13.4f, 7.9f, 8.4f, 9.9f, 12.0f,
        // Chamber
        0.0f, 11.8f, 5.5f, 11.2f, 10.4f, 38.1f, 22.2f, 29.6f, 21.2f, 24.8f,
        // Hall
        0.0f, 41.5f, 20.5f, 41.3f, 0.0f, 29.5f, 33.8f, 45.2f, 46.8f, 0.0f,
        // Cathedral
        33.1f, 43.3f, 22.8f, 37.9f, 14.9f, 35.3f, 17.9f, 34.2f, 0.0f, 43.3f,
        // Disabled
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    static inline const std::vector<float> _earlyGainBase = {
        // Room
        0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f,
        // Chamber
        0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f, 0.68f, 0.68f,
        // Hall
        0.50f, 0.70f, 0.70f, 0.68f, 0.50f, 0.68f, 0.68f, 0.70f, 0.68f, 0.00f,
        // Cathedral
        0.93f, 0.92f, 0.87f, 0.86f, 0.94f, 0.81f, 0.80f, 0.77f, 0.76f, 0.65f,
        // Disabled
        0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f};

    static inline const std::vector<float> _preDelayTimes = {
        // Room
        12.5f,
        // Chamber
        40.0f,
        // Hall
        50.0f,
        // Cathedral
        50.0f,
        // Disabled
        0.0f};

    std::vector<DelayLine> FdnDelayLines;
    std::vector<DecayDelay> DecayDelays;
    DelayLine PreDelayLine;
    DelayLine FrontCenterDelayLine;
    std::vector<uint32_t> EarlyDelayTime;
    std::vector<float> EarlyGain;
    uint32_t PreDelayLineDelayTime = 0;

    std::vector<float> HighFrequencyDecayDirectGain;
    std::vector<float> HighFrequencyDecayPreviousGain;
    std::vector<float> PreviousFeedbackOutput;

    static constexpr int32_t EarlyModeCount = 10;

    static constexpr int32_t FixedPointPrecision = 14;

  private:
    static Span<const float> GetFdnDelayTimesByLateMode(ReverbLateMode lateMode) {
        return Span<const float>(_fdnDelayTimes.data() + static_cast<size_t>(static_cast<uint32_t>(lateMode)) * 4, 4);
    }

    static Span<const float> GetDecayDelayTimesByLateMode(ReverbLateMode lateMode) {
        return Span<const float>(_decayDelayTimes.data() + static_cast<size_t>(static_cast<uint32_t>(lateMode)) * 4, 4);
    }

  private:
    static uint32_t SampleRateOf(const ReverbParameter& parameter) {
        return static_cast<uint32_t>(FixedPointHelper::ToFloat(static_cast<int64_t>(static_cast<uint32_t>(parameter.sample_rate)), FixedPointPrecision));
    }

    static float PreDelayTimeMax(bool isLongSizePreDelaySupported) { return isLongSizePreDelaySupported ? 350.0f : 150.0f; }

  public:
    ReverbState(ReverbParameter& parameter, CpuAddress workBuffer, bool isLongSizePreDelaySupported)
        : PreDelayLine(SampleRateOf(parameter), PreDelayTimeMax(isLongSizePreDelaySupported)),
          FrontCenterDelayLine(SampleRateOf(parameter), 5.0f) {
        (void)workBuffer;
        EarlyDelayTime.assign(static_cast<size_t>(EarlyModeCount), 0);
        EarlyGain.assign(static_cast<size_t>(EarlyModeCount), 0.0f);
        HighFrequencyDecayDirectGain.assign(4, 0.0f);
        HighFrequencyDecayPreviousGain.assign(4, 0.0f);
        PreviousFeedbackOutput.assign(4, 0.0f);

        Span<const float> fdnDelayTimes = GetFdnDelayTimesByLateMode(ReverbLateMode::Limit);
        Span<const float> decayDelayTimes = GetDecayDelayTimesByLateMode(ReverbLateMode::Limit);

        uint32_t sampleRate = SampleRateOf(parameter);

        for (size_t i = 0; i < 4; i++) {
            FdnDelayLines.emplace_back(sampleRate, fdnDelayTimes[i]);
            DecayDelays.emplace_back(std::make_unique<DelayLine>(sampleRate, decayDelayTimes[i]));
        }

        UpdateParameter(parameter);
    }

    void UpdateParameter(ReverbParameter& parameter) {
        uint32_t sampleRate = static_cast<uint32_t>(FixedPointHelper::ToFloat(static_cast<int64_t>(static_cast<uint32_t>(parameter.sample_rate)), FixedPointPrecision));

        float preDelayTimeInMilliseconds = FixedPointHelper::ToFloat(parameter.pre_delay_time, FixedPointPrecision);
        float earlyGain = FixedPointHelper::ToFloat(parameter.early_gain, FixedPointPrecision);
        float coloration = FixedPointHelper::ToFloat(parameter.coloration, FixedPointPrecision);
        float decayTime = FixedPointHelper::ToFloat(parameter.decay_time, FixedPointPrecision);

        for (int32_t i = 0; i < 10; i++) {
            EarlyDelayTime[i] = std::min(IDelayLine::GetSampleCount(sampleRate, _earlyDelayTimes[i] + preDelayTimeInMilliseconds), PreDelayLine.SampleCountMax()) + 1;
            EarlyGain[i] = _earlyGainBase[i] * earlyGain;
        }

        if (parameter.channel_count == 2) {
            EarlyGain[4] = EarlyGain[4] * 0.5f;
            EarlyGain[5] = EarlyGain[5] * 0.5f;
        }

        PreDelayLineDelayTime = std::min(IDelayLine::GetSampleCount(sampleRate, _preDelayTimes[static_cast<size_t>(static_cast<uint32_t>(parameter.early_mode))] + preDelayTimeInMilliseconds), PreDelayLine.SampleCountMax());

        Span<const float> fdnDelayTimes = GetFdnDelayTimesByLateMode(parameter.late_mode);
        Span<const float> decayDelayTimes = GetDecayDelayTimesByLateMode(parameter.late_mode);

        float highFrequencyDecayRatio = FixedPointHelper::ToFloat(parameter.high_frequency_decay_ratio, FixedPointPrecision);
        float highFrequencyUnknownValue = FloatingPointHelper::Cos(1280.0f / static_cast<float>(sampleRate));

        for (size_t i = 0; i < 4; i++) {
            FdnDelayLines[i].SetDelay(fdnDelayTimes[i]);
            DecayDelays[i].SetDelay(decayDelayTimes[i]);

            // C#: -3 (int) * uint -> long, then to float.
            uint32_t sampleSum = DecayDelays[i].CurrentSampleCount() + FdnDelayLines[i].CurrentSampleCount();
            float tempA = static_cast<float>(-3LL * static_cast<int64_t>(sampleSum));
            float tempB = tempA / (decayTime * static_cast<float>(sampleRate));
            float tempC;
            float tempD;

            if (highFrequencyDecayRatio < 0.995f) {
                float tempE = FloatingPointHelper::Pow10((((1.0f / highFrequencyDecayRatio) - 1.0f) * 2) / 100 * (tempB / 10));
                float tempF = 1.0f - tempE;
                float tempG = 2.0f - (tempE * 2 * highFrequencyUnknownValue);
                float tempH = std::sqrt((tempG * tempG) - (tempF * tempF * 4));

                tempC = (tempG - tempH) / (tempF * 2);
                tempD = 1.0f - tempC;
            } else {
                // no high frequency decay ratio
                tempC = 0.0f;
                tempD = 1.0f;
            }

            HighFrequencyDecayDirectGain[i] = FloatingPointHelper::Pow10(tempB / 1000) * tempD * 0.7071f;
            HighFrequencyDecayPreviousGain[i] = tempC;
            PreviousFeedbackOutput[i] = 0.0f;

            DecayDelays[i].SetDecayRate(0.6f * (1.0f - coloration));
        }
    }
};

}  // namespace nxrt::audren
