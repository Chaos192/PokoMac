// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The hid shared memory layout header (runtime/src/services/hid_shm.h) as numbers: every constant, struct size and field offset, one per
// line. ipc_diff.py compiles it against two copies of the header (-I <folder>) and compares the outputs.
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "hid_shm.h"

using namespace nxrt::services::hidshm;

#define C(x) std::printf("%s %llx\n", #x, static_cast<unsigned long long>(x))
#define S(x) std::printf("sizeof %s %zx\n", #x, sizeof(x))
#define O(t, f) std::printf("offsetof %s.%s %zx\n", #t, #f, offsetof(t, f))

int main() {
    C(kSize); C(kDebugPad); C(kTouchScreen); C(kMouse); C(kKeyboard); C(kNpads); C(kNpadEntrySize); C(kNpadCount); C(kGesture);
    C(kDebugMouse); C(kNpadCondition); C(kRingEntries); C(kRingMaxCount); C(kRingBufferCount); C(kRingIndex); C(kRingCount); C(kRingHeader);
    C(kStyleSet); C(kJoyAssignmentMode); C(kFullKeyColor); C(kJoyColor); C(kFullKeyLifo); C(kHandheldLifo); C(kJoyDualLifo);
    C(kJoyLeftLifo); C(kJoyRightLifo); C(kPalmaLifo); C(kSystemExtLifo); C(kFullKeySixAxisLifo); C(kHandheldSixAxisLifo);
    C(kJoyDualLeftSixAxisLifo); C(kJoyDualRightSixAxisLifo); C(kJoyLeftSixAxisLifo); C(kJoyRightSixAxisLifo); C(kDeviceType);
    C(kSystemProperties); C(kSystemButtonProperties); C(kBatteryJoyDual); C(kBatteryJoyLeft); C(kBatteryJoyRight); C(kFooterUiAttribute);
    C(kFooterUiType); C(kAttrConnected); C(kAttrWired); C(kAttrLeftConnected); C(kAttrRightConnected); C(kTagFullKey); C(kTagHandheld);
    C(kTagJoyDual); C(kTagJoyLeft); C(kTagJoyRight);
    S(NpadCommonState); S(SixAxisState); S(TouchState); S(TouchScreenState); S(MouseState); S(KeyboardState); S(GesturePoint);
    S(GestureState); S(DebugPadState); S(Ring<NpadCommonState>); S(Ring<SixAxisState>); S(Ring<TouchScreenState>); S(Ring<MouseState>);
    S(Ring<KeyboardState>); S(Ring<DebugPadState>); S(Ring<GestureState>); S(Ring<GestureState>::Entry);
    O(NpadCommonState, sampling); O(NpadCommonState, buttons); O(NpadCommonState, lx); O(NpadCommonState, ly); O(NpadCommonState, rx);
    O(NpadCommonState, ry); O(NpadCommonState, attributes); O(NpadCommonState, reserved);
    O(SixAxisState, delta_time); O(SixAxisState, sampling); O(SixAxisState, accel); O(SixAxisState, gyro); O(SixAxisState, angle);
    O(SixAxisState, direction); O(SixAxisState, attributes); O(SixAxisState, reserved);
    O(TouchState, delta_time); O(TouchState, attributes); O(TouchState, finger); O(TouchState, x); O(TouchState, y);
    O(TouchState, diameter_x); O(TouchState, diameter_y); O(TouchState, rotation_angle); O(TouchState, reserved);
    O(TouchScreenState, sampling); O(TouchScreenState, count); O(TouchScreenState, reserved); O(TouchScreenState, touches);
    O(MouseState, sampling); O(MouseState, x); O(MouseState, y); O(MouseState, dx); O(MouseState, dy); O(MouseState, wheel_dx);
    O(MouseState, wheel_dy); O(MouseState, buttons); O(MouseState, attributes);
    O(KeyboardState, sampling); O(KeyboardState, modifiers); O(KeyboardState, attributes); O(KeyboardState, keys);
    O(GesturePoint, x); O(GesturePoint, y);
    O(GestureState, sampling); O(GestureState, detection_count); O(GestureState, type); O(GestureState, direction); O(GestureState, pos);
    O(GestureState, delta); O(GestureState, vel_x); O(GestureState, vel_y); O(GestureState, attributes); O(GestureState, scale);
    O(GestureState, rotation_angle); O(GestureState, point_count); O(GestureState, points);
    O(DebugPadState, sampling); O(DebugPadState, attributes); O(DebugPadState, buttons); O(DebugPadState, rx); O(DebugPadState, ry);
    O(DebugPadState, lx); O(DebugPadState, ly);
    O(Ring<GestureState>, unused); O(Ring<GestureState>, buffer_count); O(Ring<GestureState>, index); O(Ring<GestureState>, count);
    O(Ring<GestureState>, entries); O(Ring<GestureState>::Entry, sampling); O(Ring<GestureState>::Entry, state);
    O(Ring<KeyboardState>, entries); O(Ring<KeyboardState>::Entry, state);
    // the three entry points keep their signatures
    static_assert(std::is_same_v<decltype(&nxrt::services::hid_sample_once), void (*)()>);
    static_assert(std::is_same_v<decltype(&nxrt::services::hid_set_background_sampling), void (*)(bool)>);
    static_assert(std::is_same_v<decltype(&nxrt::services::hid_shared_memory_for_tests), uint8_t* (*)()>);
    return 0;
}
