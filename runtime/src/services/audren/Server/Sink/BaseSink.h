// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Sink/BaseSink.cs (Ryubing fork, MIT). Base server state of an audio sink.
//
// C# calls the virtual CleanUp() from this constructor, and C# dispatches it to the derived override even during base
// construction. C++ does not, so each derived constructor calls CleanUp() again once its own part is built, which gives the
// same final state.
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"

namespace nxrt::audren {

class BaseSink {
  public:
    // The type of this BaseSink.
    SinkType Type = SinkType::Invalid;
    // Set to true if the sink is used.
    bool IsUsed = false;
    // Set to true if the sink needs to be skipped because of invalid state.
    bool ShouldSkip = false;
    // The node id of the sink.
    int32_t NodeId = 0;

    BaseSink() { CleanUp(); }
    virtual ~BaseSink() = default;

    // Clean up the internal state of the BaseSink.
    virtual void CleanUp() {
        Type = TargetSinkType();
        IsUsed = false;
        ShouldSkip = false;
    }

    // The target SinkType handled by this BaseSink (C# virtual property).
    virtual SinkType TargetSinkType() const { return SinkType::Invalid; }

    // Check if the SinkType sent by the user matches the internal SinkType.
    bool IsTypeValid(const SinkInParameter& parameter) const { return parameter.type == TargetSinkType(); }

    // Update the BaseSink state during command generation.
    virtual void UpdateForCommandGeneration() { NXAUDREN_ASSERT(Type == TargetSinkType()); }

  protected:
    // Update the internal common parameters from user parameter.
    void UpdateStandardParameter(const SinkInParameter& parameter) {
        if (IsUsed != parameter.is_used) {
            IsUsed = parameter.is_used;
            NodeId = parameter.node_id;
        }
    }

  public:
    // Update the internal state from user parameter.
    virtual void Update(BehaviourParameter::ErrorInfo& errorInfo, const SinkInParameter& parameter, SinkOutStatus& outStatus,
                        PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));
        (void)parameter;
        (void)outStatus;
        (void)mapper;

        errorInfo = BehaviourParameter::ErrorInfo{};
    }
};

}  // namespace nxrt::audren
