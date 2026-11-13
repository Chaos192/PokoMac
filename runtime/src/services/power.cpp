// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Power services (S3c): apm (IManager), apm:am (IManager, 8.0+), apm:p (IManagerPrivileged, 1.0-7.0.1), apm:sys (ISystemManager),
// psm (IPsmServer) with its IPsmSession, and the apm ISession that the game reads and writes its performance configuration through.
// Adapted from ryubing (MIT, Ryujinx fork) src/Ryujinx.HLE/HOS/Services/Apm/*.cs and src/Ryujinx.HLE/HOS/Services/Ptm/Psm/*.cs
// (MIT, Copyright (c) Ryujinx Team and Contributors): command ids, the per-mode default configuration and the InvalidParameters result.
// Command ids, enums and layouts follow https://switchbrew.org/wiki/APM_services and https://switchbrew.org/wiki/PTM_services (the psm
// part); the apm module number 148 and psm module number 136 are from https://switchbrew.org/wiki/Error_codes.
//
// What is real and what is a stand-in:
//   * the performance mode, the per-mode configuration, the CPU overclock flag and the CPU boost mode are process state: set through
//     one interface, read back through another (apm:sys RequestPerformanceMode -> apm GetPerformanceMode and so on).
//   * the throttling state is always zero (nothing throttles in the emulated console).
//   * the performance and state-change events are readable events that never signal.
//   * the battery is always at 100 percent and never charges or discharges; charging can be switched on and off and reads back.
#include <memory>

#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

// nn::apm::PerformanceMode (Switchbrew): 0 Normal, 1 Boost.
constexpr uint32_t kModeNormal = 0, kModeBoost = 1, kModeUnset = 0xFFFFFFFEu;
// nn::apm::PerformanceConfiguration defaults: Normal 0x00020003 (Switchbrew, GetCurrentPerformanceConfiguration), Boost 0x00020004
// (ryubing default; Switchbrew does not state the Boost default).
constexpr uint32_t kConfigNormal = 0x00020003u, kConfigBoost = 0x00020004u;
// nn::apm::EventTarget 0 PerformanceModeChanged, 1 SleepRequiredByLowVoltage, 2 PerformanceConfigurationChanged.
constexpr uint32_t kEventTargets = 3;
// nn::apm::ThrottlingState is 0x28 bytes (Switchbrew).
constexpr uint32_t kThrottlingStateSize = 0x28;
// nn::psm::ChargerType (Switchbrew): 0 Unconnected, 1 EnoughPower.
constexpr uint32_t kChargerUnconnected = 0, kChargerEnoughPower = 1;

// Apm InvalidParameters: module 148 (Apm) with description 1 (the description is from ryubing; Switchbrew lists no apm error rows).
uint32_t apm_invalid_parameters() { return res(148, 1); }

struct PowerState {
    uint32_t performance_mode = kModeUnset;   // set by apm:sys RequestPerformanceMode; until then it follows the operation mode
    uint32_t config[2] = {kConfigNormal, kConfigBoost};   // indexed by PerformanceMode (Normal, Boost)
    bool cpu_overclock = false;
    uint32_t cpu_boost_mode = 0;
    ServiceEvent perf_events[kEventTargets];
    bool battery_charging = true;
};

PowerState& st() {
    static PowerState s;
    return s;
}

// The mode in effect: the console is in Boost while docked and Normal in handheld mode (the applet manager tells apm so on a real
// console through RequestPerformanceMode; nxrt follows amctl::docked() until something requests a mode explicitly).
uint32_t mode_now() {
    const uint32_t m = st().performance_mode;
    return m != kModeUnset ? m : (amctl::docked() ? kModeBoost : kModeNormal);
}

uint32_t current_config() {
    const uint32_t mode = mode_now();
    if (mode == kModeNormal) return st().config[0];
    if (mode == kModeBoost) return st().config[1];
    return kConfigNormal;   // ryubing: any other mode reads as the Normal default
}

void push_zeros(ipc::Response& rsp, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) rsp.push<uint8_t>(0);
}

// ---- apm ISession (nn::apm::ISession) ------------------------------------------------------------------------------------------------

std::shared_ptr<ipc::Service> make_apm_session() {
    return make_iface("apm:ISession", [](ipc::Service& s) {
        // 0 SetPerformanceConfiguration(u32 mode, u32 config)
        s.add_cmd(0, "SetPerformanceConfiguration", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint32_t mode = req.arg<uint32_t>(0);
            const uint32_t cfg = req.arg<uint32_t>(4);
            if (mode > kModeBoost) return apm_invalid_parameters();
            NXLOG_D("apm", "SetPerformanceConfiguration(mode %u, config 0x%08x)", mode, cfg);
            st().config[mode] = cfg;
            return 0;
        });
        // 1 GetPerformanceConfiguration(u32 mode) -> u32 config
        s.add_cmd(1, "GetPerformanceConfiguration", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t mode = req.arg<uint32_t>(0);
            if (mode > kModeBoost) {
                rsp.push<uint32_t>(0);
                return apm_invalid_parameters();
            }
            rsp.push<uint32_t>(st().config[mode]);
            return 0;
        });
        // 2 [8.0.0+] SetCpuOverclockEnabled(bool)
        s.add_cmd(2, "SetCpuOverclockEnabled", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            st().cpu_overclock = req.arg<uint8_t>(0) != 0;
            NXLOG_D("apm", "SetCpuOverclockEnabled(%d)", st().cpu_overclock ? 1 : 0);
            return 0;
        });
    });
}

// ---- apm IManager (nn::apm::IManager, also apm:am) and IManagerPrivileged (apm:p) -------------------------------------------------

std::shared_ptr<ipc::Service> make_apm_manager(const char* name, bool full) {
    return make_iface(name, [full](ipc::Service& s) {
        // 0 OpenSession() -> ISession
        s.add_cmd(0, "OpenSession", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_apm_session());
            return 0;
        });
        if (!full) return;   // apm:p has OpenSession only
        // 1 GetPerformanceMode() -> u32 PerformanceMode
        s.add_cmd(1, "GetPerformanceMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(mode_now());
            return 0;
        });
        // 6 [7.0.0+] IsCpuOverclockEnabled() -> bool
        s.add_cmd(6, "IsCpuOverclockEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(st().cpu_overclock ? 1 : 0);
            return 0;
        });
    });
}

// ---- apm:sys ISystemManager ----------------------------------------------------------------------------------------------------------

std::shared_ptr<ipc::Service> make_apm_system_manager() {
    return make_iface("apm:sys", [](ipc::Service& s) {
        // 0 RequestPerformanceMode(u32 PerformanceMode)
        s.add_cmd(0, "RequestPerformanceMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            st().performance_mode = req.arg<uint32_t>(0);
            NXLOG_D("apm", "RequestPerformanceMode(%u)", st().performance_mode);
            return 0;
        });
        // 1 GetPerformanceEvent(u32 EventTarget) -> copy handle of an event with EventClearMode 0
        s.add_cmd(1, "GetPerformanceEvent", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t target = req.arg<uint32_t>(0);
            if (target >= kEventTargets) return apm_invalid_parameters();
            const uint32_t h = st().perf_events[target].new_handle();
            if (!h) return kernel::kOutOfHandles;
            rsp.out_copy.push_back(h);
            return 0;
        });
        // 2 GetThrottlingState() -> ThrottlingState (nothing throttles)
        s.add_cmd(2, "GetThrottlingState", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            push_zeros(rsp, kThrottlingStateSize);
            return 0;
        });
        // 3 GetLastThrottlingState() -> ThrottlingState
        s.add_cmd(3, "GetLastThrottlingState", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            push_zeros(rsp, kThrottlingStateSize);
            return 0;
        });
        // 4 ClearLastThrottlingState()
        s.add_cmd(4, "ClearLastThrottlingState", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        // 5 [5.0.0+] LoadAndApplySettings(): accepted, the emulated console has no settings file to apply
        s.add_cmd(5, "LoadAndApplySettings", [](ipc::Request&, ipc::Response&) -> uint32_t {
            NXLOG_D("apm", "LoadAndApplySettings: accepted and ignored");
            return 0;
        });
        // 6 [7.0.0+] SetCpuBoostMode(u32 CpuBoostMode)
        s.add_cmd(6, "SetCpuBoostMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            st().cpu_boost_mode = req.arg<uint32_t>(0);
            return 0;
        });
        // 7 [7.0.0+] GetCurrentPerformanceConfiguration() -> u32 PerformanceConfiguration of the current mode
        s.add_cmd(7, "GetCurrentPerformanceConfiguration", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(current_config());
            return 0;
        });
    });
}

// ---- psm ----------------------------------------------------------------------------------------------------------------------------

std::shared_ptr<ipc::Service> make_psm_session() {
    // One state-change event per session. It is handed out as a valid, non-signaled handle and never fires: the battery and the
    // charger never change in the emulated console.
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("psm:IPsmSession", [ev](ipc::Service& s) {
        // 0 BindStateChangeEvent() -> copy handle of the event (EventClearMode 0)
        s.add_cmd(0, "BindStateChangeEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const uint32_t h = ev->new_handle();
            if (!h) return kernel::kOutOfHandles;
            rsp.out_copy.push_back(h);
            return 0;
        });
        // 1 UnbindStateChangeEvent(): the handle the guest already holds stays valid and stays non-signaled
        s.add_cmd(1, "UnbindStateChangeEvent", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        // 2 SetChargerTypeChangeEventEnabled(bool), 3 SetPowerSupplyChangeEventEnabled(bool),
        // 4 SetBatteryVoltageStateChangeEventEnabled(bool): accepted and ignored (the events never fire)
        s.add_cmd(2, "SetChargerTypeChangeEventEnabled", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(3, "SetPowerSupplyChangeEventEnabled", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(4, "SetBatteryVoltageStateChangeEventEnabled", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
    });
}

std::shared_ptr<ipc::Service> make_psm() {
    return make_iface("psm", [](ipc::Service& s) {
        // 0 GetBatteryChargePercentage() -> u32
        s.add_cmd(0, "GetBatteryChargePercentage", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(100);
            return 0;
        });
        // 1 GetChargerType() -> ChargerType: an official charger while docked, nothing connected while in handheld mode
        s.add_cmd(1, "GetChargerType", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(amctl::docked() ? kChargerEnoughPower : kChargerUnconnected);
            return 0;
        });
        // 2 EnableBatteryCharging(), 3 DisableBatteryCharging()
        s.add_cmd(2, "EnableBatteryCharging", [](ipc::Request&, ipc::Response&) -> uint32_t {
            st().battery_charging = true;
            return 0;
        });
        s.add_cmd(3, "DisableBatteryCharging", [](ipc::Request&, ipc::Response&) -> uint32_t {
            st().battery_charging = false;
            return 0;
        });
        // 4 IsBatteryChargingEnabled() -> bool
        s.add_cmd(4, "IsBatteryChargingEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(st().battery_charging ? 1 : 0);
            return 0;
        });
        // 7 OpenSession() -> IPsmSession
        s.add_cmd(7, "OpenSession", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_psm_session());
            return 0;
        });
    });
}

}  // namespace

void register_power() {
    add("apm", [] { return make_apm_manager("apm", true); });
    add("apm:am", [] { return make_apm_manager("apm:am", true); });
    add("apm:p", [] { return make_apm_manager("apm:p", false); });
    add("apm:sys", [] { return make_apm_system_manager(); });
    add("psm", [] { return make_psm(); });
}

void power_reset() {
    PowerState& s = st();
    s.performance_mode = kModeUnset;
    s.config[0] = kConfigNormal;
    s.config[1] = kConfigBoost;
    s.cpu_overclock = false;
    s.cpu_boost_mode = 0;
    for (ServiceEvent& e : s.perf_events) e = ServiceEvent();
    s.battery_charging = true;
}

}  // namespace nxrt::services
