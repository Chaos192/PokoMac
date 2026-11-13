// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of the one Ryujinx.Common/Utilities/BitUtils.cs member the audio renderer needs (AlignUp). Not in the original stream list:
// added so that EdgeMatrix, WorkBufferAllocator and NodeStates can call it under its C# name.
#pragma once
#include "../audren_common.h"

namespace nxrt::audren {

struct BitUtils {
    // Align value up to a multiple of size (size must be a power of two). C#: (value + (size - 1)) & -size
    template <class T>
    static constexpr T AlignUp(T value, T size) {
        return static_cast<T>((value + (size - 1)) & static_cast<T>(0 - size));
    }
};

}  // namespace nxrt::audren
