// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The hid service (S3c): sm "hid" (nn::hid::IHidServer) with its applet resource (nn::hid::IAppletResource, the shared memory handle)
// and its active vibration device list; the hid shared memory, built here and refreshed by one sampling step about every 5 ms while an
// applet resource is alive; vibration values kept per device handle and handed to the play layer's sink; and the Switch 2 Joy-Con 2
// mouse (the Nmouse and Jcms blocks the game lends as transfer memory), fed from the PC mouse and from real Joy-Con 2s (nxrt/sw2).
//
// Public sources (facts, no code):
//   https://switchbrew.org/wiki/HID_services (command ids and names, NpadIdType, NpadStyleTag, NpadButton, VibrationValue, the [S2] mouse
//   commands), https://switchbrew.org/wiki/HID_Shared_Memory (the block's sections, the npad entry, the lifo header);
//   Ryujinx (MIT), src/Ryujinx.HLE/HOS/Services/Hid: IHidServer.cs (argument order, the event signalled at once, the vibration device
//   info), HidServer/HidUtils.cs (NpadIdType to entry index), Hid.cs (the stick threshold), HidDevices/NpadDevices.cs (what a connected
//   npad entry holds), Types/SharedMemory (NpadCondition, DeviceType, NpadSystemProperties bits), ResultCode.cs (module 202 results).
// The ring layout and the shared memory offsets are hid_shm.h's; the Joy-Con 2 mouse blocks are docs/hid_mouse_sw2.md's.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hid_gesture.h"
#include "hid_sdk.h"
#include "hid_shm.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/input_source.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services_api.h"
#include "nxrt/sw2/manager.h"
#include "nxrt/sw2/service.h"
#include "nxrt/webview.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

namespace hs = hidshm;
using nxrt::hid::InputSnapshot;
using nxrt::hid::NpadInput;
using nxrt::hid::NpadStyle;

constexpr const char* kTag = "hid";

// Results: hid module 202 (Ryujinx Hid/ResultCode.cs), kernel 104 / 105.
constexpr uint32_t kInvalidNpadIdType = res(202, 123);   // 0xF6CA
constexpr uint32_t kInvalidDeviceIndex = res(202, 124);  // 0xF8CA

constexpr uint32_t kEntries = hs::kNpadCount;            // 10 npad entries: 0..7 No1..No8, 8 Handheld, 9 Other
constexpr uint64_t kSixAxisDeltaNs = 5000000;            // the sampler's period, written as every six-axis state's delta time
constexpr int32_t kStickThreshold = 16383;               // half of a stick's range: past it the direction bit is set

uint64_t steady_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint8_t* host_ptr(uint64_t a) { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(a)); }

uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void wr64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// NpadIdType -> shared memory entry index (-1: not an npad id); entry index -> the input slot of input_source.h.
int entry_of_id(uint32_t id) {
    if (id < 8) return static_cast<int>(id);
    if (id == 0x20) return 8;
    if (id == 0x10) return 9;
    return -1;
}
uint32_t slot_of_entry(int e) {
    if (e == 8) return nxrt::hid::kNpadSlotHandheld;
    if (e == 9) return nxrt::hid::kNpadSlotOther;
    return static_cast<uint32_t>(e);
}

// The three bytes of a VibrationDeviceHandle / SixAxisSensorHandle: device type, NpadIdType, position.
uint32_t handle_type(uint32_t h) { return h & 0xFF; }
uint32_t handle_npad(uint32_t h) { return (h >> 8) & 0xFF; }
uint32_t handle_position(uint32_t h) { return (h >> 16) & 0xFF; }

// ======================================================================================================================================
// The lifo: every ring this file writes (the hid block, the Nmouse and Jcms blocks) is hid_shm.h's Ring<T>, written as a sequence lock
// ======================================================================================================================================
template <class T>
void ring_push(uint8_t* ring, const T& state) {
    constexpr uint64_t kEntry = 8 + sizeof(T);
    const uint64_t at = rd64(ring + hs::kRingIndex) % hs::kRingEntries;
    const uint64_t number = rd64(ring + hs::kRingHeader + at * kEntry) / 2 + 1;
    const uint64_t next = (at + 1) % hs::kRingEntries;
    uint8_t* e = ring + hs::kRingHeader + next * kEntry;
    T copy = state;
    copy.sampling = number;
    wr64(e, 2 * number - 1);   // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(e + 8, &copy, sizeof(T));
    std::atomic_thread_fence(std::memory_order_release);
    wr64(e, 2 * number);
    std::atomic_thread_fence(std::memory_order_release);
    wr64(ring + hs::kRingIndex, next);
    const uint64_t count = rd64(ring + hs::kRingCount);
    if (count < hs::kRingMaxCount) wr64(ring + hs::kRingCount, count + 1);
}

void ring_prepare(uint8_t* ring) { wr64(ring + hs::kRingBufferCount, hs::kRingEntries); }

// ======================================================================================================================================
// The Joy-Con 2 mouse blocks (docs/hid_mouse_sw2.md)
// ======================================================================================================================================
struct NmouseState {          // the state the server writes (0x70); the SDK copies its public NmouseState out of it
    uint64_t unknown0;
    uint64_t sampling;
    int32_t x, y;             // in the pointer range
    int32_t range_w, range_h;
    int32_t wheel_x;
    int32_t unused24;
    uint32_t buttons;         // nn::hid::MouseButton bits
    uint32_t attributes;      // bit 1: on its surface
    int32_t wheel_y;
    uint8_t dev[0x3C];
};
static_assert(sizeof(NmouseState) == 0x70, "NmouseState");

struct JcmsState {            // 0x20
    uint64_t unknown0;
    uint64_t sampling;
    int32_t dx, dy;
    uint32_t attributes;      // bit 0 connected, bit 1 on a surface
    uint32_t unknown1c;
};
static_assert(sizeof(JcmsState) == 0x20, "JcmsState");

constexpr uint64_t kNmTable = 0x0;          // 17 x {u64 handle, u8 valid}
constexpr uint64_t kNmTableStride = 0x10;
constexpr uint64_t kNmRings = 0x220;        // 17 rings of NmouseState
constexpr uint64_t kNmRingStride = 0x20 + hs::kRingEntries * (8 + sizeof(NmouseState));
constexpr uint64_t kNmInfo = 0x8C40;        // 17 x {u32 types, u8 valid, u32 npad id, u8 npad valid, u8 pointer enabled}
constexpr uint64_t kNmInfoStride = 0x1C;
constexpr uint64_t kNmShowCursor = 0x8E68;
constexpr uint64_t kNmBytes = 0x8E70;
constexpr uint32_t kNmSlots = 17;
constexpr uint64_t kNmHandleMerged = 0x4E4D0001, kNmHandleRight = 0x4E4D0002;
constexpr uint32_t kNmTypeMerged = 1, kNmTypeRightJcms = 4;
static_assert(kNmRingStride == 0x818, "Nmouse ring");
static_assert(kNmRings + kNmSlots * kNmRingStride <= kNmInfo, "Nmouse rings end before the info table");

constexpr uint64_t kJcmsRingStride = 0x20 + hs::kRingEntries * (8 + sizeof(JcmsState));
constexpr uint32_t kJcmsRings = 16;         // index = player * 2 + side
constexpr uint64_t kJcmsBytes = kJcmsRingStride * kJcmsRings;
static_assert(kJcmsRingStride == 0x2C8 && kJcmsBytes == 0x2C80, "Jcms rings");

constexpr int32_t kDefaultRangeW = 1920, kDefaultRangeH = 1080;
constexpr uint64_t kFeedFreshNs = 250000000;   // a feed older than this is not live
constexpr int kActiveLogMax = 200;

// The play layer's side of the Joy-Con 2 mouse: not under the hid lock.
struct Feed {
    std::mutex mu;
    nxrt::hid::HostMouse m;
    uint64_t last_ns = 0;
    float wheel_x = 0, wheel_y = 0;   // notches not handed to the game yet
};
Feed& feed() {
    static Feed* f = new Feed();
    return *f;
}
std::atomic<int> g_jc2_mode{-1};                 // -1: the config was not read yet
std::atomic<bool> g_jc2_request{false};
std::atomic<bool> g_jc2_listed{false};
std::atomic<bool> g_jc2_active{false};
std::atomic<uint64_t> g_jc2_space{0};            // w << 32 | h, 0: the pointer range

bool feed_live() {
    Feed& f = feed();
    std::lock_guard<std::mutex> lk(f.mu);
    return f.last_ns != 0 && steady_ns() - f.last_ns < kFeedFreshNs && f.m.focused && !f.m.captured;
}

// ======================================================================================================================================
// The process-wide hid state (one hid lock)
// ======================================================================================================================================
struct Vib {
    float amp_low = 0, freq_low = 0, amp_high = 0, freq_high = 0;
};

struct HidState {
    std::mutex mu;
    std::unique_ptr<ServiceSharedMemory> shm;
    ServiceEvent style_event[kEntries];
    uint32_t supported_styles = 0x1F;
    uint32_t supported_index_mask = 0x1FF;
    bool npad_active = false;
    bool debug_pad_active = false, touch_active = false, mouse_active = false, keyboard_active = false;
    bool sixaxis_fusion = false;
    hidgesture::Recognizer gesture;
    int32_t basic_gesture_id = -1;
    bool vibration_permitted = true;
    bool home_protection = false;
    uint64_t joy_hold = 0;
    int64_t handheld_activation = 0;
    int64_t npad_communication = 0;
    uint32_t last_tag[kEntries] = {};
    bool connected[kEntries] = {};
    std::map<uint32_t, Vib> vibration;
    uint64_t nm_addr = 0, nm_size = 0;
    int32_t nm_w = kDefaultRangeW, nm_h = kDefaultRangeH;
    uint64_t jcms_addr = 0, jcms_size = 0;
    uint32_t nm_merged_types = 0;
    uint64_t nm_btn_left = 0, nm_btn_right = 0;
    int32_t nm_x = -1, nm_y = -1;
    uint32_t start_jcms_calls = 0;
    int active_logs = 0;
    bool logged_214 = false;

    // not reset
    std::atomic<int> resources{0};
    std::atomic<bool> background{true};

    // the sampler thread (its own lock: never taken while the hid lock is held)
    std::mutex thread_mu;
    std::thread sampler;
    std::atomic<bool> stop{false};

    // Every field of the hid lock's group back to its start value (not resources, not the thread).
    void clear_locked() {
        for (auto& e : style_event) e = ServiceEvent();
        supported_styles = 0x1F;
        supported_index_mask = 0x1FF;
        npad_active = false;
        debug_pad_active = touch_active = mouse_active = keyboard_active = false;
        sixaxis_fusion = false;
        gesture = hidgesture::Recognizer();
        basic_gesture_id = -1;
        vibration_permitted = true;
        home_protection = false;
        joy_hold = 0;
        handheld_activation = 0;
        npad_communication = 0;
        for (auto& t : last_tag) t = 0;
        for (auto& c : connected) c = false;
        vibration.clear();
        nm_addr = nm_size = 0;
        nm_w = kDefaultRangeW;
        nm_h = kDefaultRangeH;
        jcms_addr = jcms_size = 0;
        nm_merged_types = 0;
        nm_btn_left = nm_btn_right = 0;
        nm_x = nm_y = -1;
        start_jcms_calls = 0;
        active_logs = 0;
        logged_214 = false;
    }
};

HidState& st() {
    static HidState* s = new HidState();
    return *s;
}

GaugeCounter& resources_gauge() {
    static GaugeCounter* g = new GaugeCounter("hid.applet_resources");
    return *g;
}

// ---- the shared memory ----------------------------------------------------------------------------------------------------------------
void shm_init(uint8_t* m) {
    std::memset(m, 0, hs::kSize);
    for (uint64_t off : {hs::kDebugPad, hs::kTouchScreen, hs::kMouse, hs::kKeyboard, hs::kGesture}) ring_prepare(m + off);
    static constexpr uint64_t kLifos[] = {hs::kFullKeyLifo,         hs::kHandheldLifo,         hs::kJoyDualLifo,
                                          hs::kJoyLeftLifo,         hs::kJoyRightLifo,         hs::kPalmaLifo,
                                          hs::kSystemExtLifo,       hs::kFullKeySixAxisLifo,   hs::kHandheldSixAxisLifo,
                                          hs::kJoyDualLeftSixAxisLifo, hs::kJoyDualRightSixAxisLifo, hs::kJoyLeftSixAxisLifo,
                                          hs::kJoyRightSixAxisLifo};
    for (uint32_t i = 0; i < kEntries; ++i)
        for (uint64_t off : kLifos) ring_prepare(m + hs::kNpads + i * hs::kNpadEntrySize + off);
    const uint32_t condition[4] = {0, 1, 1, 1};   // NpadCondition: hold type Horizontal
    std::memcpy(m + hs::kNpadCondition, condition, sizeof condition);
}

// The block as it stands, null when it is gone or was never made.
uint8_t* shm_usable(const HidState& s) {
    if (!s.shm || !s.shm->ok()) return nullptr;
    const uint64_t h = s.shm->host();
    if (!h || !mem::is_mapped(h, hs::kSize)) return nullptr;
    return host_ptr(h);
}

// The block, made and initialised when needed (hid lock held). Null when it cannot be made.
uint8_t* shm_get(HidState& s) {
    if (uint8_t* m = shm_usable(s)) return m;
    s.shm = std::make_unique<ServiceSharedMemory>(hs::kSize, "hid");
    if (!s.shm->ok() || !s.shm->host()) {
        s.shm.reset();
        NXLOG_W(kTag, "the hid shared memory could not be made");
        return nullptr;
    }
    uint8_t* m = host_ptr(s.shm->host());
    shm_init(m);
    return m;
}

uint8_t* entry_base(uint8_t* m, int e) { return m + hs::kNpads + static_cast<uint64_t>(e) * hs::kNpadEntrySize; }

// ======================================================================================================================================
// One sample's input
// ======================================================================================================================================
void read_input_raw(InputSnapshot* out) {
    *out = InputSnapshot{};
    const std::shared_ptr<nxrt::hid::InputSource> src = nxrt::hid::input_source();
    if (!src) {
        nxrt::hid::set_sampled_buttons(0);
        out->npad[0].connected = true;
        out->npad[0].style = NpadStyle::FullKey;
        if (!amctl::docked()) {
            out->npad[nxrt::hid::kNpadSlotHandheld].connected = true;
            out->npad[nxrt::hid::kNpadSlotHandheld].style = NpadStyle::Handheld;
        }
        return;
    }
    src->poll(*out);
    uint64_t pressed = 0;
    for (const NpadInput& p : out->npad)
        if (p.connected) pressed |= p.buttons;
    if (webview::input_held()) {
        // the page takes the controller: it gets every pad's buttons and the first deflected left stick; the game reads idle pads
        int32_t lx = 0, ly = 0;
        for (const NpadInput& p : out->npad) {
            if (p.connected && (p.left.x != 0 || p.left.y != 0)) {
                lx = p.left.x;
                ly = p.left.y;
                break;
            }
        }
        webview::feed_pad(pressed, lx, ly);
        for (NpadInput& p : out->npad) {
            p.buttons = 0;
            p.left = {};
            p.right = {};
        }
        out->touch.count = 0;
        out->keyboard = nxrt::hid::KeyboardInput{};
        out->mouse.buttons = 0;
        out->mouse.dx = out->mouse.dy = 0;
        out->mouse.wheel_dx = out->mouse.wheel_dy = 0;
        pressed = 0;
    }
    nxrt::hid::set_sampled_buttons(pressed);
}

void read_input(InputSnapshot* out) {
    read_input_raw(out);
    hid_bench_apply(*out);
}

// ======================================================================================================================================
// The npad entries
// ======================================================================================================================================
uint32_t tag_of(NpadStyle s) {
    switch (s) {
        case NpadStyle::FullKey: return hs::kTagFullKey;
        case NpadStyle::Handheld: return hs::kTagHandheld;
        case NpadStyle::JoyDual: return hs::kTagJoyDual;
        case NpadStyle::JoyLeft: return hs::kTagJoyLeft;
        case NpadStyle::JoyRight: return hs::kTagJoyRight;
        default: return 0;
    }
}

uint64_t with_stick_bits(uint64_t b, const nxrt::hid::StickValue& l, const nxrt::hid::StickValue& r) {
    using namespace nxrt::hid;
    if (l.x < -kStickThreshold) b |= btn(kBtnStickLLeft);
    if (l.x > kStickThreshold) b |= btn(kBtnStickLRight);
    if (l.y < -kStickThreshold) b |= btn(kBtnStickLDown);
    if (l.y > kStickThreshold) b |= btn(kBtnStickLUp);
    if (r.x < -kStickThreshold) b |= btn(kBtnStickRLeft);
    if (r.x > kStickThreshold) b |= btn(kBtnStickRRight);
    if (r.y < -kStickThreshold) b |= btn(kBtnStickRDown);
    if (r.y > kStickThreshold) b |= btn(kBtnStickRUp);
    return b;
}

uint32_t attributes_of(uint32_t tag) {
    switch (tag) {
        case hs::kTagFullKey:
        case hs::kTagHandheld: return hs::kAttrConnected | hs::kAttrWired;
        case hs::kTagJoyDual: return hs::kAttrConnected | hs::kAttrLeftConnected | hs::kAttrRightConnected;
        case hs::kTagJoyLeft: return hs::kAttrConnected | hs::kAttrLeftConnected;
        case hs::kTagJoyRight: return hs::kAttrConnected | hs::kAttrRightConnected;
        default: return 0;
    }
}

// nn::hid::system::DeviceType bits and NpadSystemProperties of each style (Ryujinx DeviceType.cs, NpadSystemProperties.cs).
uint32_t device_type_of(uint32_t tag) {
    switch (tag) {
        case hs::kTagFullKey: return 0x1;           // FullKey
        case hs::kTagHandheld: return 0xC;          // HandheldLeft | HandheldRight
        case hs::kTagJoyDual: return 0x30;          // JoyLeft | JoyRight
        case hs::kTagJoyLeft: return 0x10;
        case hs::kTagJoyRight: return 0x20;
        default: return 0;
    }
}
uint64_t system_properties_of(uint32_t tag) {
    constexpr uint64_t kPowered = 0x38;                                      // JoyDual, JoyLeft and JoyRight powered (bits 3-5)
    constexpr uint64_t kAbxy = 1ull << 11, kSlSr = 1ull << 12, kPlus = 1ull << 13, kMinus = 1ull << 14;
    switch (tag) {
        case hs::kTagJoyLeft: return kPowered | kSlSr | kMinus;
        case hs::kTagJoyRight: return kPowered | kSlSr | kPlus;
        default: return kPowered | kAbxy | kPlus | kMinus;
    }
}

// The fields a style change writes (tag 0: the npad went away).
void write_style(uint8_t* base, uint32_t tag, const NpadInput& in) {
    wr32(base + hs::kStyleSet, tag);
    if (tag == 0) {
        wr32(base + hs::kDeviceType, 0);
        wr64(base + hs::kSystemProperties, 0);
        return;
    }
    wr32(base + hs::kJoyAssignmentMode, (tag == hs::kTagJoyLeft || tag == hs::kTagJoyRight) ? 1u : 0u);
    const uint32_t full[3] = {0, in.color_body, in.color_buttons};
    std::memcpy(base + hs::kFullKeyColor, full, sizeof full);
    const uint32_t joy[5] = {0, in.color_body, in.color_buttons, in.color_body, in.color_buttons};
    std::memcpy(base + hs::kJoyColor, joy, sizeof joy);
    wr64(base + hs::kSystemProperties, system_properties_of(tag));
    wr32(base + hs::kDeviceType, device_type_of(tag));
    wr32(base + hs::kBatteryJoyDual, 4);
    wr32(base + hs::kBatteryJoyLeft, 4);
    wr32(base + hs::kBatteryJoyRight, 4);
}

void sample_npad(HidState& s, uint8_t* m, int e, const NpadInput& in, bool docked) {
    uint8_t* base = entry_base(m, e);
    const uint32_t wanted = tag_of(in.style);
    const bool on = in.connected && wanted != 0 && (s.supported_styles & wanted) != 0 && ((s.supported_index_mask >> e) & 1u) != 0 &&
                    !(e == 8 && docked);
    const uint32_t tag = on ? wanted : 0;
    if (tag != s.last_tag[e]) {
        write_style(base, tag, in);
        s.last_tag[e] = tag;
        s.style_event[e].signal();
    }
    s.connected[e] = on;

    hs::NpadCommonState common{};
    hs::SixAxisState six{};
    if (on) {
        common.buttons = with_stick_bits(in.buttons, in.left, in.right);
        common.lx = in.left.x;
        common.ly = in.left.y;
        common.rx = in.right.x;
        common.ry = in.right.y;
        common.attributes = attributes_of(tag);
        six.delta_time = kSixAxisDeltaNs;
        six.attributes = 1;
        std::memcpy(six.accel, in.six_axis.accel, sizeof six.accel);
        std::memcpy(six.gyro, in.six_axis.gyro, sizeof six.gyro);
        std::memcpy(six.angle, in.six_axis.angle, sizeof six.angle);
        std::memcpy(six.direction, in.six_axis.direction, sizeof six.direction);
    }
    const hs::NpadCommonState idle{};
    const struct {
        uint64_t off;
        uint32_t tag;
    } commons[] = {{hs::kFullKeyLifo, hs::kTagFullKey}, {hs::kHandheldLifo, hs::kTagHandheld}, {hs::kJoyDualLifo, hs::kTagJoyDual},
                   {hs::kJoyLeftLifo, hs::kTagJoyLeft}, {hs::kJoyRightLifo, hs::kTagJoyRight}, {hs::kPalmaLifo, 0},
                   {hs::kSystemExtLifo, 0}};
    for (const auto& c : commons) {
        const bool mine = on && (c.tag == tag || c.off == hs::kSystemExtLifo);
        ring_push(base + c.off, mine ? common : idle);
    }
    const hs::SixAxisState still{};
    const struct {
        uint64_t off;
        uint32_t tag;
    } sixes[] = {{hs::kFullKeySixAxisLifo, hs::kTagFullKey},   {hs::kHandheldSixAxisLifo, hs::kTagHandheld},
                 {hs::kJoyDualLeftSixAxisLifo, hs::kTagJoyDual}, {hs::kJoyDualRightSixAxisLifo, hs::kTagJoyDual},
                 {hs::kJoyLeftSixAxisLifo, hs::kTagJoyLeft},   {hs::kJoyRightSixAxisLifo, hs::kTagJoyRight}};
    for (const auto& c : sixes) ring_push(base + c.off, (on && c.tag == tag) ? six : still);
}

// ======================================================================================================================================
// Touch, gesture, mouse, keyboard, debug pad
// ======================================================================================================================================
void sample_devices(HidState& s, uint8_t* m, const InputSnapshot& snap) {
    ring_push(m + hs::kDebugPad, hs::DebugPadState{});

    hs::TouchScreenState touch{};
    const uint32_t n = std::min<uint32_t>(snap.touch.count, 16);
    touch.count = static_cast<int32_t>(n);
    for (uint32_t k = 0; k < n; ++k) {
        const nxrt::hid::TouchFinger& f = snap.touch.fingers[k];
        hs::TouchState& t = touch.touches[k];
        t.finger = f.id;
        t.x = f.x;
        t.y = f.y;
        t.diameter_x = f.diameter_x;
        t.diameter_y = f.diameter_y;
        t.rotation_angle = static_cast<uint32_t>(f.angle);
    }
    ring_push(m + hs::kTouchScreen, touch);

    const int64_t now = static_cast<int64_t>(steady_ns());
    s.gesture.set_touch(touch.touches, n, now);
    if (s.gesture.needs_update()) {
        hs::GestureState g{};
        s.gesture.update(g, now);
        ring_push(m + hs::kGesture, g);
    }

    hs::MouseState mouse{};
    mouse.x = snap.mouse.x;
    mouse.y = snap.mouse.y;
    mouse.dx = snap.mouse.dx;
    mouse.dy = snap.mouse.dy;
    mouse.wheel_dx = snap.mouse.wheel_dx;
    mouse.wheel_dy = snap.mouse.wheel_dy;
    mouse.buttons = snap.mouse.buttons;
    mouse.attributes = snap.mouse.connected ? 2u : 0u;
    ring_push(m + hs::kMouse, mouse);

    hs::KeyboardState kb{};
    if (snap.keyboard_connected) {
        kb.modifiers = snap.keyboard.modifiers;
        kb.attributes = 1;
        std::memcpy(kb.keys, snap.keyboard.keys, sizeof kb.keys);
    }
    ring_push(m + hs::kKeyboard, kb);
}

// ======================================================================================================================================
// The Joy-Con 2 mouse each sample
// ======================================================================================================================================
nxrt::hid::Jc2MouseMode mode_now() { return nxrt::hid::jc2_mouse_mode(); }

void sample_nmouse(HidState& s) {
    if (!s.nm_addr || !g_jc2_listed.load()) return;
    const bool active = nxrt::hid::jc2_mouse_active();
    nxrt::hid::HostMouse fm;
    float wx = 0, wy = 0;
    {
        Feed& f = feed();
        std::lock_guard<std::mutex> lk(f.mu);
        fm = f.m;
        wx = f.wheel_x;
        wy = f.wheel_y;
        if (active) f.wheel_x = f.wheel_y = 0;
    }
    NmouseState ns{};
    if (active) {
        const float u = std::clamp(fm.u, 0.0f, 1.0f), v = std::clamp(fm.v, 0.0f, 1.0f);
        const uint64_t space = g_jc2_space.load();
        const int32_t pw = space ? static_cast<int32_t>(space >> 32) : s.nm_w;
        const int32_t ph = space ? static_cast<int32_t>(space & 0xFFFFFFFFu) : s.nm_h;
        ns.x = std::min(pw - 1, static_cast<int32_t>(u * static_cast<float>(pw)));
        ns.y = std::min(ph - 1, static_cast<int32_t>(v * static_cast<float>(ph)));
        s.nm_x = ns.x;
        s.nm_y = ns.y;
        ns.wheel_x = static_cast<int32_t>(std::lround(wx * 120.0f));
        ns.wheel_y = static_cast<int32_t>(std::lround(wy * 120.0f));
        ns.buttons = fm.buttons;
        ns.attributes = 2;
    } else {
        ns.x = std::max(0, s.nm_x);
        ns.y = std::max(0, s.nm_y);
    }
    ns.range_w = s.nm_w;
    ns.range_h = s.nm_h;
    uint8_t* b = host_ptr(s.nm_addr);
    ring_push(b + kNmRings + 0 * kNmRingStride, ns);
    ring_push(b + kNmRings + 1 * kNmRingStride, ns);
    if (g_jc2_active.exchange(active) != active && s.active_logs < kActiveLogMax) {
        ++s.active_logs;
        NXLOG_I(kTag, "Joy-Con 2 mouse %s", active ? "on its surface (the game has the mouse)" : "lifted");
    }
}

void sample_jcms(HidState& s) {
    if (!s.jcms_addr) return;
    nxrt::sw2::Manager* mgr = nxrt::sw2::manager();
    if (mgr) mgr->set_mouse_wanted(true);
    uint8_t* b = host_ptr(s.jcms_addr);
    for (int player = 0; player < 8; ++player) {
        for (int side = 0; side < 2; ++side) {
            int32_t dx = 0, dy = 0;
            bool surface = false;
            const bool real = mgr && mgr->take_sensor(player, side, &dx, &dy, &surface);
            if (!real && player != 0) continue;
            JcmsState j{};
            if (real) {
                j.dx = surface ? dx : 0;
                j.dy = surface ? dy : 0;
                j.attributes = 1u | (surface ? 2u : 0u);
            }
            ring_push(b + static_cast<uint64_t>(player * 2 + side) * kJcmsRingStride, j);
        }
    }
}

// The game's transfer memory (copy handle 0) as a block of `need` bytes: its address, and in *size the bytes it lends (the smaller of the
// size asked and the memory's own). 0 when it cannot be used.
uint64_t lent_block(const ipc::Request& req, uint64_t asked, uint64_t need, const char* what, uint64_t* size) {
    *size = 0;
    const uint32_t h = req.in_copy.empty() ? 0 : req.in_copy[0];
    const std::shared_ptr<kernel::KTransferMemory> tm = h ? kernel::handle_as<kernel::KTransferMemory>(h) : nullptr;
    if (!tm || !tm->addr) {
        NXLOG_W(kTag, "%s: no transfer memory came with the request", what);
        return 0;
    }
    const uint64_t n = std::min(asked, tm->size);
    if (n < need || !mem::is_mapped(tm->addr, need)) {
        NXLOG_W(kTag, "%s: the transfer memory at 0x%llx (%llu bytes) cannot hold the 0x%llx-byte block", what,
                static_cast<unsigned long long>(tm->addr), static_cast<unsigned long long>(n), static_cast<unsigned long long>(need));
        return 0;
    }
    *size = n;
    return tm->addr;
}

void nmouse_setup(HidState& s, uint64_t addr) {
    uint8_t* b = host_ptr(addr);
    std::memset(b, 0, kNmBytes);
    for (uint32_t k = 0; k < kNmSlots; ++k) ring_prepare(b + kNmRings + k * kNmRingStride);
    s.nm_x = s.nm_y = -1;
    if (mode_now() == nxrt::hid::Jc2MouseMode::Off) return;
    const struct {
        uint32_t types;
        uint8_t npad_valid;
        uint64_t handle;
    } devices[2] = {{kNmTypeMerged, 0, kNmHandleMerged}, {kNmTypeRightJcms, 1, kNmHandleRight}};
    for (uint32_t k = 0; k < 2; ++k) {
        uint8_t* info = b + kNmInfo + k * kNmInfoStride;
        wr32(info + 0x0, devices[k].types);
        info[0x4] = 1;
        wr32(info + 0x8, 0);   // player 1
        info[0xC] = devices[k].npad_valid;
        info[0x10] = 1;        // pointer enabled
        uint8_t* slot = b + kNmTable + k * kNmTableStride;
        wr64(slot, devices[k].handle);
        std::atomic_thread_fence(std::memory_order_release);
        slot[0x8] = 1;
    }
    g_jc2_listed.store(true);
    NXLOG_I(kTag, "Joy-Con 2 mouse: a merged mouse and a Joy-Con 2 (R) mouse of player 1 listed (mode %s)",
            nxrt::hid::jc2_mouse_mode_name(mode_now()));
}

void jcms_setup(uint64_t addr) {
    uint8_t* b = host_ptr(addr);
    std::memset(b, 0, kJcmsBytes);
    for (uint32_t k = 0; k < kJcmsRings; ++k) ring_prepare(b + k * kJcmsRingStride);
}

// ======================================================================================================================================
// Vibration
// ======================================================================================================================================
struct Sent {
    uint32_t handle;
    Vib v;
};

void forward_vibration(const Sent* sent, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const int e = entry_of_id(handle_npad(sent[i].handle));
        const uint32_t hand = handle_position(sent[i].handle);
        if (e < 0 || hand > 1) continue;
        nxrt::hid::VibrationValue v;
        v.amp_low = sent[i].v.amp_low;
        v.freq_low = sent[i].v.freq_low;
        v.amp_high = sent[i].v.amp_high;
        v.freq_high = sent[i].v.freq_high;
        vibration_dispatch(slot_of_entry(e), static_cast<int>(hand), v);
    }
}

// SendVibrationValues (206 / 214): handles in the first input buffer, VibrationValues in the second.
size_t vibration_list(const ipc::Request& req, Sent* out) {
    const ipc::Buffer hb = req.in_buf(0), vb = req.in_buf(1);
    const size_t n = static_cast<size_t>(std::min<uint64_t>(std::min<uint64_t>(hb.size / 4, vb.size / 16), 16));
    uint32_t handles[16] = {};
    float values[16 * 4] = {};
    buf_read(hb, handles, n * 4);
    buf_read(vb, values, n * 16);
    for (size_t i = 0; i < n; ++i) out[i] = {handles[i], {values[4 * i], values[4 * i + 1], values[4 * i + 2], values[4 * i + 3]}};
    return n;
}

void store_vibration(HidState& s, const Sent* sent, size_t n) {
    std::lock_guard<std::mutex> lk(s.mu);
    for (size_t i = 0; i < n; ++i) s.vibration[sent[i].handle] = sent[i].v;
}

// ======================================================================================================================================
// The sampler thread
// ======================================================================================================================================
void sampler_loop() {
    HidState& s = st();
    while (!s.stop.load()) {
        if (s.background.load() && s.resources.load() > 0) hid_sample_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void sampler_start() {
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.thread_mu);
    if (s.sampler.joinable()) return;
    s.sampler = std::thread(sampler_loop);
}

void sampler_stop() {
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.thread_mu);
    s.stop.store(true);
    if (s.sampler.joinable()) s.sampler.join();
    s.stop.store(false);
}

// ======================================================================================================================================
// The interfaces
// ======================================================================================================================================
class AppletResource : public ipc::Service {
public:
    AppletResource() : ipc::Service("nn::hid::IAppletResource") {
        st().resources.fetch_add(1);
        resources_gauge().add();
        add_cmd(0, "GetSharedMemoryHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            HidState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (!shm_get(s)) return kernel::kOutOfMemory;
            const uint32_t h = s.shm->new_handle();
            if (!h) return kernel::kOutOfHandles;
            rsp.out_copy.push_back(h);
            return 0;
        });
    }
    ~AppletResource() override {
        st().resources.fetch_sub(1);
        resources_gauge().sub();
    }
};

std::shared_ptr<ipc::Service> make_vibration_list() {
    return make_iface("nn::hid::IActiveVibrationDeviceList", [](ipc::Service& s) {
        s.add_cmd(0, "ActivateVibrationDevice", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
    });
}

// A command accepted and ignored: result 0 and `zeros` zero bytes of output.
void ignore(ipc::Service& s, uint32_t id, const char* name, size_t zeros = 0) {
    s.add_cmd(id, name, [name, zeros](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        NXLOG_D(kTag, "%s: accepted and ignored", name);
        rsp.out.assign(zeros, 0);
        return 0;
    });
}

// A command answered with one copy handle of value 0 (an object this console does not have).
void zero_handle(ipc::Service& s, uint32_t id, const char* name) {
    s.add_cmd(id, name, [name](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        NXLOG_D(kTag, "%s: no object, handle 0", name);
        rsp.out_copy.push_back(0);
        return 0;
    });
}

// Sets one of the state's flags or values under the lock.
template <class F>
void with_state(F&& f) {
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    f(s);
}

void set_joy_assignment(uint32_t id, uint32_t mode) {
    const int e = entry_of_id(id);
    if (e < 0) return;
    with_state([&](HidState& s) {
        if (uint8_t* m = shm_get(s)) wr32(entry_base(m, e) + hs::kJoyAssignmentMode, mode);
    });
}

void add_activation(ipc::Service& s) {
    s.add_cmd(0, "CreateAppletResource", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(std::make_shared<AppletResource>());
        sampler_start();
        NXLOG_I(kTag, "CreateAppletResource: the 5 ms sampler runs while an applet resource is alive");
        return 0;
    });
    s.add_cmd(1, "ActivateDebugPad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.debug_pad_active = true; });
        return 0;
    });
    s.add_cmd(11, "ActivateTouchScreen", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.touch_active = true; });
        return 0;
    });
    s.add_cmd(21, "ActivateMouse", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.mouse_active = true; });
        return 0;
    });
    s.add_cmd(31, "ActivateKeyboard", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.keyboard_active = true; });
        return 0;
    });
    ignore(s, 32, "SendKeyboardLockKeyEvent");
    zero_handle(s, 40, "AcquireXpadIdEventHandle");
    ignore(s, 51, "ActivateXpad");
    ignore(s, 55, "GetXpadIds", 8);
    ignore(s, 56, "ActivateJoyXpad");
    zero_handle(s, 58, "GetJoyXpadLifoHandle");
    ignore(s, 59, "GetJoyXpadIds", 8);
    ignore(s, 60, "ActivateSixAxisSensor");
    ignore(s, 61, "DeactivateSixAxisSensor");
    zero_handle(s, 62, "GetSixAxisSensorLifoHandle");
    ignore(s, 63, "ActivateJoySixAxisSensor");
    ignore(s, 64, "DeactivateJoySixAxisSensor");
    zero_handle(s, 65, "GetJoySixAxisSensorLifoHandle");
    ignore(s, 66, "StartSixAxisSensor");
    ignore(s, 67, "StopSixAxisSensor");
    s.add_cmd(68, "IsSixAxisSensorFusionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        bool on = false;
        with_state([&](HidState& h) { on = h.sixaxis_fusion; });
        rsp.push<uint8_t>(on ? 1 : 0);
        return 0;
    });
    s.add_cmd(69, "EnableSixAxisSensorFusion", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const bool on = req.arg<uint8_t>(0) != 0;
        with_state([&](HidState& h) { h.sixaxis_fusion = on; });
        return 0;
    });
    ignore(s, 70, "SetSixAxisSensorFusionParameters");
    ignore(s, 71, "GetSixAxisSensorFusionParameters", 8);
    ignore(s, 72, "ResetSixAxisSensorFusionParameters");
    ignore(s, 73, "SetAccelerometerParameters");
    ignore(s, 74, "GetAccelerometerParameters", 8);
    ignore(s, 75, "ResetAccelerometerParameters");
    ignore(s, 76, "SetAccelerometerPlayMode");
    ignore(s, 77, "GetAccelerometerPlayMode", 4);
    ignore(s, 78, "ResetAccelerometerPlayMode");
    ignore(s, 79, "SetGyroscopeZeroDriftMode");
    ignore(s, 80, "GetGyroscopeZeroDriftMode", 4);
    ignore(s, 81, "ResetGyroscopeZeroDriftMode");
    s.add_cmd(82, "IsSixAxisSensorAtRest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int e = entry_of_id(handle_npad(req.arg<uint32_t>(0)));
        bool rest = true;
        if (e >= 0) {
            InputSnapshot snap;
            read_input(&snap);
            rest = snap.npad[slot_of_entry(e)].six_axis.at_rest;
        }
        rsp.push<uint8_t>(rest ? 1 : 0);
        return 0;
    });
    ignore(s, 83, "IsFirmwareUpdateAvailableForSixAxisSensor", 1);
    s.add_cmd(91, "ActivateGesture", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int32_t id = req.arg<int32_t>(0);
        with_state([&](HidState& h) {
            uint8_t* m = shm_get(h);
            if (m && id != h.basic_gesture_id) {
                wr64(m + hs::kGesture + hs::kRingCount, 0);
                ring_push(m + hs::kGesture, h.gesture.last_state());
            }
            h.basic_gesture_id = id;
        });
        return 0;
    });
}

void add_npad(ipc::Service& s) {
    s.add_cmd(100, "SetSupportedNpadStyleSet", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint32_t styles = req.arg<uint32_t>(0);
        uint32_t was = 0;
        with_state([&](HidState& h) {
            was = h.supported_styles;
            h.supported_styles = styles;
        });
        if (was != styles) NXLOG_I(kTag, "SetSupportedNpadStyleSet: 0x%x", styles);
        return 0;
    });
    s.add_cmd(101, "GetSupportedNpadStyleSet", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t styles = 0;
        with_state([&](HidState& h) { styles = h.supported_styles; });
        rsp.push<uint32_t>(styles);
        return 0;
    });
    s.add_cmd(102, "SetSupportedNpadIdType", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const ipc::Buffer b = req.in_buf(0);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(b.size / 4, kEntries));
        uint32_t ids[kEntries] = {};
        buf_read(b, ids, n * 4ull);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const int e = entry_of_id(ids[i]);
            if (e >= 0) mask |= 1u << e;
        }
        uint32_t was = 0;
        with_state([&](HidState& h) {
            was = h.supported_index_mask;
            h.supported_index_mask = mask;
        });
        std::string list;
        for (uint32_t i = 0; i < n; ++i) list += (i ? "," : "") + std::to_string(ids[i]);
        NXLOG_I(kTag, "SetSupportedNpadIdType: ids [%s], entry mask 0x%x", list.c_str(), mask);
        if (mask != was) hid_bench_clear("the game set other supported npad ids");
        return 0;
    });
    s.add_cmd(103, "ActivateNpad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.npad_active = true; });
        return 0;
    });
    s.add_cmd(104, "DeactivateNpad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.npad_active = false; });
        return 0;
    });
    s.add_cmd(106, "AcquireNpadStyleSetUpdateEventHandle", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int e = entry_of_id(req.arg<uint32_t>(0));
        if (e < 0) return kInvalidNpadIdType;
        uint32_t h = 0;
        with_state([&](HidState& hs_) {
            h = hs_.style_event[e].new_handle();
            if (h) hs_.style_event[e].signal();
        });
        if (!h) return kernel::kOutOfHandles;
        rsp.out_copy.push_back(h);
        return 0;
    });
    s.add_cmd(107, "DisconnectNpad", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        NXLOG_I(kTag, "DisconnectNpad(0x%x): the controller stays connected", req.arg<uint32_t>(0));
        return 0;
    });
    s.add_cmd(108, "GetPlayerLedPattern", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        static constexpr uint64_t kLeds[8] = {0b0001, 0b0011, 0b0111, 0b1111, 0b1001, 0b0101, 0b1101, 0b0110};
        const uint32_t id = req.arg<uint32_t>(0);
        if (id < 8) rsp.push<uint64_t>(kLeds[id]);
        else if (id == 0x10 || id == 0x20) rsp.push<uint64_t>(0);
        else return kInvalidNpadIdType;
        return 0;
    });
    s.add_cmd(109, "ActivateNpadWithRevision", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.npad_active = true; });
        NXLOG_I(kTag, "ActivateNpadWithRevision(%d)", req.arg<int32_t>(0));
        return 0;
    });
    s.add_cmd(120, "SetNpadJoyHoldType", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint64_t hold = req.arg<uint64_t>(8);
        with_state([&](HidState& h) { h.joy_hold = hold; });
        NXLOG_I(kTag, "SetNpadJoyHoldType(%llu)", static_cast<unsigned long long>(hold));
        return 0;
    });
    s.add_cmd(121, "GetNpadJoyHoldType", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint64_t hold = 0;
        with_state([&](HidState& h) { hold = h.joy_hold; });
        rsp.push<uint64_t>(hold);
        return 0;
    });
    s.add_cmd(122, "SetNpadJoyAssignmentModeSingleByDefault", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        set_joy_assignment(req.arg<uint32_t>(0), 1);
        return 0;
    });
    s.add_cmd(123, "SetNpadJoyAssignmentModeSingle", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        set_joy_assignment(req.arg<uint32_t>(0), 1);
        return 0;
    });
    s.add_cmd(124, "SetNpadJoyAssignmentModeDual", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        set_joy_assignment(req.arg<uint32_t>(0), 0);
        return 0;
    });
    ignore(s, 125, "MergeSingleJoyAsDualJoy");
    ignore(s, 126, "StartLrAssignmentMode");
    ignore(s, 127, "StopLrAssignmentMode");
    s.add_cmd(128, "SetNpadHandheldActivationMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int64_t mode = req.arg<int64_t>(8);
        with_state([&](HidState& h) { h.handheld_activation = mode; });
        return 0;
    });
    s.add_cmd(129, "GetNpadHandheldActivationMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        int64_t mode = 0;
        with_state([&](HidState& h) { mode = h.handheld_activation; });
        rsp.push<int64_t>(mode);
        return 0;
    });
    ignore(s, 130, "SwapNpadAssignment");
    s.add_cmd(131, "IsUnintendedHomeButtonInputProtectionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        bool on = false;
        with_state([&](HidState& h) { on = h.home_protection; });
        rsp.push<uint8_t>(on ? 1 : 0);
        return 0;
    });
    s.add_cmd(132, "EnableUnintendedHomeButtonInputProtection", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const bool on = req.arg<uint8_t>(0) != 0;
        with_state([&](HidState& h) { h.home_protection = on; });
        return 0;
    });
    s.add_cmd(133, "SetNpadJoyAssignmentModeSingleWithDestination", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        set_joy_assignment(req.arg<uint32_t>(0), 1);
        rsp.push<uint64_t>(0);   // not reassigned (u8), padding, the new npad id (u32) 0
        return 0;
    });
    ignore(s, 134, "SetNpadAnalogStickUseCenterClamp");
}

void add_vibration(ipc::Service& s) {
    s.add_cmd(200, "GetVibrationDeviceInfo", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const uint32_t h = req.arg<uint32_t>(0);
        if (entry_of_id(handle_npad(h)) < 0) return kInvalidNpadIdType;
        const uint32_t pos = handle_position(h);
        if (pos > 1) return kInvalidDeviceIndex;
        uint32_t type = 0, position = 0;
        switch (handle_type(h)) {
            case 3: case 4: case 5: case 6: case 7: case 32: case 33:   // the npad styles with a linear resonant actuator
                type = 1;
                position = pos + 1;
                break;
            case 8:   // GameCube controller: its eccentric rotating mass motor
                type = 2;
                break;
            default: break;
        }
        rsp.push<uint32_t>(type);
        rsp.push<uint32_t>(position);
        return 0;
    });
    s.add_cmd(201, "SendVibrationValue", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const Sent one = {req.arg<uint32_t>(0), {req.arg<float>(4), req.arg<float>(8), req.arg<float>(0xC), req.arg<float>(0x10)}};
        store_vibration(st(), &one, 1);
        forward_vibration(&one, 1);
        return 0;
    });
    s.add_cmd(202, "GetActualVibrationValue", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const uint32_t h = req.arg<uint32_t>(0);
        Vib v;
        with_state([&](HidState& hs_) {
            const auto it = hs_.vibration.find(h);
            if (it != hs_.vibration.end()) v = it->second;
        });
        rsp.push<float>(v.amp_low);
        rsp.push<float>(v.freq_low);
        rsp.push<float>(v.amp_high);
        rsp.push<float>(v.freq_high);
        return 0;
    });
    s.add_cmd(203, "CreateActiveVibrationDeviceList", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_vibration_list());
        return 0;
    });
    s.add_cmd(204, "PermitVibration", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const bool on = req.arg<uint8_t>(0) != 0;
        with_state([&](HidState& h) { h.vibration_permitted = on; });
        return 0;
    });
    s.add_cmd(205, "IsVibrationPermitted", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        bool on = true;
        with_state([&](HidState& h) { on = h.vibration_permitted; });
        rsp.push<uint8_t>(on ? 1 : 0);
        return 0;
    });
    s.add_cmd(206, "SendVibrationValues", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        Sent sent[16];
        const size_t n = vibration_list(req, sent);
        store_vibration(st(), sent, n);
        forward_vibration(sent, n);
        return 0;
    });
    ignore(s, 207, "SendVibrationGcErmCommand");
    ignore(s, 208, "GetActualVibrationGcErmCommand", 8);
    ignore(s, 209, "BeginPermitVibrationSession");
    ignore(s, 210, "EndPermitVibrationSession");
    s.add_cmd(211, "IsVibrationDeviceMounted", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int e = entry_of_id(handle_npad(req.arg<uint32_t>(0)));
        bool mounted = false;
        if (e >= 0) with_state([&](HidState& h) { mounted = h.connected[e]; });
        rsp.push<uint8_t>(mounted ? 1 : 0);
        return 0;
    });
    ignore(s, 212, "SendVibrationValueInBool");
    ignore(s, 213, "Unknown213");
    s.add_cmd(214, "SendVibrationValues2", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        Sent sent[16];
        const size_t n = vibration_list(req, sent);
        bool first = false;
        {
            HidState& h = st();
            std::lock_guard<std::mutex> lk(h.mu);
            for (size_t i = 0; i < n; ++i) h.vibration[sent[i].handle] = sent[i].v;
            if (n && !h.logged_214) first = h.logged_214 = true;
        }
        if (first) NXLOG_I(kTag, "SendVibrationValues2 (the Switch 2 form): %zu values, forwarded as SendVibrationValues", n);
        forward_vibration(sent, n);
        return 0;
    });
}

void add_system(ipc::Service& s) {
    ignore(s, 300, "ActivateConsoleSixAxisSensor");
    ignore(s, 301, "StartConsoleSixAxisSensor");
    ignore(s, 302, "StopConsoleSixAxisSensor");
    ignore(s, 303, "ActivateSevenSixAxisSensor");
    ignore(s, 304, "StartSevenSixAxisSensor");
    ignore(s, 305, "StopSevenSixAxisSensor");
    ignore(s, 306, "InitializeSevenSixAxisSensor");
    ignore(s, 307, "FinalizeSevenSixAxisSensor");
    ignore(s, 308, "SetSevenSixAxisSensorFusionStrength");
    ignore(s, 309, "GetSevenSixAxisSensorFusionStrength", 4);
    ignore(s, 310, "ResetSevenSixAxisSensorTimestamp");
    s.add_cmd(400, "IsUsbFullKeyControllerEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    s.add_cmd(1000, "SetNpadCommunicationMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int64_t mode = req.arg<int64_t>(0);
        with_state([&](HidState& h) { h.npad_communication = mode; });
        return 0;
    });
    s.add_cmd(1001, "GetNpadCommunicationMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        int64_t mode = 0;
        with_state([&](HidState& h) { mode = h.npad_communication; });
        rsp.push<int64_t>(mode);
        return 0;
    });
    ignore(s, 1002, "SetTouchScreenConfiguration");
    ignore(s, 1003, "IsFirmwareUpdateNeededForNotification", 1);
    ignore(s, 1004, "SetTouchScreenResolution");
}

// InitializeNmouse in both forms: u8 pointer type @0, s32 width @4, s32 height @8, u32 @0xC, aruid @0x10, u64 size @0x18.
uint32_t initialize_nmouse(ipc::Request& req, const char* what) {
    const int32_t w = req.arg<int32_t>(4), h = req.arg<int32_t>(8);
    const uint64_t size = req.arg<uint64_t>(0x18);
    with_state([&](HidState& s) {
        s.nm_w = w > 0 ? w : kDefaultRangeW;
        s.nm_h = h > 0 ? h : kDefaultRangeH;
        s.nm_addr = lent_block(req, size, kNmBytes, what, &s.nm_size);
        if (s.nm_addr) nmouse_setup(s, s.nm_addr);
        NXLOG_I(kTag, "%s: pointer range %dx%d, block 0x%llx", what, s.nm_w, s.nm_h, static_cast<unsigned long long>(s.nm_addr));
    });
    return 0;
}

void add_mouse(ipc::Service& s) {
    s.add_cmd(3110, "InitializeJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint64_t size = req.arg<uint64_t>(8);
        with_state([&](HidState& h) {
            h.jcms_addr = lent_block(req, size, kJcmsBytes, "InitializeJcms", &h.jcms_size);
            if (h.jcms_addr) jcms_setup(h.jcms_addr);
            NXLOG_I(kTag, "InitializeJcms: block 0x%llx", static_cast<unsigned long long>(h.jcms_addr));
        });
        return 0;
    });
    s.add_cmd(3111, "FinalizeJcms", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) { h.jcms_addr = h.jcms_size = 0; });
        NXLOG_I(kTag, "FinalizeJcms");
        return 0;
    });
    s.add_cmd(3112, "StartJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        uint32_t calls = 0;
        with_state([&](HidState& h) { calls = ++h.start_jcms_calls; });
        if (calls <= 3) NXLOG_I(kTag, "StartJcms(0x%x) (call %u)", req.arg<uint32_t>(0), calls);
        return 0;
    });
    ignore(s, 3113, "StopJcms");
    s.add_cmd(3130, "InitializeNmouseWithStateRange", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        return initialize_nmouse(req, "InitializeNmouseWithStateRange");
    });
    s.add_cmd(3131, "FinalizeNmouse", [](ipc::Request&, ipc::Response&) -> uint32_t {
        with_state([](HidState& h) {
            h.nm_addr = h.nm_size = 0;
            g_jc2_listed.store(false);
            g_jc2_active.store(false);
        });
        NXLOG_I(kTag, "FinalizeNmouse");
        return 0;
    });
    s.add_cmd(3132, "SetNmousePointerRanges", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int32_t w = req.arg<int32_t>(0), h = req.arg<int32_t>(4);
        with_state([&](HidState& hs_) {
            if (w > 0) hs_.nm_w = w;
            if (h > 0) hs_.nm_h = h;
            NXLOG_I(kTag, "SetNmousePointerRanges(%d, %d): %dx%d", w, h, hs_.nm_w, hs_.nm_h);
        });
        return 0;
    });
    s.add_cmd(3133, "GetNmousePointerRanges", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        int32_t w = 0, h = 0;
        with_state([&](HidState& hs_) {
            w = hs_.nm_w;
            h = hs_.nm_h;
        });
        rsp.push<int32_t>(w);
        rsp.push<int32_t>(h);
        return 0;
    });
    s.add_cmd(3135, "EnableShowingCursor", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint8_t on = req.arg<uint8_t>(0) ? 1 : 0;
        with_state([&](HidState& h) {
            if (h.nm_addr) host_ptr(h.nm_addr)[kNmShowCursor] = on;
        });
        return 0;
    });
    s.add_cmd(3136, "SetNmouseButtonConfigJoyLeftJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint64_t cfg = req.arg<uint64_t>(8);
        with_state([&](HidState& h) { h.nm_btn_left = cfg; });
        return 0;
    });
    s.add_cmd(3137, "SetNmouseButtonConfigJoyRightJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint64_t cfg = req.arg<uint64_t>(8);
        with_state([&](HidState& h) { h.nm_btn_right = cfg; });
        return 0;
    });
    s.add_cmd(3138, "SetMergedNmouseSourceTypes", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint32_t types = req.arg<uint32_t>(0);
        with_state([&](HidState& h) { h.nm_merged_types = types; });
        return 0;
    });
    s.add_cmd(3139, "InitializeNmouse", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        return initialize_nmouse(req, "InitializeNmouse");
    });
    s.add_cmd(3140, "SetNmousePointerPosition", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        NXLOG_I(kTag, "SetNmousePointerPosition(%d, %d): the pointer is the PC mouse", req.arg<int32_t>(0), req.arg<int32_t>(4));
        return 0;
    });
    s.add_cmd(3141, "EnableNmousePointer", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint8_t on = req.arg<uint8_t>(0) ? 1 : 0;
        const uint64_t handle = req.arg<uint64_t>(0x10);
        with_state([&](HidState& h) {
            if (!h.nm_addr) return;
            uint8_t* b = host_ptr(h.nm_addr);
            for (uint32_t k = 0; k < kNmSlots; ++k) {
                const uint8_t* slot = b + kNmTable + k * kNmTableStride;
                if (slot[0x8] != 0 && rd64(slot) == handle) b[kNmInfo + k * kNmInfoStride + 0x10] = on;
            }
        });
        return 0;
    });
    ignore(s, 3142, "RestrictNmousePointer");
    ignore(s, 3143, "Cmd3143");
    ignore(s, 3150, "SetMouseLibraryVersion");
}

std::shared_ptr<ipc::Service> make_hid_server() {
    return make_iface("nn::hid::IHidServer", [](ipc::Service& s) {
        add_activation(s);
        add_npad(s);
        add_vibration(s);
        add_system(s);
        add_mouse(s);
    });
}

}  // namespace

// ======================================================================================================================================
// Entry points
// ======================================================================================================================================
void hid_sample_once() {
    InputSnapshot snap;
    read_input(&snap);
    hid_sdk_on_sample(snap);
    const bool docked = amctl::docked();
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    sample_jcms(s);
    uint8_t* m = shm_get(s);
    if (!m) return;
    if (s.npad_active)
        for (uint32_t e = 0; e < kEntries; ++e) sample_npad(s, m, static_cast<int>(e), snap.npad[slot_of_entry(static_cast<int>(e))], docked);
    sample_devices(s, m, snap);
    sample_nmouse(s);
}

void hid_set_background_sampling(bool enabled) { st().background.store(enabled); }

uint8_t* hid_shared_memory_for_tests() {
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return shm_get(s);
}

void hid_read_input(nxrt::hid::InputSnapshot* out) {
    if (out) read_input(out);
}

void hid_read_raw_input(nxrt::hid::InputSnapshot* out) {
    if (out) read_input_raw(out);
}

void hid_supported_sets(uint32_t* styles, uint32_t* index_mask) {
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (styles) *styles = s.supported_styles;
    if (index_mask) *index_mask = s.supported_index_mask;
}

void hid_signal_style_event(uint32_t index) {
    if (index >= kEntries) return;
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.style_event[index].signal();
}

bool hid_sampler_live() {
    const HidState& s = st();
    return s.background.load() && s.resources.load() > 0;
}

void hid_mode_changed() { hid_sample_once(); }

void register_hid() {
    add("hid", [] { return make_hid_server(); });
}

void hid_reset() {
    hid_sdk_reset();
    sampler_stop();
    HidState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.clear_locked();
    g_jc2_listed.store(false);
    g_jc2_active.store(false);
    s.background.store(true);
    if (uint8_t* m = shm_usable(s)) shm_init(m);
    else s.shm.reset();
}

}  // namespace nxrt::services

// ======================================================================================================================================
// The play layer's side (nxrt/input_source.h, nxrt/jc2_mouse.h)
// ======================================================================================================================================
namespace nxrt::hid {

namespace svc = nxrt::services;

uint32_t supported_player_mask() {
    uint32_t mask = 0;
    svc::hid_supported_sets(nullptr, &mask);
    return mask & 0xFFu;
}

Jc2MouseMode jc2_mouse_mode() {
    int m = svc::g_jc2_mode.load();
    if (m < 0) {
        std::string v = config_get_str("input", "jc2_mouse", "auto");
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        m = static_cast<int>(v == "off" ? Jc2MouseMode::Off : v == "on" ? Jc2MouseMode::On : Jc2MouseMode::Auto);
        if (v != "off" && v != "on" && v != "auto") NXLOG_W("hid", "[input] jc2_mouse = %s: not auto, on or off; auto is used", v.c_str());
        svc::g_jc2_mode.store(m);
    }
    return static_cast<Jc2MouseMode>(m);
}

void jc2_mouse_set_mode(Jc2MouseMode m) { svc::g_jc2_mode.store(static_cast<int>(m)); }

const char* jc2_mouse_mode_name(Jc2MouseMode m) {
    if (m == Jc2MouseMode::Off) return "off";
    if (m == Jc2MouseMode::On) return "on";
    return "auto";
}

void jc2_mouse_feed(const HostMouse& m) {
    svc::Feed& f = svc::feed();
    std::lock_guard<std::mutex> lk(f.mu);
    f.m = m;
    f.wheel_x += m.wheel_x;
    f.wheel_y += m.wheel_y;
    f.last_ns = svc::steady_ns();
}

void jc2_mouse_request(bool want) { svc::g_jc2_request.store(want); }

bool jc2_mouse_wants_pointer() {
    switch (jc2_mouse_mode()) {
        case Jc2MouseMode::Off: return false;
        case Jc2MouseMode::On: return svc::g_jc2_listed.load();
        default: return svc::g_jc2_listed.load() && svc::g_jc2_request.load();
    }
}

void jc2_mouse_set_pointer_space(int w, int h) {
    const bool ok = w > 0 && w <= 16384 && h > 0 && h <= 16384;
    svc::g_jc2_space.store(ok ? (static_cast<uint64_t>(w) << 32 | static_cast<uint64_t>(h)) : 0);
}

bool jc2_mouse_active() { return jc2_mouse_mode() != Jc2MouseMode::Off && svc::g_jc2_listed.load() && svc::feed_live(); }

bool jc2_mouse_listed() { return svc::g_jc2_listed.load(); }

}  // namespace nxrt::hid
