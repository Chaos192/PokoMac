// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE for a game process (nxrt/sw2/service.h).
#include "nxrt/sw2/service.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::sw2 {
namespace {

constexpr const char* kTag = "sw2";

struct Service {
    std::mutex mu;
    std::unique_ptr<Manager> mgr;
    std::unique_ptr<Transport> tr;
    std::thread timer;
    std::atomic<bool> quit{false};
    std::atomic<Manager*> live{nullptr};
};
Service& svc() {
    static Service* s = new Service;   // never destroyed: the timer thread may outlive static destruction at exit
    return *s;
}

}  // namespace

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool configured() {
    const std::string v = config_get_str("input", "switch2_ble", "off");
    return v == "on" || v == "1" || v == "true" || v == "yes";
}

bool start() {
    Service& s = svc();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.mgr) return true;
    if (!configured()) return false;
    ManagerOptions opt;
    opt.motion = config_get_bool("input", "switch2_ble_motion", true);
    auto mgr = std::make_unique<Manager>(opt);
    const std::string host = config_get_str("input", "switch2_ble_host", "");
    if (!host.empty()) {
        uint8_t le[6];
        if (parse_address(host.c_str(), le)) mgr->set_host_address(le);
        else NXLOG_W(kTag, "[input] switch2_ble_host = %s is not an address like aa:bb:cc:dd:ee:ff: ignored", host.c_str());
    }
    std::unique_ptr<Transport> tr = make_platform_transport(*mgr);
    if (!tr) {
        NXLOG_W(kTag, "[input] switch2_ble is on, but this platform has no Bluetooth LE transport for Switch 2 controllers");
        return false;
    }
    mgr->set_transport(tr.get());
    if (!tr->start()) {
        mgr->set_transport(nullptr);
        return false;
    }
    s.quit = false;
    Manager* m = mgr.get();
    Transport* t = tr.get();
    s.timer = std::thread([m, t, &s] {
        log_set_thread_name("sw2");
        t->on_thread_start();
        auto next = std::chrono::steady_clock::now();
        while (!s.quit.load()) {
            m->tick(now_ns());
            next += std::chrono::milliseconds(5);
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;
            std::this_thread::sleep_until(next);
        }
    });
    s.mgr = std::move(mgr);
    s.tr = std::move(tr);
    s.live = s.mgr.get();
    NXLOG_I(kTag, "Switch 2 controllers over Bluetooth: on. To pair one, hold its Sync button until its lights sweep; a paired one connects at any button");
    return true;
}

void stop() {
    Service& s = svc();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.mgr) return;
    s.live = nullptr;
    s.quit = true;
    if (s.timer.joinable()) s.timer.join();
    s.tr->stop();
    s.mgr->set_transport(nullptr);
    s.tr.reset();
    s.mgr.reset();
}

bool running() { return svc().live.load() != nullptr; }

void set_manager_for_tests(Manager* m) {
    Service& s = svc();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.mgr) s.live = m;
}

Manager* manager() { return svc().live.load(); }

void game_vibration(int player, int hand, float amp_lo, float freq_lo, float amp_hi, float freq_hi) {
    if (Manager* m = svc().live.load()) m->hd_vibration(player, hand, amp_lo, freq_lo, amp_hi, freq_hi, now_ns());
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::unique_ptr<Transport> make_platform_transport(Manager&) { return nullptr; }
#endif

}  // namespace nxrt::sw2
