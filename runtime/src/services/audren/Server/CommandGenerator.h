// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/CommandGenerator.cs (Ryubing fork, MIT). Walks the voices, sub mixes, final mix, effects and sinks
// of the server state and calls the CommandBuffer for every command, in the order of the C#. The method names and the branch structure
// are the C# ones; the comments mark the C# line each block comes from where it is not obvious.
//
// C# -> C++ deviations (all others follow audren_common.h):
//   * `Memory<T>` / `ReadOnlySpan<T>` are Span<T> / Span<const T>. `ref MixInfo mix = ref _mixContext.GetState(id)` is MixInfo&.
//   * `PerformanceEntryAddresses performanceEntry = null` is a default-constructed value; `_performanceManager` is a pointer, null when
//     performance recording is disabled (the C# null).
//   * Effects and sinks are held as BaseEffect / BaseSink; the C# `(DelayEffect)effect` casts are static_cast after the Type check.
//   * `Constants.UnusedSplitterIdInt` comparisons and `mixId < _mixContext.GetCount()` compare as 64-bit, which is what C# does for an
//     int against a uint.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "../Constants.h"
#include "../audren_types.h"
#include "BehaviourInfo.h"
#include "CommandBuffer.h"
#include "Effect/AuxiliaryBufferEffect.h"
#include "Effect/BaseEffect.h"
#include "Effect/BiquadFilterEffect.h"
#include "Effect/BufferMixEffect.h"
#include "Effect/CaptureBufferEffect.h"
#include "Effect/CompressorEffect.h"
#include "Effect/DelayEffect.h"
#include "Effect/EffectContext.h"
#include "Effect/LimiterEffect.h"
#include "Effect/Reverb3dEffect.h"
#include "Effect/ReverbEffect.h"
#include "Mix/MixContext.h"
#include "Mix/MixInfo.h"
#include "Performance/PerformanceEntryAddresses.h"
#include "Performance/PerformanceManager.h"
#include "RendererSystemContext.h"
#include "Sink/BaseSink.h"
#include "Sink/CircularBufferSink.h"
#include "Sink/DeviceSink.h"
#include "Sink/SinkContext.h"
#include "Splitter/SplitterContext.h"
#include "Splitter/SplitterDestination.h"
#include "Upsampler/UpsamplerManager.h"
#include "Voice/VoiceChannelResource.h"
#include "Voice/VoiceContext.h"
#include "Voice/VoiceInfo.h"

namespace nxrt::audren {

namespace command_generator_detail {

// C# `array.AsSpan()` for the fixed arrays of the C++ structs.
template <class T, size_t N>
Span<T> AsSpan(std::array<T, N>& array) {
    return Span<T>(array.data(), N);
}
template <class T, size_t N>
Span<const T> AsConstSpan(const std::array<T, N>& array) {
    return Span<const T>(array.data(), N);
}

// C# `Memory<VoiceState>.Slice` of the raw bytes: `SpanMemoryManager<byte>.Cast(state)[..(sizeof(BiquadFilterState) * 2)]` cast back to BiquadFilterState.
inline Span<BiquadFilterState> BiquadFilterStatesOf(Span<VoiceState> state) {
    return state.Cast<uint8_t>().Slice(0, sizeof(BiquadFilterState) * static_cast<size_t>(Constants::VoiceBiquadFilterCount)).Cast<BiquadFilterState>();
}

// C# `effect.GetWorkBuffer(-1)` for the effects whose C# override is `GetSingleBuffer()` (they override GetWorkBuffer here as well).
inline DspAddress GetSingleWorkBuffer(BaseEffect& effect) { return effect.GetWorkBuffer(-1); }

}  // namespace command_generator_detail

class CommandGenerator {
  public:
    CommandGenerator(CommandBuffer& commandBuffer, RendererSystemContext& rendererContext, VoiceContext& voiceContext, MixContext& mixContext,
                     EffectContext& effectContext, SinkContext& sinkContext, SplitterContext& splitterContext, PerformanceManager* performanceManager)
        : _commandBuffer(commandBuffer),
          _rendererContext(rendererContext),
          _voiceContext(voiceContext),
          _mixContext(mixContext),
          _effectContext(effectContext),
          _sinkContext(sinkContext),
          _splitterContext(splitterContext),
          _performanceManager(performanceManager) {
        _commandBuffer.GenerateClearMixBuffer(Constants::InvalidNodeId);
    }

    void GenerateVoices() {
        for (uint32_t i = 0; i < _voiceContext.GetCount(); i++) {
            VoiceInfo& sortedInfo = _voiceContext.GetSortedState(static_cast<int32_t>(i));

            if (!sortedInfo.ShouldSkip() && sortedInfo.UpdateForCommandGeneration(_voiceContext)) {
                int32_t nodeId = sortedInfo.NodeId;

                PerformanceEntryAddresses performanceEntry;

                bool performanceInitialized = false;

                if (_performanceManager != nullptr && _performanceManager->GetNextEntry(performanceEntry, PerformanceEntryType::Voice, nodeId)) {
                    performanceInitialized = true;

                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
                }

                GenerateVoice(sortedInfo);

                if (performanceInitialized) {
                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
                }
            }
        }

        _splitterContext.UpdateInternalState();
    }

    void GeneratePerformance(PerformanceEntryAddresses& performanceEntryAddresses, PerformanceCommand::Type type, int32_t nodeId) {
        _commandBuffer.GeneratePerformance(performanceEntryAddresses, type, nodeId);
    }

    void GenerateSubMixes() {
        for (uint32_t id = 0; id < _mixContext.GetCount(); id++) {
            MixInfo& sortedInfo = _mixContext.GetSortedState(static_cast<int32_t>(id));

            if (sortedInfo.IsUsed && sortedInfo.MixId != Constants::FinalMixId) {
                int32_t nodeId = sortedInfo.NodeId;

                PerformanceEntryAddresses performanceEntry;

                bool performanceInitialized = false;

                if (_performanceManager != nullptr && _performanceManager->GetNextEntry(performanceEntry, PerformanceEntryType::SubMix, nodeId)) {
                    performanceInitialized = true;

                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
                }

                GenerateSubMix(sortedInfo);

                if (performanceInitialized) {
                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
                }
            }
        }
    }

    void GenerateFinalMixes() {
        int32_t nodeId = _mixContext.GetFinalState().NodeId;

        PerformanceEntryAddresses performanceEntry;

        bool performanceInitialized = false;

        if (_performanceManager != nullptr && _performanceManager->GetNextEntry(performanceEntry, PerformanceEntryType::FinalMix, nodeId)) {
            performanceInitialized = true;

            GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
        }

        GenerateFinalMix();

        if (performanceInitialized) {
            GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
        }
    }

    void GenerateSinks() {
        MixInfo& finalMix = _mixContext.GetFinalState();

        for (uint32_t i = 0; i < _sinkContext.GetCount(); i++) {
            BaseSink& sink = _sinkContext.GetSink(static_cast<int32_t>(i));

            if (sink.IsUsed) {
                GenerateSink(sink, finalMix);
            }
        }
    }

  private:
    CommandBuffer& _commandBuffer;
    RendererSystemContext& _rendererContext;
    VoiceContext& _voiceContext;
    MixContext& _mixContext;
    EffectContext& _effectContext;
    SinkContext& _sinkContext;
    SplitterContext& _splitterContext;
    PerformanceManager* _performanceManager;

    // C# GenerateDataSource(ref VoiceInfo, Memory<VoiceState>, int).
    void GenerateDataSource(VoiceInfo& voiceInfo, Span<VoiceState> dspState, int32_t channelIndex) {
        if (voiceInfo.MixId != Constants::UnusedMixId) {
            MixInfo& mix = _mixContext.GetState(voiceInfo.MixId);

            _commandBuffer.GenerateDepopPrepare(dspState, _rendererContext.DepopBuffer, mix.BufferCount, mix.BufferOffset, voiceInfo.NodeId,
                                                voiceInfo.WasPlaying);
        } else if (voiceInfo.SplitterId != Constants::UnusedSplitterId) {
            int32_t destinationId = 0;

            while (true) {
                SplitterDestination destination = _splitterContext.GetDestination(static_cast<int32_t>(voiceInfo.SplitterId), destinationId++);

                if (destination.IsNull()) {
                    break;
                }

                if (destination.IsConfigured()) {
                    int32_t mixId = destination.DestinationId();

                    if (static_cast<int64_t>(mixId) < static_cast<int64_t>(_mixContext.GetCount()) && mixId != Constants::UnusedSplitterIdInt) {
                        MixInfo& mix = _mixContext.GetState(mixId);

                        // _commandBuffer.GenerateFillBuffer();

                        _commandBuffer.GenerateDepopPrepare(dspState, _rendererContext.DepopBuffer, mix.BufferCount, mix.BufferOffset, voiceInfo.NodeId,
                                                            voiceInfo.WasPlaying);

                        destination.MarkAsNeedToUpdateInternalState();
                    }
                }
            }
        }

        if (!voiceInfo.WasPlaying) {
            NXAUDREN_ASSERT(voiceInfo.SampleFormat != SampleFormat::Adpcm || channelIndex == 0);

            if (_rendererContext.BehaviourInfo->IsWaveBufferVersion2Supported()) {
                _commandBuffer.GenerateDataSourceVersion2(voiceInfo, dspState, static_cast<uint16_t>(_rendererContext.MixBufferCount),
                                                          static_cast<uint16_t>(channelIndex), voiceInfo.NodeId);
            } else {
                switch (voiceInfo.SampleFormat) {
                    case SampleFormat::PcmInt16:
                        _commandBuffer.GeneratePcmInt16DataSourceVersion1(voiceInfo, dspState, static_cast<uint16_t>(_rendererContext.MixBufferCount),
                                                                          static_cast<uint16_t>(channelIndex), voiceInfo.NodeId);
                        break;
                    case SampleFormat::PcmFloat:
                        _commandBuffer.GeneratePcmFloatDataSourceVersion1(voiceInfo, dspState, static_cast<uint16_t>(_rendererContext.MixBufferCount),
                                                                          static_cast<uint16_t>(channelIndex), voiceInfo.NodeId);
                        break;
                    case SampleFormat::Adpcm:
                        _commandBuffer.GenerateAdpcmDataSourceVersion1(voiceInfo, dspState, static_cast<uint16_t>(_rendererContext.MixBufferCount),
                                                                       voiceInfo.NodeId);
                        break;
                    default:
                        NXAUDREN_FATAL("NotImplementedException: unsupported data source");
                }
            }
        }
    }

    // C# GenerateBiquadFilterForVoice(ref VoiceInfo, Memory<VoiceState>, int baseIndex, int bufferOffset, int nodeId).
    void GenerateBiquadFilterForVoice(VoiceInfo& voiceInfo, Span<VoiceState> state, int32_t baseIndex, int32_t bufferOffset, int32_t nodeId) {
        bool supportsOptimizedPath = _rendererContext.BehaviourInfo->UseMultiTapBiquadFilterProcessing();

        Span<BiquadFilterParameter2> biquadFiltersSpan = command_generator_detail::AsSpan(voiceInfo.BiquadFilters);
        Span<bool> needInitialization = voiceInfo.BiquadFilterNeedInitialization();

        if (supportsOptimizedPath && biquadFiltersSpan[0].enable && biquadFiltersSpan[1].enable) {
            Span<BiquadFilterState> stateMemory = command_generator_detail::BiquadFilterStatesOf(state);

            _commandBuffer.GenerateMultiTapBiquadFilter(baseIndex, biquadFiltersSpan, stateMemory, bufferOffset, bufferOffset, needInitialization, nodeId);
        } else {
            for (size_t i = 0; i < biquadFiltersSpan.Length; i++) {
                BiquadFilterParameter2& filter = biquadFiltersSpan[i];

                if (filter.enable) {
                    Span<BiquadFilterState> stateMemory = command_generator_detail::BiquadFilterStatesOf(state);

                    _commandBuffer.GenerateBiquadFilter(baseIndex, filter, stateMemory.Slice(i, 1), bufferOffset, bufferOffset, !needInitialization[i],
                                                        nodeId);
                }
            }
        }
    }

    // C# GenerateVoiceMixWithSplitter(SplitterDestination, Memory<VoiceState>, uint bufferOffset, uint bufferCount, uint bufferIndex, int nodeId).
    void GenerateVoiceMixWithSplitter(SplitterDestination destination, Span<VoiceState> state, uint32_t bufferOffset, uint32_t bufferCount,
                                      uint32_t bufferIndex, int32_t nodeId) {
        Span<float> mixVolumes = destination.MixBufferVolume();
        Span<float> previousMixVolumes = destination.PreviousMixBufferVolume();

        BiquadFilterParameter2& bqf0 = destination.GetBiquadFilterParameter(0);
        BiquadFilterParameter2& bqf1 = destination.GetBiquadFilterParameter(1);

        Span<BiquadFilterState> bqfState = _splitterContext.GetBiquadFilterState(destination);

        bool isFirstMixBuffer = true;

        for (uint32_t i = 0; i < bufferCount; i++) {
            float previousMixVolume = previousMixVolumes[i];
            float mixVolume = mixVolumes[i];

            if (mixVolume != 0.0f || previousMixVolume != 0.0f) {
                if (bqf0.enable && bqf1.enable) {
                    _commandBuffer.GenerateMultiTapBiquadFilterAndMix(
                        previousMixVolume, mixVolume, bufferIndex, bufferOffset + i, static_cast<int32_t>(i), state, bqf0, bqf1, bqfState.Slice(0, 1),
                        bqfState.Slice(1, 1), bqfState.Slice(2, 1), bqfState.Slice(3, 1), !destination.IsBiquadFilterEnabledPrev(),
                        !destination.IsBiquadFilterEnabledPrev(), true, isFirstMixBuffer, nodeId);

                    destination.UpdateBiquadFilterEnabledPrev(0);
                    destination.UpdateBiquadFilterEnabledPrev(1);
                } else if (bqf0.enable) {
                    _commandBuffer.GenerateBiquadFilterAndMix(previousMixVolume, mixVolume, bufferIndex, bufferOffset + i, static_cast<int32_t>(i), state,
                                                              bqf0, bqfState.Slice(0, 1), bqfState.Slice(1, 1), !destination.IsBiquadFilterEnabledPrev(),
                                                              true, isFirstMixBuffer, nodeId);

                    destination.UpdateBiquadFilterEnabledPrev(0);
                } else if (bqf1.enable) {
                    _commandBuffer.GenerateBiquadFilterAndMix(previousMixVolume, mixVolume, bufferIndex, bufferOffset + i, static_cast<int32_t>(i), state,
                                                              bqf1, bqfState.Slice(0, 1), bqfState.Slice(1, 1), !destination.IsBiquadFilterEnabledPrev(),
                                                              true, isFirstMixBuffer, nodeId);

                    destination.UpdateBiquadFilterEnabledPrev(1);
                }

                isFirstMixBuffer = false;
            }
        }
    }

    // C# GenerateVoiceMix(ReadOnlySpan<float> mixVolumes, ReadOnlySpan<float> previousMixVolumes, Memory<VoiceState>, uint bufferOffset, uint bufferCount, uint bufferIndex, int nodeId).
    void GenerateVoiceMix(Span<const float> mixVolumes, Span<const float> previousMixVolumes, Span<VoiceState> state, uint32_t bufferOffset,
                          uint32_t bufferCount, uint32_t bufferIndex, int32_t nodeId) {
        if (bufferCount > static_cast<uint32_t>(Constants::VoiceChannelCountMax)) {
            _commandBuffer.GenerateMixRampGrouped(bufferCount, bufferIndex, bufferOffset, previousMixVolumes, mixVolumes, state, nodeId);
        } else {
            for (uint32_t i = 0; i < bufferCount; i++) {
                float previousMixVolume = previousMixVolumes[i];
                float mixVolume = mixVolumes[i];

                if (mixVolume != 0.0f || previousMixVolume != 0.0f) {
                    _commandBuffer.GenerateMixRamp(previousMixVolume, mixVolume, bufferIndex, bufferOffset + i, static_cast<int32_t>(i), state, nodeId);
                }
            }
        }
    }

    // C# GenerateVoice(ref VoiceInfo).
    void GenerateVoice(VoiceInfo& voiceInfo) {
        int32_t nodeId = voiceInfo.NodeId;
        uint32_t channelsCount = voiceInfo.ChannelsCount;

        Span<int32_t> channelResourceIdsSpan = command_generator_detail::AsSpan(voiceInfo.ChannelResourceIds);
        Span<BiquadFilterParameter2> biquadFiltersSpan = command_generator_detail::AsSpan(voiceInfo.BiquadFilters);

        for (int32_t channelIndex = 0; channelIndex < static_cast<int32_t>(channelsCount); channelIndex++) {
            Span<VoiceState> dspStateMemory = _voiceContext.GetUpdateStateForDsp(channelResourceIdsSpan[channelIndex]);

            VoiceChannelResource& channelResource = _voiceContext.GetChannelResource(channelResourceIdsSpan[channelIndex]);

            PerformanceDetailType dataSourceDetailType = PerformanceDetailType::Adpcm;

            if (voiceInfo.SampleFormat == SampleFormat::PcmInt16) {
                dataSourceDetailType = PerformanceDetailType::PcmInt16;
            } else if (voiceInfo.SampleFormat == SampleFormat::PcmFloat) {
                dataSourceDetailType = PerformanceDetailType::PcmFloat;
            }

            bool performanceInitialized = false;

            PerformanceEntryAddresses performanceEntry;

            if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
                _performanceManager->GetNextEntry(performanceEntry, dataSourceDetailType, PerformanceEntryType::Voice, nodeId)) {
                performanceInitialized = true;

                GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
            }

            GenerateDataSource(voiceInfo, dspStateMemory, channelIndex);

            if (performanceInitialized) {
                GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
            }

            if (voiceInfo.WasPlaying) {
                voiceInfo.PreviousVolume = 0.0f;
            } else if (voiceInfo.HasAnyDestination()) {
                performanceInitialized = false;

                if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
                    _performanceManager->GetNextEntry(performanceEntry, PerformanceDetailType::BiquadFilter, PerformanceEntryType::Voice, nodeId)) {
                    performanceInitialized = true;

                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
                }

                GenerateBiquadFilterForVoice(voiceInfo, dspStateMemory, static_cast<int32_t>(_rendererContext.MixBufferCount), channelIndex, nodeId);

                if (performanceInitialized) {
                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
                }

                performanceInitialized = false;

                if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
                    _performanceManager->GetNextEntry(performanceEntry, PerformanceDetailType::VolumeRamp, PerformanceEntryType::Voice, nodeId)) {
                    performanceInitialized = true;

                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
                }

                _commandBuffer.GenerateVolumeRamp(voiceInfo.PreviousVolume, voiceInfo.Volume,
                                                  _rendererContext.MixBufferCount + static_cast<uint32_t>(channelIndex), nodeId);

                if (performanceInitialized) {
                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
                }

                voiceInfo.PreviousVolume = voiceInfo.Volume;

                if (voiceInfo.MixId == Constants::UnusedMixId) {
                    if (voiceInfo.SplitterId != Constants::UnusedSplitterId) {
                        int32_t destinationId = channelIndex;

                        while (true) {
                            SplitterDestination destination = _splitterContext.GetDestination(static_cast<int32_t>(voiceInfo.SplitterId), destinationId);

                            if (destination.IsNull()) {
                                break;
                            }

                            destinationId += static_cast<int32_t>(channelsCount);

                            if (destination.IsConfigured()) {
                                int32_t mixId = destination.DestinationId();

                                if (static_cast<int64_t>(mixId) < static_cast<int64_t>(_mixContext.GetCount()) &&
                                    mixId != Constants::UnusedSplitterIdInt) {
                                    MixInfo& mix = _mixContext.GetState(mixId);

                                    if (destination.IsBiquadFilterEnabled()) {
                                        GenerateVoiceMixWithSplitter(destination, dspStateMemory, mix.BufferOffset, mix.BufferCount,
                                                                     _rendererContext.MixBufferCount + static_cast<uint32_t>(channelIndex), nodeId);
                                    } else {
                                        GenerateVoiceMix(destination.MixBufferVolume(), destination.PreviousMixBufferVolume(), dspStateMemory,
                                                         mix.BufferOffset, mix.BufferCount,
                                                         _rendererContext.MixBufferCount + static_cast<uint32_t>(channelIndex), nodeId);
                                    }

                                    destination.MarkAsNeedToUpdateInternalState();
                                }
                            }
                        }
                    }
                } else {
                    MixInfo& mix = _mixContext.GetState(voiceInfo.MixId);

                    performanceInitialized = false;

                    if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
                        _performanceManager->GetNextEntry(performanceEntry, PerformanceDetailType::Mix, PerformanceEntryType::Voice, nodeId)) {
                        performanceInitialized = true;

                        GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
                    }

                    GenerateVoiceMix(command_generator_detail::AsConstSpan(channelResource.Mix), command_generator_detail::AsConstSpan(channelResource.PreviousMix),
                                     dspStateMemory, mix.BufferOffset, mix.BufferCount,
                                     _rendererContext.MixBufferCount + static_cast<uint32_t>(channelIndex), nodeId);

                    if (performanceInitialized) {
                        GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
                    }

                    channelResource.UpdateState();
                }

                Span<bool> needInitialization = voiceInfo.BiquadFilterNeedInitialization();
                for (size_t i = 0; i < needInitialization.Length; i++) {
                    needInitialization[i] = biquadFiltersSpan[i].enable;
                }
            }
        }
    }

    // C# GenerateBufferMixerEffect(int bufferOffset, BufferMixEffect effect, int nodeId).
    void GenerateBufferMixerEffect(int32_t bufferOffset, BufferMixEffect& effect, int32_t nodeId) {
        NXAUDREN_ASSERT(effect.Type == EffectType::BufferMix);

        if (effect.IsEnabled) {
            for (uint32_t i = 0; i < effect.Parameter.mixes_count; i++) {
                if (effect.Parameter.volumes[i] != 0.0f) {
                    _commandBuffer.GenerateMix(static_cast<uint32_t>(bufferOffset) + effect.Parameter.input[i],
                                               static_cast<uint32_t>(bufferOffset) + effect.Parameter.output[i], nodeId, effect.Parameter.volumes[i]);
                }
            }
        }
    }

    // C# GenerateAuxEffect(uint bufferOffset, AuxiliaryBufferEffect effect, int nodeId).
    void GenerateAuxEffect(uint32_t bufferOffset, AuxiliaryBufferEffect& effect, int32_t nodeId) {
        NXAUDREN_ASSERT(effect.Type == EffectType::AuxiliaryBuffer);

        if (effect.IsEnabled) {
            effect.GetWorkBuffer(0);
            effect.GetWorkBuffer(1);
        }

        if (effect.State.send_buffer_info_base != 0 && effect.State.return_buffer_info_base != 0) {
            int32_t i = 0;
            uint32_t writeOffset = 0;

            for (uint32_t channelIndex = effect.Parameter.channel_count; channelIndex != 0; channelIndex--) {
                uint32_t newUpdateCount = writeOffset + _commandBuffer.CommandList.SampleCount;

                uint32_t updateCount;

                if (channelIndex != 1) {
                    updateCount = 0;
                } else {
                    updateCount = newUpdateCount;
                }

                _commandBuffer.GenerateAuxEffect(bufferOffset, effect.Parameter.input[i], effect.Parameter.output[i], effect.State, effect.IsEnabled,
                                                 effect.Parameter.buffer_storage_size, effect.State.send_buffer_info_base,
                                                 effect.State.return_buffer_info_base, updateCount, writeOffset, nodeId);

                writeOffset = newUpdateCount;

                i++;
            }
        }
    }

    // C# GenerateDelayEffect(uint bufferOffset, DelayEffect effect, int nodeId, bool newEffectChannelMappingSupported).
    void GenerateDelayEffect(uint32_t bufferOffset, DelayEffect& effect, int32_t nodeId, bool newEffectChannelMappingSupported) {
        NXAUDREN_ASSERT(effect.Type == EffectType::Delay);

        DspAddress workBuffer = command_generator_detail::GetSingleWorkBuffer(effect);

        _commandBuffer.GenerateDelayEffect(bufferOffset, effect.Parameter, effect.State, effect.IsEnabled, workBuffer, nodeId,
                                           newEffectChannelMappingSupported);
    }

    // C# GenerateReverbEffect(uint bufferOffset, ReverbEffect effect, int nodeId, bool isLongSizePreDelaySupported, bool newEffectChannelMappingSupported).
    void GenerateReverbEffect(uint32_t bufferOffset, ReverbEffect& effect, int32_t nodeId, bool isLongSizePreDelaySupported,
                              bool newEffectChannelMappingSupported) {
        NXAUDREN_ASSERT(effect.Type == EffectType::Reverb);

        DspAddress workBuffer = command_generator_detail::GetSingleWorkBuffer(effect);

        _commandBuffer.GenerateReverbEffect(bufferOffset, effect.Parameter, effect.State, effect.IsEnabled, workBuffer, nodeId,
                                            isLongSizePreDelaySupported, newEffectChannelMappingSupported);
    }

    // C# GenerateReverb3dEffect(uint bufferOffset, Reverb3dEffect effect, int nodeId, bool newEffectChannelMappingSupported).
    void GenerateReverb3dEffect(uint32_t bufferOffset, Reverb3dEffect& effect, int32_t nodeId, bool newEffectChannelMappingSupported) {
        NXAUDREN_ASSERT(effect.Type == EffectType::Reverb3d);

        DspAddress workBuffer = command_generator_detail::GetSingleWorkBuffer(effect);

        _commandBuffer.GenerateReverb3dEffect(bufferOffset, effect.Parameter, effect.State, effect.IsEnabled, workBuffer, nodeId,
                                              newEffectChannelMappingSupported);
    }

    // C# GenerateBiquadFilterEffect(uint bufferOffset, BiquadFilterEffect effect, int nodeId).
    void GenerateBiquadFilterEffect(uint32_t bufferOffset, BiquadFilterEffect& effect, int32_t nodeId) {
        NXAUDREN_ASSERT(effect.Type == EffectType::BiquadFilter);

        if (effect.IsEnabled) {
            bool needInitialization = effect.Parameter.status == UsageState::Invalid ||
                                      (effect.Parameter.status == UsageState::New && !_rendererContext.BehaviourInfo->IsBiquadFilterEffectStateClearBugFixed());

            BiquadFilterParameter2 parameter{};
            parameter.enable = true;

            std::memcpy(parameter.denominator, effect.Parameter.denominator, sizeof(parameter.denominator));
            std::memcpy(parameter.numerator, effect.Parameter.numerator, sizeof(parameter.numerator));

            for (int32_t i = 0; i < static_cast<int32_t>(effect.Parameter.channel_count); i++) {
                _commandBuffer.GenerateBiquadFilter(static_cast<int32_t>(bufferOffset), parameter, effect.State.Slice(static_cast<size_t>(i), 1),
                                                    effect.Parameter.input[i], effect.Parameter.output[i], needInitialization, nodeId);
            }
        } else {
            for (int32_t i = 0; i < static_cast<int32_t>(effect.Parameter.channel_count); i++) {
                uint32_t inputBufferIndex = bufferOffset + effect.Parameter.input[i];
                uint32_t outputBufferIndex = bufferOffset + effect.Parameter.output[i];

                // If the input and output isn't the same, generate a command.
                if (inputBufferIndex != outputBufferIndex) {
                    _commandBuffer.GenerateCopyMixBuffer(inputBufferIndex, outputBufferIndex, nodeId);
                }
            }
        }
    }

    // C# GenerateLimiterEffect(uint bufferOffset, LimiterEffect effect, int nodeId, int effectId).
    void GenerateLimiterEffect(uint32_t bufferOffset, LimiterEffect& effect, int32_t nodeId, int32_t effectId) {
        NXAUDREN_ASSERT(effect.Type == EffectType::Limiter);

        DspAddress workBuffer = command_generator_detail::GetSingleWorkBuffer(effect);

        if (_rendererContext.BehaviourInfo->IsEffectInfoVersion2Supported()) {
            Span<EffectResultState> dspResultState;

            if (effect.Parameter.statistics_enabled) {
                dspResultState = _effectContext.GetDspStateMemory(effectId);
            } else {
                dspResultState = Span<EffectResultState>();
            }

            _commandBuffer.GenerateLimiterEffectVersion2(bufferOffset, effect.Parameter, effect.State, dspResultState, effect.IsEnabled, workBuffer, nodeId);
        } else {
            _commandBuffer.GenerateLimiterEffectVersion1(bufferOffset, effect.Parameter, effect.State, effect.IsEnabled, workBuffer, nodeId);
        }
    }

    // C# GenerateCaptureEffect(uint bufferOffset, CaptureBufferEffect effect, int nodeId).
    void GenerateCaptureEffect(uint32_t bufferOffset, CaptureBufferEffect& effect, int32_t nodeId) {
        NXAUDREN_ASSERT(effect.Type == EffectType::CaptureBuffer);

        if (effect.IsEnabled) {
            effect.GetWorkBuffer(0);
        }

        if (effect.State.send_buffer_info_base != 0) {
            int32_t i = 0;
            uint32_t writeOffset = 0;

            for (uint32_t channelIndex = effect.Parameter.channel_count; channelIndex != 0; channelIndex--) {
                uint32_t newUpdateCount = writeOffset + _commandBuffer.CommandList.SampleCount;

                uint32_t updateCount;

                if (channelIndex != 1) {
                    updateCount = 0;
                } else {
                    updateCount = newUpdateCount;
                }

                _commandBuffer.GenerateCaptureEffect(bufferOffset, effect.Parameter.input[i], effect.State.send_buffer_info, effect.IsEnabled,
                                                     effect.Parameter.buffer_storage_size, effect.State.send_buffer_info_base, updateCount, writeOffset,
                                                     nodeId);

                writeOffset = newUpdateCount;

                i++;
            }
        }
    }

    // C# GenerateCompressorEffect(uint bufferOffset, CompressorEffect effect, int nodeId, int effectId).
    void GenerateCompressorEffect(uint32_t bufferOffset, CompressorEffect& effect, int32_t nodeId, int32_t effectId) {
        NXAUDREN_ASSERT(effect.Type == EffectType::Compressor);

        Span<EffectResultState> dspResultState;

        if (effect.Parameter.statistics_enabled) {
            dspResultState = _effectContext.GetDspStateMemory(effectId);
        } else {
            dspResultState = Span<EffectResultState>();
        }

        _commandBuffer.GenerateCompressorEffect(bufferOffset, effect.Parameter, effect.State, dspResultState, effect.IsEnabled, nodeId);
    }

    // C# GenerateEffect(ref MixInfo mix, int effectId, BaseEffect effect).
    void GenerateEffect(MixInfo& mix, int32_t effectId, BaseEffect& effect) {
        int32_t nodeId = mix.NodeId;

        bool isFinalMix = mix.MixId == Constants::FinalMixId;

        PerformanceEntryAddresses performanceEntry;

        bool performanceInitialized = false;

        if (_performanceManager != nullptr &&
            _performanceManager->GetNextEntry(performanceEntry, effect.GetPerformanceDetailType(),
                                              isFinalMix ? PerformanceEntryType::FinalMix : PerformanceEntryType::SubMix, nodeId)) {
            performanceInitialized = true;

            GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
        }

        switch (effect.Type) {
            case EffectType::BufferMix:
                GenerateBufferMixerEffect(static_cast<int32_t>(mix.BufferOffset), static_cast<BufferMixEffect&>(effect), nodeId);
                break;
            case EffectType::AuxiliaryBuffer:
                GenerateAuxEffect(mix.BufferOffset, static_cast<AuxiliaryBufferEffect&>(effect), nodeId);
                break;
            case EffectType::Delay:
                GenerateDelayEffect(mix.BufferOffset, static_cast<DelayEffect&>(effect), nodeId, _rendererContext.BehaviourInfo->IsNewEffectChannelMappingSupported());
                break;
            case EffectType::Reverb:
                GenerateReverbEffect(mix.BufferOffset, static_cast<ReverbEffect&>(effect), nodeId, mix.IsLongSizePreDelaySupported,
                                     _rendererContext.BehaviourInfo->IsNewEffectChannelMappingSupported());
                break;
            case EffectType::Reverb3d:
                GenerateReverb3dEffect(mix.BufferOffset, static_cast<Reverb3dEffect&>(effect), nodeId,
                                       _rendererContext.BehaviourInfo->IsNewEffectChannelMappingSupported());
                break;
            case EffectType::BiquadFilter:
                GenerateBiquadFilterEffect(mix.BufferOffset, static_cast<BiquadFilterEffect&>(effect), nodeId);
                break;
            case EffectType::Limiter:
                GenerateLimiterEffect(mix.BufferOffset, static_cast<LimiterEffect&>(effect), nodeId, effectId);
                break;
            case EffectType::CaptureBuffer:
                GenerateCaptureEffect(mix.BufferOffset, static_cast<CaptureBufferEffect&>(effect), nodeId);
                break;
            case EffectType::Compressor:
                GenerateCompressorEffect(mix.BufferOffset, static_cast<CompressorEffect&>(effect), nodeId, effectId);
                break;
            default:
                NXAUDREN_FATAL("NotImplementedException: unsupported effect type");
        }

        if (performanceInitialized) {
            GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
        }

        effect.UpdateForCommandGeneration();
    }

    // C# GenerateEffects(ref MixInfo mix).
    void GenerateEffects(MixInfo& mix) {
        Span<int32_t> effectProcessingOrderArray = mix.EffectProcessingOrderArray();

        NXAUDREN_ASSERT(_effectContext.GetCount() == 0 || !effectProcessingOrderArray.IsEmpty());

        for (int32_t i = 0; i < static_cast<int32_t>(_effectContext.GetCount()); i++) {
            int32_t effectOrder = effectProcessingOrderArray[static_cast<size_t>(i)];

            if (effectOrder == Constants::InvalidProcessingOrder) {
                break;
            }

            // BaseEffect is a class, we don't need to pass it by ref
            BaseEffect& effect = *_effectContext.GetEffect(effectOrder);

            NXAUDREN_ASSERT(effect.Type != EffectType::Invalid);
            NXAUDREN_ASSERT(effect.MixId == mix.MixId);

            if (!effect.ShouldSkip()) {
                GenerateEffect(mix, effectOrder, effect);
            }
        }
    }

    // C# GenerateMixWithSplitter(uint inputBufferIndex, uint outputBufferIndex, float volume, SplitterDestination destination, ref bool isFirstMixBuffer, int nodeId).
    void GenerateMixWithSplitter(uint32_t inputBufferIndex, uint32_t outputBufferIndex, float volume, SplitterDestination destination, bool& isFirstMixBuffer,
                                 int32_t nodeId) {
        BiquadFilterParameter2& bqf0 = destination.GetBiquadFilterParameter(0);
        BiquadFilterParameter2& bqf1 = destination.GetBiquadFilterParameter(1);

        Span<BiquadFilterState> bqfState = _splitterContext.GetBiquadFilterState(destination);

        if (bqf0.enable && bqf1.enable) {
            _commandBuffer.GenerateMultiTapBiquadFilterAndMix(0.0f, volume, inputBufferIndex, outputBufferIndex, 0, Span<VoiceState>(), bqf0, bqf1,
                                                              bqfState.Slice(0, 1), bqfState.Slice(1, 1), bqfState.Slice(2, 1), bqfState.Slice(3, 1),
                                                              !destination.IsBiquadFilterEnabledPrev(), !destination.IsBiquadFilterEnabledPrev(), false,
                                                              isFirstMixBuffer, nodeId);

            destination.UpdateBiquadFilterEnabledPrev(0);
            destination.UpdateBiquadFilterEnabledPrev(1);
        } else if (bqf0.enable) {
            _commandBuffer.GenerateBiquadFilterAndMix(0.0f, volume, inputBufferIndex, outputBufferIndex, 0, Span<VoiceState>(), bqf0, bqfState.Slice(0, 1),
                                                      bqfState.Slice(1, 1), !destination.IsBiquadFilterEnabledPrev(), false, isFirstMixBuffer, nodeId);

            destination.UpdateBiquadFilterEnabledPrev(0);
        } else if (bqf1.enable) {
            _commandBuffer.GenerateBiquadFilterAndMix(0.0f, volume, inputBufferIndex, outputBufferIndex, 0, Span<VoiceState>(), bqf1, bqfState.Slice(0, 1),
                                                      bqfState.Slice(1, 1), !destination.IsBiquadFilterEnabledPrev(), false, isFirstMixBuffer, nodeId);

            destination.UpdateBiquadFilterEnabledPrev(1);
        }

        isFirstMixBuffer = false;
    }

    // C# GenerateMix(ref MixInfo mix).
    void GenerateMix(MixInfo& mix) {
        if (mix.HasAnyDestination()) {
            NXAUDREN_ASSERT(mix.DestinationMixId != Constants::UnusedMixId || mix.DestinationSplitterId != Constants::UnusedSplitterId);

            if (mix.DestinationMixId == Constants::UnusedMixId) {
                if (mix.DestinationSplitterId != Constants::UnusedSplitterId) {
                    int32_t destinationId = 0;

                    while (true) {
                        int32_t destinationIndex = destinationId++;

                        SplitterDestination destination = _splitterContext.GetDestination(static_cast<int32_t>(mix.DestinationSplitterId), destinationIndex);

                        if (destination.IsNull()) {
                            break;
                        }

                        if (destination.IsConfigured()) {
                            int32_t mixId = destination.DestinationId();

                            if (static_cast<int64_t>(mixId) < static_cast<int64_t>(_mixContext.GetCount()) && mixId != Constants::UnusedSplitterIdInt) {
                                MixInfo& destinationMix = _mixContext.GetState(mixId);

                                uint32_t inputBufferIndex = mix.BufferOffset + (static_cast<uint32_t>(destinationIndex) % mix.BufferCount);

                                bool isFirstMixBuffer = true;

                                for (uint32_t bufferDestinationIndex = 0; bufferDestinationIndex < destinationMix.BufferCount; bufferDestinationIndex++) {
                                    float volume = mix.Volume * destination.GetMixVolume(static_cast<int32_t>(bufferDestinationIndex));

                                    if (volume != 0.0f) {
                                        if (destination.IsBiquadFilterEnabled()) {
                                            GenerateMixWithSplitter(inputBufferIndex, destinationMix.BufferOffset + bufferDestinationIndex, volume, destination,
                                                                    isFirstMixBuffer, mix.NodeId);
                                        } else {
                                            _commandBuffer.GenerateMix(inputBufferIndex, destinationMix.BufferOffset + bufferDestinationIndex, mix.NodeId,
                                                                       volume);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                MixInfo& destinationMix = _mixContext.GetState(mix.DestinationMixId);

                for (uint32_t bufferIndex = 0; bufferIndex < mix.BufferCount; bufferIndex++) {
                    for (uint32_t bufferDestinationIndex = 0; bufferDestinationIndex < destinationMix.BufferCount; bufferDestinationIndex++) {
                        float volume = mix.Volume * mix.GetMixBufferVolume(static_cast<int32_t>(bufferIndex), static_cast<int32_t>(bufferDestinationIndex));

                        if (volume != 0.0f) {
                            _commandBuffer.GenerateMix(mix.BufferOffset + bufferIndex, destinationMix.BufferOffset + bufferDestinationIndex, mix.NodeId, volume);
                        }
                    }
                }
            }
        }
    }

    // C# GenerateSubMix(ref MixInfo subMix).
    void GenerateSubMix(MixInfo& subMix) {
        _commandBuffer.GenerateDepopForMixBuffers(_rendererContext.DepopBuffer, subMix.BufferOffset, subMix.BufferCount, subMix.NodeId, subMix.SampleRate);

        GenerateEffects(subMix);

        PerformanceEntryAddresses performanceEntry;

        int32_t nodeId = subMix.NodeId;

        bool performanceInitialized = false;

        if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
            _performanceManager->GetNextEntry(performanceEntry, PerformanceDetailType::Mix, PerformanceEntryType::SubMix, nodeId)) {
            performanceInitialized = true;

            GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
        }

        GenerateMix(subMix);

        if (performanceInitialized) {
            GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
        }
    }

    // C# GenerateFinalMix().
    void GenerateFinalMix() {
        MixInfo& finalMix = _mixContext.GetFinalState();

        _commandBuffer.GenerateDepopForMixBuffers(_rendererContext.DepopBuffer, finalMix.BufferOffset, finalMix.BufferCount, finalMix.NodeId, finalMix.SampleRate);

        GenerateEffects(finalMix);

        PerformanceEntryAddresses performanceEntry;

        int32_t nodeId = finalMix.NodeId;

        bool performanceInitialized = false;

        if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
            _performanceManager->GetNextEntry(performanceEntry, PerformanceDetailType::Mix, PerformanceEntryType::FinalMix, nodeId)) {
            performanceInitialized = true;

            GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
        }

        // Only generate volume command if the volume isn't 100%.
        if (finalMix.Volume != 1.0f) {
            for (uint32_t bufferIndex = 0; bufferIndex < finalMix.BufferCount; bufferIndex++) {
                bool performanceSubInitialized = false;

                if (_performanceManager != nullptr && _performanceManager->IsTargetNodeId(nodeId) &&
                    _performanceManager->GetNextEntry(performanceEntry, PerformanceDetailType::VolumeRamp, PerformanceEntryType::FinalMix, nodeId)) {
                    performanceSubInitialized = true;

                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, nodeId);
                }

                _commandBuffer.GenerateVolume(finalMix.Volume, finalMix.BufferOffset + bufferIndex, nodeId);

                if (performanceSubInitialized) {
                    GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
                }
            }
        }

        if (performanceInitialized) {
            GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, nodeId);
        }
    }

    // C# GenerateCircularBuffer(CircularBufferSink sink, ref MixInfo finalMix).
    void GenerateCircularBuffer(CircularBufferSink& sink, MixInfo& finalMix) {
        _commandBuffer.GenerateCircularBuffer(finalMix.BufferOffset, sink, Constants::InvalidNodeId);
    }

    // C# GenerateDevice(DeviceSink sink, ref MixInfo finalMix).
    void GenerateDevice(DeviceSink& sink, MixInfo& finalMix) {
        if (_commandBuffer.CommandList.SampleRate != 48000 && sink.UpsamplerInfo == nullptr) {
            sink.UpsamplerInfo = _rendererContext.UpsamplerManager->Allocate();
        }

        bool useCustomDownMixingCommand = _rendererContext.ChannelCount == 2 && sink.Parameter.down_mix_parameter_enabled;

        if (useCustomDownMixingCommand) {
            _commandBuffer.GenerateDownMixSurroundToStereo(finalMix.BufferOffset, Span<const uint8_t>(sink.Parameter.input, sizeof(sink.Parameter.input)),
                                                           Span<const uint8_t>(sink.Parameter.input, sizeof(sink.Parameter.input)),
                                                           Span<const float>(sink.DownMixCoefficients), Constants::InvalidNodeId);
        }
        // NOTE: We do the downmixing at the DSP level as it's easier that way.
        else if (_rendererContext.ChannelCount == 2 && sink.Parameter.input_count == 6) {
            _commandBuffer.GenerateDownMixSurroundToStereo(finalMix.BufferOffset, Span<const uint8_t>(sink.Parameter.input, sizeof(sink.Parameter.input)),
                                                           Span<const uint8_t>(sink.Parameter.input, sizeof(sink.Parameter.input)),
                                                           Span<const float>(Constants::DefaultSurroundToStereoCoefficients), Constants::InvalidNodeId);
        }

        CommandList& commandList = _commandBuffer.CommandList;

        if (sink.UpsamplerInfo != nullptr) {
            _commandBuffer.GenerateUpsample(finalMix.BufferOffset, *sink.UpsamplerInfo, sink.Parameter.input_count,
                                            Span<const uint8_t>(sink.Parameter.input, sizeof(sink.Parameter.input)), commandList.BufferCount,
                                            commandList.SampleCount, commandList.SampleRate, Constants::InvalidNodeId);
        }

        _commandBuffer.GenerateDeviceSink(finalMix.BufferOffset, sink, _rendererContext.SessionId, commandList.Buffers, Constants::InvalidNodeId);
    }

    // C# GenerateSink(BaseSink sink, ref MixInfo finalMix).
    void GenerateSink(BaseSink& sink, MixInfo& finalMix) {
        bool performanceInitialized = false;

        PerformanceEntryAddresses performanceEntry;

        if (_performanceManager != nullptr && _performanceManager->GetNextEntry(performanceEntry, PerformanceEntryType::Sink, sink.NodeId)) {
            performanceInitialized = true;

            GeneratePerformance(performanceEntry, PerformanceCommand::Type::Start, sink.NodeId);
        }

        if (!sink.ShouldSkip) {
            switch (sink.Type) {
                case SinkType::CircularBuffer:
                    GenerateCircularBuffer(static_cast<CircularBufferSink&>(sink), finalMix);
                    break;
                case SinkType::Device:
                    GenerateDevice(static_cast<DeviceSink&>(sink), finalMix);
                    break;
                default:
                    NXAUDREN_FATAL("NotImplementedException: unsupported sink type");
            }

            sink.UpdateForCommandGeneration();
        }

        if (performanceInitialized) {
            GeneratePerformance(performanceEntry, PerformanceCommand::Type::End, sink.NodeId);
        }
    }
};

}  // namespace nxrt::audren
