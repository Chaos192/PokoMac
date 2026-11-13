// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/ICommand.cs and CommandType.cs (Ryubing fork, MIT).
#pragma once
#include <cstdint>

namespace nxrt::audren {

class CommandList;

enum class CommandType : uint8_t {
    Invalid,
    PcmInt16DataSourceVersion1,
    PcmInt16DataSourceVersion2,
    PcmFloatDataSourceVersion1,
    PcmFloatDataSourceVersion2,
    AdpcmDataSourceVersion1,
    AdpcmDataSourceVersion2,
    Volume,
    VolumeRamp,
    BiquadFilter,
    BiquadFilterFloatCoeff,
    Mix,
    MixRamp,
    MixRampGrouped,
    DepopPrepare,
    DepopForMixBuffers,
    Delay,
    Upsample,
    DownMixSurroundToStereo,
    AuxiliaryBuffer,
    DeviceSink,
    CircularBufferSink,
    Reverb,
    Reverb3d,
    Performance,
    ClearMixBuffer,
    CopyMixBuffer,
    LimiterVersion1,
    LimiterVersion2,
    MultiTapBiquadFilter,
    MultiTapBiquadFilterFloatCoeff,
    CaptureBuffer,
    Compressor,
    BiquadFilterAndMix,
    BiquadFilterAndMixFloatCoeff,
    MultiTapBiquadFilterAndMix,
    MultiTapBiquadFilterAndMixFloatCoef,
    AuxiliaryBufferGrouped,
    FillMixBuffer,
    BiquadFilterCrossFade,
    MultiTapBiquadFilterCrossFade,
    FillBuffer,
};

// The C# interface: a command the DSP runs once per render frame. `Enabled` is a field; NodeId, CommandType and EstimatedProcessingTime
// are properties of the C# interface and become const methods here (a command class overrides them; commands that keep them in fields
// named NodeId / CommandType_ / EstimatedProcessingTime_ implement the overrides by returning those fields).
class ICommand {
  public:
    virtual ~ICommand() = default;
    bool Enabled = false;
    virtual int32_t GetNodeId() const = 0;
    virtual CommandType GetCommandType() const = 0;
    virtual uint32_t GetEstimatedProcessingTime() const = 0;
    virtual void Process(CommandList& context) = 0;
    virtual bool ShouldMeter() { return false; }
};

}  // namespace nxrt::audren
