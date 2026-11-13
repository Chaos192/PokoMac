// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/State/LimiterState.cs (Ryubing fork, MIT).
#pragma once
#include <algorithm>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/ExponentialMovingAverage.h"

namespace nxrt::audren {

struct LimiterState {
    std::vector<ExponentialMovingAverage> DetectorAverage;
    std::vector<ExponentialMovingAverage> CompressionGainAverage;
    std::vector<float> DelayedSampleBuffer;
    std::vector<int32_t> DelayedSampleBufferPosition;

    LimiterState(LimiterParameter& parameter, CpuAddress workBuffer) {
        (void)workBuffer;
        DetectorAverage.assign(parameter.channel_count, ExponentialMovingAverage(0.0f));
        CompressionGainAverage.assign(parameter.channel_count, ExponentialMovingAverage(1.0f));
        DelayedSampleBuffer.assign(static_cast<size_t>(parameter.channel_count) * static_cast<size_t>(parameter.delay_buffer_sample_count_max), 0.0f);
        DelayedSampleBufferPosition.assign(parameter.channel_count, 0);

        UpdateParameter(parameter);
    }

    static void UpdateParameter([[maybe_unused]] LimiterParameter& parameter) {}
};

}  // namespace nxrt::audren
