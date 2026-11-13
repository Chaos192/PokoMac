// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Touch gestures: a recognizer that turns the touch screen samples into nn::hid::GestureState entries, which hid.cpp pushes into the
// gesture lifo at hid shared memory 0x3BA00 (where nn::hid::GetGestureStates reads them). The type, direction and attribute values
// are Switchbrew's (HID services: GestureType, GestureDirection, GestureAttribute, GestureState); the rules and thresholds are in
// hid_gesture.cpp.
#pragma once
#include <cstdint>

#include "hid_shm.h"

namespace nxrt::services::hidgesture {

enum Type : uint32_t { kIdle = 0, kComplete = 1, kCancel = 2, kTouch = 3, kPress = 4, kTap = 5, kPan = 6, kSwipe = 7, kPinch = 8, kRotate = 9 };
enum Direction : uint32_t { kNone = 0, kLeft = 1, kUp = 2, kRight = 3, kDown = 4 };
constexpr uint32_t kAttrNewTouch = 1u << 4;    // GestureAttribute IsNewTouch
constexpr uint32_t kAttrDoubleTap = 1u << 8;   // GestureAttribute IsDoubleTap

// Feed every touch sample with set_touch; when needs_update() says so, update() fills the next state for the lifo.
class Recognizer {
public:
    void set_touch(const hidshm::TouchState* touches, uint32_t count, int64_t ns);
    bool needs_update();   // not const: it consumes a pending forced update
    void update(hidshm::GestureState& next, int64_t ns);
    const hidshm::GestureState& last_state() const { return last_; }

private:
    static constexpr uint32_t kMaxPoints = 4;   // GestureState carries 4 points
    struct Frame {   // the fingers of one sample
        hidshm::GesturePoint points[kMaxPoints] = {};
        uint32_t active = 0;              // fingers counted (at most kMaxPoints)
        hidshm::GesturePoint mid = {};    // their mid point
        int64_t detection_count = 0;      // GestureState::detection_count
        float avg = 0.0f;                 // mean distance of the fingers from the mid point (pinch)
        float angle = 0.0f;               // direction from the first finger to the mid point (rotate)
    };
    static bool same_points(const Frame& a, const Frame& b);

    Frame cur_;
    Frame prev_;                          // the frame of the last update
    hidshm::GestureState last_ = {};      // the last state made
    int64_t last_update_ns_ = 0;
    int64_t last_tap_ns_ = 0;
    float dt_ = 0.0f;                     // seconds since the last update (set_touch)
    float last_pan_dt_ = 0.0f;            // dt of the last Pan / Pinch / Rotate
    bool force_ = true;                   // the next sample makes a state whatever it holds
    bool press_and_tap_ = false;          // the current touch may still become a Press or a Tap
};

}  // namespace nxrt::services::hidgesture
