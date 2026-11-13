// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/Command/CaptureBufferCommand.cs (Ryubing fork, MIT). Guest reads and writes go through GuestPtr /
// GuestSpan; the C# memoryManager parameter is dropped.
#pragma once
#include <algorithm>
#include <cstdint>

#include "../../audren_common.h"
#include "../DataSourceHelper.h"
#include "../State/AuxiliaryBufferHeader.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class CaptureBufferCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;
    uint32_t InputBufferIndex = 0;
    uint64_t CpuBufferInfoAddress = 0;
    uint64_t DspBufferInfoAddress = 0;
    CpuAddress OutputBuffer = 0;
    uint32_t CountMax = 0;
    uint32_t UpdateCount = 0;
    uint32_t WriteOffset = 0;
    bool IsEffectEnabled = false;

    CaptureBufferCommand& Initialize(uint32_t bufferOffset, uint8_t inputBufferOffset, uint64_t sendBufferInfo, bool isEnabled, uint32_t countMax,
                                     CpuAddress outputBuffer, uint32_t updateCount, uint32_t writeOffset, int32_t nodeId) {
        Enabled = true;
        NodeId = nodeId;
        InputBufferIndex = bufferOffset + inputBufferOffset;
        CpuBufferInfoAddress = sendBufferInfo;
        DspBufferInfoAddress = sendBufferInfo + static_cast<uint64_t>(sizeof(AuxiliaryBufferHeader));
        OutputBuffer = outputBuffer;
        CountMax = countMax;
        UpdateCount = updateCount;
        WriteOffset = writeOffset;
        IsEffectEnabled = isEnabled;
        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::CaptureBuffer; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<float> inputBuffer = context.GetBuffer(static_cast<int32_t>(InputBufferIndex));
        if (IsEffectEnabled) {
            Span<int32_t> inputBufferInt = inputBuffer.Cast<int32_t>();
            // Convert input data to the target format for user (int)
            DataSourceHelper::ToInt(inputBufferInt, inputBuffer, static_cast<int32_t>(inputBuffer.Length));
            // Send the input to the user
            Write(OutputBuffer, CountMax, inputBufferInt, context.SampleCount, WriteOffset, UpdateCount);
            // Convert back to float
            DataSourceHelper::ToFloat(inputBuffer, inputBufferInt, static_cast<int32_t>(inputBuffer.Length));
        } else {
            Info::Reset(DspBufferInfoAddress);
        }
    }

  private:
    using Info = AuxiliaryBufferHeader::AuxiliaryBufferInfo;

    uint32_t Write(CpuAddress outBufferAddress, uint32_t countMax, Span<const int32_t> buffer, uint32_t count, uint32_t writeOffset,
                   uint32_t updateCount) const {
        if (countMax == 0 || outBufferAddress == 0) {
            return 0;
        }

        uint32_t targetWriteOffset = writeOffset + Info::GetWriteOffset(DspBufferInfoAddress);
        if (targetWriteOffset > countMax) {
            return 0;
        }

        uint32_t remaining = count;
        uint32_t inBufferOffset = 0;
        while (remaining != 0) {
            uint32_t countToWrite = std::min(countMax - targetWriteOffset, remaining);
            buffer.Slice(inBufferOffset, countToWrite)
                .CopyTo(GuestSpan<int32_t>(outBufferAddress + static_cast<uint64_t>(targetWriteOffset) * sizeof(int32_t), countToWrite));
            targetWriteOffset = (targetWriteOffset + countToWrite) % countMax;
            remaining -= countToWrite;
            inBufferOffset += countToWrite;
        }

        if (updateCount != 0) {
            uint32_t dspTotalSampleCount = Info::GetTotalSampleCount(DspBufferInfoAddress);
            uint32_t cpuTotalSampleCount = Info::GetTotalSampleCount(CpuBufferInfoAddress);
            uint32_t totalSampleCountDiff = dspTotalSampleCount - cpuTotalSampleCount;

            if (totalSampleCountDiff >= countMax) {
                uint32_t dspLostSampleCount = Info::GetLostSampleCount(DspBufferInfoAddress);
                uint32_t cpuLostSampleCount = Info::GetLostSampleCount(CpuBufferInfoAddress);
                uint32_t lostSampleCountDiff = dspLostSampleCount - cpuLostSampleCount;

                uint32_t newLostSampleCount = lostSampleCountDiff + updateCount;
                if (lostSampleCountDiff > newLostSampleCount) {
                    newLostSampleCount = cpuLostSampleCount - 1;
                }

                Info::SetLostSampleCount(DspBufferInfoAddress, newLostSampleCount);
            }

            uint32_t newWriteOffset = (Info::GetWriteOffset(DspBufferInfoAddress) + updateCount) % countMax;
            Info::SetWriteOffset(DspBufferInfoAddress, newWriteOffset);

            uint32_t newTotalSampleCount = totalSampleCountDiff + newWriteOffset;
            Info::SetTotalSampleCount(DspBufferInfoAddress, newTotalSampleCount);
        }

        return count;
    }
};

}  // namespace nxrt::audren
