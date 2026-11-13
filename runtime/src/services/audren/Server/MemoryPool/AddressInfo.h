// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Server/MemoryPool/AddressInfo.cs (Ryubing fork, MIT). A CPU region shared with the DSP,
// optionally backed by a MemoryPoolInfo. Field types are uint64_t for the same reason as in MemoryPoolInfo.h.
#pragma once
#include "MemoryPoolInfo.h"

namespace nxrt::audren {

struct AddressInfo {
    // The target CPU address of the region.
    uint64_t CpuAddress = 0;
    // The size of the region.
    uint64_t Size = 0;
    // The forced DSP address of the region (used when no pool backs it and force mapping is enabled).
    uint64_t ForceMappedDspAddress = 0;

  private:
    // C# `MemoryPoolInfo*`: the pool the region was found in, or null.
    MemoryPoolInfo* _memoryPools = nullptr;

    // C# private property `ref *_memoryPools`. Renamed because a member called MemoryPoolInfo would hide the type name.
    MemoryPoolInfo& MemoryPoolInfoRef() const { return *_memoryPools; }

  public:
    // C# `public readonly unsafe bool HasMemoryPoolState => (nint)_memoryPools != nint.Zero;`
    bool HasMemoryPoolState() const { return _memoryPools != nullptr; }

    // Create an new empty AddressInfo.
    static AddressInfo Create() { return Create(0, 0); }

    // Create a new AddressInfo.
    static AddressInfo Create(uint64_t cpuAddress, uint64_t size) {
        AddressInfo info;
        info.CpuAddress = cpuAddress;
        info._memoryPools = MemoryPoolInfo::Null();
        info.Size = size;
        info.ForceMappedDspAddress = 0;
        return info;
    }

    // Setup the CPU address and size of the AddressInfo.
    void Setup(uint64_t cpuAddress, uint64_t size) {
        CpuAddress = cpuAddress;
        Size = size;
        ForceMappedDspAddress = 0;
        _memoryPools = MemoryPoolInfo::Null();
    }

    // Set the MemoryPoolInfo associated (the first element of the span is the pool).
    void SetupMemoryPool(Span<MemoryPoolInfo> memoryPoolState) {
        NXAUDREN_ASSERT(!memoryPoolState.IsEmpty());
        SetupMemoryPool(memoryPoolState.Data);
    }

    // Set the MemoryPoolInfo associated.
    void SetupMemoryPool(MemoryPoolInfo* memoryPoolState) { _memoryPools = memoryPoolState; }

    // Check if the MemoryPoolInfo is mapped.
    bool HasMappedMemoryPool() const { return HasMemoryPoolState() && MemoryPoolInfoRef().IsMapped(); }

    // Get the DSP address associated to the AddressInfo.
    // markUsed: if true, mark the MemoryPoolInfo as used.
    uint64_t GetReference(bool markUsed) const {
        if (!HasMappedMemoryPool()) {
            return ForceMappedDspAddress;
        }

        if (markUsed) {
            MemoryPoolInfoRef().IsUsed = true;
        }

        return MemoryPoolInfoRef().Translate(CpuAddress, Size);
    }
};

}  // namespace nxrt::audren
