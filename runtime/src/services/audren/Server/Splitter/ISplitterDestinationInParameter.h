// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Parameter/ISplitterDestinationInParameter.cs (Ryubing fork, MIT).
// The C# interface lists the accessors that every splitter destination parameter provides. The C++ parameter structs in audren_types.h
// keep their snake_case field names, so the interface is a traits template with one specialization per struct. The accessor names are the
// C# property names. SplitterDestinationVersion1/2 use it through the Update templates.
#pragma once
#include <array>

#include "../../Constants.h"
#include "../../Dsp/BiquadFilterHelper.h"
#include "../../audren_common.h"
#include "../../audren_types.h"

namespace nxrt::audren {

// No definition for other types: only the three splitter destination parameter structs are valid.
template <class TParameter>
struct ISplitterDestinationInParameter;

template <>
struct ISplitterDestinationInParameter<SplitterDestinationInParameterVersion1> {
    static int32_t Id(const SplitterDestinationInParameterVersion1& p) { return p.id; }
    static int32_t DestinationId(const SplitterDestinationInParameterVersion1& p) { return p.destination_id; }
    static Span<const float> MixBufferVolume(const SplitterDestinationInParameterVersion1& p) {
        return Span<const float>(p.mix_buffer_volume, static_cast<size_t>(Constants::MixBufferCountMax));
    }
    static bool IsUsed(const SplitterDestinationInParameterVersion1& p) { return p.is_used; }
    static bool ResetPrevVolume(const SplitterDestinationInParameterVersion1& p) { return p.reset_prev_volume; }
    static bool IsMagicValid(const SplitterDestinationInParameterVersion1& p) { return p.IsMagicValid(); }
    // Version 1 has no biquad filters. The accessor exists so that the version 2 wrapper path can name it for every parameter type; the
    // wrapper only calls the version 2 Update when the destination is a version 2 one, so this is never used for a version 1 destination.
    static std::array<BiquadFilterParameter2, 2> BiquadFilters2(const SplitterDestinationInParameterVersion1&) { return {}; }
};

template <>
struct ISplitterDestinationInParameter<SplitterDestinationInParameterVersion2a> {
    static int32_t Id(const SplitterDestinationInParameterVersion2a& p) { return p.id; }
    static int32_t DestinationId(const SplitterDestinationInParameterVersion2a& p) { return p.destination_id; }
    static Span<const float> MixBufferVolume(const SplitterDestinationInParameterVersion2a& p) {
        return Span<const float>(p.mix_buffer_volume, static_cast<size_t>(Constants::MixBufferCountMax));
    }
    static bool IsUsed(const SplitterDestinationInParameterVersion2a& p) { return p.is_used; }
    static bool ResetPrevVolume(const SplitterDestinationInParameterVersion2a& p) { return p.reset_prev_volume; }
    static bool IsMagicValid(const SplitterDestinationInParameterVersion2a& p) { return p.IsMagicValid(); }
    // C# converts the version 1 style filters (BiquadFilterParameter1) to the version 2 ones.
    static std::array<BiquadFilterParameter2, 2> BiquadFilters2(const SplitterDestinationInParameterVersion2a& p) {
        return {BiquadFilterHelper::ToBiquadFilterParameter2(p.biquad_filters[0]),
                BiquadFilterHelper::ToBiquadFilterParameter2(p.biquad_filters[1])};
    }
};

template <>
struct ISplitterDestinationInParameter<SplitterDestinationInParameterVersion2b> {
    static int32_t Id(const SplitterDestinationInParameterVersion2b& p) { return p.id; }
    static int32_t DestinationId(const SplitterDestinationInParameterVersion2b& p) { return p.destination_id; }
    static Span<const float> MixBufferVolume(const SplitterDestinationInParameterVersion2b& p) {
        return Span<const float>(p.mix_buffer_volume, static_cast<size_t>(Constants::MixBufferCountMax));
    }
    static bool IsUsed(const SplitterDestinationInParameterVersion2b& p) { return p.is_used; }
    static bool ResetPrevVolume(const SplitterDestinationInParameterVersion2b& p) { return p.reset_prev_volume; }
    static bool IsMagicValid(const SplitterDestinationInParameterVersion2b& p) { return p.IsMagicValid(); }
    static std::array<BiquadFilterParameter2, 2> BiquadFilters2(const SplitterDestinationInParameterVersion2b& p) {
        return {p.biquad_filters[0], p.biquad_filters[1]};
    }
};

}  // namespace nxrt::audren
