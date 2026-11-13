// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Voice/VoiceChannelResource.cs (Ryubing fork, MIT). Server state of one voice channel.
// The C# struct is Size 0xD0 with Pack = 0x10; the C++ struct is alignas(16) so that sizeof matches that size.
#pragma once
#include <array>
#include <cstdint>

#include "../../audren_types.h"

namespace nxrt::audren {

struct alignas(0x10) VoiceChannelResource {
    static constexpr uint32_t Alignment = 0x10;

    // Mix volumes for the resource (C# Array24<float>).
    std::array<float, MixBufferCountMax> Mix{};

    // Previous mix volumes for resource (C# Array24<float>).
    std::array<float, MixBufferCountMax> PreviousMix{};

    // The id of the resource.
    uint32_t Id = 0;

    // Indicate if the resource is used.
    bool IsUsed = false;

    void UpdateState() { PreviousMix = Mix; }
};

static_assert(sizeof(VoiceChannelResource) == 0xD0);

}  // namespace nxrt::audren
