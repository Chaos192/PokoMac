// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceFrameHeaderVersion1.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x18)]: the fields fill the whole 0x18 bytes, no padding.
#pragma once
#include "IPerformanceHeader.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceFrameHeaderVersion1 {
    // The magic of the performance header.
    uint32_t Magic;
    // The total count of entries in this frame.
    int32_t EntryCount;
    // The total count of detailed entries in this frame.
    int32_t EntryDetailCount;
    // The offset of the next performance header.
    int32_t NextOffset;
    // The total time taken by all the commands profiled.
    int32_t TotalProcessingTime;
    // The count of voices that were dropped.
    uint32_t VoiceDropCount;

    int32_t GetEntryCount() const { return EntryCount; }
    int32_t GetEntryCountOffset() const { return 4; }
    int32_t GetEntryDetailCount() const { return EntryDetailCount; }
    void SetDspRunningBehind(bool /*isRunningBehind*/) {
        // NOTE: Not present in version 1
    }
    void SetEntryCount(int32_t entryCount) { EntryCount = entryCount; }
    void SetEntryDetailCount(int32_t entryDetailCount) { EntryDetailCount = entryDetailCount; }
    void SetIndex(uint32_t /*index*/) {
        // NOTE: Not present in version 1
    }
    void SetMagic(uint32_t magic) { Magic = magic; }
    void SetNextOffset(int32_t nextOffset) { NextOffset = nextOffset; }
    void SetStartRenderingTicks(uint64_t /*startTicks*/) {
        // NOTE: not present in version 1
    }
    void SetTotalProcessingTime(int32_t totalProcessingTime) { TotalProcessingTime = totalProcessingTime; }
    void SetVoiceDropCount(uint32_t voiceCount) { VoiceDropCount = voiceCount; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceFrameHeaderVersion1) == 0x18);
static_assert(IPerformanceHeader<PerformanceFrameHeaderVersion1>);

}  // namespace nxrt::audren
