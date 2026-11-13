// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// GetJcmsStates from a real Joy-Con 2's mouse sensor: a Switch 2 Joy-Con over Bluetooth (nxrt/sw2, driven through a fake transport) whose
// sensor slides on a surface reaches the game's Jcms transfer memory (hid InitializeJcms 3110) as the movement of each sample, connected
// and on a surface, in the ring of its player and side (index npad * 2 + type); lifted it reports no movement; the rings of a player without
// one keep their old behaviour (player 1 counts empty states, the others stay empty).
#include <cstring>
#include <memory>
#include <vector>

#include "hid_shm.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/sw2/manager.h"
#include "nxrt/sw2/service.h"
#include "svc_test.h"

using namespace svctest;
using namespace nxrt::sw2;

namespace {

constexpr uint64_t kRingSize = 0x2C8;   // per JcmsHandle index: 0x20 header + 17 x (u64 seq, 0x20 state)
struct Jcms {                            // hid.cpp JcmsState
    uint64_t unknown0, sampling;
    int32_t dx, dy;
    uint32_t attributes, unknown1c;
};
static_assert(sizeof(Jcms) == 0x20, "JcmsState");

// The newest state of ring `index`, and how many the ring holds.
Jcms newest(uint64_t base, uint32_t index, uint64_t* count) {
    const uint8_t* r = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(base + index * kRingSize));
    uint64_t at = 0;
    std::memcpy(&at, r + 0x10, 8);
    std::memcpy(count, r + 0x18, 8);
    Jcms j{};
    std::memcpy(&j, r + 0x20 + at * 0x28 + 8, sizeof j);
    return j;
}

struct NullTransport : Transport {
    std::vector<std::vector<uint8_t>> cmds;
    bool start() override { return true; }
    void stop() override {}
    void connect(uint64_t) override {}
    void disconnect(uint64_t) override {}
    void enable_input(uint64_t) override {}
    bool write(uint64_t, Channel ch, const uint8_t* d, size_t n) override {
        if (ch == Channel::Command) cmds.emplace_back(d, d + n);
        return true;
    }
};

// Answers every command the manager wrote, as a Joy-Con 2 (R) does (memory reads: the read echoed and zero-filled data, the identity block).
void answer(Manager& m, NullTransport& t, uint64_t dev, size_t* done, uint64_t now) {
    while (*done < t.cmds.size()) {
        const std::vector<uint8_t> f = t.cmds[(*done)++];
        if (f.size() < 8 || f[0] == kCmdLeds) continue;
        std::vector<uint8_t> r = {f[0], 0x01, 0x01, f[3], 0, 0, 0, 0};
        if (f[0] == kCmdMemory && f.size() >= 16) {
            r.insert(r.end(), f.begin() + 8, f.begin() + 16);
            std::vector<uint8_t> data(f[8], 0xFF);
            const uint32_t addr = f[12] | (f[13] << 8) | (f[14] << 16) | (static_cast<uint32_t>(f[15]) << 24);
            if (addr == kAddrInfo) {
                data.assign(f[8], 0);
                data[18] = 0x7E;
                data[19] = 0x05;
                data[20] = 0x66;
                data[21] = 0x20;
            }
            r.insert(r.end(), data.begin(), data.end());
        }
        m.on_reply(dev, r.data(), r.size(), now);
    }
}

void mouse_report(Manager& m, uint64_t dev, int16_t x, int16_t y, uint16_t dist, uint64_t now) {
    uint8_t rep[63] = {};
    rep[0x10] = static_cast<uint8_t>(x);
    rep[0x11] = static_cast<uint8_t>(static_cast<uint16_t>(x) >> 8);
    rep[0x12] = static_cast<uint8_t>(y);
    rep[0x13] = static_cast<uint8_t>(static_cast<uint16_t>(y) >> 8);
    rep[0x14] = 0x40;
    rep[0x16] = static_cast<uint8_t>(dist);
    rep[0x17] = static_cast<uint8_t>(dist >> 8);
    m.on_input(dev, rep, sizeof rep, now);
}

}  // namespace

SVC_TEST(hid_jcms, joycon2_sensor_states) {
    nxrt::services::hid_set_background_sampling(false);
    // a right Joy-Con 2, bonded before, comes up and is player 2 (npad 1)
    NullTransport tr;
    Manager m;
    m.set_transport(&tr);
    const uint8_t host[6] = {1, 2, 3, 4, 5, 6};
    m.set_host_address(host);
    uint64_t now = 1000000000ull;
    size_t done = 0;
    std::vector<uint8_t> adv = {0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x66, 0x20, 0x00, 0x01, 0x81, 1, 2, 3, 4, 5, 6, 0x0f, 0, 0, 0, 0, 0, 0, 0};
    m.on_advert(5, adv.data(), adv.size(), -40, now);
    m.on_link_up(5, true, now);
    answer(m, tr, 5, &done, now);
    CHECK_EQ(m.live_sessions(), 1u);
    PadView v[1];
    CHECK_EQ(m.list(v, 1), 1u);
    m.set_player(v[0].id, 1);
    nxrt::sw2::set_manager_for_tests(&m);

    // the game lends its Jcms buffer
    const uint64_t buf = nxrt::mem::alloc_kernel(0x4000, 0x1000);
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(buf)), 0, 0x4000);
    auto tm = std::make_shared<nxrt::kernel::KTransferMemory>();
    tm->addr = buf;
    tm->size = 0x4000;
    Session hid = open("hid");
    CHECK(hid.valid());
    Req rq;
    rq.put<uint64_t>(0).put<uint64_t>(0x4000);
    rq.copy.push_back(nxrt::kernel::handle_add(tm));
    CHECK_SHAPE(hid.call(3110, rq), 0);   // InitializeJcms

    // the first sample turns the Joy-Cons' sensors on (the game reads them): the manager sends the mouse feature mask
    nxrt::services::hid_sample_once();
    m.tick(now += 5000000ull);
    answer(m, tr, 5, &done, now);
    bool mask = false;
    for (const auto& c : tr.cmds) mask = mask || (c.size() >= 9 && c[0] == kCmdFeature && c[8] == 0x37);
    CHECK(mask);

    // on a surface: (100, 50) then (130, 42): one sample reports the 30, -8 between them, connected and on the surface
    mouse_report(m, 5, 100, 50, 150, now);
    mouse_report(m, 5, 130, 42, 150, now);
    nxrt::services::hid_sample_once();
    uint64_t count = 0;
    Jcms j = newest(buf, 1 * 2 + 1, &count);
    CHECK(count >= 1);
    CHECK_EQ(j.dx, 30);
    CHECK_EQ(j.dy, -8);
    CHECK_EQ(j.attributes, 3u);
    CHECK(j.sampling > 0);
    // no new movement: the next sample is still, still connected
    nxrt::services::hid_sample_once();
    j = newest(buf, 1 * 2 + 1, &count);
    CHECK(j.dx == 0 && j.dy == 0 && j.attributes == 3u);
    // lifted: no movement, connected, not on a surface
    mouse_report(m, 5, 900, 900, 3200, now);
    nxrt::services::hid_sample_once();
    j = newest(buf, 1 * 2 + 1, &count);
    CHECK(j.dx == 0 && j.dy == 0 && j.attributes == 1u);
    // player 2's left ring (no left Joy-Con) stays empty; player 1's rings count empty states as before
    newest(buf, 1 * 2 + 0, &count);
    CHECK_EQ(count, 0u);
    j = newest(buf, 0 * 2 + 1, &count);
    CHECK(count >= 1 && j.attributes == 0u && j.dx == 0);

    nxrt::sw2::set_manager_for_tests(nullptr);
    hid.call(3111, Req().put<uint64_t>(0));   // FinalizeJcms
}
