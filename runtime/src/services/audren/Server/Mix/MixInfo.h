// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Mix/MixInfo.cs (Ryubing fork, MIT).
// Server state of one mix: volumes, sample rate, buffer layout, the mix / splitter it outputs to, and the effect processing order.
// Deviations:
//   * C# properties with a body are methods: MixBufferVolume() (a non-const and a const overload), EffectProcessingOrderArray(),
//     GetMixBufferVolume, HasAnyDestination.
//   * The effect processing order storage is a raw pointer plus EffectProcessingOrderArrayMaxCount, as in C# (the storage is owned by the
//     work buffer).
//   * Update() is a template on the effect context type, because Server/Effect is not in this tree yet. The effect context must provide
//     GetCount() and GetEffect(i) returning an object with the members MixId and ProcessingOrder (C# BaseEffect).
//   * alignas(0x10) makes sizeof 0x940, the C# StructLayout Size (the work buffer size depends on it).
#pragma once
#include <cstdint>

#include "../../Common/EdgeMatrix.h"
#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BehaviourInfo.h"
#include "../Splitter/SplitterContext.h"

namespace nxrt::audren {

// C# StructLayout Size 0x940, Pack 0x10.
class alignas(0x10) MixInfo {
  public:
    // C# public const uint InvalidDistanceFromFinalMix = 0x80000000.
    static constexpr uint32_t InvalidDistanceFromFinalMix = 0x80000000u;

    // C# public const int Alignment = 0x10.
    static constexpr int32_t Alignment = 0x10;

    // Base volume of the mix.
    float Volume = 0.0f;
    // Target sample rate of the mix.
    uint32_t SampleRate = 0;
    // Target buffer count.
    uint32_t BufferCount = 0;
    // Set to true if in use.
    bool IsUsed = false;
    // The id of the mix.
    int32_t MixId = 0;
    // The mix node id.
    int32_t NodeId = 0;
    // The buffer offset to use for command generation.
    uint32_t BufferOffset = 0;
    // The distance of the mix from the final mix.
    uint32_t DistanceFromFinalMix = 0;
    // The max element count that can be found in the effect processing order storage.
    uint32_t EffectProcessingOrderArrayMaxCount = 0;
    // The mix to output the result of this mix.
    int32_t DestinationMixId = 0;
    // The splitter to output the result of this mix.
    uint32_t DestinationSplitterId = 0;
    // If set to true, the long size pre-delay is supported on the reverb command.
    bool IsLongSizePreDelaySupported = false;

    MixInfo() = default;

    // Create a new MixInfo. effectProcessingOrderArray comes from the work buffer.
    MixInfo(Span<int32_t> effectProcessingOrderArray, BehaviourInfo& behaviourInfo) {
        MixId = Constants::UnusedMixId;

        DistanceFromFinalMix = InvalidDistanceFromFinalMix;

        DestinationMixId = Constants::UnusedMixId;

        DestinationSplitterId = Constants::UnusedSplitterId;

        _effectProcessingOrderArrayPointer = effectProcessingOrderArray.Data;

        EffectProcessingOrderArrayMaxCount = static_cast<uint32_t>(effectProcessingOrderArray.Length);

        IsLongSizePreDelaySupported = behaviourInfo.IsLongSizePreDelaySupported();

        ClearEffectProcessingOrder();
    }

    // Mix buffer volumes. Used when no splitter id is specified.
    Span<float> MixBufferVolume() { return Span<float>(_mixVolumeArray, kMixVolumeCount); }
    Span<const float> MixBufferVolume() const { return Span<const float>(_mixVolumeArray, kMixVolumeCount); }

    // Get the volume for a given connection destination.
    float GetMixBufferVolume(int32_t sourceIndex, int32_t destinationIndex) const {
        return MixBufferVolume()[static_cast<size_t>(sourceIndex * Constants::MixBufferCountMax + destinationIndex)];
    }

    // The array used to order effects associated to this mix. Empty when there is no storage.
    Span<int32_t> EffectProcessingOrderArray() const {
        if (_effectProcessingOrderArrayPointer == nullptr) {
            return Span<int32_t>();
        }

        return Span<int32_t>(_effectProcessingOrderArrayPointer, static_cast<size_t>(EffectProcessingOrderArrayMaxCount));
    }

    // Clear the DistanceFromFinalMix value to its default state.
    void ClearDistanceFromFinalMix() { DistanceFromFinalMix = InvalidDistanceFromFinalMix; }

    // Clear the EffectProcessingOrderArray to its default state.
    void ClearEffectProcessingOrder() const { EffectProcessingOrderArray().Fill(-1); }

    // Return true if the mix has any destinations.
    bool HasAnyDestination() const { return DestinationMixId != Constants::UnusedMixId || DestinationSplitterId != Constants::UnusedSplitterId; }

    // Update the mix from user information.
    // edgeMatrix: the adjacency matrix. parameter: the input parameter of the mix. effectContext: the effect context (see the note above).
    // splitterContext: the splitter context. behaviourInfo: the behaviour context.
    // Returns true if the mix was changed (the connection changed, or on a revision without splitters the destination changed).
    template <class TEffectContext>
    bool Update(EdgeMatrix& edgeMatrix, const MixParameter& parameter, TEffectContext& effectContext, SplitterContext& splitterContext,
                BehaviourInfo& behaviourInfo) {
        bool isDirty = false;

        Volume = parameter.volume;
        SampleRate = parameter.sample_rate;
        BufferCount = parameter.buffer_count;
        IsUsed = parameter.is_used;
        MixId = parameter.mix_id;
        NodeId = parameter.node_id;
        Span<const float>(parameter.mix_buffer_volume, kMixVolumeCount).CopyTo(MixBufferVolume());

        if (behaviourInfo.IsSplitterSupported()) {
            isDirty = UpdateConnection(edgeMatrix, parameter, splitterContext);
        } else {
            isDirty = DestinationMixId != parameter.destination_mix_id;

            DestinationMixId = parameter.destination_mix_id;

            DestinationSplitterId = Constants::UnusedSplitterId;
        }

        ClearEffectProcessingOrder();

        for (int32_t i = 0; i < effectContext.GetCount(); i++) {
            auto& effect = effectContext.GetEffect(i);

            if (effect.MixId == MixId) {
                NXAUDREN_ASSERT(effect.ProcessingOrder <= EffectProcessingOrderArrayMaxCount);

                if (effect.ProcessingOrder > EffectProcessingOrderArrayMaxCount) {
                    return isDirty;
                }

                EffectProcessingOrderArray()[static_cast<size_t>(effect.ProcessingOrder)] = i;
            }
        }

        return isDirty;
    }

  private:
    static constexpr size_t kMixVolumeCount =
        static_cast<size_t>(Constants::MixBufferCountMax) * static_cast<size_t>(Constants::MixBufferCountMax);

    // Update the mix connection on the adjacency matrix. Returns true if new connections were done on the adjacency matrix.
    bool UpdateConnection(EdgeMatrix& edgeMatrix, const MixParameter& parameter, SplitterContext& splitterContext) {
        bool hasNewConnections = false;

        if (DestinationSplitterId == Constants::UnusedSplitterId) {
            hasNewConnections = false;
        } else {
            SplitterState& splitter = splitterContext.GetState(static_cast<int32_t>(DestinationSplitterId));

            hasNewConnections = splitter.HasNewConnection;
        }

        if (DestinationMixId == parameter.destination_mix_id && DestinationSplitterId == parameter.destination_splitter_id && !hasNewConnections) {
            return false;
        }

        edgeMatrix.RemoveEdges(MixId);

        if (parameter.destination_mix_id == Constants::UnusedMixId) {
            if (parameter.destination_splitter_id != Constants::UnusedSplitterId) {
                SplitterState& splitter = splitterContext.GetState(static_cast<int32_t>(parameter.destination_splitter_id));

                for (int32_t i = 0; i < splitter.DestinationCount; i++) {
                    SplitterDestination destination = splitter.GetData(i);

                    if (!destination.IsNull()) {
                        int32_t destinationMixId = destination.DestinationId();

                        if (destinationMixId != Constants::UnusedMixId) {
                            edgeMatrix.Connect(MixId, destinationMixId);
                        }
                    }
                }
            }
        } else {
            edgeMatrix.Connect(MixId, parameter.destination_mix_id);
        }

        DestinationMixId = parameter.destination_mix_id;
        DestinationSplitterId = parameter.destination_splitter_id;

        return true;
    }

    // Mix buffer volumes storage (C# MixVolumeArray, MixBufferCountMax * MixBufferCountMax floats).
    float _mixVolumeArray[kMixVolumeCount] = {};
    // The effect processing order storage (C# nint pointer, owned by the work buffer).
    int32_t* _effectProcessingOrderArrayPointer = nullptr;
};

static_assert(sizeof(MixInfo) == 0x940);

}  // namespace nxrt::audren
