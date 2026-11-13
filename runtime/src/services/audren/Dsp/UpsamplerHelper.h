// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/UpsamplerHelper.cs (Ryubing fork, MIT). Scalar filter-bank loop only: the C#
// System.Numerics Vector<float> block path is not ported (same result up to float summation order).
//
// UpsamplerBufferState is ported here because Ryujinx keeps it in Renderer/Server/Upsampler/UpsamplerBufferState.cs, which
// is outside this file's scope. Only the fields and HistoryLength are needed; the C# Array20<float> is a std::array<float, 20>.
#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include "../audren_common.h"

namespace nxrt::audren {

// Renderer/Server/Upsampler/UpsamplerBufferState.cs (C# struct, no size stated).
struct UpsamplerBufferState {
    static constexpr int32_t HistoryLength = 20;

    float Scale = 0.0f;
    std::array<float, HistoryLength> History{};
    bool Initialized = false;
    int32_t Phase = 0;
};

class UpsamplerHelper {
  private:
    static constexpr int32_t HistoryLength = UpsamplerBufferState::HistoryLength;
    static constexpr int32_t FilterBankLength = 20;
    // Bank0 = [0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    static constexpr int32_t Bank0CenterIndex = 9;

    static std::array<float, FilterBankLength> PrecomputeFilterBank(float offset) {
        auto Sinc = [](float x) -> float {
            if (x == 0) {
                return 1.0f;
            }
            return std::sin(3.14159274f * x) / (3.14159274f * x);
        };

        auto BlackmanWindow = [](float x) -> float {
            constexpr float A = 0.18f;
            constexpr float A0 = 0.5f - 0.5f * A;
            constexpr float A1 = -0.5f;
            constexpr float A2 = 0.5f * A;
            constexpr float PI = 3.14159274f;
            return A0 + A1 * std::cos(2 * PI * x) + A2 * std::cos(4 * PI * x);
        };

        std::array<float, FilterBankLength> result{};

        for (int32_t i = 0; i < FilterBankLength; i++) {
            float x = static_cast<float>(Bank0CenterIndex - i) + offset;
            result[i] = Sinc(x) * BlackmanWindow(x / static_cast<float>(FilterBankLength) + 0.5f);
        }

        return result;
    }

    static inline const std::array<float, FilterBankLength> _bank1 = PrecomputeFilterBank(1.0f / 6.0f);
    static inline const std::array<float, FilterBankLength> _bank2 = PrecomputeFilterBank(2.0f / 6.0f);
    static inline const std::array<float, FilterBankLength> _bank3 = PrecomputeFilterBank(3.0f / 6.0f);
    static inline const std::array<float, FilterBankLength> _bank4 = PrecomputeFilterBank(4.0f / 6.0f);
    static inline const std::array<float, FilterBankLength> _bank5 = PrecomputeFilterBank(5.0f / 6.0f);

    static float DoFilterBank(UpsamplerBufferState& state, const std::array<float, FilterBankLength>& bank) {
        float result = 0.0f;

        NXAUDREN_ASSERT(HistoryLength == FilterBankLength);
        const float* bankSpan = bank.data();
        const float* historySpan = state.History.data();

        for (int32_t curIdx = 0; curIdx < FilterBankLength; curIdx++) {
            result += bankSpan[curIdx] * historySpan[curIdx];
        }

        return result;
    }

    static void NextInput(UpsamplerBufferState& state, float input) {
        // History[1..] -> History[0..]
        std::memmove(state.History.data(), state.History.data() + 1, sizeof(float) * (HistoryLength - 1));
        state.History[static_cast<size_t>(HistoryLength - 1)] = input;
    }

  public:
    // Polyphase upsampling algorithm
    static void Upsample(Span<float> outputBuffer, Span<const float> inputBuffer, int32_t outputSampleCount, int32_t inputSampleCount,
                         UpsamplerBufferState& state) {
        if (!state.Initialized) {
            if (inputSampleCount == 40) {
                state.Scale = 6.0f;
            } else if (inputSampleCount == 80) {
                state.Scale = 3.0f;
            } else if (inputSampleCount == 160) {
                state.Scale = 1.5f;
            } else {
                NXAUDREN_FATAL("UpsamplerHelper.Upsample: inputSampleCount must be 40, 80 or 160");
            }
            state.Initialized = true;
        }

        if (outputSampleCount == 0) {
            return;
        }

        int32_t inputBufferIndex = 0;

        if (state.Scale == 6.0f) {
            for (int32_t i = 0; i < outputSampleCount; i++) {
                switch (state.Phase) {
                    case 0:
                        NextInput(state, inputBuffer[static_cast<size_t>(inputBufferIndex++)]);
                        outputBuffer[i] = state.History[static_cast<size_t>(Bank0CenterIndex)];
                        break;
                    case 1:
                        outputBuffer[i] = DoFilterBank(state, _bank1);
                        break;
                    case 2:
                        outputBuffer[i] = DoFilterBank(state, _bank2);
                        break;
                    case 3:
                        outputBuffer[i] = DoFilterBank(state, _bank3);
                        break;
                    case 4:
                        outputBuffer[i] = DoFilterBank(state, _bank4);
                        break;
                    case 5:
                        outputBuffer[i] = DoFilterBank(state, _bank5);
                        break;
                }

                state.Phase = (state.Phase + 1) % 6;
            }
        } else if (state.Scale == 3.0f) {
            for (int32_t i = 0; i < outputSampleCount; i++) {
                switch (state.Phase) {
                    case 0:
                        NextInput(state, inputBuffer[static_cast<size_t>(inputBufferIndex++)]);
                        outputBuffer[i] = state.History[static_cast<size_t>(Bank0CenterIndex)];
                        break;
                    case 1:
                        outputBuffer[i] = DoFilterBank(state, _bank2);
                        break;
                    case 2:
                        outputBuffer[i] = DoFilterBank(state, _bank4);
                        break;
                }

                state.Phase = (state.Phase + 1) % 3;
            }
        } else if (state.Scale == 1.5f) {
            // Upsample by 3 then decimate by 2.
            for (int32_t i = 0; i < outputSampleCount; i++) {
                switch (state.Phase) {
                    case 0:
                        NextInput(state, inputBuffer[static_cast<size_t>(inputBufferIndex++)]);
                        outputBuffer[i] = state.History[static_cast<size_t>(Bank0CenterIndex)];
                        break;
                    case 1:
                        outputBuffer[i] = DoFilterBank(state, _bank4);
                        break;
                    case 2:
                        NextInput(state, inputBuffer[static_cast<size_t>(inputBufferIndex++)]);
                        outputBuffer[i] = DoFilterBank(state, _bank2);
                        break;
                }

                state.Phase = (state.Phase + 1) % 3;
            }
        } else {
            NXAUDREN_FATAL("UpsamplerHelper.Upsample: unsupported state.Scale");
        }
    }
};

}  // namespace nxrt::audren
