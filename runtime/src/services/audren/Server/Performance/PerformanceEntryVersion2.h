// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceEntryVersion2.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x18)]: the fields take 13 bytes, the declared size adds 11 bytes of trailing padding.
#pragma once
#include "IPerformanceEntry.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceEntryVersion2 {
    // The node id associated to this entry.
    int32_t NodeId;
    // The start time (in microseconds) associated to this entry.
    int32_t StartTime;
    // The processing time (in microseconds) associated to this entry.
    int32_t ProcessingTime;
    // The entry type associated to this entry.
    PerformanceEntryType EntryType;
    // Trailing padding up to the C# Size (0x18).
    uint8_t Padding[11];

    int32_t GetProcessingTime() const { return ProcessingTime; }
    int32_t GetProcessingTimeOffset() const { return 8; }
    int32_t GetStartTime() const { return StartTime; }
    int32_t GetStartTimeOffset() const { return 4; }
    void SetEntryType(PerformanceEntryType type) { EntryType = type; }
    void SetNodeId(int32_t nodeId) { NodeId = nodeId; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceEntryVersion2) == 0x18);
static_assert(IPerformanceEntry<PerformanceEntryVersion2>);

}  // namespace nxrt::audren
