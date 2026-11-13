// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterDestinationVersion1.cs (Ryubing fork, MIT).
// Server state of one splitter destination, used when the revision has no biquad filters on splitters (revision below 12).
// Deviation: Next() returns a pointer, null where the C# ref would be a null ref (no next element).
#pragma once
#include <cstddef>
#include <cstdint>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "ISplitterDestinationInParameter.h"

namespace nxrt::audren {

// C# StructLayout Size 0xE0, Pack 0x10.
class alignas(0x10) SplitterDestinationVersion1 {
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

    SplitterDestinationVersion1() = default;

    // Create a new SplitterDestinationVersion1.
    explicit SplitterDestinationVersion1(int32_t id) : Id(id) {
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
    SplitterDestinationVersion1* Next() const { return _next; }

    // Update the SplitterDestinationVersion1 from user parameter.
    // isPrevVolumeResetSupported: the revision supports explicitly resetting the volume.
    template <class T>
    void Update(const T& parameter, bool isPrevVolumeResetSupported) {
        using P = ISplitterDestinationInParameter<T>;

        NXAUDREN_ASSERT(Id == P::Id(parameter));

        if (P::IsMagicValid(parameter) && Id == P::Id(parameter)) {
            DestinationId = P::DestinationId(parameter);

            P::MixBufferVolume(parameter).CopyTo(MixBufferVolume());

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

    // Return true if the SplitterDestinationVersion1 is used and has a destination.
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

    // Link the next element to the given SplitterDestinationVersion1.
    void Link(SplitterDestinationVersion1& next) { _next = &next; }

    // Remove the link to the next element.
    void Unlink() { _next = nullptr; }

  private:
    static constexpr size_t kMixBufferCount = static_cast<size_t>(Constants::MixBufferCountMax);

    // Mix buffer volumes storage.
    float _mix[kMixBufferCount] = {};
    float _previousMix[kMixBufferCount] = {};
    // Pointer to the next linked element.
    SplitterDestinationVersion1* _next = nullptr;
};

static_assert(sizeof(SplitterDestinationVersion1) == 0xE0);

}  // namespace nxrt::audren
