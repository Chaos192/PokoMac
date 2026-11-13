// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The audio renderer's threads and the real-time band (audren/Integration/ThreadBoost.h): macOS puts them there only with
// [audio] realtime_band = true; by default they keep the user-interactive QoS class "Pro Audio" gives them. Other hosts: nothing to check.
#include <thread>

#include "audren/Integration/ThreadBoost.h"
#include "nxrt/config.h"
#include "svc_test.h"

using namespace svctest;

namespace {

#if defined(__APPLE__)
bool in_realtime_band() {
    thread_time_constraint_policy_data_t p{};
    mach_msg_type_number_t n = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t is_default = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p), &n,
                          &is_default) != KERN_SUCCESS)
        return false;
    return !is_default;   // a thread without the policy reads the defaults
}

// BoostAudioThread on a new thread, as the renderer's threads call it: whether that thread ended up in the band
bool boosted_thread_in_band() {
    bool in = false;
    std::thread([&in] {
        nxrt::audren::BoostAudioThread();
        in = in_realtime_band();
    }).join();
    return in;
}
#endif

}  // namespace

SVC_TEST(audio_thread, realtime_band_off_by_default) {
#if defined(__APPLE__)
    if (!nxrt::config_has("audio", "realtime_band")) CHECK(!boosted_thread_in_band());   // the default
    nxrt::config_set("audio", "realtime_band", "false");
    CHECK(!boosted_thread_in_band());
    nxrt::config_set("audio", "realtime_band", "true");
    CHECK(boosted_thread_in_band());
    nxrt::config_set("audio", "realtime_band", "false");
#else
    CHECK(true);
#endif
}
