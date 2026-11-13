// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/IPerformanceEntry.cs (Ryubing fork, MIT).
// The C# interface becomes a C++20 concept (see IPerformanceDetailEntry.h for the reasoning).
#pragma once
#include <concepts>
#include <type_traits>

#include "../../audren_common.h"
#include "../../audren_types.h"

namespace nxrt::audren {

template <class T>
concept IPerformanceEntry = std::is_trivially_copyable_v<T> && requires(T& t, const T& ct, int32_t value, PerformanceEntryType entryType) {
    // Get the start time of this entry event (in microseconds).
    { ct.GetStartTime() } -> std::same_as<int32_t>;
    // Get the start time offset in this structure.
    { ct.GetStartTimeOffset() } -> std::same_as<int32_t>;
    // Get the processing time of this entry event (in microseconds).
    { ct.GetProcessingTime() } -> std::same_as<int32_t>;
    // Get the processing time offset in this structure.
    { ct.GetProcessingTimeOffset() } -> std::same_as<int32_t>;
    // Set the node id of this entry.
    t.SetNodeId(value);
    // Set the PerformanceEntryType of this entry.
    t.SetEntryType(entryType);
};

}  // namespace nxrt::audren
