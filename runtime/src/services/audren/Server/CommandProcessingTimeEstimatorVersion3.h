// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/CommandProcessingTimeEstimatorVersion3.cs (Ryubing fork, MIT). Version 3 (added with REV8).
// Version 4 and 5 derive from this class and override some of its Estimate overloads.
#pragma once
#include "ICommandProcessingTimeEstimator.h"

namespace nxrt::audren {

class CommandProcessingTimeEstimatorVersion3 : public ICommandProcessingTimeEstimator {
  protected:
    uint32_t SampleCount;
    uint32_t BufferCount;

  public:
    CommandProcessingTimeEstimatorVersion3(uint32_t sampleCount, uint32_t bufferCount) : SampleCount(sampleCount), BufferCount(bufferCount) {}

    uint32_t Estimate(const PerformanceCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(498.17f);
        }

        return estimator_detail::FloatToUInt32(489.42f);
    }

    uint32_t Estimate(const ClearMixBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        float costPerBuffer = 440.68f;
        float baseCost = 0;

        if (SampleCount == 160) {
            costPerBuffer = 266.65f;
        }

        return estimator_detail::FloatToUInt32(baseCost + costPerBuffer * BufferCount);
    }

    uint32_t Estimate(const BiquadFilterCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(4173.2f);
        }

        return estimator_detail::FloatToUInt32(5585.1f);
    }

    uint32_t Estimate(const MixRampGroupedCommand& command) override {
        float costPerSample = 6.4434f;

        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            costPerSample = 6.708f;
        }

        int volumeCount = 0;

        for (uint32_t i = 0; i < command.MixBufferCount; i++) {
            if (command.Volume0[i] != 0.0f || command.Volume1[i] != 0.0f) {
                volumeCount++;
            }
        }

        return estimator_detail::FloatToUInt32(SampleCount * costPerSample * volumeCount);
    }

    uint32_t Estimate(const MixRampCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(1968.7f);
        }

        return estimator_detail::FloatToUInt32(2459.4f);
    }

    uint32_t Estimate(const DepopPrepareCommand& command) override { return 0; }

    uint32_t Estimate(const VolumeRampCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(1425.3f);
        }

        return estimator_detail::FloatToUInt32(1700.0f);
    }

    uint32_t Estimate(const PcmInt16DataSourceCommandVersion1& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        float costPerSample = 710.143f;
        float baseCost = 7853.286f;

        if (SampleCount == 160) {
            costPerSample = 427.52f;
            baseCost = 6329.442f;
        }

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / SampleCount) * (command.Pitch * 0.000030518f))));
    }

    uint32_t Estimate(const AdpcmDataSourceCommandVersion1& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        float costPerSample = 3564.1f;
        float baseCost = 9736.702f;

        if (SampleCount == 160) {
            costPerSample = 2125.6f;
            baseCost = 7913.808f;
        }

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / SampleCount) * (command.Pitch * 0.000030518f))));
    }

    uint32_t Estimate(const DepopForMixBuffersCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(739.64f);
        }

        return estimator_detail::FloatToUInt32(910.97f);
    }

    uint32_t Estimate(const CopyMixBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(842.59f);
        }

        return estimator_detail::FloatToUInt32(986.72f);
    }

    uint32_t Estimate(const MixCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(1402.8f);
        }

        return estimator_detail::FloatToUInt32(1853.2f);
    }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const DelayCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 8929.04f, 25500.75f, 47759.62f, 82203.07f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 1295.20f, 1213.60f, 942.03f, 1001.55f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 11941.05f, 37197.37f, 69749.84f, 120042.40f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 997.67f, 977.63f, 792.30f, 875.43f);
    }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const ReverbCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 81475.05f, 84975.0f, 91625.15f, 95332.27f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 536.30f, 588.70f, 643.70f, 706.0f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 120174.47f, 25262.22f, 135751.23f, 141129.23f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 617.64f, 659.54f, 711.43f, 778.07f);
    }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const Reverb3dCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const uint16_t channelCount = command.Parameter().channel_count;

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::SelectByChannelCount(channelCount, 116754.0f, 125912.05f, 146336.03f, 165812.66f);
            }

            return estimator_detail::SelectByChannelCount(channelCount, 734.0f, 766.62f, 797.46f, 867.43f);
        }

        if (command.Enabled) {
            return estimator_detail::SelectByChannelCount(channelCount, 170292.34f, 183875.63f, 214696.19f, 243846.77f);
        }

        return estimator_detail::SelectByChannelCount(channelCount, 508.47f, 582.45f, 626.42f, 682.47f);
    }

    uint32_t Estimate(const AuxiliaryBufferCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            if (command.Enabled) {
                return estimator_detail::FloatToUInt32(7182.14f);
            }

            return estimator_detail::FloatToUInt32(472.11f);
        }

        if (command.Enabled) {
            return estimator_detail::FloatToUInt32(9435.96f);
        }

        return estimator_detail::FloatToUInt32(462.62f);
    }

    uint32_t Estimate(const VolumeCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(1311.1f);
        }

        return estimator_detail::FloatToUInt32(1713.6f);
    }

    uint32_t Estimate(const CircularBufferSinkCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        float costPerBuffer = 770.26f;
        float baseCost = 0.0f;

        if (SampleCount == 160) {
            costPerBuffer = 531.07f;
        }

        return estimator_detail::FloatToUInt32(baseCost + costPerBuffer * command.InputCount);
    }

    uint32_t Estimate(const DownMixSurroundToStereoCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(9949.7f);
        }

        return estimator_detail::FloatToUInt32(14679.0f);
    }

    uint32_t Estimate(const UpsampleCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(312990.0f);
        }

        return estimator_detail::FloatToUInt32(0.0f);
    }

    uint32_t Estimate(const DeviceSinkCommand& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        NXAUDREN_ASSERT(command.InputCount == 2 || command.InputCount == 6);

        if (command.InputCount == 2) {
            if (SampleCount == 160) {
                return estimator_detail::FloatToUInt32(8980.0f);
            }

            return estimator_detail::FloatToUInt32(9221.9f);
        }

        if (SampleCount == 160) {
            return estimator_detail::FloatToUInt32(9177.9f);
        }

        return estimator_detail::FloatToUInt32(9725.9f);
    }

    uint32_t Estimate(const PcmFloatDataSourceCommandVersion1& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        float costPerSample = 3490.9f;
        float baseCost = 10090.9f;

        if (SampleCount == 160) {
            costPerSample = 2310.4f;
            baseCost = 7845.25f;
        }

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / SampleCount) * (command.Pitch * 0.000030518f))));
    }

    uint32_t Estimate(const DataSourceVersion2Command& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        const auto [baseCost, costPerSample] = GetCostByFormat(SampleCount, command.SampleFormat_, command.SrcQuality);

        return estimator_detail::FloatToUInt32(
            baseCost + (costPerSample * (((static_cast<float>(command.SampleRate) / 200.0f) / SampleCount) * (command.Pitch * 0.000030518f) - 1.0f)));
    }

  private:
    static std::pair<float, float> GetCostByFormat(uint32_t sampleCount, SampleFormat format, SampleRateConversionQuality quality) {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(sampleCount));

        switch (format) {
            case SampleFormat::PcmInt16:
                switch (quality) {
                    case SampleRateConversionQuality::Default:
                        if (sampleCount == 160) {
                            return {6329.44f, 427.52f};
                        }

                        return {7853.28f, 710.14f};
                    case SampleRateConversionQuality::High:
                        if (sampleCount == 160) {
                            return {8049.42f, 371.88f};
                        }

                        return {10138.84f, 610.49f};
                    case SampleRateConversionQuality::Low:
                        if (sampleCount == 160) {
                            return {5062.66f, 423.43f};
                        }

                        return {5810.96f, 676.72f};
                    default: NXAUDREN_FATAL("DataSourceVersion2: NotImplementedException (sample rate conversion quality)");
                }
            case SampleFormat::PcmFloat:
                switch (quality) {
                    case SampleRateConversionQuality::Default:
                        if (sampleCount == 160) {
                            return {7845.25f, 2310.4f};
                        }

                        return {10090.9f, 3490.9f};
                    case SampleRateConversionQuality::High:
                        if (sampleCount == 160) {
                            return {9446.36f, 2308.91f};
                        }

                        return {12520.85f, 3480.61f};
                    case SampleRateConversionQuality::Low:
                        if (sampleCount == 160) {
                            return {9446.36f, 2308.91f};
                        }

                        return {12520.85f, 3480.61f};
                    default: NXAUDREN_FATAL("DataSourceVersion2: NotImplementedException (sample rate conversion quality)");
                }
            case SampleFormat::Adpcm:
                switch (quality) {
                    case SampleRateConversionQuality::Default:
                        if (sampleCount == 160) {
                            return {7913.81f, 1827.66f};
                        }

                        return {9736.70f, 2756.37f};
                    case SampleRateConversionQuality::High:
                        if (sampleCount == 160) {
                            return {9607.81f, 1829.29f};
                        }

                        return {12154.38f, 2731.31f};
                    case SampleRateConversionQuality::Low:
                        if (sampleCount == 160) {
                            return {6517.48f, 1824.61f};
                        }

                        return {7929.44f, 2732.15f};
                    default: NXAUDREN_FATAL("DataSourceVersion2: NotImplementedException (sample rate conversion quality)");
                }
            default: NXAUDREN_FATAL("DataSourceVersion2: NotImplementedException (sample format)");
        }
    }

    uint32_t EstimateLimiterCommandCommon(LimiterParameter parameter, bool enabled) {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        if (SampleCount == 160) {
            if (enabled) {
                return estimator_detail::SelectByChannelCount(parameter.channel_count, 21392.0f, 26829.0f, 32405.0f, 52219.0f);
            }

            return estimator_detail::SelectByChannelCount(parameter.channel_count, 897.0f, 931.55f, 975.39f, 1016.8f);
        }

        if (enabled) {
            return estimator_detail::SelectByChannelCount(parameter.channel_count, 30556.0f, 39011.0f, 48270.0f, 76712.0f);
        }

        return estimator_detail::SelectByChannelCount(parameter.channel_count, 874.43f, 921.55f, 945.26f, 992.26f);
    }

  public:
    uint32_t Estimate(const LimiterCommandVersion1& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));

        return EstimateLimiterCommandCommon(command.Parameter(), command.IsEffectEnabled);
    }

    uint32_t Estimate(const LimiterCommandVersion2& command) override {
        NXAUDREN_ASSERT(estimator_detail::IsSampleCountSupported(SampleCount));
        const LimiterParameter parameter = command.Parameter();

        if (!parameter.statistics_enabled || !command.IsEffectEnabled) {
            return EstimateLimiterCommandCommon(parameter, command.IsEffectEnabled);
        }

        if (SampleCount == 160) {
            return estimator_detail::SelectByChannelCount(parameter.channel_count, 23309.0f, 29954.0f, 35807.0f, 58340.0f);
        }

        return estimator_detail::SelectByChannelCount(parameter.channel_count, 33526.0f, 43549.0f, 52190.0f, 85527.0f);
    }

    // C# `public virtual` (version 4 overrides it).
    uint32_t Estimate(const MultiTapBiquadFilterCommand& command) override { return 0; }

    // C# `public virtual` (version 4 overrides it).
    uint32_t Estimate(const CaptureBufferCommand& command) override { return 0; }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const CompressorCommand& command) override { return 0; }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const BiquadFilterAndMixCommand& command) override { return 0; }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const MultiTapBiquadFilterAndMixCommand& command) override { return 0; }

    // C# `public virtual` (version 5 overrides it).
    uint32_t Estimate(const FillBufferCommand& command) override { return 0; }
};

}  // namespace nxrt::audren
