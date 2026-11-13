// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Performance/IPerformanceHeader.cs (Ryubing fork, MIT).
// The C# interface becomes a C++20 concept (see IPerformanceDetailEntry.h for the reasoning).
#pragma once
#include <concepts>
#include <type_traits>

#include "../../audren_common.h"

namespace nxrt::audren {

template <class T>
concept IPerformanceHeader = std::is_trivially_copyable_v<T> && requires(T& t, const T& ct, bool flag, uint32_t u32, uint64_t u64, int32_t i32) {
    // Get the entry count offset in this structure.
    { ct.GetEntryCountOffset() } -> std::same_as<int32_t>;
    // Set the DSP running behind flag.
    t.SetDspRunningBehind(flag);
    // Set the count of voices that were dropped.
    t.SetVoiceDropCount(u32);
    // Set the start ticks of the audio processor (before sending commands).
    t.SetStartRenderingTicks(u64);
    // Set the header magic.
    t.SetMagic(u32);
    // Set the offset of the next performance header.
    t.SetNextOffset(i32);
    // Set the total time taken by all the commands profiled.
    t.SetTotalProcessingTime(i32);
    // Set the index of this performance frame.
    t.SetIndex(u32);
    // Get the total count of entries in this frame.
    { ct.GetEntryCount() } -> std::same_as<int32_t>;
    // Get the total count of detailed entries in this frame.
    { ct.GetEntryDetailCount() } -> std::same_as<int32_t>;
    // Set the total count of entries in this frame.
    t.SetEntryCount(i32);
    // Set the total count of detailed entries in this frame.
    t.SetEntryDetailCount(i32);
};

}  // namespace nxrt::audren
