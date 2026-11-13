// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE: the controllers this machine talks to and the pads players hold, on top of a platform
// Bluetooth stack (Transport). Platform-neutral and clock-driven, so tests/services/test_sw2_manager.cpp drives the whole connect, pair
// and reconnect cycle with a fake transport.
//
// A controller (one Bluetooth device) goes through:
//   advertisement -> Connecting (Transport::connect; the transport discovers the service and subscribes to command replies, then
//   on_link_up) -> Handshake (one command in flight at a time, each with a 2 s answer timeout: its identity at 0x13000; bonding to this
//   host when Sync is held and this host's address is known (four 0x15 commands); the gyro's factory offset; the stick calibration, the
//   user's when set, else the factory's; the GameCube triggers' rest values; features set and enabled) -> Live (input reports
//   subscribed, player LEDs written once a second, which also keeps the link: it drops after about ten seconds without a host write;
//   rumble packets every 15 ms while the game vibrates, then three of silence) -> gone (on_link_down, or a timeout: connecting 10 s,
//   the handshake 20 s). A controller that failed is not connected again for 3 s. A controller bonded to this host advertises toward it
//   when a button is pressed (wake) and connects without Sync; one bonded to another host (a Switch 2 nearby) is never touched.
//
// A pad is what one player holds: a Pro Controller 2, a GameCube controller, a left and a right Joy-Con 2 together, or one Joy-Con 2 held
// sideways. A Joy-Con that comes up joins a pad that holds only the other side; otherwise it starts a pad of its own. SL + SR held for
// 3 s on a Joy-Con of a pair makes it a pad of its own; L on a left one alone and R on a right one alone, held together for 3 s, pair
// them. A pad is presented as its wired counterpart (PadView: SDL gamepad numbering, the same vendor and product ids).
//
// Threads: every entry point is thread safe. Work is done under one lock; the transport is called after the lock is released, in the
// order the work asked for it, so a transport may call back from inside its own calls.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "nxrt/sw2/proto.h"

namespace nxrt::sw2 {

enum class Channel : uint8_t { Command, Vibration };

// The platform's Bluetooth LE stack. dev is the transport's own id for a device (Windows: its address; macOS: a number per peripheral).
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool start() = 0;                            // scan; false: not available here (the transport logs why)
    virtual void stop() = 0;                             // stop scanning, drop every link; no callbacks after it returns
    virtual void connect(uint64_t dev) = 0;              // link, discover, subscribe to replies -> Manager::on_link_up (or on_link_down)
    virtual void disconnect(uint64_t dev) = 0;           // the manager has already forgotten it
    virtual void enable_input(uint64_t dev) = 0;         // subscribe to input reports -> Manager::on_input
    virtual bool write(uint64_t dev, Channel ch, const uint8_t* d, size_t n) = 0;   // without response; false: not sent
    virtual size_t max_write(uint64_t dev) { (void)dev; return kVibReportLen; }    // the largest write the link takes now
    virtual void on_thread_start() {}                    // the service's timer thread starts (it calls write through tick)
    // read the controller's own report (proto.h kUuidReport*) once -> Manager::on_power; a transport without it leaves the level unknown
    virtual void read_power(uint64_t dev) { (void)dev; }
};

// SDL's gamepad button numbering (SDL_GamepadButton), so a pad reads exactly as a wired one through SDL (the bridge asserts it).
enum PadButton : uint8_t {
    kPbSouth = 0, kPbEast, kPbWest, kPbNorth, kPbBack, kPbGuide, kPbStart, kPbLeftStick, kPbRightStick, kPbLeftShoulder, kPbRightShoulder,
    kPbDpadUp, kPbDpadDown, kPbDpadLeft, kPbDpadRight, kPbMisc1, kPbRightPaddle1, kPbLeftPaddle1, kPbRightPaddle2, kPbLeftPaddle2,
    kPbTouchpad, kPbMisc2, kPbCount
};
enum PadAxis : uint8_t { kPaLeftX = 0, kPaLeftY, kPaRightX, kPaRightY, kPaLeftTrigger, kPaRightTrigger, kPaCount };

enum class PadType : uint8_t { Pro, GameCube, JoyConPair, JoyConLeft, JoyConRight };
const char* pad_type_name(PadType t);   // the name a wired one has
uint16_t pad_type_pid(PadType t);       // the product id a wired one has (0x2068 for a pair, as SDL's combined Joy-Cons)

// One pad as a gamepad.
struct PadView {
    uint32_t id = 0;                // stable while the pad exists
    uint32_t generation = 0;        // changes when its controllers change (a Joy-Con joins or leaves): a consumer makes its device again
    PadType type = PadType::Pro;
    uint16_t vendor = kVendorId, product = kPidPro;
    char name[48] = {};
    int player = -1;                // what set_player gave it (0-based), -1 none
    uint32_t buttons = 0;           // bit n = PadButton n
    int16_t axes[kPaCount] = {};    // SDL ranges: sticks -32768..32767 (y down), triggers -32768 released .. 32767 full
    bool motion = false;            // accel / gyro hold a sample
    float accel[3] = {}, gyro[3] = {};   // SDL's sensor frame: m/s^2, rad/s
    uint64_t updated_ns = 0;        // the newest report's arrival
    uint32_t reports = 0;           // reports so far (all its controllers)
    uint16_t battery_mv = 0;        // the lowest of its controllers
    int battery_level = -1;         // the controllers' own battery level 0..9 (the lowest of them), -1 not known yet (read 0.5 s after it
                                    // comes up, then every 30 s)
    bool charging = false;          // one of its controllers is charging
    // The Joy-Con 2 mouse sensor of each side (0 left, 1 right): movement summed since the last take_mouse, and whether it is on a surface.
    bool mouse_present[2] = {}, mouse_surface[2] = {};
    uint32_t raw_buttons[2] = {};   // each controller's own raw buttons (proto.h kRaw*): [0] a Pro / GameCube controller or a left Joy-Con, [1] a right Joy-Con
};

struct ManagerOptions {
    bool motion = true;             // report the motion sensors
    bool pair = true;               // bond controllers held in Sync (else use them unbonded, as when this host's address is unknown)
};

class Manager {
public:
    explicit Manager(const ManagerOptions& opt = {});
    ~Manager();
    void set_transport(Transport* t);   // not owned; set before any event
    // This host's own Bluetooth address, little endian (as the advertisement carries it). Unknown until set: controllers in pairing mode
    // are then used without bonding.
    void set_host_address(const uint8_t le[6]);
    bool host_address_known() const;

    // ---- transport events (any thread) ----
    void on_advert(uint64_t dev, const uint8_t* mfr, size_t n, int rssi, uint64_t now_ns);
    void on_link_up(uint64_t dev, bool has_vibration, uint64_t now_ns);
    void on_link_down(uint64_t dev, const char* why, uint64_t now_ns);
    void on_reply(uint64_t dev, const uint8_t* d, size_t n, uint64_t now_ns);
    void on_input(uint64_t dev, const uint8_t* d, size_t n, uint64_t now_ns);
    void on_power(uint64_t dev, const uint8_t* d, size_t n, uint64_t now_ns);   // the controller's own report, read (Transport::read_power)

    // ---- timers: call every few milliseconds (timeouts, LED keep-alive, rumble packets, the Joy-Con hold gestures) ----
    void tick(uint64_t now_ns);

    // ---- consumers ----
    size_t list(PadView* out, size_t max) const;     // every pad that has a live controller, oldest first
    bool view(uint32_t pad_id, PadView* out) const;
    void set_player(uint32_t pad_id, int player);   // its LEDs show it (0..7); -1: no player (player 1's lights, as the console's waiting pad)
    // The game's vibration of a player's two hands (0 left, 1 right): each value plays once, in its own 5 ms sample.
    void hd_vibration(int player, int hand, float amp_lo, float freq_lo, float amp_hi, float freq_hi, uint64_t now_ns);
    // SDL's plain rumble (0..65535 a motor) for a pad, used only while its player has no HD vibration (none for a second).
    void simple_rumble(uint32_t pad_id, uint16_t low, uint16_t high, uint64_t now_ns);
    // The game reads the Joy-Con 2 mouse sensors: Joy-Cons turn theirs on (and keep it on while connected).
    void set_mouse_wanted(bool on);
    // The mouse movement of a pad's side since the last take (counts), whether it is on a surface now.
    bool take_mouse(uint32_t pad_id, int side, int32_t* dx, int32_t* dy, bool* on_surface);
    // The same sensor by player (0-based) and side (0 left, 1 right Joy-Con of the pads that player holds), with its own running sum: the
    // hid service's raw sensor states (GetJcmsStates) take it, the pointer (take_mouse) independently. False: no such Joy-Con with its sensor on.
    bool take_sensor(int player, int side, int32_t* dx, int32_t* dy, bool* on_surface);

    // ---- tests and diagnostics ----
    size_t sessions() const;         // controllers known (any state)
    size_t live_sessions() const;
    bool bonded(uint64_t dev) const;  // the bonding finished in this session

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace nxrt::sw2
