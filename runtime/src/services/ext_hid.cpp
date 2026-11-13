// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// hid commands beyond hid.cpp's (ipc::extend on nn::hid::IHidServer: hid.cpp's own handlers win). A console with no extra hardware: the
// controllers have no Poké Ball Plus (Palma), the six-axis sensors are the plain ones, no USB full-key controller. Layouts: Lichii
// src/core/hle/service/hid/hid_server.cpp (tools/services/reference.json), Switchbrew "HID services".
//   26 ActivateDebugMouse, 41 ReleaseXpadIdEventHandle, 84 EnableSixAxisSensorUnalteredPassthrough, 89 ResetIsSixAxisSensorDeviceNewlyAssigned,
//   92 SetGestureOutputRanges, 135 / 136 Set / ClearNpadCaptureButtonAssignment, 401 EnableUsbFullKeyController: accepted.
//   85 IsSixAxisSensorUnalteredPassthroughEnabled: false.
//   87 LoadSixAxisSensorCalibrationParameter, 88 GetSixAxisSensorIcInformation: the output structure zeroed (no factory data to give).
//   402 IsUsbFullKeyControllerConnected: false. 403 HasBattery: true; 404 HasLeftRightBattery: true, true. 405 GetNpadInterfaceType and
//   406 GetNpadLeftRightInterfaceType: Bluetooth (1). 407 GetNpadOfHighestBatteryLevel: the first npad id the caller listed.
//   500-529 Palma: GetPalmaConnectionHandle and every command on a handle answer hid InvalidPalmaHandle (202-3302: none is connected);
//   the connection policy commands (522, 523, 525, 527, 529) are accepted.
//   3000-3015 pad and keyboard maps: none set (below).
#include <cstring>

#include "ext_common.h"

namespace nxrt::services::ext {

namespace {
constexpr const char* kHid = "nn::hid::IHidServer";
constexpr uint32_t kInvalidPalmaHandle = res(202, 3302);
constexpr uint8_t kInterfaceBluetooth = 1;   // nn::hid::NpadInterfaceType: 1 Bluetooth, 2 Rail, 3 USB, 4 Embedded
}  // namespace

void register_hid_extra() {
    ok(kHid, 26, "ActivateDebugMouse");
    ok(kHid, 41, "ReleaseXpadIdEventHandle");
    ok(kHid, 84, "EnableSixAxisSensorUnalteredPassthrough");
    value<uint8_t>(kHid, 85, "IsSixAxisSensorUnalteredPassthroughEnabled", 0);
    zero_buffer(kHid, 87, "LoadSixAxisSensorCalibrationParameter");
    zero_buffer(kHid, 88, "GetSixAxisSensorIcInformation");
    ok(kHid, 89, "ResetIsSixAxisSensorDeviceNewlyAssigned");
    ok(kHid, 92, "SetGestureOutputRanges");
    ok(kHid, 135, "SetNpadCaptureButtonAssignment");
    ok(kHid, 136, "ClearNpadCaptureButtonAssignment");
    ok(kHid, 401, "EnableUsbFullKeyController");
    value<uint8_t>(kHid, 402, "IsUsbFullKeyControllerConnected", 0);
    value<uint8_t>(kHid, 403, "HasBattery", 1);
    add(kHid, 404, "HasLeftRightBattery", [](Service&, Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(1);
        rsp.push<uint8_t>(1);
        return 0;
    });
    value<uint8_t>(kHid, 405, "GetNpadInterfaceType", kInterfaceBluetooth);
    add(kHid, 406, "GetNpadLeftRightInterfaceType", [](Service&, Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(kInterfaceBluetooth);
        rsp.push<uint8_t>(kInterfaceBluetooth);
        return 0;
    });
    add(kHid, 407, "GetNpadOfHighestBatteryLevel", [](Service&, Request& req, Response& rsp) -> uint32_t {
        uint32_t first = 0;   // nn::hid::NpadIdType No1
        buf_read(req.in_buf(0), &first, sizeof first);
        rsp.push<uint32_t>(first);
        return 0;
    });

    // Palma (Poké Ball Plus): none is connected.
    static const struct {
        uint32_t id;
        const char* name;
    } kPalma[] = {
        {500, "GetPalmaConnectionHandle"}, {501, "InitializePalma"}, {502, "AcquirePalmaOperationCompleteEvent"},
        {503, "GetPalmaOperationInfo"}, {504, "PlayPalmaActivity"}, {505, "SetPalmaFrModeType"}, {506, "ReadPalmaStep"},
        {507, "EnablePalmaStep"}, {508, "ResetPalmaStep"}, {509, "ReadPalmaApplicationSection"}, {510, "WritePalmaApplicationSection"},
        {511, "ReadPalmaUniqueCode"}, {512, "SetPalmaUniqueCodeInvalid"}, {513, "WritePalmaActivityEntry"},
        {514, "WritePalmaRgbLedPatternEntry"}, {515, "WritePalmaWaveEntry"}, {516, "SetPalmaDataBaseIdentificationVersion"},
        {517, "GetPalmaDataBaseIdentificationVersion"}, {518, "SuspendPalmaFeature"}, {519, "GetPalmaOperationResult"},
        {520, "ReadPalmaPlayLog"}, {521, "ResetPalmaPlayLog"}, {524, "PairPalma"}, {526, "CancelWritePalmaWaveEntry"},
        {528, "GetPalmaBluetoothAddress"},
    };
    for (const auto& p : kPalma) unavailable(kHid, p.id, p.name, kInvalidPalmaHandle);
    ok(kHid, 522, "SetIsPalmaAllConnectable");
    ok(kHid, 523, "SetIsPalmaPairedConnectable");
    ok(kHid, 525, "SetPalmaBoostMode");
    ok(kHid, 527, "EnablePalmaBoostMode");
    ok(kHid, 529, "SetDisallowedPalmaConnection");

    // 3000-3015 (21.0.0+ for 3012-3015): the debug pad and full-key generic pad / keyboard maps (nn::settings::GetFullKeyGenericPadMap,
    // which Legends Z-A imports). No remapping is set: the Get forms answer as Lichii does (success, 0x80 zero bytes) and zero the
    // caller's map buffer; Set and Reset are accepted.
    static const struct {
        uint32_t id;
        const char* name;
        bool get;
    } kMaps[] = {
        {3000, "GetDebugPadGenericPadMap", true},  {3001, "SetDebugPadGenericPadMap", false},  {3002, "ResetDebugPadGenericPadMap", false},
        {3003, "GetDebugPadKeyboardMap", true},    {3004, "SetDebugPadKeyboardMap", false},    {3005, "ResetDebugPadKeyboardMap", false},
        {3006, "GetFullKeyGenericPadMap", true},   {3007, "SetFullKeyGenericPadMap", false},   {3008, "ResetFullKeyGenericPadMap", false},
        {3009, "GetFullKeyKeyboardMap", true},     {3010, "SetFullKeyKeyboardMap", false},     {3011, "ResetFullKeyKeyboardMap", false},
        {3012, "GetDebugPadGenericPadMap2", true}, {3013, "SetDebugPadGenericPadMap2", false}, {3014, "GetDebugPadKeyboardMap2", true},
        {3015, "SetDebugPadKeyboardMap2", false},
    };
    for (const auto& m : kMaps) {
        if (m.get) zero_buffer(kHid, m.id, m.name, 0x80);
        else ok(kHid, m.id, m.name);
    }
}

}  // namespace nxrt::services::ext
