// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterState.cs (Ryubing fork, MIT).
// Server state of one splitter: its sample rate and the head of a linked list of splitter destinations.
// Deviations: the C# Destination property is a method, and the head pointers are C++ pointers (C# unsafe pointers).
// Update() is declared here and defined at the end of SplitterContext.h, because it needs the full SplitterContext.
#pragma once
#include <algorithm>
#include <cstdint>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "SequenceReader.h"
#include "SplitterDestination.h"

namespace nxrt::audren {

class SplitterContext;

// C# StructLayout Size 0x20, Pack 0x10.
class alignas(0x10) SplitterState {
  public:
    // C# public const int Alignment = 0x10.
    static constexpr int32_t Alignment = 0x10;

    // The unique id of this SplitterState.
    int32_t Id = 0;
    // Target sample rate to use on the splitter.
    uint32_t SampleRate = 0;
    // Count of splitter destinations.
    int32_t DestinationCount = 0;
    // Set to true if the splitter has a new connection.
    bool HasNewConnection = false;

    SplitterState() = default;

    // Create a new SplitterState.
    explicit SplitterState(int32_t id) : Id(id) {}

    // First element of the linked list of splitter destinations data.
    SplitterDestination Destination() const { return SplitterDestination(_destinationDataV1, _destinationDataV2); }

    // The splitter destination at the given index of the list (or the last one reached, if the list is shorter).
    SplitterDestination GetData(int32_t index) const {
        int32_t i = 0;

        SplitterDestination result = Destination();

        while (i < index) {
            if (result.IsNull()) {
                break;
            }

            result = result.Next();
            i++;
        }

        return result;
    }

    // Clear the new connection flag.
    void ClearNewConnectionFlag() { HasNewConnection = false; }

    // Update the SplitterState from user parameter. Defined at the end of SplitterContext.h.
    void Update(SplitterContext& context, const SplitterInParameter& parameter, SequenceReader& input);

    // Set the head of the linked list of Destination.
    void SetDestination(SplitterDestination newValue) {
        _destinationDataV1 = newValue.GetV1RefOrNull();
        _destinationDataV2 = newValue.GetV2RefOrNull();
    }

    // Update the internal state of this instance.
    void UpdateInternalState() const {
        ForEachDestination([](SplitterDestination destination, int32_t) { destination.UpdateInternalState(); });
    }

    // Clear all links from the Destination.
    void ClearLinks() {
        ForEachDestination([](SplitterDestination destination, int32_t) { destination.Unlink(); });

        _destinationDataV1 = nullptr;
        _destinationDataV2 = nullptr;
    }

    // Initialize all the given SplitterState.
    static void InitializeSplitters(Span<SplitterState> splitters) {
        for (SplitterState& splitter : splitters) {
            splitter._destinationDataV1 = nullptr;
            splitter._destinationDataV2 = nullptr;

            splitter.DestinationCount = 0;
        }
    }

  private:
    // Utility function to apply an action to all Destination. The next element is read before the action, as in C#, because the action may
    // unlink the current element.
    template <class F>
    void ForEachDestination(F&& action) const {
        SplitterDestination temp = Destination();

        int32_t i = 0;

        while (true) {
            if (temp.IsNull()) {
                break;
            }

            SplitterDestination next = temp.Next();

            action(temp, i++);

            temp = next;
        }
    }

    // First element of the linked list of version 1 splitter destinations (C# _destinationDataV1).
    SplitterDestinationVersion1* _destinationDataV1 = nullptr;
    // First element of the linked list of version 2 splitter destinations (C# _destinationDataV2).
    SplitterDestinationVersion2* _destinationDataV2 = nullptr;
};

}  // namespace nxrt::audren
