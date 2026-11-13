// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceFrameHeaderVersion2.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x30)]: the fields take 37 bytes, the declared size adds 11 bytes of trailing padding.
#pragma once
#include "IPerformanceHeader.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceFrameHeaderVersion2 {
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
    // The start ticks of the audio processor (before sending commands).
    uint64_t StartRenderingTicks;
    // The index of this performance frame.
    uint32_t Index;
    // If set to true, the DSP is running behind.
    bool IsDspRunningBehind;
    // Trailing padding up to the C# Size (0x30).
    uint8_t Padding[11];

    int32_t GetEntryCount() const { return EntryCount; }
    int32_t GetEntryCountOffset() const { return 4; }
    int32_t GetEntryDetailCount() const { return EntryDetailCount; }
    void SetDspRunningBehind(bool isRunningBehind) { IsDspRunningBehind = isRunningBehind; }
    void SetEntryCount(int32_t entryCount) { EntryCount = entryCount; }
    void SetEntryDetailCount(int32_t entryDetailCount) { EntryDetailCount = entryDetailCount; }
    void SetIndex(uint32_t index) { Index = index; }
    void SetMagic(uint32_t magic) { Magic = magic; }
    void SetNextOffset(int32_t nextOffset) { NextOffset = nextOffset; }
    void SetStartRenderingTicks(uint64_t startTicks) { StartRenderingTicks = startTicks; }
    void SetTotalProcessingTime(int32_t totalProcessingTime) { TotalProcessingTime = totalProcessingTime; }
    void SetVoiceDropCount(uint32_t voiceCount) { VoiceDropCount = voiceCount; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceFrameHeaderVersion2) == 0x30);
static_assert(IPerformanceHeader<PerformanceFrameHeaderVersion2>);

}  // namespace nxrt::audren
