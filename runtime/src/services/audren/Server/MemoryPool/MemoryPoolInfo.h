// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/MemoryPool/MemoryPoolInfo.cs (Ryubing fork, MIT). Server state of one memory pool.
// This is not a binary layout (the guest sees MemoryPoolInParameter / MemoryPoolOutStatus), so it is a plain struct with the C# names.
// Field types are spelled uint64_t rather than the CpuAddress / DspAddress aliases: inside a struct whose data members have
// those names, the alias would be hidden by the member.
#pragma once
#include "../../audren_common.h"

namespace nxrt::audren {

struct MemoryPoolInfo {
    static constexpr uint32_t Alignment = 0x10;

    // C# nested enum LocationType : uint.
    enum class LocationType : uint32_t {
        // MemoryPoolInfo located on the CPU side for user use.
        Cpu,
        // MemoryPoolInfo located on the DSP side for system use.
        Dsp,
    };

    // The CPU address associated to the MemoryPoolInfo.
    uint64_t CpuAddress = 0;
    // The DSP address associated to the MemoryPoolInfo (0 when not mapped).
    uint64_t DspAddress = 0;
    // The size associated to the MemoryPoolInfo.
    uint64_t Size = 0;
    // The LocationType associated to the MemoryPoolInfo.
    LocationType Location = LocationType::Cpu;
    // Set to true if the MemoryPoolInfo is used.
    bool IsUsed = false;

    // C# `public static unsafe MemoryPoolInfo* Null` (a null pointer).
    static MemoryPoolInfo* Null() { return nullptr; }

    // Create a new MemoryPoolInfo with the given LocationType.
    static MemoryPoolInfo Create(LocationType location) {
        MemoryPoolInfo info;
        info.CpuAddress = 0;
        info.DspAddress = 0;
        info.Size = 0;
        info.Location = location;
        return info;
    }

    // Set the CpuAddress and size of the MemoryPoolInfo.
    void SetCpuAddress(uint64_t cpuAddress, uint64_t size) {
        CpuAddress = cpuAddress;
        Size = size;
    }

    // Check if the given CpuAddress and size are contained in the MemoryPoolInfo.
    bool Contains(uint64_t targetCpuAddress, uint64_t size) const {
        if (CpuAddress <= targetCpuAddress && size + targetCpuAddress <= Size + CpuAddress) {
            return true;
        }

        return false;
    }

    // Translate the given CPU address to a DSP address (0 when not contained or not mapped).
    uint64_t Translate(uint64_t targetCpuAddress, uint64_t size) const {
        if (Contains(targetCpuAddress, size) && IsMapped()) {
            uint64_t offset = targetCpuAddress - CpuAddress;

            return DspAddress + offset;
        }

        return 0;
    }

    // Is the MemoryPoolInfo mapped on the DSP?
    bool IsMapped() const { return DspAddress != 0; }
};

}  // namespace nxrt::audren
