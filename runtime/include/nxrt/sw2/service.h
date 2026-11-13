// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE for a game process: the manager (nxrt/sw2/manager.h), the platform's transport (CoreBluetooth on
// macOS, platform/mac/core/sw2_corebluetooth.mm; WinRT on Windows, src/input/sw2/sw2_winrt.cpp) and the timer thread that drives them.
//
// Config: [input] switch2_ble = off (default) | on.
//   off  no Bluetooth client is created at all, so the system never asks for Bluetooth permission.
//   on   controllers are found and used: a Joy-Con 2, Pro Controller 2 or GameCube controller for Switch 2 held in Sync bonds to this
//        machine and is used at once; from then on any button wakes it toward this machine. macOS asks once for Bluetooth permission;
//        the app that starts the game must declare NSBluetoothAlwaysUsageDescription (without it the transport stays off instead of
//        macOS ending the process).
// [input] switch2_ble_host = aa:bb:cc:dd:ee:ff names this machine's Bluetooth address when the system does not tell it (the address a
// controller is bonded to and wakes toward).
// [input] switch2_ble_motion = true (default) | false: the controllers' motion sensors reach the game.
// The pads reach the game as gamepads of the platform layer (src/gfx/sw2_bridge.cpp): the same kinds, buttons and sensors as wired ones.
#pragma once
#include <cstdint>
#include <memory>

#include "nxrt/sw2/manager.h"

namespace nxrt::sw2 {

bool configured();    // [input] switch2_ble is on
bool start();         // starts when configured (idempotent); false when off or the platform has no Bluetooth LE for it
void stop();
bool running();
Manager* manager();   // nullptr when not running
uint64_t now_ns();    // the clock the manager runs on (steady)

// The game's vibration (nn::hid VibrationValue, amplitudes already scaled by the player's rumble settings) of a player's hand, to the
// Switch 2 pads that player holds. No-op when not running.
void game_vibration(int player, int hand, float amp_lo, float freq_lo, float amp_hi, float freq_hi);

// Tests: makes `m` (not owned) what manager() returns while the service is not started; nullptr undoes it.
void set_manager_for_tests(Manager* m);

// The platform's transport (one per platform; nullptr where there is none).
std::unique_ptr<Transport> make_platform_transport(Manager& m);

}  // namespace nxrt::sw2
