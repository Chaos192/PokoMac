// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// bt (IBluetoothUser) and btm:u (IBtmUser, IBtmUserCore): Bluetooth LE for games, on a console with no BLE device near it. Facts: Lichii's
// service/btdrv (bt) and service/btm (btm_user.cpp, btm_user_core.cpp: command ids, the acquire answers) and nxnative's notes
// (docs/reference/services.md "Bluetooth LE at boot": Pokémon Scarlet / Violet 3.0.1 call nn::bluetooth::InitializeBle at boot, for the
// Pokémon GO Plus +, and abort when bt or btm:u is not there or answers with no event).
//
// What a game gets: the BLE events (each a valid event that never fires: nothing is ever found, connected, discovered or configured),
// stopping a scan succeeds, and the lists of scan results, connections and paired devices are empty. Starting a scan, connecting and the
// GATT commands have no device to work on and are not answered (the service's "not implemented" result, logged), so the game's request fails.
#include <array>
#include <cstring>
#include <memory>
#include <mutex>

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;

struct BtState {
    std::mutex mu;
    std::array<std::shared_ptr<ServiceEvent>, 5> events;   // bt RegisterBleEvent; btm scan, connection, service discovery, MTU config
};
BtState& bt() {
    static BtState s;
    return s;
}
uint32_t event_handle(size_t which) {
    BtState& s = bt();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.events[which]) s.events[which] = std::make_shared<ServiceEvent>(false);
    return s.events[which]->new_handle();
}

// IBtmUserCore Acquire*Event: (bool valid, copy handle).
ipc::Handler acquire(size_t which, const char* name) {
    return [which, name](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(1);
        rsp.out_copy.push_back(event_handle(which));
        NXLOG_D("btm", "%s: an event that never fires (no BLE device)", name);
        return 0;
    };
}
// An empty list: u32 count 0 (the output buffer is left as the game gave it).
uint32_t empty_list(Request&, Response& rsp) {
    rsp.push<uint32_t>(0);
    return 0;
}
uint32_t done(Request&, Response&) { return 0; }

std::shared_ptr<Service> make_btm_user_core() {
    return make_iface("IBtmUserCore", [](Service& s) {
        s.add_cmd(0, "AcquireBleScanEvent", acquire(1, "AcquireBleScanEvent"));
        s.add_cmd(4, "StopBleScanForGeneral", done);
        s.add_cmd(5, "GetBleScanResultsForGeneral", empty_list);
        s.add_cmd(7, "StopBleScanForPaired", done);
        s.add_cmd(9, "StopBleScanForSmartDevice", done);
        s.add_cmd(10, "GetBleScanResultsForSmartDevice", empty_list);
        s.add_cmd(17, "AcquireBleConnectionEvent", acquire(2, "AcquireBleConnectionEvent"));
        s.add_cmd(20, "BleGetConnectionState", empty_list);
        s.add_cmd(25, "BleGetPairedDevices", empty_list);
        s.add_cmd(26, "AcquireBleServiceDiscoveryEvent", acquire(3, "AcquireBleServiceDiscoveryEvent"));
        s.add_cmd(33, "AcquireBleMtuConfigEvent", acquire(4, "AcquireBleMtuConfigEvent"));
    });
}

}  // namespace

void register_bluetooth() {
    add("bt", [] {
        return make_iface("bt", [](Service& s) {
            s.add_cmd(9, "RegisterBleEvent", [](Request&, Response& rsp) -> uint32_t {
                rsp.out_copy.push_back(event_handle(0));
                return 0;
            });
        });
    });
    add("btm:u", [] {
        return make_iface("btm:u", [](Service& s) {
            s.add_cmd(0, "GetCore", [](Request&, Response& rsp) -> uint32_t {
                rsp.push_object(make_btm_user_core());
                return 0;
            });
        });
    });
}

void bluetooth_reset() {
    BtState& s = bt();
    std::lock_guard<std::mutex> lk(s.mu);
    for (auto& e : s.events) e.reset();
}

}  // namespace nxrt::services
