// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Effect/EffectContext.cs (Ryubing fork, MIT).
//
// Naming notes:
//   * C# `ref BaseEffect GetEffect(int)` returns a reference to the array slot, so callers may replace the effect object
//     (StateUpdater.ResetEffect does). The port returns std::unique_ptr<BaseEffect>& for the same reason.
//   * C# `Memory<EffectResultState> GetDspStateMemory` (SpanIOHelper.GetMemory, one element) is Span<EffectResultState>.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "BaseEffect.h"

namespace nxrt::audren {

class EffectContext {
  private:
    // Storage for the effects (C# BaseEffect[]).
    std::vector<std::unique_ptr<BaseEffect>> _effects;

    // The total effect count.
    uint32_t _effectCount = 0;

    std::vector<EffectResultState> _resultStatesCpu;
    std::vector<EffectResultState> _resultStatesDsp;

  public:
    EffectContext() = default;

    // Initialize the EffectContext: every effect starts as a BaseEffect (EffectType.Invalid), result states are zeroed.
    void Initialize(uint32_t effectCount, uint32_t resultStateCount) {
        _effectCount = effectCount;

        _effects.clear();
        _effects.reserve(effectCount);
        for (uint32_t i = 0; i < _effectCount; i++) {
            _effects.push_back(std::make_unique<BaseEffect>());
        }

        _resultStatesCpu.assign(resultStateCount, EffectResultState{});
        _resultStatesDsp.assign(resultStateCount, EffectResultState{});
    }

    // Get the total effect count.
    uint32_t GetCount() const { return _effectCount; }

    // Get a reference to the effect slot at the given index (may be replaced by assignment).
    std::unique_ptr<BaseEffect>& GetEffect(int index) {
        NXAUDREN_ASSERT(index >= 0 && static_cast<uint32_t>(index) < _effectCount);
        return _effects[static_cast<size_t>(index)];
    }

    // Get a reference to the EffectResultState at the given index (used when updating the server state).
    EffectResultState& GetState(int index) {
        NXAUDREN_ASSERT(index >= 0 && static_cast<size_t>(index) < _resultStatesCpu.size());
        return _resultStatesCpu[static_cast<size_t>(index)];
    }

    // Get a reference to the EffectResultState at the given index (used in the DSP processing).
    EffectResultState& GetDspState(int index) {
        NXAUDREN_ASSERT(index >= 0 && static_cast<size_t>(index) < _resultStatesDsp.size());
        return _resultStatesDsp[static_cast<size_t>(index)];
    }

    // A one-element view of the DSP EffectResultState at the given index (C# SpanIOHelper.GetMemory).
    Span<EffectResultState> GetDspStateMemory(int index) {
        NXAUDREN_ASSERT(index >= 0 && static_cast<size_t>(index) < _resultStatesDsp.size());
        return Span<EffectResultState>(_resultStatesDsp.data() + index, 1);
    }

    // Update internal state during command generation: every result state is updated from its DSP counterpart by the effect
    // at the same index.
    void UpdateResultStateForCommandGeneration() {
        for (size_t index = 0; index < _resultStatesCpu.size(); index++) {
            // C# indexes _effects here and throws when there are more result states than effects; that is fatal here.
            if (index >= _effects.size()) {
                NXAUDREN_FATAL("EffectContext: more result states than effects");
            }
            _effects[index]->UpdateResultState(_resultStatesCpu[index], _resultStatesDsp[index]);
        }
    }
};

}  // namespace nxrt::audren
