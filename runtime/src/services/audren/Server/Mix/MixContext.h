// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Mix/MixContext.cs (Ryubing fork, MIT).
// Owns the mixes, their sorted order, the mix graph (EdgeMatrix) and its node states, and computes the mix buffer offsets.
// Deviations:
//   * The members NodeStates and EdgeMatrix have the same names as their types. They are declared with the qualified type name, so the
//     member does not change the meaning of the type name inside the class.
//   * The C# Memory<T> members are Span<T> views of the work buffer.
//   * Sort() (without splitters) uses ArraySortHelper, a literal port of the .NET introsort used by Array.Sort, so the order of equal keys
//     is the same as in C#.
#pragma once
#include <cstdint>
#include <vector>

#include "../../Common/EdgeMatrix.h"
#include "../../Common/NodeStates.h"
#include "../../Constants.h"
#include "../../audren_common.h"
#include "../Splitter/SplitterContext.h"
#include "ArraySortHelper.h"
#include "MixInfo.h"

namespace nxrt::audren {

class MixContext {
  public:
    // Graph state (C# public NodeStates NodeStates { get; }).
    nxrt::audren::NodeStates NodeStates;

    // The instance of the adjacent matrix (C# public EdgeMatrix EdgeMatrix { get; }).
    nxrt::audren::EdgeMatrix EdgeMatrix;

    MixContext() = default;

    // Initialize the MixContext. The node states and the edge matrix are only initialized when both work buffers are given.
    void Initialize(Span<int32_t> sortedMixes, Span<MixInfo> mixes, Span<uint8_t> nodeStatesWorkBuffer, Span<uint8_t> edgeMatrixWorkBuffer) {
        _mixesCount = static_cast<uint32_t>(mixes.Length);
        _mixes = mixes;
        _sortedMixes = sortedMixes;

        if (!nodeStatesWorkBuffer.IsEmpty() && !edgeMatrixWorkBuffer.IsEmpty()) {
            NodeStates.Initialize(nodeStatesWorkBuffer, static_cast<int32_t>(mixes.Length));
            EdgeMatrix.Initialize(edgeMatrixWorkBuffer, static_cast<int32_t>(mixes.Length));
        }

        int32_t sortedId = 0;
        for (int32_t i = 0; i < static_cast<int32_t>(_mixes.Length); i++) {
            SetSortedState(sortedId++, i);
        }
    }

    // Get a reference to the final MixInfo.
    MixInfo& GetFinalState() { return GetState(Constants::FinalMixId); }

    // Get a reference to a MixInfo at the given id.
    MixInfo& GetState(int32_t id) { return SpanIOHelper<uint8_t>::GetFromMemory(_mixes, id, _mixesCount); }

    // Get a reference to a MixInfo at the given id of the sorted mix info.
    MixInfo& GetSortedState(int32_t id) {
        NXAUDREN_ASSERT(id >= 0 && static_cast<uint32_t>(id) < _mixesCount);

        return GetState(_sortedMixes[static_cast<size_t>(id)]);
    }

    // Get the total mix count.
    uint32_t GetCount() const { return _mixesCount; }

    // Sort the mixes using distance from the final mix (C# Sort()).
    void Sort() {
        UpdateDistancesFromFinalMix();

        uint32_t count = GetCount();
        std::vector<int32_t> sortedMixesTemp(_sortedMixes.Data, _sortedMixes.Data + count);

        ArraySortHelper::IntrospectiveSort(Span<int32_t>(sortedMixesTemp), [this](int32_t a, int32_t b) {
            MixInfo& infoA = GetState(a);
            MixInfo& infoB = GetState(b);

            return CompareUInt32(infoB.DistanceFromFinalMix, infoA.DistanceFromFinalMix);
        });

        Span<int32_t>(sortedMixesTemp).CopyTo(_sortedMixes);

        UpdateMixBufferOffset();
    }

    // Sort the mixes and splitters using the adjacency matrix. Returns true if no errors in the graph were detected.
    bool Sort(SplitterContext& splitterContext) {
        if (splitterContext.UsingSplitter()) {
            bool isValid = NodeStates.Sort(EdgeMatrix);

            if (isValid) {
                Span<const int32_t> sortedMixesIndex = NodeStates.GetTsortResult();

                int32_t id = 0;

                for (int32_t i = static_cast<int32_t>(sortedMixesIndex.Length) - 1; i >= 0; i--) {
                    SetSortedState(id++, sortedMixesIndex[static_cast<size_t>(i)]);
                }

                UpdateMixBufferOffset();
            }

            return isValid;
        }

        UpdateMixBufferOffset();

        return true;
    }

  private:
    // Associate the given targetIndex to a given sorted id.
    void SetSortedState(int32_t id, int32_t targetIndex) { _sortedMixes[static_cast<size_t>(id)] = targetIndex; }

    // C# int.CompareTo / uint.CompareTo: -1, 0 or 1.
    static int32_t CompareUInt32(uint32_t lhs, uint32_t rhs) { return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0); }

    // Update the distance from the final mix of every MixInfo (C# UpdateDistancesFromFinalMix).
    void UpdateDistancesFromFinalMix() {
        for (MixInfo& mix : _mixes) {
            mix.ClearDistanceFromFinalMix();
        }

        for (uint32_t i = 0; i < GetCount(); i++) {
            MixInfo& mix = GetState(static_cast<int32_t>(i));

            SetSortedState(static_cast<int32_t>(i), static_cast<int32_t>(i));

            if (mix.IsUsed) {
                uint32_t distance = 0;

                if (mix.MixId != Constants::FinalMixId) {
                    int32_t mixId = mix.MixId;

                    for (distance = 0; distance < GetCount(); distance++) {
                        if (mixId == Constants::UnusedMixId) {
                            distance = MixInfo::InvalidDistanceFromFinalMix;
                            break;
                        }

                        MixInfo& distanceMix = GetState(mixId);

                        if (distanceMix.DistanceFromFinalMix != MixInfo::InvalidDistanceFromFinalMix) {
                            distance = distanceMix.DistanceFromFinalMix + 1;
                            break;
                        }

                        mixId = distanceMix.DestinationMixId;

                        if (mixId == Constants::FinalMixId) {
                            break;
                        }
                    }

                    if (distance > GetCount()) {
                        distance = MixInfo::InvalidDistanceFromFinalMix;
                    }
                } else {
                    distance = MixInfo::InvalidDistanceFromFinalMix;
                }

                mix.DistanceFromFinalMix = distance;
            }
        }
    }

    // Update the buffer offset of all MixInfo, in index order (C# UpdateMixBufferOffset).
    void UpdateMixBufferOffset() {
        uint32_t offset = 0;

        for (MixInfo& mix : _mixes) {
            mix.BufferOffset = offset;

            offset += mix.BufferCount;
        }
    }

    // The total mix count.
    uint32_t _mixesCount = 0;
    // Storage for MixInfo.
    Span<MixInfo> _mixes;
    // Storage of the sorted indices to MixInfo.
    Span<int32_t> _sortedMixes;
};

}  // namespace nxrt::audren
