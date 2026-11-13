// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/PcmHelper.cs (Ryubing fork, MIT). Static class -> struct with static members.
#pragma once
#include <algorithm>
#include <cstdint>

#include "../audren_common.h"

namespace nxrt::audren {

struct PcmHelper {
    static int32_t GetCountToDecode(int32_t startSampleOffset, int32_t endSampleOffset, int32_t offset, int32_t count) {
        return std::min(count, endSampleOffset - startSampleOffset - offset);
    }

    template <class T>
    static uint64_t GetBufferOffset(int32_t startSampleOffset, int32_t offset, int32_t channelCount) {
        return static_cast<uint64_t>(static_cast<int32_t>(sizeof(T)) * channelCount * (startSampleOffset + offset));
    }

    template <class T>
    static int32_t GetBufferSize(int32_t startSampleOffset, int32_t endSampleOffset, int32_t offset, int32_t count) {
        if (endSampleOffset < startSampleOffset) return 0;
        return GetCountToDecode(startSampleOffset, endSampleOffset, offset, count) * static_cast<int32_t>(sizeof(T));
    }

    static float ConvertSampleToPcmFloat(int16_t sample) { return static_cast<float>(sample) / 32767.0f; }

    static int16_t ConvertSampleToPcmInt16(float sample) { return Saturate(sample * 32767.0f); }

    // Output most significant byte.
    static void ConvertSampleToPcm8(Span<int8_t> output, Span<const int16_t> input) {
        for (size_t i = 0; i < input.Length; i++) {
            output[i] = static_cast<int8_t>(input[i] >> 8);
        }
    }

    static void ConvertSampleToPcm24(Span<uint8_t> output, Span<const int16_t> input) {
        for (size_t i = 0; i < input.Length; i++) {
            output[i * 3 + 2] = static_cast<uint8_t>(input[i] >> 8);
            output[i * 3 + 1] = static_cast<uint8_t>(input[i] & 0xff);
            output[i * 3 + 0] = 0;
        }
    }

    static void ConvertSampleToPcm32(Span<int32_t> output, Span<const int16_t> input) {
        for (size_t i = 0; i < input.Length; i++) {
            output[i] = static_cast<int32_t>(input[i]) * 65536;  // C# input[i] << 16 (no overflow for an int16 value)
        }
    }

    static void ConvertSampleToPcmFloat(Span<float> output, Span<const int16_t> input) {
        for (size_t i = 0; i < input.Length; i++) {
            output[i] = ConvertSampleToPcmFloat(input[i]);
        }
    }

    static int32_t Decode(Span<int16_t> output, Span<const int16_t> input, int32_t startSampleOffset, int32_t endSampleOffset, int32_t channelIndex, int32_t channelCount) {
        if (input.IsEmpty() || endSampleOffset < startSampleOffset) return 0;

        int32_t decodedCount = static_cast<int32_t>(input.Length) / channelCount;
        for (int32_t i = 0; i < decodedCount; i++) {
            output[i] = input[static_cast<size_t>(i * channelCount + channelIndex)];
        }
        return decodedCount;
    }

    static int32_t Decode(Span<int16_t> output, Span<const float> input, int32_t startSampleOffset, int32_t endSampleOffset, int32_t channelIndex, int32_t channelCount) {
        if (input.IsEmpty() || endSampleOffset < startSampleOffset) return 0;

        int32_t decodedCount = static_cast<int32_t>(input.Length) / channelCount;
        for (int32_t i = 0; i < decodedCount; i++) {
            output[i] = ConvertSampleToPcmInt16(input[static_cast<size_t>(i * channelCount + channelIndex)]);
        }
        return decodedCount;
    }

    // C# (short) cast after clamping. NaN (no C# equivalent in the clamps) maps to 0 to avoid undefined behaviour in C++.
    static int16_t Saturate(float value) {
        if (value > 32767.0f) return 32767;
        if (value < -32768.0f) return -32768;
        if (value != value) return 0;
        return static_cast<int16_t>(value);
    }
};

}  // namespace nxrt::audren
