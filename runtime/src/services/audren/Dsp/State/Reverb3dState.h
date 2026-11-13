// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Dsp/State/Reverb3dState.cs (Ryubing fork, MIT).
// C# IDelayLine[] / DecayDelay / IDelayLine fields become owning unique_ptr / DecayDelay values (see report).
#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../Effect/DecayDelay.h"
#include "../Effect/DelayLineReverb3d.h"
#include "../Effect/IDelayLine.h"
#include "../FloatingPointHelper.h"

namespace nxrt::audren {

struct Reverb3dState {
    std::vector<float> _fdnDelayMinTimes = {5.0f, 6.0f, 13.0f, 14.0f};
    std::vector<float> _fdnDelayMaxTimes = {45.704f, 82.782f, 149.94f, 271.58f};
    std::vector<float> _decayDelayMaxTimes1 = {17.0f, 13.0f, 9.0f, 7.0f};
    std::vector<float> _decayDelayMaxTimes2 = {19.0f, 11.0f, 10.0f, 6.0f};
    std::vector<float> _earlyDelayTimes = {
        0.017136f, 0.059154f, 0.16173f, 0.39019f, 0.42526f, 0.45541f, 0.68974f, 0.74591f, 0.83384f, 0.8595f,
        0.0f, 0.075024f, 0.16879f, 0.2999f, 0.33744f, 0.3719f, 0.59901f, 0.71674f, 0.81786f, 0.85166f};
    std::vector<float> EarlyGain = {
        0.67096f, 0.61027f, 1.0f, 0.35680f, 0.68361f, 0.65978f, 0.51939f, 0.24712f, 0.45945f, 0.45021f,
        0.64196f, 0.54879f, 0.92925f, 0.38270f, 0.72867f, 0.69794f, 0.5464f, 0.24563f, 0.45214f, 0.44042f};

    std::vector<std::unique_ptr<IDelayLine>> FdnDelayLines;
    std::vector<DecayDelay> DecayDelays1;
    std::vector<DecayDelay> DecayDelays2;
    std::unique_ptr<IDelayLine> PreDelayLine;
    std::unique_ptr<IDelayLine> FrontCenterDelayLine;
    float DryGain = 0.0f;
    std::vector<uint32_t> EarlyDelayTime;
    float PreviousPreDelayValue = 0.0f;
    float PreviousPreDelayGain = 0.0f;
    float TargetPreDelayGain = 0.0f;
    float EarlyReflectionsGain = 0.0f;
    float LateReverbGain = 0.0f;
    uint32_t ReflectionDelayTime = 0;
    float EchoLateReverbDecay = 0.0f;
    std::vector<float> DecayDirectFdnGain;
    std::vector<float> DecayCurrentFdnGain;
    std::vector<float> DecayCurrentOutputGain;
    std::vector<float> PreviousFeedbackOutputDecayed;

    Reverb3dState(Reverb3dParameter& parameter, CpuAddress workBuffer) {
        (void)workBuffer;
        DecayDirectFdnGain.assign(4, 0.0f);
        DecayCurrentFdnGain.assign(4, 0.0f);
        DecayCurrentOutputGain.assign(4, 0.0f);
        PreviousFeedbackOutputDecayed.assign(4, 0.0f);

        uint32_t sampleRate = parameter.sample_rate / 1000;

        for (size_t i = 0; i < 4; i++) {
            FdnDelayLines.push_back(std::make_unique<DelayLine3d>(sampleRate, _fdnDelayMaxTimes[i]));
            DecayDelays1.emplace_back(std::make_unique<DelayLine3d>(sampleRate, _decayDelayMaxTimes1[i]));
            DecayDelays2.emplace_back(std::make_unique<DelayLine3d>(sampleRate, _decayDelayMaxTimes2[i]));
        }

        PreDelayLine = std::make_unique<DelayLine3d>(sampleRate, 400.0f);
        FrontCenterDelayLine = std::make_unique<DelayLine3d>(sampleRate, 5.0f);

        UpdateParameter(parameter);
    }

    void UpdateParameter(Reverb3dParameter& parameter) {
        uint32_t sampleRate = parameter.sample_rate / 1000;

        EarlyDelayTime = std::vector<uint32_t>(20, 0);
        DryGain = parameter.dry_gain;
        std::fill(PreviousFeedbackOutputDecayed.begin(), PreviousFeedbackOutputDecayed.end(), 0.0f);
        PreviousPreDelayValue = 0;

        EarlyReflectionsGain = FloatingPointHelper::Pow10(std::min(parameter.room_gain + parameter.reflections_gain, 5000.0f) / 2000.0f);
        LateReverbGain = FloatingPointHelper::Pow10(std::min(parameter.room_gain + parameter.reverb_gain, 5000.0f) / 2000.0f);

        float highFrequencyRoomGain = FloatingPointHelper::Pow10(parameter.room_hf / 2000.0f);

        if (highFrequencyRoomGain < 1.0f) {
            float tempA = 1.0f - highFrequencyRoomGain;
            float tempB = 2.0f - ((2.0f * highFrequencyRoomGain) * FloatingPointHelper::Cos(256.0f * parameter.hf_reference / static_cast<float>(parameter.sample_rate)));
            float tempC = std::sqrt(std::pow(tempB, 2.0f) - (4.0f * (1.0f - highFrequencyRoomGain) * (1.0f - highFrequencyRoomGain)));

            PreviousPreDelayGain = (tempB - tempC) / (2.0f * tempA);
            TargetPreDelayGain = 1.0f - PreviousPreDelayGain;
        } else {
            PreviousPreDelayGain = 0.0f;
            TargetPreDelayGain = 1.0f;
        }

        ReflectionDelayTime = IDelayLine::GetSampleCount(sampleRate, 1000.0f * (parameter.reflection_delay + parameter.reverb_delay_time));
        EchoLateReverbDecay = 0.6f * parameter.diffusion * 0.01f;

        for (size_t i = 0; i < FdnDelayLines.size(); i++) {
            FdnDelayLines[i]->SetDelay(_fdnDelayMinTimes[i] + (parameter.density / 100.0f * (_fdnDelayMaxTimes[i] - _fdnDelayMinTimes[i])));

            uint32_t tempSampleCount = FdnDelayLines[i]->CurrentSampleCount() + DecayDelays1[i].CurrentSampleCount() + DecayDelays2[i].CurrentSampleCount();

            float tempA = (-60.0f * static_cast<float>(tempSampleCount)) / (parameter.decay_time * static_cast<float>(parameter.sample_rate));
            float tempB = tempA / parameter.hf_decay_ratio;
            float tempC = FloatingPointHelper::Cos(128.0f * 0.5f * parameter.hf_reference / static_cast<float>(parameter.sample_rate)) / FloatingPointHelper::Sin(128.0f * 0.5f * parameter.hf_reference / static_cast<float>(parameter.sample_rate));
            float tempD = FloatingPointHelper::Pow10((tempB - tempA) / 40.0f);
            float tempE = FloatingPointHelper::Pow10((tempB + tempA) / 40.0f) * 0.7071f;

            DecayDirectFdnGain[i] = tempE * ((tempD * tempC) + 1.0f) / (tempC + tempD);
            DecayCurrentFdnGain[i] = tempE * (1.0f - (tempD * tempC)) / (tempC + tempD);
            DecayCurrentOutputGain[i] = (tempC - tempD) / (tempC + tempD);

            DecayDelays1[i].SetDecayRate(EchoLateReverbDecay);
            DecayDelays2[i].SetDecayRate(EchoLateReverbDecay * -0.9f);
        }

        for (size_t i = 0; i < EarlyDelayTime.size(); i++) {
            uint32_t sampleCount = std::min(IDelayLine::GetSampleCount(sampleRate, (parameter.reflection_delay * 1000.0f) + (_earlyDelayTimes[i] * 1000.0f * ((parameter.reverb_delay_time * 0.9998f) + 0.02f))), PreDelayLine->SampleCountMax());
            EarlyDelayTime[i] = sampleCount;
        }
    }
};

}  // namespace nxrt::audren
