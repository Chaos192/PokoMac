// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The hid shared memory block (0x40000 bytes) the SDK's nn::hid reads: section offsets, the ring lifo every input kind is published in,
// the state structs, the npad entry's fields, and the hid sampling entry points (defined in hid.cpp).
// Layouts: https://switchbrew.org/wiki/HID_Shared_Memory (SharedMemoryFormat, NpadSharedMemoryFormat, NpadInternalState,
// GestureSharedMemoryFormat) and Ryujinx (MIT) src/Ryujinx.HLE/HOS/Services/Hid/Types/SharedMemory, which agree on every offset here.
// AnySwitch's own choices: KeyboardState's 8-byte modifier word at +0x8 is split into u32 modifiers and u32 attributes (bit 0 connected,
// as the SDK reads it); a ring keeps at most 16 states (Ryujinx's cap); each entry's sequence word is twice its state's sampling number
// (odd while the entry is being written: hid.cpp ring_push).
#pragma once
#include <cstdint>

namespace nxrt::services {

namespace hidshm {

// ---- sections of the block ------------------------------------------------------------------------------------------------------
constexpr uint64_t kSize = 0x40000;
constexpr uint64_t kDebugPad = 0x0;          // Ring<DebugPadState>
constexpr uint64_t kTouchScreen = 0x400;     // Ring<TouchScreenState>
constexpr uint64_t kMouse = 0x3400;          // Ring<MouseState>
constexpr uint64_t kKeyboard = 0x3800;       // Ring<KeyboardState>
constexpr uint64_t kNpads = 0x9A00;          // kNpadCount entries: 0..7 players 1..8, 8 handheld, 9 other
constexpr uint64_t kNpadEntrySize = 0x5000;
constexpr uint32_t kNpadCount = 10;
constexpr uint64_t kGesture = 0x3BA00;       // Ring<GestureState>
constexpr uint64_t kDebugMouse = 0x3DC00;    // not written here
constexpr uint64_t kNpadCondition = 0x3E200; // 4 x u32

// ---- the ring lifo --------------------------------------------------------------------------------------------------------------
constexpr uint32_t kRingEntries = 17;
constexpr uint64_t kRingMaxCount = 16;       // the largest count kept
constexpr uint64_t kRingBufferCount = 0x8;   // header field offsets
constexpr uint64_t kRingIndex = 0x10;
constexpr uint64_t kRingCount = 0x18;
constexpr uint64_t kRingHeader = 0x20;       // the entries start here

template <class T>
struct Ring {
    struct Entry {
        uint64_t sampling;   // sequence word: 2 x the state's sampling number, odd while it is written
        T state;
    };
    uint64_t unused;
    uint64_t buffer_count;   // kRingEntries once initialised
    uint64_t index;          // the entry written last
    uint64_t count;          // states in the ring, at most kRingMaxCount
    Entry entries[kRingEntries];
};

// ---- states ---------------------------------------------------------------------------------------------------------------------
struct NpadCommonState {     // 0x28 bytes: the state each npad style's lifo holds
    uint64_t sampling;
    uint64_t buttons;          // nn::hid::NpadButton
    int32_t lx, ly, rx, ry;    // analog sticks L then R
    uint32_t attributes;       // nn::hid::NpadAttribute
    uint32_t reserved;
};

struct SixAxisState {        // 0x60 bytes (the SDK's SixAxisSensorState)
    uint64_t delta_time;       // nanoseconds from the sample before
    uint64_t sampling;
    float accel[3];
    float gyro[3];             // angular velocity
    float angle[3];
    float direction[9];        // 3x3 rotation
    uint32_t attributes;       // connected = bit 0, interpolated = bit 1
    uint32_t reserved;
};

struct TouchState {          // 0x28 bytes: one finger
    uint64_t delta_time;
    uint32_t attributes;       // bit 0 start, bit 1 end
    uint32_t finger;
    uint32_t x, y;
    uint32_t diameter_x, diameter_y;
    uint32_t rotation_angle;
    uint32_t reserved;
};

struct TouchScreenState {    // 0x290 bytes: up to 16 fingers
    uint64_t sampling;
    int32_t count;
    int32_t reserved;
    TouchState touches[16];
};

struct MouseState {          // 0x28 bytes
    uint64_t sampling;
    int32_t x, y;
    int32_t dx, dy;
    int32_t wheel_dx, wheel_dy;
    uint32_t buttons;
    uint32_t attributes;       // connected = bit 1
};

struct KeyboardState {       // 0x30 bytes
    uint64_t sampling;
    uint32_t modifiers;        // nn::hid::KeyboardModifier
    uint32_t attributes;       // nn::hid::KeyboardAttribute (connected = bit 0)
    uint64_t keys[4];          // bit n = HID usage n
};

struct GesturePoint {        // 8 bytes: x, y in touch screen pixels
    int32_t x, y;
};

struct GestureState {        // 0x60 bytes
    uint64_t sampling;
    int64_t detection_count;
    uint32_t type;             // GestureType (hid_gesture.h)
    uint32_t direction;
    GesturePoint pos;          // the points' mid point
    GesturePoint delta;        // movement since the last state
    float vel_x, vel_y;        // velocity in px/s
    uint32_t attributes;
    float scale;
    float rotation_angle;
    int32_t point_count;
    GesturePoint points[4];
};

struct DebugPadState {       // 0x20 bytes
    uint64_t sampling;
    uint32_t attributes;
    uint32_t buttons;
    int32_t rx, ry;            // the right stick first
    int32_t lx, ly;
};

// ---- one npad entry (offsets inside it) -----------------------------------------------------------------------------------------
constexpr uint64_t kStyleSet = 0x0;              // u32 nn::hid::NpadStyleTag, 0 = not connected
constexpr uint64_t kJoyAssignmentMode = 0x4;     // u32: 0 dual, 1 single
constexpr uint64_t kFullKeyColor = 0x8;          // u32 attribute, body, buttons
constexpr uint64_t kJoyColor = 0x14;             // u32 attribute, left body, left buttons, right body, right buttons
constexpr uint64_t kFullKeyLifo = 0x28;          // Ring<NpadCommonState> each
constexpr uint64_t kHandheldLifo = 0x378;
constexpr uint64_t kJoyDualLifo = 0x6C8;
constexpr uint64_t kJoyLeftLifo = 0xA18;
constexpr uint64_t kJoyRightLifo = 0xD68;
constexpr uint64_t kPalmaLifo = 0x10B8;
constexpr uint64_t kSystemExtLifo = 0x1408;
constexpr uint64_t kFullKeySixAxisLifo = 0x1758;      // Ring<SixAxisState> each
constexpr uint64_t kHandheldSixAxisLifo = 0x1E60;
constexpr uint64_t kJoyDualLeftSixAxisLifo = 0x2568;
constexpr uint64_t kJoyDualRightSixAxisLifo = 0x2C70;
constexpr uint64_t kJoyLeftSixAxisLifo = 0x3378;
constexpr uint64_t kJoyRightSixAxisLifo = 0x3A80;
constexpr uint64_t kDeviceType = 0x4188;         // u32 nn::hid::system::DeviceType bits
constexpr uint64_t kSystemProperties = 0x4190;   // u64 NpadSystemProperties
constexpr uint64_t kSystemButtonProperties = 0x4198;
constexpr uint64_t kBatteryJoyDual = 0x419C;     // u32 battery level 0..4 (4 = full)
constexpr uint64_t kBatteryJoyLeft = 0x41A0;
constexpr uint64_t kBatteryJoyRight = 0x41A4;
constexpr uint64_t kFooterUiAttribute = 0x41A8;
constexpr uint64_t kFooterUiType = 0x41AC;       // u8
// (0x4228: the GameCube trigger lifo, not written here)

// NpadAttribute and NpadStyleTag bits
constexpr uint32_t kAttrConnected = 1u << 0;
constexpr uint32_t kAttrWired = 1u << 1;
constexpr uint32_t kAttrLeftConnected = 1u << 2;
constexpr uint32_t kAttrRightConnected = 1u << 4;
constexpr uint32_t kTagFullKey = 1u << 0;
constexpr uint32_t kTagHandheld = 1u << 1;
constexpr uint32_t kTagJoyDual = 1u << 2;
constexpr uint32_t kTagJoyLeft = 1u << 3;
constexpr uint32_t kTagJoyRight = 1u << 4;

static_assert(sizeof(NpadCommonState) == 0x28, "NpadCommonState");
static_assert(sizeof(SixAxisState) == 0x60, "SixAxisSensorState");
static_assert(sizeof(TouchState) == 0x28, "TouchState");
static_assert(sizeof(TouchScreenState) == 0x290, "TouchScreenState");
static_assert(sizeof(MouseState) == 0x28, "MouseState");
static_assert(sizeof(KeyboardState) == 0x30, "KeyboardState");
static_assert(sizeof(GestureState) == 0x60, "GestureState");
static_assert(sizeof(DebugPadState) == 0x20, "DebugPadState");
static_assert(sizeof(Ring<NpadCommonState>) == 0x350, "npad lifo");
static_assert(sizeof(Ring<SixAxisState>) == 0x708, "six-axis lifo");
static_assert(sizeof(Ring<TouchScreenState>) == 0x2C38, "touch screen lifo");
static_assert(sizeof(Ring<MouseState>) == 0x350, "mouse lifo");
static_assert(sizeof(Ring<KeyboardState>) == 0x3D8, "keyboard lifo");
static_assert(sizeof(Ring<DebugPadState>) == 0x2C8, "debug pad lifo");
static_assert(sizeof(Ring<GestureState>) == 0x708, "gesture lifo");
static_assert(kNpads + kNpadCount * kNpadEntrySize == 0x3BA00, "the npad entries end where the gesture section starts");
static_assert(kKeyboard + sizeof(Ring<KeyboardState>) <= kDebugMouse, "keyboard lifo");
static_assert(kJoyRightSixAxisLifo + sizeof(Ring<SixAxisState>) == kDeviceType, "npad lifos end at the device type");

}  // namespace hidshm

// ---- hid.cpp ----------------------------------------------------------------------------------------------------------------------
void hid_sample_once();                          // one sampling step now: one entry more in every live lifo
void hid_set_background_sampling(bool enabled);  // the 5 ms sampling thread on / off (tests: off)
uint8_t* hid_shared_memory_for_tests();          // where the service writes the block; null before it exists

}  // namespace nxrt::services
