// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceDetailVersion1.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x10)]: the fields take 14 bytes, the declared size adds 2 bytes of trailing padding.
#pragma once
#include "IPerformanceDetailEntry.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceDetailVersion1 {
    // The node id associated to this detailed entry.
    int32_t NodeId;
    // The start time (in microseconds) associated to this detailed entry.
    int32_t StartTime;
    // The processing time (in microseconds) associated to this detailed entry.
    int32_t ProcessingTime;
    // The detailed entry type associated to this detailed entry.
    PerformanceDetailType DetailType;
    // The entry type associated to this detailed entry.
    PerformanceEntryType EntryType;
    // Trailing padding up to the C# Size (0x10).
    uint8_t Padding[2];

    int32_t GetProcessingTime() const { return ProcessingTime; }
    int32_t GetProcessingTimeOffset() const { return 8; }
    int32_t GetStartTime() const { return StartTime; }
    int32_t GetStartTimeOffset() const { return 4; }
    void SetDetailType(PerformanceDetailType detailType) { DetailType = detailType; }
    void SetEntryType(PerformanceEntryType type) { EntryType = type; }
    void SetNodeId(int32_t nodeId) { NodeId = nodeId; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceDetailVersion1) == 0x10);
static_assert(IPerformanceDetailEntry<PerformanceDetailVersion1>);

}  // namespace nxrt::audren
