// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/CommandProcessingTimeEstimatorVersion5.cs (Ryubing fork, MIT). Version 5 (added with REV11).
#pragma once
#include "CommandProcessingTimeEstimatorVersion4.h"

namespace nxrt::audren {

class CommandProcessingTimeEstimatorVersion5 : public CommandProcessingTimeEstimatorVersion4 {
  public:
    CommandProcessingTimeEstimatorVersion5(uint32_t sampleCount, uint32_t bufferCount) : CommandProcessingTimeEstimatorVersion4(sampleCount, bufferCount) {}

    // Keeps the version 3 and 4 overloads visible next to the ones overridden here (C# inherits them).
    using CommandProcessingTimeEstimatorVersion4::Estimate;

    uint32_t Estimate(const DelayCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 8929.0f, 25501.0f, 47760.0f, 82203.0f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 1295.20f, 1213.60f, 942.03f, 1001.6f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 11941.0f, 37197.0f, 69750.0f, 12004.0f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 997.67f, 977.63f, 792.31f, 875.43f);
    }

    uint32_t Estimate(const ReverbCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 81475.0f, 84975.0f, 91625.0f, 95332.0f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 536.30f, 588.80f, 643.70f, 706.0f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 120170.0f, 125260.0f, 135750.0f, 141130.0f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 617.64f, 659.54f, 711.44f, 778.07f);
    }

    uint32_t Estimate(const Reverb3dCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 116750.0f, 125910.0f, 146340.0f, 165810.0f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 735.0f, 766.62f, 834.07f, 875.44f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 170290.0f, 183880.0f, 214700.0f, 243850.0f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 508.47f, 582.45f, 626.42f, 682.47f);
    }

    uint32_t Estimate(const CompressorCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const CompressorParameter parameter = command.Parameter();

        if (SampleCount == 160) {
            if (command.Enabled) {
                if (parameter.statistics_enabled) {
                    return estimator_detail::SelectByChannelCount(parameter.channel_count, 22100.0f, 33211.0f, 41587.0f, 58819.0f);
                }

                return estimator_detail::SelectByChannelCount(parameter.channel_count, 19052.0f, 29852.0f, 37904.0f, 55020.0f);
            }

            return estimator_detail::SelectByChannelCount(parameter.channel_count, 630.12f, 638.27f, 705.86f, 782.02f);
        }

        if (command.Enabled) {
            if (parameter.statistics_enabled) {
                return estimator_detail::SelectByChannelCount(parameter.channel_count, 32518.0f, 49102.0f, 61685.0f, 87250.0f);
            }

            return estimator_detail::SelectByChannelCount(parameter.channel_count, 27963.0f, 44016.0f, 56183.0f, 81862.0f);
        }

        return estimator_detail::SelectByChannelCount(parameter.channel_count, 840.14f, 826.1f, 901.88f, 965.29f);
    }

    uint32_t Estimate(const BiquadFilterAndMixCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (command.HasVolumeRamp) {
            if (SampleCount == 160) {
                return 5204;
            }

            return 6683;
        }

        if (SampleCount == 160) {
            return 3427;
        }

        return 4752;
    }

    uint32_t Estimate(const MultiTapBiquadFilterAndMixCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (command.HasVolumeRamp) {
            if (SampleCount == 160) {
                return 7939;
            }

            return 10669;
        }

        if (SampleCount == 160) {
            return 6256;
        }

        return 8683;
    }

    uint32_t Estimate(const FillBufferCommand& command) override { return 0; }
};

}  // namespace nxrt::audren
