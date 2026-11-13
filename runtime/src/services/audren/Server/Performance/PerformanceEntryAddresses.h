// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceEntryAddresses.cs (Ryubing fork, MIT). Information used by the performance
// command to store information in the performance entry. The C# object pool is dropped (the generator allocates one per use).
#pragma once
#include "../../audren_common.h"

namespace nxrt::audren {

class PerformanceEntryAddresses {
  public:
    Span<int32_t> BaseMemory;   // the memory storing the performance entry
    uint32_t StartTimeOffset = 0;
    uint32_t EntryCountOffset = 0;
    uint32_t ProcessingTimeOffset = 0;

    void IncrementEntryCount() { BaseMemory[static_cast<int32_t>(EntryCountOffset) / 4]++; }
    void SetStartTime(uint64_t startTimeNano) { BaseMemory[static_cast<int32_t>(StartTimeOffset) / 4] = static_cast<int32_t>(startTimeNano / 1000); }
    void SetProcessingTime(uint64_t endTimeNano) {
        BaseMemory[static_cast<int32_t>(ProcessingTimeOffset) / 4] =
            static_cast<int32_t>(endTimeNano / 1000) - BaseMemory[static_cast<int32_t>(StartTimeOffset) / 4];
    }
    void Clear() {}
};

}  // namespace nxrt::audren
