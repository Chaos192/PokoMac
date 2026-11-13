// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Shared support for the Server/Effect ports (S3c). It holds what the C# keeps on the parameter structs and interfaces
// (IEffectInParameter, IEffectOutStatus, the IsChannelCount* members of the Parameter/Effect structs, the Statistics.Reset
// methods, MemoryMarshal.Cast of SpecificData) and two runtime helpers with no C++ counterpart yet (the DSP cache
// invalidation and the default-constructed state slot). Nothing here is in the C# Effect folder itself.
#pragma once
#include <concepts>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "../../audren_common.h"
#include "../../audren_types.h"

namespace nxrt::audren {

// C# interface IEffectInParameter. The C++ parameter structs keep their lower_snake_case field names (audren_common.h,
// exception for audren_types.h), so the requirements name those fields: type, is_new, is_enabled, mix_id, buffer_base,
// buffer_size, processing_order and specific_data.
template <class T>
concept IEffectInParameter = requires(const T& p) {
    { p.type } -> std::convertible_to<EffectType>;
    { p.is_new } -> std::convertible_to<bool>;
    { p.is_enabled } -> std::convertible_to<bool>;
    { p.mix_id } -> std::convertible_to<int32_t>;
    { p.buffer_base } -> std::convertible_to<uint64_t>;
    { p.buffer_size } -> std::convertible_to<uint64_t>;
    { p.processing_order } -> std::convertible_to<uint32_t>;
    p.specific_data;
};

// C# interface IEffectOutStatus (State is the only member it requires).
template <class T>
concept IEffectOutStatus = requires(T& s) {
    s.state;
};

// The parameter structs that carry channel_count and channel_count_max (ReverbParameter, DelayParameter, LimiterParameter,
// CompressorParameter, Reverb3dParameter).
template <class T>
concept HasChannelCounts = requires(const T& p) {
    p.channel_count;
    p.channel_count_max;
};

struct EffectParameterHelpers {
    // C# EffectInParameterVersion1.IsChannelCountValid(int): 1, 2, 4 or 6 channels.
    static bool IsChannelCountValid(int32_t channelCount) {
        return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6;
    }

    // C# instance member IsChannelCountValid() of the parameter structs.
    template <class TParam>
        requires HasChannelCounts<TParam>
    static bool IsChannelCountValid(const TParam& parameter) {
        return IsChannelCountValid(static_cast<int32_t>(parameter.channel_count));
    }

    // C# instance member IsChannelCountMaxValid() of the parameter structs.
    template <class TParam>
        requires HasChannelCounts<TParam>
    static bool IsChannelCountMaxValid(const TParam& parameter) {
        return IsChannelCountValid(static_cast<int32_t>(parameter.channel_count_max));
    }

    // C# LimiterStatistics.Reset(): input maxima to 0, compression gain minima to 1.
    static void Reset(LimiterStatistics& statistics) {
        for (float& value : statistics.input_max) {
            value = 0.0f;
        }
        for (float& value : statistics.compression_gain_min) {
            value = 1.0f;
        }
    }

    // C# CompressorStatistics.Reset(ushort channelCount): maximum mean 0, minimum gain 1, and the first channelCount last
    // samples cleared (C# `LastSamples.AsSpan()[..channelCount].Clear()` throws above 6, so that case is fatal here).
    static void Reset(CompressorStatistics& statistics, uint16_t channelCount) {
        if (channelCount > 6) {
            NXAUDREN_FATAL("CompressorStatistics.Reset: channelCount above the 6 last samples");
        }
        statistics.maximum_mean = 0.0f;
        statistics.minimum_gain = 1.0f;
        for (uint16_t i = 0; i < channelCount; i++) {
            statistics.last_samples[i] = 0.0f;
        }
    }

    // Read a T from the raw bytes at source (C# MemoryMarshal.Cast<byte, T>(span)[0]). Copies, so no alignment is assumed.
    template <class T>
    static T LoadBytes(const uint8_t* source) {
        T value{};
        std::memcpy(&value, source, sizeof(T));
        return value;
    }

    // Write a T to the raw bytes at destination.
    template <class T>
    static void StoreBytes(uint8_t* destination, const T& value) {
        std::memcpy(destination, &value, sizeof(T));
    }

    // The effect-specific parameter of an input (C# SpecificData reinterpreted as T). The area is 0xA0 bytes in both versions.
    template <class T, IEffectInParameter TIn>
    static T SpecificDataAs(const TIn& parameter) {
        static_assert(sizeof(T) <= 0xA0, "effect parameter larger than EffectInParameterVersion1.specific_data");
        return LoadBytes<T>(parameter.specific_data);
    }

    // C# AudioProcessorMemoryManager.InvalidateDataCache. Guest memory is host memory and the DSP shares the host caches,
    // so there is nothing to flush or invalidate in this port.
    static void InvalidateDataCache(uint64_t address, uint64_t size) {
        (void)address;
        (void)size;
    }
};

// Storage for one DSP state object, the C++ stand-in for the C# `new T[1]` of the Effect classes. The DSP state types
// (Dsp/State/*.h) only have constructors that take a parameter, so the slot starts as zeroed bytes (the C# default value)
// and the effect command constructs the state in it before first use (ReplaceState, the C#'s `state = new ...`). The slot
// destroys whatever it holds when the owning effect is destroyed. The owning effect must not be copied or moved, because
// the Span view points into this object.
template <class T>
class EffectStateSlot {
  public:
    EffectStateSlot() { std::memset(_storage, 0, sizeof(_storage)); }
    ~EffectStateSlot() { Object()->~T(); }
    EffectStateSlot(const EffectStateSlot&) = delete;
    EffectStateSlot& operator=(const EffectStateSlot&) = delete;

    // The one-element view (C# Memory<T> of length 1).
    Span<T> View() { return Span<T>(Object(), 1); }

  private:
    T* Object() { return std::launder(reinterpret_cast<T*>(_storage)); }

    alignas(T) unsigned char _storage[sizeof(T)];
};

}  // namespace nxrt::audren
