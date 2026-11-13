// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/CommandProcessingTimeEstimatorVersion4.cs (Ryubing fork, MIT). Version 4 (added with REV10).
#pragma once
#include "CommandProcessingTimeEstimatorVersion3.h"

namespace nxrt::audren {

class CommandProcessingTimeEstimatorVersion4 : public CommandProcessingTimeEstimatorVersion3 {
  public:
    CommandProcessingTimeEstimatorVersion4(uint32_t sampleCount, uint32_t bufferCount) : CommandProcessingTimeEstimatorVersion3(sampleCount, bufferCount) {}

    // Keeps the version 3 overloads visible next to the ones overridden here (C# inherits them).
    using CommandProcessingTimeEstimatorVersion3::Estimate;

    uint32_t Estimate(const MultiTapBiquadFilterCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(7424.5f);
        }

        return estimator_detail::FloatToUInt32(9730.4f);
    }

    uint32_t Estimate(const CaptureBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::FloatToUInt32(435.2f);
            }

            return estimator_detail::FloatToUInt32(4261.0f);
        }

        if (command.Enabled) {
            return estimator_detail::FloatToUInt32(5858.26f);
        }

        return estimator_detail::FloatToUInt32(435.2f);
    }
};

}  // namespace nxrt::audren
