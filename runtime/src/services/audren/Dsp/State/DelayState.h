// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/State/DelayState.cs (Ryubing fork, MIT).
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/DelayLine.h"
#include "../FixedPointHelper.h"

namespace nxrt::audren {

struct DelayState {
    std::vector<DelayLine> DelayLines;
    std::vector<float> LowPassZ;
    float FeedbackGain = 0.0f;
    float DelayFeedbackBaseGain = 0.0f;
    float DelayFeedbackCrossGain = 0.0f;
    float LowPassFeedbackGain = 0.0f;
    float LowPassBaseGain = 0.0f;

    static constexpr int32_t FixedPointPrecision = 14;

    DelayState(DelayParameter& parameter, CpuAddress workBuffer) {
        (void)workBuffer;
        DelayLines.reserve(parameter.channel_count);
        LowPassZ.assign(parameter.channel_count, 0.0f);

        uint32_t sampleRate = static_cast<uint32_t>(FixedPointHelper::ToInt(static_cast<int64_t>(parameter.sample_rate), FixedPointPrecision)) / 1000;

        for (uint32_t i = 0; i < parameter.channel_count; i++) {
            DelayLines.emplace_back(sampleRate, static_cast<float>(parameter.delay_time_max));
            DelayLines[i].SetDelay(static_cast<float>(parameter.delay_time));
        }

        UpdateParameter(parameter);
    }

    void UpdateParameter(DelayParameter& parameter) {
        FeedbackGain = FixedPointHelper::ToFloat(static_cast<int64_t>(parameter.feedback_gain), FixedPointPrecision) * 0.98f;

        float channelSpread = FixedPointHelper::ToFloat(static_cast<int64_t>(parameter.channel_spread), FixedPointPrecision);

        DelayFeedbackBaseGain = (1.0f - channelSpread) * FeedbackGain;

        if (parameter.channel_count == 4 || parameter.channel_count == 6) {
            DelayFeedbackCrossGain = channelSpread * 0.5f * FeedbackGain;
        } else {
            DelayFeedbackCrossGain = channelSpread * FeedbackGain;
        }

        LowPassFeedbackGain = 0.95f * FixedPointHelper::ToFloat(static_cast<int64_t>(parameter.low_pass_amount), FixedPointPrecision);
        LowPassBaseGain = 1.0f - LowPassFeedbackGain;
    }

    // C# `ref float tempRawRef` + Unsafe.Add(ref tempRawRef, i): element i of the array starting at tempRawRef.
    void UpdateLowPassFilter(float& tempRawRef, uint32_t channelCount) {
        for (uint32_t i = 0; i < channelCount; i++) {
            float lowPassResult = LowPassFeedbackGain * LowPassZ[i] + (&tempRawRef)[i] * LowPassBaseGain;

            LowPassZ[i] = lowPassResult;
            DelayLines[i].Update(lowPassResult);
        }
    }
};

}  // namespace nxrt::audren
