// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// IAudioDevice output volume (audio.cpp, audio_device.h; nxnative audio.md "Audio device output volume and names"): the volume a game
// sets for the active output device reads back and scales what the host outputs play; another device's volume is kept for that device.
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio_device.h"
#include "audio_mixbus.h"
#include "audren/Integration/HostAudioDevice.h"
#include "nxrt/audio_sink.h"
#include "nxrt/memory.h"
#include "svc_test.h"

using namespace svctest;
namespace ad = nxrt::services::audio_device;

namespace {

struct Capture : nxrt::audio::AudioSink {
    std::vector<int16_t> got;
    bool open(uint32_t, uint32_t) override { return true; }
    void close() override {}
    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        got.insert(got.end(), pcm, pcm + frames * 2);
        return frames;
    }
    uint32_t queued_frames() override { return 0; }
    const char* name() const override { return "capture"; }
};

Session device() {
    Session s = open("audren:u");
    return s.object(s.call(2, Req().put<uint64_t>(0).with_pid()));   // GetAudioDeviceService
}

struct Name {
    char s[0x100] = {};
    explicit Name(const std::string& n) { std::strncpy(s, n.c_str(), sizeof s - 1); }
};

uint32_t set_volume(const Session& d, const std::string& name, float v) {
    Name n(name);
    return d.call(1, Req().put<float>(v).buf(A(n.s, sizeof n.s))).result;   // SetAudioDeviceOutputVolume
}

float get_volume(const Session& d, const std::string& name) {
    Name n(name);
    return d.call(2, Req().buf(A(n.s, sizeof n.s))).get<float>(0);   // GetAudioDeviceOutputVolume
}

std::string active(const Session& d) {
    char n[0x100] = {};
    d.call(3, Req().buf(B(n, sizeof n)));   // GetActiveAudioDeviceName
    return n;
}

}  // namespace

SVC_TEST(audio_device, output_volume_scales_the_output) {
    reset();
    Session d = device();
    const std::string name = active(d);
    CHECK(!name.empty());
    CHECK(ad::output_volume() == 1.0f);
    CHECK_EQ(set_volume(d, name, 0.5f), 0u);
    CHECK(get_volume(d, name) == 0.5f);
    CHECK(ad::output_volume() == 0.5f);
    const std::string other = name == "AudioTvOutput" ? "AudioBuiltInSpeakerOutput" : "AudioTvOutput";
    CHECK_EQ(set_volume(d, other, 0.25f), 0u);
    CHECK(get_volume(d, other) == 0.25f);
    CHECK(ad::output_volume() == 0.5f);   // the active device's

    // the audio renderer's host output plays at that volume
    const std::shared_ptr<nxrt::audio::AudioSink> before = nxrt::audio::sink();
    auto cap = std::make_shared<Capture>();
    nxrt::audio::set_sink(cap);
    {
        nxrt::audren::HostAudioDevice dev(48000, 2);
        const int16_t pcm[4] = {1000, -2000, 32767, -32768};
        dev.AppendBuffer(nxrt::audren::Span<const int16_t>(pcm, 4), 2);
    }
    CHECK_EQ(cap->got.size(), 4u);
    if (cap->got.size() == 4) {
        CHECK_EQ(cap->got[0], 500);
        CHECK_EQ(cap->got[1], -1000);
        CHECK_EQ(cap->got[2], 16383);
        CHECK_EQ(cap->got[3], -16384);
    }
    nxrt::audio::set_sink(before);

    CHECK_EQ(set_volume(d, name, 3.0f), 0u);   // out of range: read back as set, played at full volume
    CHECK(ad::output_volume() == 1.0f);
    reset();
    CHECK(ad::output_volume() == 1.0f);
}

// Two renderers at once (nxnative audio.md "A game with more than one renderer"): the host sink plays one stream, so the first device's
// blocks go to the sink with the second one's mixed in (through the mix bus), and the sink is opened once, by the first.
namespace {
struct CountingCapture : Capture {
    int opens = 0;
    bool open(uint32_t rate, uint32_t channels) override {
        ++opens;
        return rate == 48000 && channels == 2;
    }
};
}  // namespace

SVC_TEST(audio_device, two_renderers_share_the_sink) {
    reset();
    nxrt::audio::mixbus::reset();
    const std::shared_ptr<nxrt::audio::AudioSink> before = nxrt::audio::sink();
    auto cap = std::make_shared<CountingCapture>();
    nxrt::audio::set_sink(cap);
    {
        nxrt::audren::HostAudioDevice first(48000, 2), second(48000, 2);
        std::vector<int16_t> a(240 * 2, 1000), b(240 * 2, 300);
        first.AppendBuffer(nxrt::audren::Span<const int16_t>(a.data(), a.size()), 2);    // the first owns the sink
        second.AppendBuffer(nxrt::audren::Span<const int16_t>(b.data(), b.size()), 2);   // the second's frames wait in the mix bus
        CHECK_EQ(cap->got.size(), a.size());                                              // the sink has the first block only
        first.AppendBuffer(nxrt::audren::Span<const int16_t>(a.data(), a.size()), 2);    // ... and the next one with the second's mixed in
        CHECK_EQ(cap->got.size(), a.size() * 2);
        if (cap->got.size() == a.size() * 2) {
            CHECK_EQ(cap->got[0], 1000);
            CHECK_EQ(cap->got[a.size()], 1300);
            CHECK_EQ(cap->got[a.size() * 2 - 1], 1300);
        }
        CHECK_EQ(cap->opens, 1);                                                          // one stream on the sink
    }
    nxrt::audio::set_sink(before);
    nxrt::audio::mixbus::reset();
}

// Several outputs at once (nxnative audio.md "Several outputs at once"; audio_mixbus.h): the mix bus adds its voices together. With no
// renderer it writes the sink itself, and a voice that is ahead waits for the others instead of playing first.
namespace {
struct ClockedCapture : Capture {   // a device clock with room for everything: the bus's own timing alone decides
    bool has_device_clock() const override { return true; }
};
}  // namespace

SVC_TEST(audio_device, mix_bus_voices_are_mixed) {
    namespace bus = nxrt::audio::mixbus;
    bus::reset();
    const std::shared_ptr<nxrt::audio::AudioSink> before = nxrt::audio::sink();
    auto cap = std::make_shared<ClockedCapture>();
    nxrt::audio::set_sink(cap);
    const uint32_t a = bus::open_voice("a"), b = bus::open_voice("b");
    std::vector<int16_t> pa(240 * 2, 1000), pb(240 * 2, 300);
    bus::push(a, pa.data(), 240);
    CHECK(cap->got.empty());                                  // b is playing too (just opened): a waits for it
    bus::push(b, pb.data(), 240);
    CHECK_EQ(cap->got.size(), pa.size());                     // both at once
    if (cap->got.size() == pa.size()) {
        CHECK_EQ(cap->got[0], 1300);
        CHECK_EQ(cap->got.back(), 1300);
    }
    bus::close_voice(b);
    bus::push(a, pa.data(), 240);                             // alone now
    CHECK_EQ(cap->got.size(), pa.size() * 2);
    if (cap->got.size() == pa.size() * 2) CHECK_EQ(cap->got.back(), 1000);
    nxrt::audio::set_sink(before);
    bus::reset();
}

// Two IAudioOut streams started together play mixed, through the bus (audio_out.cpp shared_output), not one after the other.
SVC_TEST(audio_device, two_audio_outs_are_mixed) {
    namespace bus = nxrt::audio::mixbus;
    reset();
    bus::reset();
    bus::set_idle_ms_for_tests(10000);                        // a late pacing thread on a loaded machine does not decide the order
    const std::shared_ptr<nxrt::audio::AudioSink> before = nxrt::audio::sink();
    auto cap = std::make_shared<ClockedCapture>();
    nxrt::audio::set_sink(cap);
    {
        char name_in[0x100] = {}, name_out[0x100] = {};
        Session root = open("audout:u");
        auto open_out = [&] {
            return root.object(root.call(1, Req().put<uint32_t>(48000).put<uint16_t>(2).put<uint16_t>(0).put<uint64_t>(0).with_pid()
                                             .buf(A(name_in, sizeof name_in)).buf(B(name_out, sizeof name_out))));   // OpenAudioOut
        };
        Session o1 = open_out(), o2 = open_out();
        CHECK(o1.valid() && o2.valid());
        CHECK_SHAPE(o1.call(1), 0);                           // Start
        CHECK_SHAPE(o2.call(1), 0);
        constexpr uint32_t kFrames = 480;
        auto append = [&](Session& o, int16_t value) {
            const uint64_t samples = nxrt::mem::alloc_kernel(kFrames * 4, 0x1000);
            const uint64_t desc = nxrt::mem::alloc_kernel(0x28, 0x1000);
            int16_t* p = reinterpret_cast<int16_t*>(samples);
            for (uint32_t i = 0; i < kFrames * 2; ++i) p[i] = value;
            const uint64_t d[5] = {0, samples, kFrames * 4, kFrames * 4, 0};
            std::memcpy(reinterpret_cast<void*>(desc), d, sizeof d);
            return o.call(3, Req().put<uint64_t>(value).buf(A(reinterpret_cast<void*>(desc), 0x28)));   // AppendAudioOutBuffer
        };
        CHECK_SHAPE(append(o1, 1000), 0);
        CHECK_SHAPE(append(o2, 300), 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (cap->got.size() < kFrames * 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK_EQ(cap->got.size(), size_t{kFrames} * 2);      // 480 stereo frames: the two buffers at once
        if (cap->got.size() == size_t{kFrames} * 2) {
            CHECK_EQ(cap->got[0], 1300);
            CHECK_EQ(cap->got.back(), 1300);
        }
        CHECK_SHAPE(o1.call(2), 0);                           // Stop
        CHECK_SHAPE(o2.call(2), 0);
    }
    nxrt::audio::set_sink(before);
    bus::set_idle_ms_for_tests(50);
    bus::reset();
}
