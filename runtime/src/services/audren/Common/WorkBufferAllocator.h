// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Common/WorkBufferAllocator.cs. Hands out aligned sub-ranges of a work buffer given as base + size.
// The C# Memory<byte> backing store is a Span<uint8_t> here; SpanMemoryManager<T>.Cast becomes Span<uint8_t>::Cast<T>().
#pragma once
#include <cstddef>
#include <type_traits>

#include "../audren_common.h"
#include "../Utils/BitUtils.h"

namespace nxrt::audren {

class WorkBufferAllocator {
public:
    // C# public Memory<byte> BackingMemory { get; }
    Span<uint8_t> BackingMemory;

    // C# public ulong Offset { get; private set; }. Only Allocate changes it.
    uint64_t Offset = 0;

    explicit WorkBufferAllocator(Span<uint8_t> backingMemory) : BackingMemory(backingMemory) {}

    // Allocate `size` bytes aligned to `align`, cleared to zero. Returns an empty span when it does not fit.
    Span<uint8_t> Allocate(uint64_t size, int32_t align) {
        NXAUDREN_ASSERT(align != 0);

        if (size != 0) {
            uint64_t alignedOffset = BitUtils::AlignUp(Offset, static_cast<uint64_t>(align));

            if (alignedOffset + size <= static_cast<uint64_t>(BackingMemory.Length)) {
                Span<uint8_t> result = BackingMemory.Slice(static_cast<size_t>(alignedOffset), static_cast<size_t>(size));

                Offset = alignedOffset + size;

                // Clear the memory to be sure that is does not contain any garbage.
                result.Clear();

                return result;
            }
        }

        return Span<uint8_t>();
    }

    // Allocate `count` elements of T, aligned to `align`.
    template <class T>
    Span<T> Allocate(uint64_t count, int32_t align) {
        static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

        Span<uint8_t> allocatedMemory = Allocate(static_cast<uint64_t>(sizeof(T)) * count, align);

        if (allocatedMemory.IsEmpty()) {
            return Span<T>();
        }

        return allocatedMemory.Cast<T>();
    }

    // Size a work buffer needs so that one more `count` elements of T can be allocated after `currentSize`.
    template <class T>
    static uint64_t GetTargetSize(uint64_t currentSize, uint64_t count, int32_t align) {
        static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

        return BitUtils::AlignUp(currentSize, static_cast<uint64_t>(align)) + static_cast<uint64_t>(sizeof(T)) * count;
    }
};

}  // namespace nxrt::audren
