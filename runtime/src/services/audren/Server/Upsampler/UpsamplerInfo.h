// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/Upsampler/UpsamplerInfo.cs (Ryubing fork, MIT). One allocated upsampler slot.
// UpsamplerBufferState is reused from Dsp/UpsamplerHelper.h.
//
// Release() is defined at the end of UpsamplerManager.h, because it needs the complete UpsamplerManager type.
#pragma once
#include <cstdint>
#include <vector>

#include "../../Dsp/UpsamplerHelper.h"
#include "../../audren_common.h"

namespace nxrt::audren {

class UpsamplerManager;

class UpsamplerInfo {
  public:
    // The output buffer containing the target samples (C# get-only Memory<float>).
    Span<float> OutputBuffer;
    // The target sample count (C# get-only uint).
    uint32_t SampleCount = 0;
    // The source sample count.
    uint32_t SourceSampleCount = 0;
    // The input buffer indices of the buffers holding the samples that need upsampling.
    std::vector<uint16_t> InputBufferIndices;
    // State of each input buffer index kept across invocations of the upsampler.
    std::vector<UpsamplerBufferState> BufferStates;

  private:
    // The index of the UpsamplerInfo in its manager (used to free it).
    int32_t _index;
    // The UpsamplerManager that owns this object.
    UpsamplerManager* _manager;

  public:
    // Create a new UpsamplerInfo.
    UpsamplerInfo(UpsamplerManager& manager, int32_t index, Span<float> outputBuffer, uint32_t sampleCount)
        : OutputBuffer(outputBuffer), SampleCount(sampleCount), _index(index), _manager(&manager) {}

    // Release the UpsamplerInfo. The manager destroys this object, so nothing may touch it after this call.
    void Release();
};

}  // namespace nxrt::audren
