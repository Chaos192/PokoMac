// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/State/AuxiliaryBufferHeader.cs (Ryubing fork, MIT).
// C# IVirtualMemoryManager reads/writes become direct guest pointer accesses (GuestPtr); the memoryManager parameter is dropped.
#pragma once
#include <cstdint>

#include "../../audren_common.h"

namespace nxrt::audren {

#pragma pack(push, 1)

// [StructLayout(Pack = 1, Size = 0x80)]
struct AuxiliaryBufferHeader {
    // [StructLayout(Pack = 1, Size = 0x40)]
    struct AuxiliaryBufferInfo {
        static constexpr uint32_t ReadOffsetPosition = 0x0;
        static constexpr uint32_t WriteOffsetPosition = 0x4;
        static constexpr uint32_t LostSampleCountPosition = 0x8;
        static constexpr uint32_t TotalSampleCountPosition = 0xC;

        uint32_t ReadOffset;
        uint32_t WriteOffset;
        uint32_t LostSampleCount;
        uint32_t TotalSampleCount;

      private:
        [[maybe_unused]] uint32_t _unknown[12];  // C# private unsafe fixed uint _unknown[12]

      public:
        static uint32_t GetReadOffset(CpuAddress bufferAddress) { return *GuestPtr<uint32_t>(bufferAddress + ReadOffsetPosition); }

        static uint32_t GetWriteOffset(CpuAddress bufferAddress) { return *GuestPtr<uint32_t>(bufferAddress + WriteOffsetPosition); }

        static uint32_t GetLostSampleCount(CpuAddress bufferAddress) { return *GuestPtr<uint32_t>(bufferAddress + LostSampleCountPosition); }

        static uint32_t GetTotalSampleCount(CpuAddress bufferAddress) { return *GuestPtr<uint32_t>(bufferAddress + TotalSampleCountPosition); }

        static void SetReadOffset(CpuAddress bufferAddress, uint32_t value) { *GuestPtr<uint32_t>(bufferAddress + ReadOffsetPosition) = value; }

        static void SetWriteOffset(CpuAddress bufferAddress, uint32_t value) { *GuestPtr<uint32_t>(bufferAddress + WriteOffsetPosition) = value; }

        static void SetLostSampleCount(CpuAddress bufferAddress, uint32_t value) { *GuestPtr<uint32_t>(bufferAddress + LostSampleCountPosition) = value; }

        static void SetTotalSampleCount(CpuAddress bufferAddress, uint32_t value) { *GuestPtr<uint32_t>(bufferAddress + TotalSampleCountPosition) = value; }

        static void Reset(CpuAddress bufferAddress) {
            // NOTE: Lost sample count is never reset, since REV10.
            // C# writes a ulong (8 bytes) at ReadOffsetPosition here, which covers ReadOffset and WriteOffset.
            *GuestPtr<uint64_t>(bufferAddress + ReadOffsetPosition) = 0;
            *GuestPtr<uint32_t>(bufferAddress + TotalSampleCountPosition) = 0;
        }
    };

    AuxiliaryBufferInfo CpuBufferInfo;
    AuxiliaryBufferInfo DspBufferInfo;
};

#pragma pack(pop)

static_assert(sizeof(AuxiliaryBufferHeader::AuxiliaryBufferInfo) == 0x40);
static_assert(sizeof(AuxiliaryBufferHeader) == 0x80);

}  // namespace nxrt::audren
