// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/State/CompressorState.cs (Ryubing fork, MIT).
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/ExponentialMovingAverage.h"
#include "../FloatingPointHelper.h"

namespace nxrt::audren {

struct CompressorState {
    ExponentialMovingAverage InputMovingAverage;
    float Unknown4 = 0.0f;
    ExponentialMovingAverage CompressionGainAverage;
    float CompressorGainReduction = 0.0f;
    float Unknown10 = 0.0f;
    float Unknown14 = 0.0f;
    float PreviousCompressionEmaAlpha = 0.0f;
    float MakeupGain = 0.0f;
    float OutputGain = 0.0f;

    explicit CompressorState(CompressorParameter& parameter)
        : InputMovingAverage(0.0f), Unknown4(1.0f), CompressionGainAverage(1.0f) {
        UpdateParameter(parameter);
    }

    void UpdateParameter(CompressorParameter& parameter) {
        float threshold = parameter.threshold;
        float ratio = 1.0f / parameter.ratio;
        float attackCoefficient = parameter.attack_coefficient;
        float makeupGain;

        if (parameter.makeup_gain_enabled) {
            makeupGain = (threshold * 0.5f * (ratio - 1.0f)) - 3.0f;
        } else {
            makeupGain = 0.0f;
        }

        PreviousCompressionEmaAlpha = attackCoefficient;
        MakeupGain = makeupGain;
        CompressorGainReduction = (1.0f - ratio) / static_cast<float>(ChannelCountMax);
        Unknown10 = threshold - 1.5f;
        Unknown14 = threshold + 1.5f;
        OutputGain = FloatingPointHelper::DecibelToLinear(parameter.output_gain + makeupGain);
    }
};

}  // namespace nxrt::audren
