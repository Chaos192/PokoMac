// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Port of Ryujinx.Audio/Renderer/Server/StateUpdater.cs (Ryubing fork, MIT): parses the RequestUpdate input buffer into the server
// objects and writes the output buffer. Method names and order follow the C# exactly; AudioRenderSystem.Update calls them in the
// order of the C# AudioRenderSystem.Update.
//
// Deviations from the C#, all forced by the port conventions in audren_common.h:
//   * C# `ref struct StateUpdater` is a class. The input is one Span<const uint8_t> read through SequenceReader (Splitter/SequenceReader.h).
//     The output is a SpanIOHelper<uint8_t> cursor (Utils/SpanIOHelper.h); C# `ref _output` becomes `_output.GetWriteRef<T>()`.
//   * C# ReadOnlySequence<byte> input becomes one contiguous Span<const uint8_t>. Guest memory is host memory (no memoryManager).
//   * C# `ref readonly UpdateDataHeader _inputHeader` is a copy (the reader returns copies; the header is read-only here).
//   * C# `ref UpdateDataHeader OutputHeader` is a pointer into the first 0x40 bytes of the output (the output header).
//   * C# UpdateDataHeader.Initialize(int) is missing from audren_types.h: implemented as the free function InitializeUpdateDataHeader.
//   * C# BehaviourInfo is a class owned by AudioRenderSystem: held as a pointer.
//   * C# `ArrayPool<Memory<VoiceState>>` becomes a std::array of Span<VoiceState> on the stack (the pool is not ported).
//   * C# BaseEffect / BaseSink slots that get replaced (ResetEffect / ResetSink) are the owning unique_ptr slots of EffectContext /
//     SinkContext, so ResetEffect and ResetSink take the slot.
//   * MixInfo::Update is a template over the effect context and calls GetEffect(i).MixId; EffectContext::GetEffect returns a
//     unique_ptr<BaseEffect>&, so the call does not compile. EffectContextForMixUpdate adapts the context to BaseEffect&. MixInfo.h
//     is not changed.
//   * C# Logger.Error in CheckConsumedSize becomes a message on stderr.
//   * C# Debug.Assert becomes NXAUDREN_ASSERT; C# throw becomes NXAUDREN_FATAL (audren_common.h).
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "../Constants.h"
#include "../Utils/SpanIOHelper.h"
#include "../audren_common.h"
#include "../audren_types.h"
#include "BehaviourInfo.h"
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
#include "MemoryPool/MemoryPoolInfo.h"
#include "MemoryPool/PoolMapper.h"
#include "Mix/MixContext.h"
#include "Mix/MixInfo.h"
#include "Performance/PerformanceManager.h"
#include "Sink/BaseSink.h"
#include "Sink/CircularBufferSink.h"
#include "Sink/DeviceSink.h"
#include "Sink/SinkContext.h"
#include "Splitter/SequenceReader.h"
#include "Splitter/SplitterContext.h"
#include "Voice/VoiceChannelResource.h"
#include "Voice/VoiceContext.h"
#include "Voice/VoiceInfo.h"

namespace nxrt::audren {

// C# UpdateDataHeader.Initialize(int revision): sets the revision and the total size (the header's own size).
inline void InitializeUpdateDataHeader(UpdateDataHeader& header, int32_t revision) {
    header.revision = revision;
    header.total_size = static_cast<uint32_t>(sizeof(UpdateDataHeader));
}

// C# MixInfo.Update takes the effect context as a generic argument and reads effect.MixId / ProcessingOrder from GetEffect(i).
// EffectContext::GetEffect returns the owning slot, so this adapter gives MixInfo the BaseEffect& it expects. It does not own anything.
class EffectContextForMixUpdate {
  public:
    explicit EffectContextForMixUpdate(EffectContext& context) : _context(context) {}

    int32_t GetCount() const { return static_cast<int32_t>(_context.GetCount()); }

    BaseEffect& GetEffect(int32_t index) { return *_context.GetEffect(index); }

  private:
    EffectContext& _context;
};

class StateUpdater {
  public:
    StateUpdater(Span<const uint8_t> input, Span<uint8_t> output, uint32_t processHandle, BehaviourInfo* behaviourInfo)
        : _inputReader(input),
          _outputOrigin(output),
          _output(output),
          _processHandle(processHandle),
          _behaviourInfo(behaviourInfo),
          _inputHeader{},
          _outputHeader(nullptr) {
        _inputHeader = _inputReader.GetRefOrRefToCopy<UpdateDataHeader>();

        Span<UpdateDataHeader> outputHeader = _output.GetWriteRef<UpdateDataHeader>();
        _outputHeader = &outputHeader[0];
        InitializeUpdateDataHeader(*_outputHeader, _behaviourInfo->UserRevision);
    }

    ResultCode UpdateBehaviourInfo() {
        BehaviourParameter parameter = _inputReader.GetRefOrRefToCopy<BehaviourParameter>();

        // The revision numbers must match, not the magic: Switch 2 SDKs open the renderer with 'REV'+n and tag their updates 'REv'+n
        // (Lichii NX2_NOTES row 39).
        if (!BehaviourInfo::CheckValidRevision(parameter.user_revision) ||
            BehaviourInfo::GetRevisionNumber(parameter.user_revision) != BehaviourInfo::GetRevisionNumber(_behaviourInfo->UserRevision)) {
            return ResultCode::InvalidUpdateInfo;
        }

        _behaviourInfo->ClearError();
        _behaviourInfo->UpdateFlags(parameter.flags);

        if (_inputHeader.behaviour_size != sizeof(BehaviourParameter)) {
            return ResultCode::InvalidUpdateInfo;
        }

        return ResultCode::Success;
    }

    ResultCode UpdateMemoryPools(Span<MemoryPoolInfo> memoryPools) {
        PoolMapper mapper(_processHandle, _behaviourInfo->IsMemoryPoolForceMappingEnabled());

        if (memoryPools.Length * sizeof(MemoryPoolInParameter) != _inputHeader.memory_pools_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        for (size_t i = 0; i < memoryPools.Length; i++) {
            MemoryPoolInParameter parameter = _inputReader.GetRefOrRefToCopy<MemoryPoolInParameter>();

            MemoryPoolOutStatus& outStatus = _output.GetWriteRef<MemoryPoolOutStatus>()[0];

            PoolMapper::UpdateResult updateResult = mapper.Update(memoryPools[i], parameter, outStatus);

            if (updateResult != PoolMapper::UpdateResult::Success && updateResult != PoolMapper::UpdateResult::MapError &&
                updateResult != PoolMapper::UpdateResult::UnmapError) {
                if (updateResult != PoolMapper::UpdateResult::InvalidParameter) {
                    NXAUDREN_FATAL("StateUpdater::UpdateMemoryPools: unexpected PoolMapper result");
                }

                return ResultCode::InvalidUpdateInfo;
            }
        }

        OutputHeader().memory_pools_size = static_cast<uint32_t>(sizeof(MemoryPoolOutStatus) * memoryPools.Length);
        OutputHeader().total_size += OutputHeader().memory_pools_size;

        return ResultCode::Success;
    }

    ResultCode UpdateVoiceChannelResources(VoiceContext& context) {
        if (static_cast<uint64_t>(context.GetCount()) * sizeof(VoiceChannelResourceInParameter) != _inputHeader.voice_resources_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        for (uint32_t i = 0; i < context.GetCount(); i++) {
            VoiceChannelResourceInParameter parameter = _inputReader.GetRefOrRefToCopy<VoiceChannelResourceInParameter>();

            VoiceChannelResource& resource = context.GetChannelResource(static_cast<int32_t>(i));

            resource.Id = parameter.id;
            Span<const float>(parameter.mix, MixBufferCountMax).CopyTo(Span<float>(resource.Mix.data(), resource.Mix.size()));
            resource.IsUsed = parameter.is_used;
        }

        return ResultCode::Success;
    }

    ResultCode UpdateVoices2(VoiceContext& context, PoolMapper& mapper) {
        if (static_cast<uint64_t>(context.GetCount()) * sizeof(VoiceInParameter2) != _inputHeader.voices_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        [[maybe_unused]] size_t initialOutputSize = _output.Remaining().Length;

        int64_t initialInputConsumed = _inputReader.Consumed();

        // First make everything not in use.
        for (uint32_t i = 0; i < context.GetCount(); i++) {
            VoiceInfo& info = context.GetState(static_cast<int32_t>(i));

            info.InUse = false;
        }

        std::array<Span<VoiceState>, VoiceChannelCountMax> voiceStates{};

        // Start processing
        for (uint32_t i = 0; i < context.GetCount(); i++) {
            VoiceInParameter2 parameter = _inputReader.GetRefOrRefToCopy<VoiceInParameter2>();

            voiceStates.fill(Span<VoiceState>());

            VoiceOutStatus& outStatus = _output.GetWriteRef<VoiceOutStatus>()[0];

            if (parameter.in_use) {
                VoiceInfo& currentVoiceInfo = context.GetState(static_cast<int32_t>(i));

                for (uint32_t channelResourceIndex = 0; channelResourceIndex < parameter.channel_count; channelResourceIndex++) {
                    int32_t channelId = parameter.channel_resource_ids[channelResourceIndex];

                    NXAUDREN_ASSERT(channelId >= 0 && static_cast<uint32_t>(channelId) < context.GetCount());
                    NXAUDREN_ASSERT(channelResourceIndex < VoiceChannelCountMax);

                    voiceStates[channelResourceIndex] = context.GetUpdateStateForCpu(channelId);
                }

                if (parameter.is_new) {
                    currentVoiceInfo.Initialize();
                }

                BehaviourParameter::ErrorInfo updateParameterError{};
                currentVoiceInfo.UpdateParameters2(updateParameterError, parameter, mapper, *_behaviourInfo);

                if (updateParameterError.error_code != ResultCode::Success) {
                    _behaviourInfo->AppendError(updateParameterError);
                }

                std::vector<BehaviourParameter::ErrorInfo> waveBufferUpdateErrorInfos;
                currentVoiceInfo.UpdateWaveBuffers2(waveBufferUpdateErrorInfos, parameter,
                                                    Span<const Span<VoiceState>>(voiceStates.data(), voiceStates.size()), mapper, *_behaviourInfo);

                for (BehaviourParameter::ErrorInfo& errorInfo : waveBufferUpdateErrorInfos) {
                    if (errorInfo.error_code != ResultCode::Success) {
                        _behaviourInfo->AppendError(errorInfo);
                    }
                }

                currentVoiceInfo.WriteOutStatus2(outStatus, parameter, Span<const Span<VoiceState>>(voiceStates.data(), voiceStates.size()));
            }
        }

        [[maybe_unused]] size_t currentOutputSize = _output.Remaining().Length;

        OutputHeader().voices_size = static_cast<uint32_t>(sizeof(VoiceOutStatus) * context.GetCount());
        OutputHeader().total_size += OutputHeader().voices_size;

        NXAUDREN_ASSERT((initialOutputSize - currentOutputSize) == OutputHeader().voices_size);

        _inputReader.SetConsumed(initialInputConsumed + _inputHeader.voices_size);

        return ResultCode::Success;
    }

    ResultCode UpdateVoices1(VoiceContext& context, PoolMapper& mapper) {
        if (static_cast<uint64_t>(context.GetCount()) * sizeof(VoiceInParameter1) != _inputHeader.voices_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        [[maybe_unused]] size_t initialOutputSize = _output.Remaining().Length;

        int64_t initialInputConsumed = _inputReader.Consumed();

        // First make everything not in use.
        for (uint32_t i = 0; i < context.GetCount(); i++) {
            VoiceInfo& info = context.GetState(static_cast<int32_t>(i));

            info.InUse = false;
        }

        std::array<Span<VoiceState>, VoiceChannelCountMax> voiceStates{};

        // Start processing
        for (uint32_t i = 0; i < context.GetCount(); i++) {
            VoiceInParameter1 parameter = _inputReader.GetRefOrRefToCopy<VoiceInParameter1>();

            voiceStates.fill(Span<VoiceState>());

            VoiceOutStatus& outStatus = _output.GetWriteRef<VoiceOutStatus>()[0];

            if (parameter.in_use) {
                VoiceInfo& currentVoiceInfo = context.GetState(static_cast<int32_t>(i));

                for (uint32_t channelResourceIndex = 0; channelResourceIndex < parameter.channel_count; channelResourceIndex++) {
                    int32_t channelId = parameter.channel_resource_ids[channelResourceIndex];

                    NXAUDREN_ASSERT(channelId >= 0 && static_cast<uint32_t>(channelId) < context.GetCount());
                    NXAUDREN_ASSERT(channelResourceIndex < VoiceChannelCountMax);

                    voiceStates[channelResourceIndex] = context.GetUpdateStateForCpu(channelId);
                }

                if (parameter.is_new) {
                    currentVoiceInfo.Initialize();
                }

                BehaviourParameter::ErrorInfo updateParameterError{};
                currentVoiceInfo.UpdateParameters1(updateParameterError, parameter, mapper, *_behaviourInfo);

                if (updateParameterError.error_code != ResultCode::Success) {
                    _behaviourInfo->AppendError(updateParameterError);
                }

                std::vector<BehaviourParameter::ErrorInfo> waveBufferUpdateErrorInfos;
                currentVoiceInfo.UpdateWaveBuffers1(waveBufferUpdateErrorInfos, parameter,
                                                    Span<const Span<VoiceState>>(voiceStates.data(), voiceStates.size()), mapper, *_behaviourInfo);

                for (BehaviourParameter::ErrorInfo& errorInfo : waveBufferUpdateErrorInfos) {
                    if (errorInfo.error_code != ResultCode::Success) {
                        _behaviourInfo->AppendError(errorInfo);
                    }
                }

                currentVoiceInfo.WriteOutStatus1(outStatus, parameter, Span<const Span<VoiceState>>(voiceStates.data(), voiceStates.size()));
            }
        }

        [[maybe_unused]] size_t currentOutputSize = _output.Remaining().Length;

        OutputHeader().voices_size = static_cast<uint32_t>(sizeof(VoiceOutStatus) * context.GetCount());
        OutputHeader().total_size += OutputHeader().voices_size;

        NXAUDREN_ASSERT((initialOutputSize - currentOutputSize) == OutputHeader().voices_size);

        _inputReader.SetConsumed(initialInputConsumed + _inputHeader.voices_size);

        return ResultCode::Success;
    }

    ResultCode UpdateEffects(EffectContext& context, bool isAudioRendererActive, PoolMapper& mapper) {
        if (_behaviourInfo->IsEffectInfoVersion2Supported()) {
            return UpdateEffectsVersion2(context, isAudioRendererActive, mapper);
        }

        return UpdateEffectsVersion1(context, isAudioRendererActive, mapper);
    }

    ResultCode UpdateEffectsVersion2(EffectContext& context, bool isAudioRendererActive, PoolMapper& mapper) {
        if (static_cast<uint64_t>(context.GetCount()) * sizeof(EffectInParameterVersion2) != _inputHeader.effects_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        [[maybe_unused]] size_t initialOutputSize = _output.Remaining().Length;

        int64_t initialInputConsumed = _inputReader.Consumed();

        for (uint32_t i = 0; i < context.GetCount(); i++) {
            EffectInParameterVersion2 parameter = _inputReader.GetRefOrRefToCopy<EffectInParameterVersion2>();

            EffectOutStatusVersion2& outStatus = _output.GetWriteRef<EffectOutStatusVersion2>()[0];

            std::unique_ptr<BaseEffect>& effect = context.GetEffect(static_cast<int32_t>(i));

            if (!effect->IsTypeValid(parameter)) {
                ResetEffect(effect, parameter, mapper);
            }

            BehaviourParameter::ErrorInfo updateErrorInfo{};
            effect->Update(updateErrorInfo, parameter, mapper);

            if (updateErrorInfo.error_code != ResultCode::Success) {
                _behaviourInfo->AppendError(updateErrorInfo);
            }

            effect->StoreStatus(outStatus, isAudioRendererActive);

            if (parameter.is_new) {
                effect->InitializeResultState(context.GetDspState(static_cast<int32_t>(i)));
                effect->InitializeResultState(context.GetState(static_cast<int32_t>(i)));
            }

            effect->UpdateResultState(outStatus.result_state, context.GetState(static_cast<int32_t>(i)));
        }

        [[maybe_unused]] size_t currentOutputSize = _output.Remaining().Length;

        OutputHeader().effects_size = static_cast<uint32_t>(sizeof(EffectOutStatusVersion2) * context.GetCount());
        OutputHeader().total_size += OutputHeader().effects_size;

        NXAUDREN_ASSERT((initialOutputSize - currentOutputSize) == OutputHeader().effects_size);

        _inputReader.SetConsumed(initialInputConsumed + _inputHeader.effects_size);

        return ResultCode::Success;
    }

    ResultCode UpdateEffectsVersion1(EffectContext& context, bool isAudioRendererActive, PoolMapper& mapper) {
        if (static_cast<uint64_t>(context.GetCount()) * sizeof(EffectInParameterVersion1) != _inputHeader.effects_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        [[maybe_unused]] size_t initialOutputSize = _output.Remaining().Length;

        int64_t initialInputConsumed = _inputReader.Consumed();

        for (uint32_t i = 0; i < context.GetCount(); i++) {
            EffectInParameterVersion1 parameter = _inputReader.GetRefOrRefToCopy<EffectInParameterVersion1>();

            EffectOutStatusVersion1& outStatus = _output.GetWriteRef<EffectOutStatusVersion1>()[0];

            std::unique_ptr<BaseEffect>& effect = context.GetEffect(static_cast<int32_t>(i));

            if (!effect->IsTypeValid(parameter)) {
                ResetEffect(effect, parameter, mapper);
            }

            BehaviourParameter::ErrorInfo updateErrorInfo{};
            effect->Update(updateErrorInfo, parameter, mapper);

            if (updateErrorInfo.error_code != ResultCode::Success) {
                _behaviourInfo->AppendError(updateErrorInfo);
            }

            effect->StoreStatus(outStatus, isAudioRendererActive);
        }

        [[maybe_unused]] size_t currentOutputSize = _output.Remaining().Length;

        OutputHeader().effects_size = static_cast<uint32_t>(sizeof(EffectOutStatusVersion1) * context.GetCount());
        OutputHeader().total_size += OutputHeader().effects_size;

        NXAUDREN_ASSERT((initialOutputSize - currentOutputSize) == OutputHeader().effects_size);

        _inputReader.SetConsumed(initialInputConsumed + _inputHeader.effects_size);

        return ResultCode::Success;
    }

    ResultCode UpdateSplitter(SplitterContext& context) {
        int64_t initialInputConsumed = _inputReader.Consumed();

        if (context.Update(_inputReader)) {
            _inputReader.SetConsumed(initialInputConsumed + _inputHeader.splitter_size);

            return ResultCode::Success;
        }

        return ResultCode::InvalidUpdateInfo;
    }

    ResultCode UpdateMixes(MixContext& mixContext, uint32_t mixBufferCount, EffectContext& effectContext, SplitterContext& splitterContext) {
        uint32_t mixCount = 0;
        uint32_t inputMixSize = 0;
        uint32_t inputSize = 0;

        if (_behaviourInfo->IsMixInParameterDirtyOnlyUpdateSupported()) {
            MixInParameterDirtyOnlyUpdate parameter = _inputReader.GetRefOrRefToCopy<MixInParameterDirtyOnlyUpdate>();

            mixCount = parameter.mix_count;

            inputSize += static_cast<uint32_t>(sizeof(MixInParameterDirtyOnlyUpdate));
        } else {
            mixCount = mixContext.GetCount();
        }

        inputMixSize = mixCount * static_cast<uint32_t>(sizeof(MixParameter));

        inputSize += inputMixSize;

        if (inputSize != _inputHeader.mixes_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        int64_t initialInputConsumed = _inputReader.Consumed();

        int32_t parameterCount = static_cast<int32_t>(inputMixSize / sizeof(MixParameter));

        if (CheckMixParametersValidity(mixContext, mixBufferCount, mixCount, _inputReader)) {
            return ResultCode::InvalidUpdateInfo;
        }

        bool isMixContextDirty = false;

        for (int32_t i = 0; i < parameterCount; i++) {
            MixParameter parameter = _inputReader.GetRefOrRefToCopy<MixParameter>();

            int32_t mixId = i;

            if (_behaviourInfo->IsMixInParameterDirtyOnlyUpdateSupported()) {
                mixId = parameter.mix_id;
            }

            MixInfo& mix = mixContext.GetState(mixId);

            if (parameter.is_used != mix.IsUsed) {
                mix.IsUsed = parameter.is_used;

                if (parameter.is_used) {
                    mix.ClearEffectProcessingOrder();
                }

                isMixContextDirty = true;
            }

            if (mix.IsUsed) {
                EffectContextForMixUpdate effectView(effectContext);

                isMixContextDirty |= mix.Update(mixContext.EdgeMatrix, parameter, effectView, splitterContext, *_behaviourInfo);
            }
        }

        if (isMixContextDirty) {
            if (_behaviourInfo->IsSplitterSupported() && splitterContext.UsingSplitter()) {
                if (!mixContext.Sort(splitterContext)) {
                    return ResultCode::InvalidMixSorting;
                }
            } else {
                mixContext.Sort();
            }
        }

        _inputReader.SetConsumed(initialInputConsumed + inputMixSize);

        return ResultCode::Success;
    }

    ResultCode UpdateSinks(SinkContext& context, PoolMapper& mapper) {
        if (static_cast<uint64_t>(context.GetCount()) * sizeof(SinkInParameter) != _inputHeader.sinks_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        [[maybe_unused]] size_t initialOutputSize = _output.Remaining().Length;

        int64_t initialInputConsumed = _inputReader.Consumed();

        for (uint32_t i = 0; i < context.GetCount(); i++) {
            SinkInParameter parameter = _inputReader.GetRefOrRefToCopy<SinkInParameter>();
            SinkOutStatus& outStatus = _output.GetWriteRef<SinkOutStatus>()[0];
            std::unique_ptr<BaseSink>& sink = context.GetSinkSlot(static_cast<int32_t>(i));

            if (!sink->IsTypeValid(parameter)) {
                ResetSink(sink, parameter);
            }

            BehaviourParameter::ErrorInfo updateErrorInfo{};
            sink->Update(updateErrorInfo, parameter, outStatus, mapper);

            if (updateErrorInfo.error_code != ResultCode::Success) {
                _behaviourInfo->AppendError(updateErrorInfo);
            }
        }

        [[maybe_unused]] size_t currentOutputSize = _output.Remaining().Length;

        OutputHeader().sinks_size = static_cast<uint32_t>(sizeof(SinkOutStatus) * context.GetCount());
        OutputHeader().total_size += OutputHeader().sinks_size;

        NXAUDREN_ASSERT((initialOutputSize - currentOutputSize) == OutputHeader().sinks_size);

        _inputReader.SetConsumed(initialInputConsumed + _inputHeader.sinks_size);

        return ResultCode::Success;
    }

    ResultCode UpdatePerformanceBuffer(PerformanceManager* manager, Span<uint8_t> performanceOutput) {
        if (sizeof(PerformanceInParameter) != _inputHeader.performance_buffer_size) {
            return ResultCode::InvalidUpdateInfo;
        }

        PerformanceInParameter parameter = _inputReader.GetRefOrRefToCopy<PerformanceInParameter>();

        PerformanceOutStatus& outStatus = _output.GetWriteRef<PerformanceOutStatus>()[0];

        if (manager != nullptr) {
            outStatus.history_size = manager->CopyHistories(performanceOutput);

            manager->SetTargetNodeId(parameter.target_node_id);
        } else {
            outStatus.history_size = 0;
        }

        OutputHeader().performance_buffer_size = static_cast<uint32_t>(sizeof(PerformanceOutStatus));
        OutputHeader().total_size += OutputHeader().performance_buffer_size;

        return ResultCode::Success;
    }

    ResultCode UpdateErrorInfo() {
        BehaviourErrorInfoOutStatus& outStatus = _output.GetWriteRef<BehaviourErrorInfoOutStatus>()[0];

        _behaviourInfo->CopyErrorInfo(Span<BehaviourParameter::ErrorInfo>(outStatus.error_infos, MaxErrorInfos), outStatus.error_infos_count);

        OutputHeader().behaviour_size = static_cast<uint32_t>(sizeof(BehaviourErrorInfoOutStatus));
        OutputHeader().total_size += OutputHeader().behaviour_size;

        return ResultCode::Success;
    }

    ResultCode UpdateRendererInfo(uint64_t elapsedFrameCount) {
        RendererInfoOutStatus& outStatus = _output.GetWriteRef<RendererInfoOutStatus>()[0];

        outStatus.elapsed_frame_count = elapsedFrameCount;

        OutputHeader().render_info_size = static_cast<uint32_t>(sizeof(RendererInfoOutStatus));
        OutputHeader().total_size += OutputHeader().render_info_size;

        return ResultCode::Success;
    }

    ResultCode CheckConsumedSize() const {
        int64_t consumedInputSize = _inputReader.Consumed();
        int64_t consumedOutputSize = static_cast<int64_t>(_outputOrigin.Length) - static_cast<int64_t>(_output.Remaining().Length);

        if (consumedInputSize != static_cast<int64_t>(_inputHeader.total_size)) {
            std::fprintf(stderr, "Consumed input size mismatch (got %lld expected %u)\n", static_cast<long long>(consumedInputSize),
                         _inputHeader.total_size);

            return ResultCode::InvalidUpdateInfo;
        }

        if (consumedOutputSize != static_cast<int64_t>(OutputHeader().total_size)) {
            std::fprintf(stderr, "Consumed output size mismatch (got %lld expected %u)\n", static_cast<long long>(consumedOutputSize),
                         OutputHeader().total_size);

            return ResultCode::InvalidUpdateInfo;
        }

        return ResultCode::Success;
    }

  private:
    // C# `ref UpdateDataHeader OutputHeader => ref _outputHeader.Span[0]`.
    UpdateDataHeader& OutputHeader() { return *_outputHeader; }
    const UpdateDataHeader& OutputHeader() const { return *_outputHeader; }

    // C# ResetEffect<T>: unmaps the work buffers of the old effect and replaces it with the effect of the requested type.
    template <class T>
    void ResetEffect(std::unique_ptr<BaseEffect>& effect, const T& parameter, PoolMapper& mapper) {
        effect->ForceUnmapBuffers(mapper);

        switch (parameter.type) {
            case EffectType::Invalid:
                effect = std::make_unique<BaseEffect>();
                break;
            case EffectType::BufferMix:
                effect = std::make_unique<BufferMixEffect>();
                break;
            case EffectType::AuxiliaryBuffer:
                effect = std::make_unique<AuxiliaryBufferEffect>();
                break;
            case EffectType::Delay:
                effect = std::make_unique<DelayEffect>();
                break;
            case EffectType::Reverb:
                effect = std::make_unique<ReverbEffect>();
                break;
            case EffectType::Reverb3d:
                effect = std::make_unique<Reverb3dEffect>();
                break;
            case EffectType::BiquadFilter:
                if (_behaviourInfo->IsBiquadFilterParameterFloatSupported()) {
                    effect = std::make_unique<BiquadFilterEffect>(2);
                } else {
                    effect = std::make_unique<BiquadFilterEffect>(1);
                }
                break;
            case EffectType::Limiter:
                effect = std::make_unique<LimiterEffect>();
                break;
            case EffectType::CaptureBuffer:
                effect = std::make_unique<CaptureBufferEffect>();
                break;
            case EffectType::Compressor:
                effect = std::make_unique<CompressorEffect>();
                break;
            default:
                NXAUDREN_FATAL("StateUpdater::ResetEffect: EffectType not implemented");
        }
    }

    // C# CheckMixParametersValidity. The C# takes the SequenceReader by value (a copy), so the copy is kept here too.
    static bool CheckMixParametersValidity(MixContext& mixContext, uint32_t mixBufferCount, uint32_t inputMixCount, SequenceReader parameters) {
        uint32_t maxMixStateCount = mixContext.GetCount();
        uint32_t totalRequiredMixBufferCount = 0;

        for (uint32_t i = 0; i < inputMixCount; i++) {
            MixParameter parameter = parameters.GetRefOrRefToCopy<MixParameter>();

            if (parameter.is_used) {
                // C# compares the signed DestinationMixId with the unsigned count; a 64-bit compare keeps the sign.
                if (parameter.destination_mix_id != Constants::UnusedMixId &&
                    static_cast<int64_t>(parameter.destination_mix_id) > static_cast<int64_t>(maxMixStateCount) &&
                    parameter.mix_id != Constants::FinalMixId) {
                    return true;
                }

                totalRequiredMixBufferCount += parameter.buffer_count;
            }
        }

        return totalRequiredMixBufferCount > mixBufferCount;
    }

    // C# ResetSink: cleans up the old sink and replaces it with the sink of the requested type.
    static void ResetSink(std::unique_ptr<BaseSink>& sink, const SinkInParameter& parameter) {
        sink->CleanUp();

        switch (parameter.type) {
            case SinkType::Invalid:
                sink = std::make_unique<BaseSink>();
                break;
            case SinkType::CircularBuffer:
                sink = std::make_unique<CircularBufferSink>();
                break;
            case SinkType::Device:
                sink = std::make_unique<DeviceSink>();
                break;
            default:
                NXAUDREN_FATAL("StateUpdater::ResetSink: SinkType not implemented");
        }
    }

    // The input cursor (C# SequenceReader<byte> _inputReader).
    SequenceReader _inputReader;

    // The whole output buffer, kept for the consumed-size check (C# _outputOrigin).
    Span<uint8_t> _outputOrigin;

    // The output cursor (C# Memory<byte> _output).
    SpanIOHelper<uint8_t> _output;

    // The handle of the process owning the memory (C# _processHandle).
    uint32_t _processHandle;

    // The behaviour context, owned by AudioRenderSystem (C# BehaviourInfo _behaviourInfo).
    BehaviourInfo* _behaviourInfo;

    // A copy of the input header (C# ref readonly UpdateDataHeader _inputHeader).
    UpdateDataHeader _inputHeader;

    // The output header, the first 0x40 bytes of the output buffer (C# Memory<UpdateDataHeader> _outputHeader).
    UpdateDataHeader* _outputHeader;
};

}  // namespace nxrt::audren
