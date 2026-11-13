// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Touch gestures (nxnative services.md "Touch gestures recognized from the touch points"; services/hid_gesture.h): the recognizer's
// rules on made-up touch sequences, and the gesture lifo at hid shared memory 0x3BA00 filled from the touch samples.
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "hid_gesture.h"
#include "hid_shm.h"
#include "nxrt/input_source.h"
#include "svc_test.h"

using namespace svctest;
namespace hs = nxrt::services::hidshm;
namespace hg = nxrt::services::hidgesture;

namespace {

constexpr int64_t kMs = 1000000;
constexpr int64_t kBase = 10000;   // ms: a clock well past 0, as the host's (the first tap is not a double tap of time 0)

struct Pt {
    uint32_t x, y;
};

// One sample: the states the recognizer writes for these points at `ms` (none, or one).
struct Driver {
    hg::Recognizer r;
    std::vector<hs::GestureState> out;
    bool step(std::initializer_list<Pt> pts, int64_t ms) {
        hs::TouchState t[4] = {};
        uint32_t n = 0;
        for (const Pt& p : pts) {
            t[n].x = p.x;
            t[n].y = p.y;
            ++n;
        }
        const int64_t ns = (kBase + ms) * kMs;
        r.set_touch(t, n, ns);
        if (!r.needs_update()) return false;
        hs::GestureState g{};
        r.update(g, ns);
        out.push_back(g);
        return true;
    }
    uint32_t type() const { return out.empty() ? 0xFFFFFFFFu : out.back().type; }
};

}  // namespace

SVC_TEST(hid_gesture, tap_and_double_tap) {
    Driver d;
    d.step({}, 0);                                           // the first sample always writes (idle)
    CHECK_EQ(d.type(), hg::kIdle);
    CHECK(!d.step({}, 5));                                   // nothing changes: nothing written
    CHECK(d.step({{100, 200}}, 10));
    CHECK_EQ(d.type(), hg::kTouch);
    CHECK(d.out.back().attributes & hg::kAttrNewTouch);
    CHECK_EQ(d.out.back().pos.x, 100);
    CHECK_EQ(d.out.back().point_count, 1);
    CHECK(d.step({}, 60));                                   // lifted after a touch: a tap
    CHECK_EQ(d.type(), hg::kTap);
    CHECK(!(d.out.back().attributes & hg::kAttrDoubleTap));
    CHECK(d.step({}, 65));                                   // ... then complete
    CHECK_EQ(d.type(), hg::kComplete);
    CHECK(d.step({}, 70));                                   // ... then idle once more, and nothing after it
    CHECK_EQ(d.type(), hg::kIdle);
    CHECK(!d.step({}, 75));
    CHECK(d.step({{102, 201}}, 150));                        // a second tap within 0.35 s
    CHECK(d.step({}, 200));
    CHECK_EQ(d.type(), hg::kTap);
    CHECK(d.out.back().attributes & hg::kAttrDoubleTap);
}

SVC_TEST(hid_gesture, press_after_half_a_second) {
    Driver d;
    d.step({}, 0);
    CHECK(d.step({{640, 360}}, 10));
    CHECK(!d.step({{640, 360}}, 300));                       // held still: nothing yet
    CHECK(d.step({{640, 360}}, 600));                        // 0.5 s after the touch
    CHECK_EQ(d.type(), hg::kPress);
    CHECK(d.step({}, 700));
    CHECK_EQ(d.type(), hg::kComplete);
}

SVC_TEST(hid_gesture, pan_then_swipe) {
    Driver d;
    d.step({}, 0);
    d.step({{100, 300}}, 10);
    CHECK(d.step({{300, 300}}, 60));                         // moved 200 px in 50 ms
    CHECK_EQ(d.type(), hg::kPan);
    CHECK_EQ(d.out.back().delta.x, 200);
    CHECK(std::fabs(d.out.back().vel_x - 4000.0f) < 1.0f);
    CHECK(d.step({}, 110));                                  // lifted fast: a swipe to the right
    CHECK_EQ(d.type(), hg::kSwipe);
    CHECK_EQ(d.out.back().direction, hg::kRight);
    Driver slow;
    slow.step({}, 0);
    slow.step({{100, 300}}, 10);
    slow.step({{110, 300}}, 510);                            // 10 px in 0.5 s
    CHECK_EQ(slow.type(), hg::kPan);
    slow.step({}, 1010);
    CHECK_EQ(slow.type(), hg::kComplete);                    // too slow for a swipe
}

SVC_TEST(hid_gesture, second_finger_cancels_and_pinch) {
    Driver d;
    d.step({}, 0);
    d.step({{500, 360}}, 10);
    CHECK(d.step({{500, 360}, {700, 360}}, 20));             // the number of fingers changed
    CHECK_EQ(d.type(), hg::kCancel);
    Driver p;
    p.step({}, 0);
    p.step({{500, 360}, {700, 360}}, 10);                    // two fingers down together
    CHECK_EQ(p.type(), hg::kTouch);
    CHECK(p.step({{450, 360}, {750, 360}}, 30));             // apart along the same line
    CHECK_EQ(p.type(), hg::kPinch);
    CHECK(p.out.back().scale > 1.4f && p.out.back().scale < 1.6f);
}

// The service: touch samples reach the gesture lifo at 0x3BA00 (the ring the SDK's GetGestureStates reads).
SVC_TEST(hid_gesture, lifo_filled_from_touch_samples) {
    reset();
    nxrt::services::hid_set_background_sampling(false);
    auto src = std::make_shared<nxrt::hid::ManualInputSource>();
    nxrt::hid::set_input_source(src);
    nxrt::services::hid_sample_once();                       // idle
    nxrt::hid::InputSnapshot s;
    s.touch.count = 1;
    s.touch.fingers[0].x = 320;
    s.touch.fingers[0].y = 240;
    src->set(s);
    nxrt::services::hid_sample_once();                       // touch
    uint8_t* shm = nxrt::services::hid_shared_memory_for_tests();
    CHECK(shm != nullptr);
    if (shm) {
        const auto* r = reinterpret_cast<const hs::Ring<hs::GestureState>*>(shm + hs::kGesture);
        CHECK_EQ(r->buffer_count, uint64_t{17});
        CHECK(r->count >= 2);
        const hs::GestureState& g = r->entries[r->index].state;
        CHECK_EQ(g.type, hg::kTouch);
        CHECK_EQ(g.pos.x, 320);
        CHECK_EQ(g.pos.y, 240);
        CHECK_EQ(r->entries[r->index].sampling, 2 * g.sampling);   // the sequence word, as every other lifo here
    }
    nxrt::hid::set_input_source(nullptr);
    nxrt::services::hid_set_background_sampling(true);
}
