// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/ICommandProcessingTimeEstimator.cs (Ryubing fork, MIT). Estimates the DSP time a command takes;
// the renderer uses it for voice dropping. The estimator versions (CommandProcessingTimeEstimatorVersion1..5.h) implement this interface.
#pragma once
#include <cstdint>
#include <limits>
#include <utility>

#include "../Dsp/Command/AdpcmDataSourceCommandVersion1.h"
#include "../Dsp/Command/AuxiliaryBufferCommand.h"
#include "../Dsp/Command/BiquadFilterAndMixCommand.h"
#include "../Dsp/Command/BiquadFilterCommand.h"
#include "../Dsp/Command/CaptureBufferCommand.h"
#include "../Dsp/Command/CircularBufferSinkCommand.h"
#include "../Dsp/Command/ClearMixBufferCommand.h"
#include "../Dsp/Command/CompressorCommand.h"
#include "../Dsp/Command/CopyMixBufferCommand.h"
#include "../Dsp/Command/DataSourceVersion2Command.h"
#include "../Dsp/Command/DelayCommand.h"
#include "../Dsp/Command/DepopForMixBuffersCommand.h"
#include "../Dsp/Command/DepopPrepareCommand.h"
#include "../Dsp/Command/DeviceSinkCommand.h"
#include "../Dsp/Command/DownMixSurroundToStereoCommand.h"
#include "../Dsp/Command/FillBufferCommand.h"
#include "../Dsp/Command/LimiterCommandVersion1.h"
#include "../Dsp/Command/LimiterCommandVersion2.h"
#include "../Dsp/Command/MixCommand.h"
#include "../Dsp/Command/MixRampCommand.h"
#include "../Dsp/Command/MixRampGroupedCommand.h"
#include "../Dsp/Command/MultiTapBiquadFilterAndMixCommand.h"
#include "../Dsp/Command/MultiTapBiquadFilterCommand.h"
#include "../Dsp/Command/PcmFloatDataSourceCommandVersion1.h"
#include "../Dsp/Command/PcmInt16DataSourceCommandVersion1.h"
#include "../Dsp/Command/PerformanceCommand.h"
#include "../Dsp/Command/Reverb3dCommand.h"
#include "../Dsp/Command/ReverbCommand.h"
#include "../Dsp/Command/UpsampleCommand.h"
#include "../Dsp/Command/VolumeCommand.h"
#include "../Dsp/Command/VolumeRampCommand.h"
#include "../audren_common.h"
#include "../audren_types.h"

namespace nxrt::audren {

namespace estimator_detail {
// C# `(uint)x` for a float x on .NET 10 (x64): truncation toward zero; NaN and negative values give 0; values at or above 2^32 saturate.
inline uint32_t FloatToUInt32(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 4294967296.0f) return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(v);
}

// Debug.Assert(_sampleCount is 160 or 240)
inline bool IsSampleCountSupported(uint32_t sampleCount) { return sampleCount == 160 || sampleCount == 240; }

// The static IsChannelCountValid of the C# parameter structs (private in the C++ command classes, so it is repeated here).
inline bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }

// C# `switch (channelCount) { 1 => a, 2 => b, 4 => c, 6 => d, _ => throw new NotImplementedException(...) }` with float results.
// Integer arms of the C# (`1 => 8929`) are passed as the same value in float; they are exact.
inline uint32_t SelectByChannelCount(uint16_t channelCount, float c1, float c2, float c4, float c6) {
    switch (channelCount) {
        case 1: return FloatToUInt32(c1);
        case 2: return FloatToUInt32(c2);
        case 4: return FloatToUInt32(c4);
        case 6: return FloatToUInt32(c6);
        default: NXAUDREN_FATAL("CommandProcessingTimeEstimator: NotImplementedException (unsupported channel count)");
    }
}
}  // namespace estimator_detail

// The C# interface. Every overload takes the command by const reference: the estimators only read it.
class ICommandProcessingTimeEstimator {
  public:
    virtual ~ICommandProcessingTimeEstimator() = default;

    virtual uint32_t Estimate(const AuxiliaryBufferCommand& command) = 0;
    virtual uint32_t Estimate(const BiquadFilterCommand& command) = 0;
    virtual uint32_t Estimate(const ClearMixBufferCommand& command) = 0;
    virtual uint32_t Estimate(const DelayCommand& command) = 0;
    virtual uint32_t Estimate(const Reverb3dCommand& command) = 0;
    virtual uint32_t Estimate(const ReverbCommand& command) = 0;
    virtual uint32_t Estimate(const DepopPrepareCommand& command) = 0;
    virtual uint32_t Estimate(const DepopForMixBuffersCommand& command) = 0;
    virtual uint32_t Estimate(const MixCommand& command) = 0;
    virtual uint32_t Estimate(const MixRampCommand& command) = 0;
    virtual uint32_t Estimate(const MixRampGroupedCommand& command) = 0;
    virtual uint32_t Estimate(const CopyMixBufferCommand& command) = 0;
    virtual uint32_t Estimate(const PerformanceCommand& command) = 0;
    virtual uint32_t Estimate(const VolumeCommand& command) = 0;
    virtual uint32_t Estimate(const VolumeRampCommand& command) = 0;
    virtual uint32_t Estimate(const PcmInt16DataSourceCommandVersion1& command) = 0;
    virtual uint32_t Estimate(const PcmFloatDataSourceCommandVersion1& command) = 0;
    virtual uint32_t Estimate(const AdpcmDataSourceCommandVersion1& command) = 0;
    virtual uint32_t Estimate(const DataSourceVersion2Command& command) = 0;
    virtual uint32_t Estimate(const CircularBufferSinkCommand& command) = 0;
    virtual uint32_t Estimate(const DeviceSinkCommand& command) = 0;
    virtual uint32_t Estimate(const DownMixSurroundToStereoCommand& command) = 0;
    virtual uint32_t Estimate(const UpsampleCommand& command) = 0;
    virtual uint32_t Estimate(const LimiterCommandVersion1& command) = 0;
    virtual uint32_t Estimate(const LimiterCommandVersion2& command) = 0;
    virtual uint32_t Estimate(const MultiTapBiquadFilterCommand& command) = 0;
    virtual uint32_t Estimate(const CaptureBufferCommand& command) = 0;
    virtual uint32_t Estimate(const CompressorCommand& command) = 0;
    virtual uint32_t Estimate(const BiquadFilterAndMixCommand& command) = 0;
    virtual uint32_t Estimate(const MultiTapBiquadFilterAndMixCommand& command) = 0;
    virtual uint32_t Estimate(const FillBufferCommand& command) = 0;
};

}  // namespace nxrt::audren
