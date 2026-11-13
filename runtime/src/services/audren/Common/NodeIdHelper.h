// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Common/NodeIdHelper.cs. Helper for manipulating node ids.
#pragma once
#include "../audren_common.h"
#include "../audren_types.h"

namespace nxrt::audren {

struct NodeIdHelper {
    // Get the type of a node from a given node id.
    static NodeIdType GetType(int32_t nodeId) { return static_cast<NodeIdType>(nodeId >> 28); }

    // Get the base of a node from a given node id.
    static int32_t GetBase(int32_t nodeId) { return (nodeId >> 16) & 0xFFF; }
};

}  // namespace nxrt::audren
