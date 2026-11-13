// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The touch gesture recognizer (hid_gesture.h). One state per sample that changes something:
//   - a finger comes down: Touch (IsNewTouch, unless it follows a Cancel); held still for more than 0.5 s: Press;
//   - lifted after a Touch: Tap (IsDoubleTap within 0.35 s of the last tap), then Complete; after a Press, Swipe, Pinch or Rotate:
//     Complete; a Complete or Cancel is followed by one Idle;
//   - moved: Pan with its delta and velocity; Pinch when the fingers' mean distance from their mid point changes by more than 0.5 px
//     (scale = the ratio); Rotate when the turn of the first finger around the mid point exceeds 0.015 rad (angle in degrees);
//   - lifted after a Pan: Swipe (with its direction) when the last movement was faster than 400 px/s, else Complete;
//   - the number of fingers changed: Cancel.
// All velocity, scale and angle arithmetic is single precision.
#include "hid_gesture.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace nxrt::services::hidgesture {
namespace {

constexpr float kPressSeconds = 0.5f;
constexpr float kDoubleTapSeconds = 0.35f;
constexpr float kSwipeSpeed = 400.0f;      // pixels per second
constexpr float kPinchDistance = 0.5f;     // pixels
constexpr float kRotateRadians = 0.015f;
constexpr float kNsPerSecond = 1e9f;

}  // namespace

bool Recognizer::same_points(const Frame& a, const Frame& b) {
    for (uint32_t i = 0; i < kMaxPoints; ++i) {
        if (a.points[i].x != b.points[i].x || a.points[i].y != b.points[i].y) return false;
    }
    return true;
}

void Recognizer::set_touch(const hidshm::TouchState* touches, uint32_t count, int64_t ns) {
    Frame f;
    f.active = std::min(count, kMaxPoints);
    for (uint32_t i = 0; i < f.active; ++i) {
        f.points[i].x = static_cast<int32_t>(touches[i].x);
        f.points[i].y = static_cast<int32_t>(touches[i].y);
    }
    if (f.active > 0) {
        const int32_t n = static_cast<int32_t>(f.active);
        for (uint32_t i = 0; i < f.active; ++i) {   // each share truncated on its own
            f.mid.x += f.points[i].x / n;
            f.mid.y += f.points[i].y / n;
        }
        for (uint32_t i = 0; i < f.active; ++i) {
            const float dx = static_cast<float>(f.mid.x - f.points[i].x);
            const float dy = static_cast<float>(f.mid.y - f.points[i].y);
            const float xx = dx * dx;   // separate statements: no fused multiply-add on any host
            const float yy = dy * dy;
            f.avg += std::sqrt(xx + yy) / static_cast<float>(f.active);
        }
        f.angle = std::atan2(static_cast<float>(f.mid.y - f.points[0].y), static_cast<float>(f.mid.x - f.points[0].x));
    }
    f.detection_count = prev_.detection_count;
    cur_ = f;
    const int64_t t = last_update_ns_ > ns ? last_tap_ns_ : ns;   // a clock that went back: measure from the last tap
    dt_ = static_cast<float>(t - last_update_ns_) / kNsPerSecond;
}

bool Recognizer::needs_update() {
    if (force_) {
        force_ = false;
        return true;
    }
    if (!same_points(cur_, prev_)) return true;
    if (last_.type == kTouch && last_.point_count == 1 && dt_ > kPressSeconds) return press_and_tap_;
    return false;
}

void Recognizer::update(hidshm::GestureState& next, int64_t ns) {
    last_update_ns_ = ns;
    hidshm::GestureState g{};
    g.sampling = last_.sampling + 1;
    g.type = kIdle;
    g.direction = kNone;
    uint32_t type = kIdle;
    uint32_t attributes = 0;
    const uint32_t now = cur_.active;
    const uint32_t before = prev_.active;

    if (now > 0 && before == 0) {   // a new touch
        type = kTouch;
        ++cur_.detection_count;
        if (last_.type != kCancel) {
            attributes = kAttrNewTouch;
            press_and_tap_ = true;
        }
    } else if (now > 0 && now != before) {   // fingers added or lifted while touching
        type = kCancel;
        press_and_tap_ = false;
        for (hidshm::GesturePoint& p : cur_.points) p = {};
        cur_.active = 0;
        cur_.mid = {};
    } else if (now > 0 && !same_points(cur_, prev_)) {   // moved
        g.delta.x = cur_.mid.x - last_.pos.x;
        g.delta.y = cur_.mid.y - last_.pos.y;
        g.vel_x = static_cast<float>(g.delta.x) / dt_;
        g.vel_y = static_cast<float>(g.delta.y) / dt_;
        last_pan_dt_ = dt_;
        type = kPan;
        if (std::abs(cur_.avg - prev_.avg) > kPinchDistance) {
            type = kPinch;
            g.scale = cur_.avg / prev_.avg;
        }
        // The two angles are combined as if they were slopes: atan((a - b) / (1 + a * b)).
        const float turn = std::atan((cur_.angle - prev_.angle) / (1.0f + cur_.angle * prev_.angle));
        if (std::abs(turn) > kRotateRadians) {
            type = kRotate;
            g.scale = 0.0f;
            g.rotation_angle = turn * 180.0f / std::numbers::pi_v<float>;
        }
    } else if (now > 0) {   // held still
        type = last_.type == kTouch ? kPress : kIdle;
    } else if (before > 0) {   // all fingers lifted
        switch (last_.type) {
        case kTouch:
            force_ = true;
            if (press_and_tap_) {
                type = kTap;
                cur_ = prev_;   // the tap is reported where the finger was
                const float since = static_cast<float>(last_update_ns_ - last_tap_ns_) / kNsPerSecond;
                last_tap_ns_ = ns;
                if (since < kDoubleTapSeconds) attributes = kAttrDoubleTap;
            } else {
                type = kCancel;
            }
            break;
        case kPress:
        case kTap:
        case kSwipe:
        case kPinch:
        case kRotate:
            type = kComplete;
            force_ = true;
            break;
        case kPan: {
            g.vel_x = static_cast<float>(last_.delta.x) / (last_pan_dt_ + dt_);
            g.vel_y = static_cast<float>(last_.delta.y) / (last_pan_dt_ + dt_);
            const float speed = std::sqrt(g.vel_x * g.vel_x + g.vel_y * g.vel_y);
            force_ = true;
            if (speed > kSwipeSpeed) {
                type = kSwipe;
                cur_ = prev_;   // reported at the last moved position
                g.delta = last_.delta;
                if (std::abs(g.delta.x) > std::abs(g.delta.y)) {
                    g.direction = g.delta.x > 0 ? kRight : kLeft;
                } else {
                    g.direction = g.delta.y > 0 ? kDown : kUp;
                }
            } else {
                type = kComplete;
                g.vel_x = 0.0f;
                g.vel_y = 0.0f;
            }
            break;
        }
        default:   // Idle, Complete, Cancel
            type = kIdle;
            break;
        }
    } else {   // no fingers before or now
        type = kIdle;
        if (last_.type == kComplete || last_.type == kCancel) ++cur_.detection_count;
    }

    g.detection_count = cur_.detection_count;
    g.type = type;
    g.attributes = attributes;
    g.pos = cur_.mid;
    g.point_count = static_cast<int32_t>(cur_.active);
    for (uint32_t i = 0; i < kMaxPoints; ++i) g.points[i] = cur_.points[i];
    prev_ = cur_;
    last_ = g;
    next = g;
}

}  // namespace nxrt::services::hidgesture
