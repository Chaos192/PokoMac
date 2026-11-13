// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/RendererSystemContext.cs (Ryubing fork, MIT).
// A lite version of AudioRenderSystem used by the audio processor, so that the DSP side does not depend on the whole system.
// Deviation: BehaviourInfo and UpsamplerManager are C# class instances owned by the AudioRenderSystem, so they are pointers here (not
// value members). A value copy would not see later updates (for example the user revision) made by the owner.
#pragma once
#include <cstdint>

#include "../audren_common.h"
#include "BehaviourInfo.h"
#include "Upsampler/UpsamplerManager.h"

namespace nxrt::audren {

class RendererSystemContext {
  public:
    // The session id of the current renderer.
    int32_t SessionId = 0;

    // The target channel count for sink.
    uint32_t ChannelCount = 0;

    // The total count of mix buffer.
    uint32_t MixBufferCount = 0;

    // Instance of the BehaviourInfo used to derive bug fixes and features of the current audio renderer revision (owned elsewhere).
    nxrt::audren::BehaviourInfo* BehaviourInfo = nullptr;

    // Instance of the UpsamplerManager used for upsampling (owned elsewhere).
    nxrt::audren::UpsamplerManager* UpsamplerManager = nullptr;

    // The memory to use for depop processing.
    Span<float> DepopBuffer;
};

}  // namespace nxrt::audren
