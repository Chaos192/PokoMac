// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterDestinationVersion2.cs (Ryubing fork, MIT).
// Server state of one splitter destination, used when the revision has biquad filters on splitters (revision 12 and later).
// Deviations: the C# Array2<T> fields are plain C arrays of 2 elements. Next() returns a pointer (null for no next element).
#pragma once
#include <cstddef>
#include <cstdint>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "ISplitterDestinationInParameter.h"

namespace nxrt::audren {

// C# StructLayout Size 0x128, Pack 0x10. Not alignas(16): 0x128 is not a multiple of 16. The reserved tail below makes sizeof 0x128, so the
// work buffer size (sizeof based) matches the C# GetWorkBufferSize.
class SplitterDestinationVersion2 {
  public:
    // C# public const int Alignment = 0x10.
    static constexpr int32_t Alignment = 0x10;

    // The unique id of this splitter destination.
    int32_t Id = 0;
    // The mix to output the result of the splitter.
    int32_t DestinationId = 0;
    // Set to true if in use.
    bool IsUsed = false;
    // Set to true if the internal state need to be updated.
    bool NeedToUpdateInternalState = false;

    SplitterDestinationVersion2() = default;

    // Create a new SplitterDestinationVersion2.
    explicit SplitterDestinationVersion2(int32_t id) : Id(id) {
        DestinationId = Constants::UnusedMixId;
        ClearVolumes();
    }

    // Mix buffer volumes. Used when a splitter id is specified in the mix.
    Span<float> MixBufferVolume() { return Span<float>(_mix, kMixBufferCount); }
    Span<const float> MixBufferVolume() const { return Span<const float>(_mix, kMixBufferCount); }

    // Previous mix buffer volumes. Used when a splitter id is specified in the mix.
    Span<float> PreviousMixBufferVolume() { return Span<float>(_previousMix, kMixBufferCount); }
    Span<const float> PreviousMixBufferVolume() const { return Span<const float>(_previousMix, kMixBufferCount); }

    // C# Next: the next element, or null when there is none.
    SplitterDestinationVersion2* Next() const { return _next; }

    // Update the SplitterDestinationVersion2 from user parameter.
    // isPrevVolumeResetSupported: the revision supports explicitly resetting the volume.
    template <class T>
    void Update(const T& parameter, bool isPrevVolumeResetSupported) {
        using P = ISplitterDestinationInParameter<T>;

        NXAUDREN_ASSERT(Id == P::Id(parameter));

        if (P::IsMagicValid(parameter) && Id == P::Id(parameter)) {
            DestinationId = P::DestinationId(parameter);

            P::MixBufferVolume(parameter).CopyTo(MixBufferVolume());

            auto filters = P::BiquadFilters2(parameter);
            _biquadFilters[0] = filters[0];
            _biquadFilters[1] = filters[1];

            bool resetPrevVolume = isPrevVolumeResetSupported ? P::ResetPrevVolume(parameter) : (!IsUsed && P::IsUsed(parameter));
            if (resetPrevVolume) {
                MixBufferVolume().CopyTo(PreviousMixBufferVolume());

                NeedToUpdateInternalState = false;
            }

            IsUsed = P::IsUsed(parameter);
        }
    }

    // Update the internal state of the instance.
    void UpdateInternalState() {
        if (IsUsed && NeedToUpdateInternalState) {
            MixBufferVolume().CopyTo(PreviousMixBufferVolume());
        }

        NeedToUpdateInternalState = false;
    }

    // Set the update internal state marker.
    void MarkAsNeedToUpdateInternalState() { NeedToUpdateInternalState = true; }

    // Return true if the SplitterDestinationVersion2 is used and has a destination.
    bool IsConfigured() const { return IsUsed && DestinationId != Constants::UnusedMixId; }

    // Get the volume for a given destination.
    float GetMixVolume(int32_t destinationIndex) const {
        NXAUDREN_ASSERT(destinationIndex >= 0 && destinationIndex < Constants::MixBufferCountMax);

        return MixBufferVolume()[static_cast<size_t>(destinationIndex)];
    }

    // Get the previous volume for a given destination.
    float GetMixVolumePrev(int32_t destinationIndex) const {
        NXAUDREN_ASSERT(destinationIndex >= 0 && destinationIndex < Constants::MixBufferCountMax);

        return PreviousMixBufferVolume()[static_cast<size_t>(destinationIndex)];
    }

    // Clear the volumes.
    void ClearVolumes() {
        MixBufferVolume().Clear();
        PreviousMixBufferVolume().Clear();
    }

    // Link the next element to the given SplitterDestinationVersion2.
    void Link(SplitterDestinationVersion2& next) { _next = &next; }

    // Remove the link to the next element.
    void Unlink() { _next = nullptr; }

    // Checks if any biquad filter is enabled.
    bool IsBiquadFilterEnabled() const { return _biquadFilters[0].enable || _biquadFilters[1].enable; }

    // Checks if any biquad filter was previously enabled.
    bool IsBiquadFilterEnabledPrev() const { return _isPreviousBiquadFilterEnabled[0]; }

    // Gets the biquad filter parameters. index: biquad filter index (0 or 1).
    BiquadFilterParameter2& GetBiquadFilterParameter(int32_t index) {
        NXAUDREN_ASSERT(index >= 0 && index < 2);

        return _biquadFilters[index];
    }

    // Remember whether the biquad filter at index was enabled. index: biquad filter index (0 or 1).
    void UpdateBiquadFilterEnabledPrev(int32_t index) {
        NXAUDREN_ASSERT(index >= 0 && index < 2);

        _isPreviousBiquadFilterEnabled[index] = _biquadFilters[index].enable;
    }

  private:
    static constexpr size_t kMixBufferCount = static_cast<size_t>(Constants::MixBufferCountMax);

    // Mix buffer volumes storage.
    float _mix[kMixBufferCount] = {};
    float _previousMix[kMixBufferCount] = {};
    // Pointer to the next linked element.
    SplitterDestinationVersion2* _next = nullptr;
    // Biquad filter parameters (C# Array2<BiquadFilterParameter2>).
    BiquadFilterParameter2 _biquadFilters[2] = {};
    // Previous enable state of the biquad filters (C# Array2<bool>).
    bool _isPreviousBiquadFilterEnabled[2] = {};
    // Unused tail of the C# struct (the C# Size is 0x128). The fields above end at offset 266 (public Id, DestinationId, IsUsed and
    // NeedToUpdateInternalState come first, then the 4-byte aligned volumes and the 8-byte next pointer).
    [[maybe_unused]] uint8_t _reservedToCSize[0x128 - 266] = {};
};

static_assert(sizeof(SplitterDestinationVersion2) == 0x128);

}  // namespace nxrt::audren
