// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/CommandProcessingTimeEstimatorVersion2.cs (Ryubing fork, MIT). Version 2 (added with REV5).
#pragma once
#include "ICommandProcessingTimeEstimator.h"

namespace nxrt::audren {

class CommandProcessingTimeEstimatorVersion2 : public ICommandProcessingTimeEstimator {
  private:
    uint32_t _sampleCount;
    uint32_t _bufferCount;

  public:
    CommandProcessingTimeEstimatorVersion2(uint32_t sampleCount, uint32_t bufferCount) : _sampleCount(sampleCount), _bufferCount(bufferCount) {}

    uint32_t Estimate(const PerformanceCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(489.35f);
        }

        return estimator_detail::FloatToUInt32(491.18f);
    }

    uint32_t Estimate(const ClearMixBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        float costPerBuffer = 668.8f;
        float baseCost = 193.2f;

        if (_sampleCount == 160) {
            costPerBuffer = 260.4f;
            baseCost = 139.65f;
        }

        return estimator_detail::FloatToUInt32(baseCost + costPerBuffer * _bufferCount);
    }

    uint32_t Estimate(const BiquadFilterCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(4813.2f);
        }

        return estimator_detail::FloatToUInt32(6915.4f);
    }

    uint32_t Estimate(const MixRampGroupedCommand& command) override {
        const float CostPerSample = 7.245f;

        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        int volumeCount = 0;

        for (uint32_t i = 0; i < command.MixBufferCount; i++) {
            if (command.Volume0[i] != 0.0f || command.Volume1[i] != 0.0f) {
                volumeCount++;
            }
        }

        return estimator_detail::FloatToUInt32(_sampleCount * CostPerSample * volumeCount);
    }

    uint32_t Estimate(const MixRampCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(1859.0f);
        }

        return estimator_detail::FloatToUInt32(2286.1f);
    }

    uint32_t Estimate(const DepopPrepareCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(306.62f);
        }

        return estimator_detail::FloatToUInt32(293.22f);
    }

    uint32_t Estimate(const VolumeRampCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(1403.9f);
        }

        return estimator_detail::FloatToUInt32(1884.3f);
    }

    uint32_t Estimate(const PcmInt16DataSourceCommandVersion1& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        float costPerSample = 1195.5f;
        float baseCost = 7797.0f;

        if (_sampleCount == 160) {
            costPerSample = 749.27f;
            baseCost = 6138.9f;
        }

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / _sampleCount) * (command.Pitch * 0.000030518f))));
    }

    uint32_t Estimate(const AdpcmDataSourceCommandVersion1& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        float costPerSample = 3564.1f;
        float baseCost = 6225.5f;

        if (_sampleCount == 160) {
            costPerSample = 2125.6f;
            baseCost = 9039.5f;
        }

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / _sampleCount) * (command.Pitch * 0.000030518f))));
    }

    uint32_t Estimate(const DepopForMixBuffersCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(762.96f);
        }

        return estimator_detail::FloatToUInt32(726.96f);
    }

    uint32_t Estimate(const CopyMixBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(836.32f);
        }

        return estimator_detail::FloatToUInt32(1000.9f);
    }

    uint32_t Estimate(const MixCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(1342.2f);
        }

        return estimator_detail::FloatToUInt32(1833.2f);
    }

    uint32_t Estimate(const DelayCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (_sampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 41636.0f, 97861.0f, 192520.0f, 301760.0f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 578.53f, 663.06f, 703.98f, 760.03f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 8770.3f, 25741.0f, 47551.0f, 81629.0f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 521.28f, 585.4f, 629.88f, 713.57f);
    }

    uint32_t Estimate(const ReverbCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (_sampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 97192.0f, 103280.0f, 109580.0f, 115070.0f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 492.01f, 554.46f, 595.86f, 656.62f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 136460.0f, 145750.0f, 154800.0f, 161970.0f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 495.79f, 527.16f, 598.75f, 666.03f);
    }

    uint32_t Estimate(const Reverb3dCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (_sampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 138840.0f, 135430.0f, 199180.0f, 247350.0f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 718.7f, 751.3f, 797.46f, 867.43f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 199950.0f, 195200.0f, 290580.0f, 363490.0f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 534.24f, 570.87f, 660.93f, 694.6f);
    }

    uint32_t Estimate(const AuxiliaryBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::FloatToUInt32(7177.9f);
            }

            return estimator_detail::FloatToUInt32(489.16f);
        }

        if (command.Enabled) {
            return estimator_detail::FloatToUInt32(9499.8f);
        }

        return estimator_detail::FloatToUInt32(485.56f);
    }

    uint32_t Estimate(const VolumeCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(1280.3f);
        }

        return estimator_detail::FloatToUInt32(1737.8f);
    }

    uint32_t Estimate(const CircularBufferSinkCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        float costPerBuffer = 1726.0f;
        float baseCost = 1369.7f;

        if (_sampleCount == 160) {
            costPerBuffer = 853.63f;
            baseCost = 1284.5f;
        }

        return estimator_detail::FloatToUInt32(baseCost + costPerBuffer * command.InputCount);
    }

    uint32_t Estimate(const DownMixSurroundToStereoCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(10009.0f);
        }

        return estimator_detail::FloatToUInt32(14577.0f);
    }

    uint32_t Estimate(const UpsampleCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(292000.0f);
        }

        return estimator_detail::FloatToUInt32(0.0f);
    }

    uint32_t Estimate(const DeviceSinkCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));
        NXAUDREN_ASSERT(command.InputCount == 2 || command.InputCount == 6);

        if (command.InputCount == 2) {
            if (_sampleCount == 160) {
                return estimator_detail::FloatToUInt32(9261.5f);
            }

            return estimator_detail::FloatToUInt32(9336.1f);
        }

        if (_sampleCount == 160) {
            return estimator_detail::FloatToUInt32(9111.8f);
        }

        return estimator_detail::FloatToUInt32(9566.7f);
    }

    uint32_t Estimate(const PcmFloatDataSourceCommandVersion1& command) override {
        // NOTE: This was added between REV7 and REV8 and for some reasons the estimator v2 was changed...
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(_sampleCount));

        float costPerSample = 3490.9f;
        float baseCost = 10091.0f;

        if (_sampleCount == 160) {
            costPerSample = 2310.4f;
            baseCost = 7845.3f;
        }

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / _sampleCount) * (command.Pitch * 0.000030518f))));
    }

    uint32_t Estimate(const DataSourceVersion2Command& command) override { return 0; }

    uint32_t Estimate(const LimiterCommandVersion1& command) override { return 0; }

    uint32_t Estimate(const LimiterCommandVersion2& command) override { return 0; }

    uint32_t Estimate(const MultiTapBiquadFilterCommand& command) override { return 0; }

    uint32_t Estimate(const CaptureBufferCommand& command) override { return 0; }

    uint32_t Estimate(const CompressorCommand& command) override { return 0; }

    uint32_t Estimate(const BiquadFilterAndMixCommand& command) override { return 0; }

    uint32_t Estimate(const MultiTapBiquadFilterAndMixCommand& command) override { return 0; }

    uint32_t Estimate(const FillBufferCommand& command) override { return 0; }
};

}  // namespace nxrt::audren
