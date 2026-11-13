// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Sink/SinkContext.cs (Ryubing fork, MIT). The table of sink states.
//
// C# stores `BaseSink[]` and replaces entries with derived objects (StateUpdater.ResetSink does `sink = new DeviceSink()`).
// Replacing a slot in C++ needs owning pointers, so the entries are std::unique_ptr<BaseSink>. GetSink returns the object, as
// the C# `ref BaseSink` does for reading and updating it; GetSinkSlot returns the slot itself, for the reset that the
// StateUpdater port has to do.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "../../audren_common.h"
#include "BaseSink.h"

namespace nxrt::audren {

class SinkContext {
  private:
    // Storage for BaseSink (C# BaseSink[]).
    std::vector<std::unique_ptr<BaseSink>> _sinks;
    // The total sink count.
    uint32_t _sinkCount = 0;

  public:
    // Initialize the SinkContext: every slot starts as a plain BaseSink.
    void Initialize(uint32_t sinksCount) {
        _sinkCount = sinksCount;
        _sinks.clear();
        _sinks.reserve(_sinkCount);

        for (uint32_t i = 0; i < _sinkCount; i++) {
            _sinks.push_back(std::make_unique<BaseSink>());
        }
    }

    // Get the total sink count.
    uint32_t GetCount() const { return _sinkCount; }

    // Get a reference to the BaseSink at the given id.
    BaseSink& GetSink(int32_t id) {
        NXAUDREN_ASSERT(id >= 0 && static_cast<uint32_t>(id) < _sinkCount);

        return *_sinks[static_cast<size_t>(id)];
    }

    // Get the slot holding the BaseSink at the given id (C# `ref BaseSink` that can be reassigned).
    std::unique_ptr<BaseSink>& GetSinkSlot(int32_t id) {
        NXAUDREN_ASSERT(id >= 0 && static_cast<uint32_t>(id) < _sinkCount);

        return _sinks[static_cast<size_t>(id)];
    }
};

}  // namespace nxrt::audren
