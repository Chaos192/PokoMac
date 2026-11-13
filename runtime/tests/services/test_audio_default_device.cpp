// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The XAudio2 sink follows a change of the system's default output device (audio_xaudio2.cpp EndpointWatch): the endpoint notification
// flags it as a loss when the sink plays on the default device, the writes go to the silent stand-in meanwhile, and the device is opened
// again on the new default (here the same one: the test cannot change the system's default) within a second. Windows with an output
// device only; elsewhere, or with no device, there is nothing to follow.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "svc_test.h"

using namespace svctest;

SVC_TEST(audio_xaudio2, follows_a_default_device_change) {
#ifdef _WIN32
    nxrt::config_set("audio", "device", "default");
    auto sink = nxrt::audio::make_xaudio2_sink();
    if (!sink || !sink->open(48000, 2)) {
        std::printf("  (no XAudio2 output device here: nothing to follow)\n");
        return;
    }
    std::vector<int16_t> silence(480 * 2, 0);   // 10 ms
    CHECK(sink->write(silence.data(), 480) > 0);
    CHECK_EQ(sink->stats().device_reopens, 0u);
    nxrt::audio::xaudio2_default_device_changed(*sink);
    // the writes go on (to the stand-in, then to the device opened again): every one is accepted, and the device is back within a second
    bool accepted = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sink->stats().device_reopens == 0 && std::chrono::steady_clock::now() < end) {
        accepted &= sink->write(silence.data(), 480) > 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(accepted);
    CHECK_EQ(sink->stats().device_reopens, 1u);
    CHECK(sink->write(silence.data(), 480) > 0);
    sink->close();
#endif
}
