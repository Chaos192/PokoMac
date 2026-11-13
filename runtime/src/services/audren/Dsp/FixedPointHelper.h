// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/FixedPointHelper.cs (Ryubing fork, MIT). Static class -> struct with static members.
#pragma once
#include <cstdint>

namespace nxrt::audren {

struct FixedPointHelper {
    static int32_t ToInt(int64_t value, int32_t qBits) { return static_cast<int32_t>(value >> qBits); }

    static float ToFloat(int64_t value, int32_t qBits) {
        return static_cast<float>(value) / static_cast<float>(1 << qBits);
    }

    static float ConvertFloat(float value, int32_t qBits) { return value / static_cast<float>(1 << qBits); }

    static int32_t ToFixed(float value, int32_t qBits) { return static_cast<int32_t>(value * static_cast<float>(1 << qBits)); }

    static int32_t RoundUpAndToInt(int64_t value, int32_t qBits) {
        int32_t half = 1 << (qBits - 1);
        return ToInt(value + half, qBits);
    }
};

}  // namespace nxrt::audren
