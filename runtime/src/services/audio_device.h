// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The output device volume a game sets (IAudioDevice 1 SetAudioDeviceOutputVolume / 7 ...Auto, audio.cpp): what the game hears goes out
// at the active output device's volume, so the host outputs (the audio renderer's, audren/Integration/HostAudioDevice.cpp; each audio
// out's, audio_out.cpp) scale by it.
#pragma once

namespace nxrt::services::audio_device {

float output_volume();   // the active output device's volume, 0..1 (1 until the game sets one); safe from any thread, takes no lock

}  // namespace nxrt::services::audio_device
