// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceManagerGeneric.cs (Ryubing fork, MIT).
// C# generics `<THeader, TEntry, TEntryDetail>` with constraints `unmanaged, IPerformanceHeader/IPerformanceEntry/IPerformanceDetailEntry`
// become C++ templates; the constraints are static_asserts against the concepts in IPerformanceHeader.h and friends.
// The C# guest-memory views (MemoryMarshal.Cast over Memory<byte>) are Span<uint8_t> slices with Cast<T>(). The C# Memory<byte>
// parameters are Span<uint8_t> (non-owning: the caller keeps the guest buffer alive).
#pragma once
#include <cstdint>
#include <memory>

#include "IPerformanceDetailEntry.h"
#include "IPerformanceEntry.h"
#include "IPerformanceHeader.h"
#include "PerformanceDetailVersion1.h"
#include "PerformanceDetailVersion2.h"
#include "PerformanceEntryVersion1.h"
#include "PerformanceEntryVersion2.h"
#include "PerformanceFrameHeaderVersion1.h"
#include "PerformanceFrameHeaderVersion2.h"
#include "PerformanceManager.h"

namespace nxrt::audren {

template <class THeader, class TEntry, class TEntryDetail>
class PerformanceManagerGeneric : public PerformanceManager {
    static_assert(IPerformanceHeader<THeader>);
    static_assert(IPerformanceEntry<TEntry>);
    static_assert(IPerformanceDetailEntry<TEntryDetail>);

  private:
    // The magic used for the THeader.
    static constexpr uint32_t MagicPerformanceBuffer = 0x46524550;

    // The fixed amount of TEntryDetail that can be stored in a frame.
    static constexpr int32_t MaxFrameDetailCount = 100;

    Span<uint8_t> _buffer;
    Span<uint8_t> _historyBuffer;

    int32_t _frameSize = 0;
    int32_t _availableFrameCount = 0;
    int32_t _entryCountPerFrame = 0;
    int32_t _detailTarget = 0;
    int32_t _entryIndex = 0;
    int32_t _entryDetailIndex = 0;
    int32_t _indexHistoryWrite = 0;
    int32_t _indexHistoryRead = 0;
    uint32_t _historyFrameIndex = 0;

    // C# `CurrentBuffer => _buffer[.._frameSize]`
    Span<uint8_t> CurrentBuffer() const { return _buffer.Slice(0, static_cast<size_t>(_frameSize)); }
    // C# `CurrentBufferData => CurrentBuffer[Unsafe.SizeOf<THeader>()..]`
    Span<uint8_t> CurrentBufferData() const { return CurrentBuffer().Slice(sizeof(THeader)); }
    // C# `ref CurrentHeader => ref MemoryMarshal.template Cast<byte, THeader>(CurrentBuffer.Span)[0]`
    THeader& CurrentHeader() const { return CurrentBuffer().template Cast<THeader>()[0]; }
    // C# `Entries => MemoryMarshal.template Cast<byte, TEntry>(CurrentBufferData.Span[..GetEntriesSize()])`
    Span<TEntry> Entries() const { return CurrentBufferData().Slice(0, static_cast<size_t>(GetEntriesSize())).template Cast<TEntry>(); }
    // C# `EntriesDetail => MemoryMarshal.template Cast<byte, TEntryDetail>(CurrentBufferData.Span.Slice(GetEntriesSize(), GetEntriesDetailSize()))`
    Span<TEntryDetail> EntriesDetail() const {
        return CurrentBufferData()
            .Slice(static_cast<size_t>(GetEntriesSize()), static_cast<size_t>(GetEntriesDetailSize()))
            .template Cast<TEntryDetail>();
    }

    Span<uint8_t> GetBufferFromIndex(Span<uint8_t> data, int32_t index) const {
        return data.Slice(static_cast<size_t>(index) * static_cast<size_t>(_frameSize), static_cast<size_t>(_frameSize));
    }

    THeader& GetHeaderFromBuffer(Span<uint8_t> data, int32_t index) const { return GetBufferFromIndex(data, index).template Cast<THeader>()[0]; }

    Span<TEntry> GetEntriesFromBuffer(Span<uint8_t> data, int32_t index) const {
        return GetBufferFromIndex(data, index).Slice(sizeof(THeader), static_cast<size_t>(GetEntriesSize())).template Cast<TEntry>();
    }

    Span<TEntryDetail> GetEntriesDetailFromBuffer(Span<uint8_t> data, int32_t index) const {
        return GetBufferFromIndex(data, index)
            .Slice(sizeof(THeader) + static_cast<size_t>(GetEntriesSize()), static_cast<size_t>(GetEntriesDetailSize()))
            .template Cast<TEntryDetail>();
    }

    void SetupNewHeader() {
        _entryIndex = 0;
        _entryDetailIndex = 0;

        CurrentHeader().SetEntryCount(0);
        CurrentHeader().SetEntryDetailCount(0);
    }

  public:
    PerformanceManagerGeneric(Span<uint8_t> buffer, AudioRendererConfiguration& parameter) : _buffer(buffer) {
        _frameSize = GetRequiredBufferSizeForPerformanceMetricsPerFrame(parameter);

        _entryCountPerFrame = static_cast<int32_t>(GetEntryCount(parameter));
        _availableFrameCount = static_cast<int32_t>(buffer.Length / static_cast<size_t>(_frameSize)) - 1;

        _historyFrameIndex = 0;

        _historyBuffer = _buffer.Slice(static_cast<size_t>(_frameSize));

        SetupNewHeader();
    }

    static uint32_t GetEntryCount(AudioRendererConfiguration& parameter) {
        return parameter.voice_count + parameter.effect_count + parameter.sub_mix_buffer_count + parameter.sink_count + 1;
    }

    int32_t GetEntriesSize() const { return static_cast<int32_t>(sizeof(TEntry)) * _entryCountPerFrame; }

    static int32_t GetEntriesDetailSize() { return static_cast<int32_t>(sizeof(TEntryDetail)) * MaxFrameDetailCount; }

    static int32_t GetRequiredBufferSizeForPerformanceMetricsPerFrame(AudioRendererConfiguration& parameter) {
        return static_cast<int32_t>(sizeof(TEntry)) * static_cast<int32_t>(GetEntryCount(parameter)) + GetEntriesDetailSize() +
               static_cast<int32_t>(sizeof(THeader));
    }

    uint32_t CopyHistories(Span<uint8_t> performanceOutput) override {
        if (performanceOutput.IsEmpty()) {
            return 0;
        }

        int32_t nextOffset = 0;

        while (_indexHistoryRead != _indexHistoryWrite) {
            if (static_cast<size_t>(nextOffset) >= performanceOutput.Length) {
                break;
            }

            THeader& inputHeader = GetHeaderFromBuffer(_historyBuffer, _indexHistoryRead);
            Span<TEntry> inputEntries = GetEntriesFromBuffer(_historyBuffer, _indexHistoryRead);
            Span<TEntryDetail> inputEntriesDetail = GetEntriesDetailFromBuffer(_historyBuffer, _indexHistoryRead);

            Span<uint8_t> targetSpan = performanceOutput.Slice(static_cast<size_t>(nextOffset));

            // NOTE: We check for the space for two headers for the final blank header.
            int32_t requiredSpace = static_cast<int32_t>(sizeof(THeader)) + static_cast<int32_t>(sizeof(TEntry)) * inputHeader.GetEntryCount() +
                                    static_cast<int32_t>(sizeof(TEntryDetail)) * inputHeader.GetEntryDetailCount() +
                                    static_cast<int32_t>(sizeof(THeader));

            if (targetSpan.Length < static_cast<size_t>(requiredSpace)) {
                break;
            }

            THeader& outputHeader = targetSpan.template Cast<THeader>()[0];

            nextOffset += static_cast<int32_t>(sizeof(THeader));

            Span<TEntry> outputEntries = performanceOutput.Slice(static_cast<size_t>(nextOffset)).template Cast<TEntry>();

            int32_t totalProcessingTime = 0;

            int32_t effectiveEntryCount = 0;

            for (int32_t entryIndex = 0; entryIndex < inputHeader.GetEntryCount(); entryIndex++) {
                TEntry& input = inputEntries[static_cast<size_t>(entryIndex)];

                if (input.GetProcessingTime() != 0 || input.GetStartTime() != 0) {
                    TEntry& output = outputEntries[static_cast<size_t>(effectiveEntryCount++)];

                    output = input;

                    nextOffset += static_cast<int32_t>(sizeof(TEntry));

                    totalProcessingTime += input.GetProcessingTime();
                }
            }

            Span<TEntryDetail> outputEntriesDetail = performanceOutput.Slice(static_cast<size_t>(nextOffset)).template Cast<TEntryDetail>();

            int32_t effectiveEntryDetailCount = 0;

            for (int32_t entryDetailIndex = 0; entryDetailIndex < inputHeader.GetEntryDetailCount(); entryDetailIndex++) {
                TEntryDetail& input = inputEntriesDetail[static_cast<size_t>(entryDetailIndex)];

                if (input.GetProcessingTime() != 0 || input.GetStartTime() != 0) {
                    TEntryDetail& output = outputEntriesDetail[static_cast<size_t>(effectiveEntryDetailCount++)];

                    output = input;

                    nextOffset += static_cast<int32_t>(sizeof(TEntryDetail));
                }
            }

            outputHeader = inputHeader;
            outputHeader.SetMagic(MagicPerformanceBuffer);
            outputHeader.SetTotalProcessingTime(totalProcessingTime);
            outputHeader.SetNextOffset(nextOffset);
            outputHeader.SetEntryCount(effectiveEntryCount);
            outputHeader.SetEntryDetailCount(effectiveEntryDetailCount);

            _indexHistoryRead = (_indexHistoryRead + 1) % _availableFrameCount;
        }

        if (static_cast<size_t>(nextOffset) < performanceOutput.Length &&
            (performanceOutput.Length - static_cast<size_t>(nextOffset)) >= sizeof(THeader)) {
            THeader& outputHeader = performanceOutput.Slice(static_cast<size_t>(nextOffset)).template Cast<THeader>()[0];

            outputHeader = THeader{};
        }

        return static_cast<uint32_t>(nextOffset);
    }

    bool GetNextEntry(PerformanceEntryAddresses& performanceEntry, PerformanceEntryType entryType, int32_t nodeId) override {
        // Deviation from C#: the C# indexes past the frame's entry array (an exception) when more entries are requested than
        // GetEntryCount allows. Here the extra request is dropped, like the detailed-entry overload does.
        if (_entryIndex >= _entryCountPerFrame) {
            performanceEntry = PerformanceEntryAddresses{};
            return false;
        }

        performanceEntry = PerformanceEntryAddresses{};
        performanceEntry.BaseMemory = CurrentBuffer().template Cast<int32_t>();
        performanceEntry.EntryCountOffset = static_cast<uint32_t>(CurrentHeader().GetEntryCountOffset());

        uint32_t baseEntryOffset = static_cast<uint32_t>(sizeof(THeader) + sizeof(TEntry) * static_cast<size_t>(_entryIndex));

        TEntry& entry = Entries()[static_cast<size_t>(_entryIndex)];

        performanceEntry.StartTimeOffset = baseEntryOffset + static_cast<uint32_t>(entry.GetStartTimeOffset());
        performanceEntry.ProcessingTimeOffset = baseEntryOffset + static_cast<uint32_t>(entry.GetProcessingTimeOffset());

        entry = TEntry{};
        entry.SetEntryType(entryType);
        entry.SetNodeId(nodeId);

        _entryIndex++;

        return true;
    }

    bool GetNextEntry(PerformanceEntryAddresses& performanceEntry, PerformanceDetailType detailType, PerformanceEntryType entryType,
                      int32_t nodeId) override {
        performanceEntry = PerformanceEntryAddresses{};

        if (_entryDetailIndex >= MaxFrameDetailCount) {
            return false;
        }

        performanceEntry.BaseMemory = CurrentBuffer().template Cast<int32_t>();
        performanceEntry.EntryCountOffset = static_cast<uint32_t>(CurrentHeader().GetEntryCountOffset());

        uint32_t baseEntryOffset = static_cast<uint32_t>(sizeof(THeader) + static_cast<size_t>(GetEntriesSize()) +
                                                         sizeof(TEntryDetail) * static_cast<size_t>(_entryDetailIndex));

        TEntryDetail& entryDetail = EntriesDetail()[static_cast<size_t>(_entryDetailIndex)];

        performanceEntry.StartTimeOffset = baseEntryOffset + static_cast<uint32_t>(entryDetail.GetStartTimeOffset());
        performanceEntry.ProcessingTimeOffset = baseEntryOffset + static_cast<uint32_t>(entryDetail.GetProcessingTimeOffset());

        entryDetail = TEntryDetail{};
        entryDetail.SetDetailType(detailType);
        entryDetail.SetEntryType(entryType);
        entryDetail.SetNodeId(nodeId);

        _entryDetailIndex++;

        return true;
    }

    bool IsTargetNodeId(int32_t target) override { return _detailTarget == target; }

    void SetTargetNodeId(int32_t target) override { _detailTarget = target; }

    void TapFrame(bool dspRunningBehind, uint32_t voiceDropCount, uint64_t startRenderingTicks) override {
        if (_availableFrameCount > 0) {
            int32_t targetIndexForHistory = _indexHistoryWrite;

            _indexHistoryWrite = (_indexHistoryWrite + 1) % _availableFrameCount;

            THeader& targetHeader = GetHeaderFromBuffer(_historyBuffer, targetIndexForHistory);

            CurrentBuffer().CopyTo(GetBufferFromIndex(_historyBuffer, targetIndexForHistory));

            uint32_t targetHistoryFrameIndex = _historyFrameIndex;

            if (_historyFrameIndex == UINT32_MAX) {
                _historyFrameIndex = 0;
            } else {
                _historyFrameIndex++;
            }

            targetHeader.SetDspRunningBehind(dspRunningBehind);
            targetHeader.SetVoiceDropCount(voiceDropCount);
            targetHeader.SetStartRenderingTicks(startRenderingTicks);
            targetHeader.SetIndex(targetHistoryFrameIndex);

            // Finally setup the new header
            SetupNewHeader();
        }
    }
};

// ---- PerformanceManager statics (declared in PerformanceManager.h), defined here because they need the generic type ----

template <class TBehaviourInfo>
uint64_t PerformanceManager::GetRequiredBufferSizeForPerformanceMetricsPerFrame(AudioRendererConfiguration& parameter,
                                                                               const TBehaviourInfo& behaviourInfo) {
    uint32_t version = behaviourInfo.GetPerformanceMetricsDataFormat();

    if (version == 2) {
        return static_cast<uint64_t>(PerformanceManagerGeneric<PerformanceFrameHeaderVersion2, PerformanceEntryVersion2,
                                                               PerformanceDetailVersion2>::GetRequiredBufferSizeForPerformanceMetricsPerFrame(parameter));
    }

    if (version == 1) {
        return static_cast<uint64_t>(PerformanceManagerGeneric<PerformanceFrameHeaderVersion1, PerformanceEntryVersion1,
                                                               PerformanceDetailVersion1>::GetRequiredBufferSizeForPerformanceMetricsPerFrame(parameter));
    }

    NXAUDREN_FATAL("Unknown Performance metrics data format version");
}

template <class TBehaviourInfo>
std::unique_ptr<PerformanceManager> PerformanceManager::Create(Span<uint8_t> performanceBuffer, AudioRendererConfiguration& parameter,
                                                               const TBehaviourInfo& behaviourInfo) {
    uint32_t version = behaviourInfo.GetPerformanceMetricsDataFormat();

    switch (version) {
        case 1:
            return std::make_unique<PerformanceManagerGeneric<PerformanceFrameHeaderVersion1, PerformanceEntryVersion1, PerformanceDetailVersion1>>(
                performanceBuffer, parameter);
        case 2:
            return std::make_unique<PerformanceManagerGeneric<PerformanceFrameHeaderVersion2, PerformanceEntryVersion2, PerformanceDetailVersion2>>(
                performanceBuffer, parameter);
        default:
            NXAUDREN_FATAL("Unknown Performance metrics data format version");
    }
}

}  // namespace nxrt::audren
