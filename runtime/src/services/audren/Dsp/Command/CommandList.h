// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/CommandList.cs (Ryubing fork, MIT). The command list owns its commands (the C# recycles
// them through object pools in CommandBuffer.ReleaseCommand); the mix buffer is a slab of floats owned by the renderer.
#pragma once
#include <chrono>
#include <memory>
#include <vector>

#include "../../Integration/IHardwareDevice.h"
#include "../../audren_common.h"
#include "ICommand.h"

namespace nxrt::audren {

class CommandList {
  public:
    uint64_t StartTime = 0;
    uint64_t EndTime = 0;
    uint32_t SampleCount = 0;
    uint32_t SampleRate = 0;

    Span<float> Buffers;   // BufferCount buffers of SampleCount floats, one after the other
    uint32_t BufferCount = 0;
    std::vector<std::unique_ptr<ICommand>> Commands;
    IHardwareDevice* OutputDevice = nullptr;

    CommandList(Span<float> mixBuffer, uint32_t sampleCount, uint32_t sampleRate, uint32_t mixBufferCount, uint32_t voiceChannelCountMax)
        : SampleCount(sampleCount), SampleRate(sampleRate), Buffers(mixBuffer), BufferCount(mixBufferCount + voiceChannelCountMax) {
        _sampleCount = static_cast<int32_t>(sampleCount);
        // The C# bounds indices by Buffers.Length (a float count), which lets an index run past the slab; here the bound is the buffer count.
        _buffersEntryCount = static_cast<int32_t>(BufferCount);
        _floatCount = mixBuffer.Length;
        Commands.reserve(256);
    }

    void AddCommand(std::unique_ptr<ICommand> command) { Commands.push_back(std::move(command)); }

    float* GetBufferPointer(int32_t index) {
        if (index >= 0 && index < _buffersEntryCount) return Buffers.Data + static_cast<size_t>(index) * _sampleCount;
        NXAUDREN_FATAL("CommandList::GetBufferPointer: index out of range");
    }

    void ClearBuffer(int32_t index) { std::memset(GetBufferPointer(index), 0, static_cast<size_t>(SampleCount) * sizeof(float)); }

    void ClearBuffers() { std::memset(Buffers.Data, 0, _floatCount * sizeof(float)); }

    void CopyBuffer(int32_t outputBufferIndex, int32_t inputBufferIndex) {
        std::memcpy(GetBufferPointer(outputBufferIndex), GetBufferPointer(inputBufferIndex), static_cast<size_t>(SampleCount) * sizeof(float));
    }

    Span<float> GetBuffer(int32_t index) {
        if (index < 0 || index >= _buffersEntryCount) return Span<float>();
        return Span<float>(Buffers.Data + static_cast<size_t>(index) * _sampleCount, static_cast<size_t>(_sampleCount));
    }

    uint64_t GetTimeElapsedSinceDspStartedProcessing() const { return NowNs() - StartTime; }

    void Process(IHardwareDevice* outputDevice) {
        OutputDevice = outputDevice;
        StartTime = NowNs();
        for (auto& command : Commands) {
            if (command->Enabled) command->Process(*this);
        }
        EndTime = NowNs();
    }

  private:
    static uint64_t NowNs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    int32_t _sampleCount = 0;
    int32_t _buffersEntryCount = 0;
    size_t _floatCount = 0;
};

}  // namespace nxrt::audren
