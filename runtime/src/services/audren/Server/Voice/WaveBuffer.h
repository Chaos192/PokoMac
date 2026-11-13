// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Voice/WaveBuffer.cs (Ryubing fork, MIT): the server-side wave buffer description.
//
// Name clash: audren_types.h already defines nxrt::audren::WaveBuffer (the Common layout, C# Renderer/Common/WaveBuffer.cs).
// The server one lives in nxrt::audren::server, the same resolution audren_types.h uses for the Server PlayState. Inside
// nxrt::audren::server, an unqualified WaveBuffer is this struct; the Common one is written ::nxrt::audren::WaveBuffer.
//
// Not a binary layout: BufferAddressInfo / ContextAddressInfo are the C++ AddressInfo class (guest-invisible), so the C# Size
// 0x58 / Pack 1 is not reproduced and not asserted.
#pragma once
#include <cstdint>

#include "../../audren_types.h"
#include "../MemoryPool/AddressInfo.h"

namespace nxrt::audren::server {

// A wavebuffer used for server update.
struct WaveBuffer {
    // The AddressInfo of the sample data of the wavebuffer.
    AddressInfo BufferAddressInfo;

    // The AddressInfo of the context of the wavebuffer. Only used by SampleFormat.Adpcm.
    AddressInfo ContextAddressInfo;

    // First sample to play of the wavebuffer.
    uint32_t StartSampleOffset = 0;

    // Last sample to play of the wavebuffer.
    uint32_t EndSampleOffset = 0;

    // Set to true if the wavebuffer is looping.
    bool ShouldLoop = false;

    // Set to true if the wavebuffer is the end of stream.
    bool IsEndOfStream = false;

    // Set to true if the wavebuffer wasn't sent to the AudioProcessor.
    bool IsSendToAudioProcessor = false;

    // First sample to play when looping the wavebuffer.
    uint32_t LoopStartSampleOffset = 0;

    // Last sample to play when looping the wavebuffer.
    uint32_t LoopEndSampleOffset = 0;

    // The max loop count.
    int32_t LoopCount = 0;

    // Create a new Common WaveBuffer for use by the AudioProcessor (C# ToCommon(int version)).
    ::nxrt::audren::WaveBuffer ToCommon(int32_t version) const {
        ::nxrt::audren::WaveBuffer waveBuffer{};

        waveBuffer.buffer = BufferAddressInfo.GetReference(true);
        waveBuffer.buffer_size = static_cast<uint32_t>(BufferAddressInfo.Size);

        if (ContextAddressInfo.CpuAddress != 0) {
            waveBuffer.context = ContextAddressInfo.GetReference(true);
            waveBuffer.context_size = static_cast<uint32_t>(ContextAddressInfo.Size);
        }

        waveBuffer.start_sample_offset = StartSampleOffset;
        waveBuffer.end_sample_offset = EndSampleOffset;
        waveBuffer.looping = ShouldLoop;
        waveBuffer.is_end_of_stream = IsEndOfStream;

        if (version == 2) {
            waveBuffer.loop_count = LoopCount;
            waveBuffer.loop_start_sample_offset = LoopStartSampleOffset;
            waveBuffer.loop_end_sample_offset = LoopEndSampleOffset;
        } else {
            waveBuffer.loop_count = -1;
        }

        return waveBuffer;
    }
};

}  // namespace nxrt::audren::server
