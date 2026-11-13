// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The mix bus: the host sink plays one stream, so every producer of sound other than the audio renderer that owns the sink (another
// renderer, the Switch 2 mixer's streams in ext_audsmx.cpp, IAudioOut streams while something else plays: audio_out.cpp) is a voice
// here, and the voices are mixed: added together, not played one after another (nxnative audio.md "Several outputs at once"). While a
// renderer runs, its output (audren/Integration/HostAudioDevice.cpp) adds what the voices hold into each block it writes; while none
// does, the bus mixes the voices and writes the sink itself. Interleaved stereo int16 at 48 kHz.
#pragma once
#include <cstdint>

namespace nxrt::audio::mixbus {

constexpr uint32_t kRate = 48000;

// A producer of its own (never 0). `who` names it in the log.
uint32_t open_voice(const char* who);
void close_voice(uint32_t voice);
// Frames the voice can take now (the bus holds at most ~60 ms ahead of whoever drains it): a producer takes no more than this from its
// source, so it runs at the drain's pace instead of overflowing.
uint32_t room(uint32_t voice);
// Adds `frames` stereo frames to the voice. With no renderer draining the bus, the bus writes the sink: as many frames as every voice
// that is playing (pushed, or was opened, in the last 50 ms) has, mixed, so a voice ahead waits for the others instead of playing first.
void push(uint32_t voice, const int16_t* stereo, uint32_t frames);
// The renderer's side: adds (with saturation) up to `frames` queued frames of every voice into `pcm` (`channels` interleaved: the bus goes
// to the first two), when `rate` is the bus rate. Returns the most frames any voice added.
uint32_t mix_into(int16_t* pcm, uint32_t frames, uint32_t channels, uint32_t rate);
// Whether a renderer drained the bus in the last 50 ms.
bool renderer_active();
// Voices other than `voice` (0: any) that are playing.
uint32_t others_playing(uint32_t voice);
// Tests: drops every voice and forgets the renderer.
void reset();
// Tests: how long a voice counts as playing after its last push or its open (default 50 ms), so a loaded test machine's late thread
// does not decide the order.
void set_idle_ms_for_tests(uint32_t ms);

}  // namespace nxrt::audio::mixbus
