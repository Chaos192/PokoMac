// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/AuxiliaryBufferCommand.cs (Ryubing fork, MIT).
// Guest memory: the C# IVirtualMemoryManager reads/writes become direct accesses through GuestPtr; memoryManager parameters are dropped.
// C# converts the float buffer to ints in place (MemoryMarshal.Cast aliasing). Here the int views are scratch vectors of the same size;
// the float buffers end up with the same values as in the C#.
#pragma once
#include <algorithm>
#include <cstring>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../DataSourceHelper.h"
#include "../State/AuxiliaryBufferHeader.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class AuxiliaryBufferCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    uint32_t InputBufferIndex = 0;
    uint32_t OutputBufferIndex = 0;

    AuxiliaryBufferAddresses BufferInfo{};

    CpuAddress InputBuffer = 0;
    CpuAddress OutputBuffer = 0;
    uint32_t CountMax = 0;
    uint32_t UpdateCount = 0;
    uint32_t WriteOffset = 0;

    bool IsEffectEnabled = false;

    AuxiliaryBufferCommand() = default;

    AuxiliaryBufferCommand& Initialize(uint32_t bufferOffset, uint8_t inputBufferOffset, uint8_t outputBufferOffset,
                                       AuxiliaryBufferAddresses& sendBufferInfo, bool isEnabled, uint32_t countMax, CpuAddress outputBuffer,
                                       CpuAddress inputBuffer, uint32_t updateCount, uint32_t writeOffset, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = bufferOffset + inputBufferOffset;
        OutputBufferIndex = bufferOffset + outputBufferOffset;
        BufferInfo = sendBufferInfo;
        InputBuffer = inputBuffer;
        OutputBuffer = outputBuffer;
        CountMax = countMax;
        UpdateCount = updateCount;
        WriteOffset = writeOffset;
        IsEffectEnabled = isEnabled;

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::AuxiliaryBuffer; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<float> inputBuffer = context.GetBuffer(static_cast<int32_t>(InputBufferIndex));
        Span<float> outputBuffer = context.GetBuffer(static_cast<int32_t>(OutputBufferIndex));

        if (IsEffectEnabled) {
            const size_t inputLength = inputBuffer.Length;
            _inputInt.resize(inputLength);
            _outputInt.resize(outputBuffer.Length);

            // Convert input data to the target format for user (int)
            DataSourceHelper::ToInt(Span<int32_t>(_inputInt.data(), inputLength), inputBuffer, static_cast<int32_t>(inputBuffer.Length));

            // Send the input to the user
            Write(OutputBuffer, CountMax, _inputInt.data(), context.SampleCount, WriteOffset, UpdateCount);

            // Convert back to float just in case it's reused
            DataSourceHelper::ToFloat(inputBuffer, Span<const int32_t>(_inputInt.data(), inputLength), static_cast<int32_t>(inputBuffer.Length));

            // Retrieve the input from user
            uint32_t readResult = Read(InputBuffer, CountMax, _outputInt.data(), context.SampleCount, WriteOffset, UpdateCount);

            // Convert the outputBuffer back to the target format of the renderer (float)
            DataSourceHelper::ToFloat(outputBuffer, Span<const int32_t>(_outputInt.data(), _outputInt.size()),
                                      static_cast<int32_t>(outputBuffer.Length));

            if (readResult != context.SampleCount) {
                outputBuffer.Slice(readResult, context.SampleCount - readResult).Clear();
            }
        } else {
            AuxiliaryBufferHeader::AuxiliaryBufferInfo::Reset(BufferInfo.send_buffer_info);
            AuxiliaryBufferHeader::AuxiliaryBufferInfo::Reset(BufferInfo.return_buffer_info);

            if (InputBufferIndex != OutputBufferIndex) {
                inputBuffer.CopyTo(outputBuffer);
            }
        }
    }

  private:
    std::vector<int32_t> _inputInt;
    std::vector<int32_t> _outputInt;

    // C# Read: returns `count` when samples were read from the buffer, 0 otherwise.
    uint32_t Read(CpuAddress bufferAddress, uint32_t countMax, int32_t* outBuffer, uint32_t count, uint32_t readOffset, uint32_t updateCount) {
        if (countMax == 0 || bufferAddress == 0) {
            return 0;
        }

        uint32_t targetReadOffset = readOffset + AuxiliaryBufferHeader::AuxiliaryBufferInfo::GetReadOffset(BufferInfo.return_buffer_info);

        if (targetReadOffset > countMax) {
            return 0;
        }

        uint32_t remaining = count;

        uint32_t outBufferOffset = 0;

        while (remaining != 0) {
            uint32_t countToWrite = std::min(countMax - targetReadOffset, remaining);

            std::memcpy(outBuffer + outBufferOffset,
                        GuestPtr<int32_t>(bufferAddress + static_cast<uint64_t>(targetReadOffset) * sizeof(int32_t)),
                        static_cast<size_t>(countToWrite) * sizeof(int32_t));

            targetReadOffset = (targetReadOffset + countToWrite) % countMax;
            remaining -= countToWrite;
            outBufferOffset += countToWrite;
        }

        if (updateCount != 0) {
            uint32_t newReadOffset =
                (AuxiliaryBufferHeader::AuxiliaryBufferInfo::GetReadOffset(BufferInfo.return_buffer_info) + updateCount) % countMax;

            AuxiliaryBufferHeader::AuxiliaryBufferInfo::SetReadOffset(BufferInfo.return_buffer_info, newReadOffset);
        }

        return count;
    }

    // C# Write: returns `count` when samples were written to the buffer, 0 otherwise.
    uint32_t Write(CpuAddress outBufferAddress, uint32_t countMax, const int32_t* buffer, uint32_t count, uint32_t writeOffset, uint32_t updateCount) {
        if (countMax == 0 || outBufferAddress == 0) {
            return 0;
        }

        uint32_t targetWriteOffset = writeOffset + AuxiliaryBufferHeader::AuxiliaryBufferInfo::GetWriteOffset(BufferInfo.send_buffer_info);

        if (targetWriteOffset > countMax) {
            return 0;
        }

        uint32_t remaining = count;

        uint32_t inBufferOffset = 0;

        while (remaining != 0) {
            uint32_t countToWrite = std::min(countMax - targetWriteOffset, remaining);

            std::memcpy(GuestPtr<int32_t>(outBufferAddress + static_cast<uint64_t>(targetWriteOffset) * sizeof(int32_t)),
                        buffer + inBufferOffset, static_cast<size_t>(countToWrite) * sizeof(int32_t));

            targetWriteOffset = (targetWriteOffset + countToWrite) % countMax;
            remaining -= countToWrite;
            inBufferOffset += countToWrite;
        }

        if (updateCount != 0) {
            uint32_t newWriteOffset =
                (AuxiliaryBufferHeader::AuxiliaryBufferInfo::GetWriteOffset(BufferInfo.send_buffer_info) + updateCount) % countMax;

            AuxiliaryBufferHeader::AuxiliaryBufferInfo::SetWriteOffset(BufferInfo.send_buffer_info, newWriteOffset);
        }

        return count;
    }
};

}  // namespace nxrt::audren
