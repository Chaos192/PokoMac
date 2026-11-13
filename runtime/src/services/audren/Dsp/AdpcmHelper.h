// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/AdpcmHelper.cs (Ryubing fork, MIT). Static class -> struct with static members.
//
// The C# int arithmetic wraps on overflow (unchecked context). The helpers WrapAdd/WrapMul/Shl reproduce that with uint32_t,
// because signed overflow is undefined in C++.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "../audren_common.h"
#include "../audren_types.h"
#include "FixedPointHelper.h"

namespace nxrt::audren {

struct AdpcmHelper {
    static constexpr int32_t FixedPointPrecision = 11;
    static constexpr int32_t SamplesPerFrame = 14;
    static constexpr int32_t NibblesPerFrame = SamplesPerFrame + 2;
    static constexpr int32_t BytesPerFrame = 8;

    static uint32_t GetAdpcmDataSize(int32_t sampleCount) {
        NXAUDREN_ASSERT(sampleCount >= 0);

        int32_t frames = sampleCount / SamplesPerFrame;
        int32_t extraSize = 0;

        if ((sampleCount % SamplesPerFrame) != 0) {
            extraSize = (sampleCount % SamplesPerFrame) / 2 + 1 + (sampleCount % 2);
        }

        return static_cast<uint32_t>(BytesPerFrame * frames + extraSize);
    }

    static int32_t GetAdpcmOffsetFromSampleOffset(int32_t sampleOffset) {
        NXAUDREN_ASSERT(sampleOffset >= 0);
        return GetNibblesFromSampleCount(sampleOffset) / 2;
    }

    static int32_t NibbleToSample(int32_t nibble) {
        int32_t frames = nibble / NibblesPerFrame;
        int32_t extraNibbles = nibble % NibblesPerFrame;
        int32_t samples = SamplesPerFrame * frames;

        return samples + extraNibbles - 2;
    }

    static int32_t GetNibblesFromSampleCount(int32_t sampleCount) {
        uint8_t headerSize = 0;

        if ((sampleCount % SamplesPerFrame) != 0) {
            headerSize = 2;
        }

        return sampleCount % SamplesPerFrame + NibblesPerFrame * (sampleCount / SamplesPerFrame) + headerSize;
    }

    static int16_t Saturate(int32_t value) {
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        return static_cast<int16_t>(value);
    }

    static int16_t GetCoefficientAtIndex(Span<const int16_t> coefficients, int32_t index) {
        if (static_cast<uint32_t>(index) >= static_cast<uint32_t>(coefficients.Length)) {
            std::fprintf(stderr, "[audren] Out of bound read for coefficient at index %d\n", index);
            return 0;
        }

        return coefficients[static_cast<size_t>(index)];
    }

    static int32_t Decode(Span<int16_t> output, Span<const uint8_t> input, int32_t startSampleOffset, int32_t endSampleOffset, int32_t offset, int32_t count, Span<const int16_t> coefficients, AdpcmLoopContext& loopContext) {
        if (input.IsEmpty() || endSampleOffset < startSampleOffset) {
            return 0;
        }

        uint8_t predScale = static_cast<uint8_t>(loopContext.pred_scale);
        uint8_t scale = static_cast<uint8_t>(predScale & 0xF);
        uint8_t coefficientIndex = static_cast<uint8_t>((predScale >> 4) & 0xF);
        int16_t history0 = loopContext.history0;
        int16_t history1 = loopContext.history1;
        int16_t coefficient0 = GetCoefficientAtIndex(coefficients, coefficientIndex * 2 + 0);
        int16_t coefficient1 = GetCoefficientAtIndex(coefficients, coefficientIndex * 2 + 1);

        int32_t decodedCount = std::min(count, endSampleOffset - startSampleOffset - offset);
        int32_t nibbles = GetNibblesFromSampleCount(offset + startSampleOffset);
        int32_t remaining = decodedCount;
        int32_t outputBufferIndex = 0;
        int32_t inputIndex = 0;

        Span<const uint8_t> targetInput = input.Slice(static_cast<size_t>(nibbles / 2));

        while (remaining > 0) {
            int32_t samplesCount;

            if ((static_cast<uint32_t>(nibbles) % NibblesPerFrame) == 0) {
                predScale = targetInput[static_cast<size_t>(inputIndex++)];

                scale = static_cast<uint8_t>(predScale & 0xF);

                coefficientIndex = static_cast<uint8_t>((predScale >> 4) & 0xF);

                coefficient0 = GetCoefficientAtIndex(coefficients, coefficientIndex * 2);
                coefficient1 = GetCoefficientAtIndex(coefficients, coefficientIndex * 2 + 1);

                nibbles += 2;

                samplesCount = std::min(remaining, SamplesPerFrame);
            } else {
                samplesCount = 1;
            }

            int32_t scaleFixedPoint = static_cast<int32_t>(static_cast<uint32_t>(FixedPointHelper::ToFixed(1.0f, FixedPointPrecision)) << scale);

            if (samplesCount < SamplesPerFrame) {
                for (int32_t i = 0; i < samplesCount; i++) {
                    int32_t value = targetInput[static_cast<size_t>(inputIndex)];

                    int32_t sample;

                    if ((nibbles & 1) != 0) {
                        sample = Sar(Shl(value, 28), 28);

                        inputIndex++;
                    } else {
                        sample = Sar(Shl(value, 24), 28);
                    }

                    nibbles++;

                    int32_t prediction = WrapAdd(coefficient0 * history0, coefficient1 * history1);

                    sample = FixedPointHelper::RoundUpAndToInt(WrapAdd(WrapMul(sample, scaleFixedPoint), prediction), FixedPointPrecision);

                    int16_t saturatedSample = Saturate(sample);

                    history1 = history0;
                    history0 = saturatedSample;

                    output[static_cast<size_t>(outputBufferIndex++)] = saturatedSample;

                    remaining--;
                }
            } else {
                for (int32_t i = 0; i < SamplesPerFrame / 2; i++) {
                    int32_t value = targetInput[static_cast<size_t>(inputIndex)];

                    int32_t sample0;
                    int32_t sample1;

                    sample0 = Sar(Shl(value, 24), 28);
                    sample1 = Sar(Shl(value, 28), 28);

                    inputIndex++;

                    int32_t prediction0 = WrapAdd(coefficient0 * history0, coefficient1 * history1);
                    sample0 = FixedPointHelper::RoundUpAndToInt(WrapAdd(WrapMul(sample0, scaleFixedPoint), prediction0), FixedPointPrecision);
                    int16_t saturatedSample0 = Saturate(sample0);

                    int32_t prediction1 = WrapAdd(coefficient0 * saturatedSample0, coefficient1 * history0);
                    sample1 = FixedPointHelper::RoundUpAndToInt(WrapAdd(WrapMul(sample1, scaleFixedPoint), prediction1), FixedPointPrecision);
                    int16_t saturatedSample1 = Saturate(sample1);

                    history1 = saturatedSample0;
                    history0 = saturatedSample1;

                    output[static_cast<size_t>(outputBufferIndex++)] = saturatedSample0;
                    output[static_cast<size_t>(outputBufferIndex++)] = saturatedSample1;
                }

                nibbles += SamplesPerFrame;
                remaining -= SamplesPerFrame;
            }
        }

        loopContext.pred_scale = static_cast<int16_t>(predScale);
        loopContext.history0 = history0;
        loopContext.history1 = history1;

        return decodedCount;
    }

  private:
    static int32_t Shl(int32_t value, int32_t shift) { return static_cast<int32_t>(static_cast<uint32_t>(value) << shift); }
    static int32_t Sar(int32_t value, int32_t shift) { return value >> shift; }  // arithmetic shift (C++20 defines it)
    static int32_t WrapAdd(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
    static int32_t WrapMul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
};

}  // namespace nxrt::audren
