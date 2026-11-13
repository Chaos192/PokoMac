// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Utils/BitArray.cs. A bit array over a non-owning byte span.
#pragma once
#include "../audren_common.h"

namespace nxrt::audren {

class BitArray {
public:
    // C# has no parameterless constructor; EdgeMatrix holds a BitArray as a member that is bound later by Initialize.
    BitArray() = default;

    explicit BitArray(Span<uint8_t> storage) : _storage(storage) {}

    // Test if the bit at the given index is set.
    bool Test(int32_t index) const {
        uint8_t mask = static_cast<uint8_t>(1u << ToBitPosition(index));
        return (_storage[ToPosition(index)] & mask) == mask;
    }

    // Set the bit at the given index.
    void Set(int32_t index) { Set(index, true); }

    // Reset the bit at the given index.
    void Reset(int32_t index) { Set(index, false); }

    // Reset all bits in the storage.
    void Reset() { _storage.Clear(); }

private:
    // Get the byte position of a given bit index.
    static int32_t ToPosition(int32_t index) { return index / 8; }

    // Get the bit position of a given bit index inside a byte.
    static int32_t ToBitPosition(int32_t index) { return index % 8; }

    // Set a bit value at the given index.
    void Set(int32_t index, bool value) {
        uint8_t mask = static_cast<uint8_t>(1u << ToBitPosition(index));
        if (value) {
            _storage[ToPosition(index)] |= mask;
        } else {
            _storage[ToPosition(index)] &= static_cast<uint8_t>(~mask);
        }
    }

    // The backing storage of the BitArray.
    Span<uint8_t> _storage;
};

}  // namespace nxrt::audren
