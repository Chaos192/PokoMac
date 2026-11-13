// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/CommandBuffer.cs (Ryubing fork, MIT). An API to generate commands and add them to a CommandList.
//
// C# -> C++ deviations (all others follow audren_common.h):
//   * The object pools (`ObjectPool<T>`, `ReleaseCommand`) are dropped. Each Generate method makes a std::unique_ptr<Xxx>, calls its
//     Initialize, sets EstimatedProcessingTime from the estimator, adds it to the list and gives ownership to the CommandList.
//   * The C# `CommandList` property is the public reference member `CommandList` (qualified as ::nxrt::audren::CommandList so that the
//     member name does not change the meaning of the type name inside the class).
//   * `ref VoiceInfo` / `ref BiquadFilterParameter2` / `ref AuxiliaryBufferAddresses` become references; `Memory<T>` becomes Span<T>.
//   * C# `parameter.IsChannelCountValid()` (EffectInParameterVersion1.IsChannelCountValid) is repeated below as a free function, since the
//     parameter structs of audren_types.h have no such member.
#pragma once
#include <memory>
#include <utility>

#include "../Dsp/Command/CommandList.h"
#include "../Dsp/Command/PerformanceCommand.h"
#include "Sink/CircularBufferSink.h"
#include "Sink/DeviceSink.h"
#include "Splitter/SplitterDestination.h"
#include "Upsampler/UpsamplerInfo.h"
#include "Voice/VoiceInfo.h"
#include "../audren_common.h"
#include "../audren_types.h"
#include "ICommandProcessingTimeEstimator.h"

namespace nxrt::audren {

namespace command_buffer_detail {
// C# EffectInParameterVersion1.IsChannelCountValid(int channelCount): `channelCount is 1 or 2 or 4 or 6`.
inline bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }
}  // namespace command_buffer_detail

class CommandBuffer {
  public:
    // The estimated total processing time.
    uint32_t EstimatedProcessingTime = 0;

    // The command list that is populated by the CommandBuffer.
    ::nxrt::audren::CommandList& CommandList;

    // Create a new CommandBuffer. The C# constructor takes the command list and the estimator; both are non-null, so both are references.
    CommandBuffer(::nxrt::audren::CommandList& commandList, ICommandProcessingTimeEstimator& commandProcessingTimeEstimator)
        : CommandList(commandList), _commandProcessingTimeEstimator(commandProcessingTimeEstimator) {}

    // Generate a new ClearMixBufferCommand.
    void GenerateClearMixBuffer(int32_t nodeId) {
        auto command = std::make_unique<ClearMixBufferCommand>();
        command->Initialize(nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new DepopPrepareCommand.
    void GenerateDepopPrepare(Span<VoiceState> state, Span<float> depopBuffer, uint32_t bufferCount, uint32_t bufferOffset, int32_t nodeId, bool wasPlaying) {
        auto command = std::make_unique<DepopPrepareCommand>();
        command->Initialize(state, depopBuffer, bufferCount, bufferOffset, nodeId, wasPlaying);
        AddCommand(std::move(command));
    }

    // Generate a new PerformanceCommand.
    void GeneratePerformance(PerformanceEntryAddresses& performanceEntryAddresses, PerformanceCommand::Type type, int32_t nodeId) {
        auto command = std::make_unique<PerformanceCommand>();
        command->Initialize(performanceEntryAddresses, type, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new VolumeRampCommand.
    void GenerateVolumeRamp(float previousVolume, float volume, uint32_t bufferIndex, int32_t nodeId) {
        auto command = std::make_unique<VolumeRampCommand>();
        command->Initialize(previousVolume, volume, bufferIndex, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new DataSourceVersion2Command.
    void GenerateDataSourceVersion2(VoiceInfo& voiceInfo, Span<VoiceState> state, uint16_t outputBufferIndex, uint16_t channelIndex, int32_t nodeId) {
        auto command = std::make_unique<DataSourceVersion2Command>();
        command->Initialize(voiceInfo, state, outputBufferIndex, channelIndex, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new PcmInt16DataSourceCommandVersion1.
    void GeneratePcmInt16DataSourceVersion1(VoiceInfo& voiceInfo, Span<VoiceState> state, uint16_t outputBufferIndex, uint16_t channelIndex, int32_t nodeId) {
        auto command = std::make_unique<PcmInt16DataSourceCommandVersion1>();
        command->Initialize(voiceInfo, state, outputBufferIndex, channelIndex, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new PcmFloatDataSourceCommandVersion1.
    void GeneratePcmFloatDataSourceVersion1(VoiceInfo& voiceInfo, Span<VoiceState> state, uint16_t outputBufferIndex, uint16_t channelIndex, int32_t nodeId) {
        auto command = std::make_unique<PcmFloatDataSourceCommandVersion1>();
        command->Initialize(voiceInfo, state, outputBufferIndex, channelIndex, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new AdpcmDataSourceCommandVersion1.
    void GenerateAdpcmDataSourceVersion1(VoiceInfo& voiceInfo, Span<VoiceState> state, uint16_t outputBufferIndex, int32_t nodeId) {
        auto command = std::make_unique<AdpcmDataSourceCommandVersion1>();
        command->Initialize(voiceInfo, state, outputBufferIndex, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new BiquadFilterCommand.
    void GenerateBiquadFilter(int32_t baseIndex, BiquadFilterParameter2& filter, Span<BiquadFilterState> biquadFilterStateMemory, int32_t inputBufferOffset,
                              int32_t outputBufferOffset, bool needInitialization, int32_t nodeId) {
        auto command = std::make_unique<BiquadFilterCommand>();
        command->Initialize(baseIndex, filter, biquadFilterStateMemory, inputBufferOffset, outputBufferOffset, needInitialization, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new MultiTapBiquadFilterCommand.
    void GenerateMultiTapBiquadFilter(int32_t baseIndex, Span<const BiquadFilterParameter2> filters, Span<BiquadFilterState> biquadFilterStatesMemory,
                                      int32_t inputBufferOffset, int32_t outputBufferOffset, Span<const bool> isInitialized, int32_t nodeId) {
        auto command = std::make_unique<MultiTapBiquadFilterCommand>();
        command->Initialize(baseIndex, filters, biquadFilterStatesMemory, inputBufferOffset, outputBufferOffset, isInitialized, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new MixRampGroupedCommand.
    void GenerateMixRampGrouped(uint32_t mixBufferCount, uint32_t inputBufferIndex, uint32_t outputBufferIndex, Span<const float> previousVolume,
                                Span<const float> volume, Span<VoiceState> state, int32_t nodeId) {
        auto command = std::make_unique<MixRampGroupedCommand>();
        command->Initialize(mixBufferCount, inputBufferIndex, outputBufferIndex, previousVolume, volume, state, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new MixRampCommand.
    void GenerateMixRamp(float previousVolume, float volume, uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t lastSampleIndex,
                         Span<VoiceState> state, int32_t nodeId) {
        auto command = std::make_unique<MixRampCommand>();
        command->Initialize(previousVolume, volume, inputBufferIndex, outputBufferIndex, lastSampleIndex, state, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new BiquadFilterAndMixCommand.
    void GenerateBiquadFilterAndMix(float previousVolume, float volume, uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t lastSampleIndex,
                                    Span<VoiceState> state, BiquadFilterParameter2& filter, Span<BiquadFilterState> biquadFilterState,
                                    Span<BiquadFilterState> previousBiquadFilterState, bool needInitialization, bool hasVolumeRamp, bool isFirstMixBuffer,
                                    int32_t nodeId) {
        auto command = std::make_unique<BiquadFilterAndMixCommand>();
        command->Initialize(previousVolume, volume, inputBufferIndex, outputBufferIndex, lastSampleIndex, state, filter, biquadFilterState,
                            previousBiquadFilterState, needInitialization, hasVolumeRamp, isFirstMixBuffer, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new MultiTapBiquadFilterAndMixCommand.
    void GenerateMultiTapBiquadFilterAndMix(float previousVolume, float volume, uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t lastSampleIndex,
                                            Span<VoiceState> state, BiquadFilterParameter2& filter0, BiquadFilterParameter2& filter1,
                                            Span<BiquadFilterState> biquadFilterState0, Span<BiquadFilterState> biquadFilterState1,
                                            Span<BiquadFilterState> previousBiquadFilterState0, Span<BiquadFilterState> previousBiquadFilterState1,
                                            bool needInitialization0, bool needInitialization1, bool hasVolumeRamp, bool isFirstMixBuffer, int32_t nodeId) {
        auto command = std::make_unique<MultiTapBiquadFilterAndMixCommand>();
        command->Initialize(previousVolume, volume, inputBufferIndex, outputBufferIndex, lastSampleIndex, state, filter0, filter1, biquadFilterState0,
                            biquadFilterState1, previousBiquadFilterState0, previousBiquadFilterState1, needInitialization0, needInitialization1,
                            hasVolumeRamp, isFirstMixBuffer, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new DepopForMixBuffersCommand.
    void GenerateDepopForMixBuffers(Span<float> depopBuffer, uint32_t bufferOffset, uint32_t bufferCount, int32_t nodeId, uint32_t sampleRate) {
        auto command = std::make_unique<DepopForMixBuffersCommand>();
        command->Initialize(depopBuffer, bufferOffset, bufferCount, nodeId, sampleRate);
        AddCommand(std::move(command));
    }

    // Generate a new CopyMixBufferCommand.
    void GenerateCopyMixBuffer(uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t nodeId) {
        auto command = std::make_unique<CopyMixBufferCommand>();
        command->Initialize(inputBufferIndex, outputBufferIndex, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new MixCommand.
    void GenerateMix(uint32_t inputBufferIndex, uint32_t outputBufferIndex, int32_t nodeId, float volume) {
        auto command = std::make_unique<MixCommand>();
        command->Initialize(inputBufferIndex, outputBufferIndex, nodeId, volume);
        AddCommand(std::move(command));
    }

    // Generate a new ReverbCommand (only when the parameter's channel count is valid).
    void GenerateReverbEffect(uint32_t bufferOffset, ReverbParameter parameter, Span<ReverbState> state, bool isEnabled, CpuAddress workBuffer, int32_t nodeId,
                              bool isLongSizePreDelaySupported, bool newEffectChannelMappingSupported) {
        if (command_buffer_detail::IsChannelCountValid(parameter.channel_count)) {
            auto command = std::make_unique<ReverbCommand>();
            command->Initialize(bufferOffset, parameter, state, isEnabled, workBuffer, nodeId, isLongSizePreDelaySupported, newEffectChannelMappingSupported);
            AddCommand(std::move(command));
        }
    }

    // Generate a new Reverb3dCommand (only when the parameter's channel count is valid).
    void GenerateReverb3dEffect(uint32_t bufferOffset, Reverb3dParameter parameter, Span<Reverb3dState> state, bool isEnabled, CpuAddress workBuffer,
                                int32_t nodeId, bool newEffectChannelMappingSupported) {
        if (command_buffer_detail::IsChannelCountValid(parameter.channel_count)) {
            auto command = std::make_unique<Reverb3dCommand>();
            command->Initialize(bufferOffset, parameter, state, isEnabled, workBuffer, nodeId, newEffectChannelMappingSupported);
            AddCommand(std::move(command));
        }
    }

    // Generate a new DelayCommand (only when the parameter's channel count is valid).
    void GenerateDelayEffect(uint32_t bufferOffset, DelayParameter parameter, Span<DelayState> state, bool isEnabled, CpuAddress workBuffer, int32_t nodeId,
                             bool newEffectChannelMappingSupported) {
        if (command_buffer_detail::IsChannelCountValid(parameter.channel_count)) {
            auto command = std::make_unique<DelayCommand>();
            command->Initialize(bufferOffset, parameter, state, isEnabled, workBuffer, nodeId, newEffectChannelMappingSupported);
            AddCommand(std::move(command));
        }
    }

    // Generate a new LimiterCommandVersion1 (only when the parameter's channel count is valid).
    void GenerateLimiterEffectVersion1(uint32_t bufferOffset, LimiterParameter parameter, Span<LimiterState> state, bool isEnabled, uint64_t workBuffer,
                                       int32_t nodeId) {
        if (command_buffer_detail::IsChannelCountValid(parameter.channel_count)) {
            auto command = std::make_unique<LimiterCommandVersion1>();
            command->Initialize(bufferOffset, parameter, state, isEnabled, workBuffer, nodeId);
            AddCommand(std::move(command));
        }
    }

    // Generate a new LimiterCommandVersion2 (only when the parameter's channel count is valid).
    void GenerateLimiterEffectVersion2(uint32_t bufferOffset, LimiterParameter parameter, Span<LimiterState> state, Span<EffectResultState> effectResultState,
                                       bool isEnabled, uint64_t workBuffer, int32_t nodeId) {
        if (command_buffer_detail::IsChannelCountValid(parameter.channel_count)) {
            auto command = std::make_unique<LimiterCommandVersion2>();
            command->Initialize(bufferOffset, parameter, state, effectResultState, isEnabled, workBuffer, nodeId);
            AddCommand(std::move(command));
        }
    }

    // Generate a new AuxiliaryBufferCommand (only when both send and return buffer info addresses are set).
    void GenerateAuxEffect(uint32_t bufferOffset, uint8_t inputBufferOffset, uint8_t outputBufferOffset, AuxiliaryBufferAddresses& state, bool isEnabled,
                           uint32_t countMax, CpuAddress outputBuffer, CpuAddress inputBuffer, uint32_t updateCount, uint32_t writeOffset, int32_t nodeId) {
        if (state.send_buffer_info_base != 0 && state.return_buffer_info_base != 0) {
            auto command = std::make_unique<AuxiliaryBufferCommand>();
            command->Initialize(bufferOffset, inputBufferOffset, outputBufferOffset, state, isEnabled, countMax, outputBuffer, inputBuffer, updateCount,
                                writeOffset, nodeId);
            AddCommand(std::move(command));
        }
    }

    // Generate a new CaptureBufferCommand (only when the send buffer info is set).
    void GenerateCaptureEffect(uint32_t bufferOffset, uint8_t inputBufferOffset, uint64_t sendBufferInfo, bool isEnabled, uint32_t countMax,
                               CpuAddress outputBuffer, uint32_t updateCount, uint32_t writeOffset, int32_t nodeId) {
        if (sendBufferInfo != 0) {
            auto command = std::make_unique<CaptureBufferCommand>();
            command->Initialize(bufferOffset, inputBufferOffset, sendBufferInfo, isEnabled, countMax, outputBuffer, updateCount, writeOffset, nodeId);
            AddCommand(std::move(command));
        }
    }

    // Generate a new CompressorCommand (only when the parameter's channel count is valid).
    void GenerateCompressorEffect(uint32_t bufferOffset, CompressorParameter parameter, Span<CompressorState> state,
                                  Span<EffectResultState> effectResultState, bool isEnabled, int32_t nodeId) {
        if (command_buffer_detail::IsChannelCountValid(parameter.channel_count)) {
            auto command = std::make_unique<CompressorCommand>();
            command->Initialize(bufferOffset, parameter, state, effectResultState, isEnabled, nodeId);
            AddCommand(std::move(command));
        }
    }

    // Generate a new VolumeCommand.
    void GenerateVolume(float volume, uint32_t bufferOffset, int32_t nodeId) {
        auto command = std::make_unique<VolumeCommand>();
        command->Initialize(volume, bufferOffset, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new CircularBufferSinkCommand.
    void GenerateCircularBuffer(uint32_t bufferOffset, CircularBufferSink& sink, int32_t nodeId) {
        auto command = std::make_unique<CircularBufferSinkCommand>();
        command->Initialize(bufferOffset, sink.Parameter, sink.CircularBufferAddressInfo, sink.CurrentWriteOffset, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new DownMixSurroundToStereoCommand.
    void GenerateDownMixSurroundToStereo(uint32_t bufferOffset, Span<const uint8_t> inputBufferOffset, Span<const uint8_t> outputBufferOffset,
                                         Span<const float> downMixParameter, int32_t nodeId) {
        auto command = std::make_unique<DownMixSurroundToStereoCommand>();
        command->Initialize(bufferOffset, inputBufferOffset, outputBufferOffset, downMixParameter, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new UpsampleCommand.
    void GenerateUpsample(uint32_t bufferOffset, UpsamplerInfo& upsampler, uint32_t inputCount, Span<const uint8_t> inputBufferOffset,
                          uint32_t bufferCountPerSample, uint32_t sampleCount, uint32_t sampleRate, int32_t nodeId) {
        auto command = std::make_unique<UpsampleCommand>();
        command->Initialize(bufferOffset, upsampler, inputCount, inputBufferOffset, bufferCountPerSample, sampleCount, sampleRate, nodeId);
        AddCommand(std::move(command));
    }

    // Create a new DeviceSinkCommand.
    void GenerateDeviceSink(uint32_t bufferOffset, DeviceSink& sink, int32_t sessionId, Span<float> buffer, int32_t nodeId) {
        auto command = std::make_unique<DeviceSinkCommand>();
        command->Initialize(bufferOffset, sink, sessionId, buffer, nodeId);
        AddCommand(std::move(command));
    }

    // Generate a new FillBufferCommand.
    void GenerateFillBuffer(SplitterDestination destination, float value, int32_t length, int32_t nodeId) {
        auto command = std::make_unique<FillBufferCommand>();
        command->Initialize(destination, length, value, nodeId);
        AddCommand(std::move(command));
    }

  private:
    // The command processing time estimator in use (C# field _commandProcessingTimeEstimator).
    ICommandProcessingTimeEstimator& _commandProcessingTimeEstimator;

    // C# AddCommand: the estimate is set on the command, the total is accumulated, and the command is handed to the CommandList.
    template <class TCommand>
    void AddCommand(std::unique_ptr<TCommand> command) {
        command->EstimatedProcessingTime = _commandProcessingTimeEstimator.Estimate(*command);
        EstimatedProcessingTime += command->EstimatedProcessingTime;
        CommandList.AddCommand(std::move(command));
    }
};

}  // namespace nxrt::audren
