// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE: the connect / pair / reconnect state machine and the pads (nxrt/sw2/manager.h), driven through a
// fake transport and fake controllers that answer the commands as the controllers do (memory reads echo the read and return the data,
// every other command answers with its id and 0x01).
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "nxrt/sw2/manager.h"
#include "svc_test.h"

using namespace nxrt::sw2;

namespace {

constexpr uint64_t kMs = 1000000ull;
const uint8_t kHost[6] = {0xE5, 0xD4, 0xC3, 0xB2, 0xA1, 0x02};   // made up (locally administered), little-endian

struct Write {
    uint64_t dev;
    Channel ch;
    std::vector<uint8_t> d;
};

// A controller as seen over its command channel.
struct FakePad {
    uint16_t pid = kPidPro;
    bool answer = true;        // false: never answers (a link that went quiet)
    bool refuse_pair = false;  // never answers the 0x15 commands
    std::map<uint32_t, std::vector<uint8_t>> mem;
    std::vector<std::vector<uint8_t>> commands;   // every command it got
    explicit FakePad(uint16_t p) : pid(p) {
        std::vector<uint8_t> info(0x40, 0);
        const char serial[] = "XKW00000000000";
        std::memcpy(info.data() + 2, serial, 14);
        info[18] = 0x7E;
        info[19] = 0x05;
        info[20] = static_cast<uint8_t>(pid);
        info[21] = static_cast<uint8_t>(pid >> 8);
        mem[kAddrInfo] = info;
    }
    // the reply to a command frame, empty for none
    std::vector<uint8_t> reply(const std::vector<uint8_t>& f) {
        commands.push_back(f);
        if (!answer || f.size() < 8) return {};
        if (f[0] == kCmdLeds) return {};   // its answers are not waited for: leave them out, so they cannot be taken for another
        if (f[0] == kCmdPair && refuse_pair) return {};
        std::vector<uint8_t> r = {f[0], 0x01, 0x01, f[3], 0x00, 0x00, 0x00, 0x00};
        if (f[0] == kCmdMemory && f.size() >= 16) {
            const uint8_t len = f[8];
            const uint32_t addr = f[12] | (f[13] << 8) | (f[14] << 16) | (static_cast<uint32_t>(f[15]) << 24);
            r.insert(r.end(), f.begin() + 8, f.begin() + 16);
            std::vector<uint8_t> data(len, 0xFF);
            auto it = mem.find(addr);
            if (it != mem.end()) std::memcpy(data.data(), it->second.data(), std::min<size_t>(len, it->second.size()));
            r.insert(r.end(), data.begin(), data.end());
        }
        return r;
    }
};

struct FakeTransport : Transport {
    Manager* m = nullptr;
    std::vector<uint64_t> connects, disconnects, inputs, power_reads;
    std::vector<Write> writes;
    bool start() override { return true; }
    void stop() override {}
    void connect(uint64_t dev) override { connects.push_back(dev); }
    void disconnect(uint64_t dev) override { disconnects.push_back(dev); }
    void enable_input(uint64_t dev) override { inputs.push_back(dev); }
    void read_power(uint64_t dev) override { power_reads.push_back(dev); }
    bool write(uint64_t dev, Channel ch, const uint8_t* d, size_t n) override {
        writes.push_back({dev, ch, std::vector<uint8_t>(d, d + n)});
        return true;
    }
    size_t count(uint64_t dev, Channel ch, uint8_t first = 0xFF) const {
        size_t n = 0;
        for (const Write& w : writes) n += w.dev == dev && w.ch == ch && (first == 0xFF || (!w.d.empty() && w.d[0] == first));
        return n;
    }
};

struct Rig {
    FakeTransport tr;
    Manager m;
    std::map<uint64_t, FakePad*> pads;
    uint64_t now = 1000 * kMs;
    size_t delivered = 0;
    explicit Rig(bool host_known = true) {
        m.set_transport(&tr);
        tr.m = &m;
        if (host_known) m.set_host_address(kHost);
    }
    // answers the commands written so far (and those the answers cause), as the controllers would
    void pump() {
        while (delivered < tr.writes.size()) {
            const Write w = tr.writes[delivered++];
            if (w.ch != Channel::Command) continue;
            auto it = pads.find(w.dev);
            if (it == pads.end()) continue;
            const std::vector<uint8_t> r = it->second->reply(w.d);
            if (!r.empty()) m.on_reply(w.dev, r.data(), r.size(), now);
        }
    }
    void advance(uint64_t ms) {
        for (uint64_t t = 0; t < ms; t += 5) {
            now += 5 * kMs;
            m.tick(now);
            pump();
        }
    }
    static std::vector<uint8_t> advert(uint16_t pid, const uint8_t* host, bool wake = false) {
        std::vector<uint8_t> a = {0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, static_cast<uint8_t>(pid), static_cast<uint8_t>(pid >> 8), 0x00, 0x01,
                                  static_cast<uint8_t>(wake ? 0x81 : 0x00), 0, 0, 0, 0, 0, 0, 0x0f, 0, 0, 0, 0, 0, 0, 0};
        if (host) std::memcpy(a.data() + 0x0C, host, 6);
        return a;
    }
    // a controller comes up: advertisement, link, handshake
    void bring_up(uint64_t dev, FakePad* p, const uint8_t* host) {
        pads[dev] = p;
        const std::vector<uint8_t> a = advert(p->pid, host);
        m.on_advert(dev, a.data(), a.size(), -50, now);
        m.on_link_up(dev, true, now);
        pump();
    }
    void report(uint64_t dev, uint32_t buttons, uint16_t lx = 2048, uint16_t ly = 2048, uint16_t rx = 2048, uint16_t ry = 2048) {
        uint8_t r[63] = {};
        for (int i = 0; i < 4; ++i) r[4 + i] = static_cast<uint8_t>(buttons >> (8 * i));
        const uint32_t a = lx | (static_cast<uint32_t>(ly) << 12), b = rx | (static_cast<uint32_t>(ry) << 12);
        for (int i = 0; i < 3; ++i) {
            r[0x0A + i] = static_cast<uint8_t>(a >> (8 * i));
            r[0x0D + i] = static_cast<uint8_t>(b >> (8 * i));
        }
        r[0x34] = 0x00;
        r[0x35] = 0x10;   // accel z = 4096: lying face up
        m.on_input(dev, r, sizeof r, now);
    }
};

bool has_command(const FakePad& p, uint8_t cmd, uint8_t sub) {
    for (const auto& c : p.commands)
        if (c.size() >= 4 && c[0] == cmd && c[3] == sub) return true;
    return false;
}

}  // namespace

SVC_TEST(sw2, pair_and_bond) {
    Rig rig;
    FakePad pad(kPidJoyConR);
    rig.bring_up(7, &pad, nullptr);   // Sync held: no host in the advertisement
    CHECK_EQ(rig.tr.connects.size(), 1u);
    CHECK(rig.m.bonded(7));
    CHECK_EQ(rig.m.live_sessions(), 1u);
    CHECK(has_command(pad, kCmdPair, kSubPairHost) && has_command(pad, kCmdPair, kSubPairKey1) && has_command(pad, kCmdPair, kSubPairKey2) &&
          has_command(pad, kCmdPair, kSubPairDone));
    for (const auto& c : pad.commands) {
        if (c.size() >= 22 && c[0] == kCmdPair && c[3] == kSubPairHost) {
            CHECK(c[8] == 0x00 && c[9] == 0x02 && std::memcmp(c.data() + 10, kHost, 6) == 0 && std::memcmp(c.data() + 16, kHost, 6) == 0);
        }
        if (c.size() >= 9 && c[0] == kCmdFeature) CHECK_EQ(c[8], 0x27);   // the console's Joy-Con mask, without the mouse
    }
    CHECK(has_command(pad, kCmdFeature, kSubFeatureSet) && has_command(pad, kCmdFeature, kSubFeatureEnable));
    CHECK_EQ(rig.tr.inputs.size(), 1u);
    CHECK(rig.tr.count(7, Channel::Command, kCmdLeds) >= 1);
    PadView v[4];
    CHECK_EQ(rig.m.list(v, 4), 1u);
    CHECK(v[0].type == PadType::JoyConRight);
    CHECK_EQ(v[0].product, kPidJoyConR);
    CHECK_EQ(v[0].vendor, 0x057E);
    // a second advertisement of a live controller changes nothing
    const std::vector<uint8_t> again = Rig::advert(kPidJoyConR, kHost, true);
    rig.m.on_advert(7, again.data(), again.size(), -50, rig.now);
    CHECK_EQ(rig.tr.connects.size(), 1u);
}

SVC_TEST(sw2, wake_reconnect_ignore) {
    Rig rig;
    FakePad pad(kPidPro);
    rig.bring_up(3, &pad, kHost);   // bonded before: it wakes toward this host
    CHECK_EQ(rig.m.live_sessions(), 1u);
    CHECK(!has_command(pad, kCmdPair, kSubPairHost));   // no bonding again
    CHECK(!rig.m.bonded(3));
    // a Pro Controller 2 reads both sticks' calibration
    size_t reads = 0;
    for (const auto& c : pad.commands) reads += c[0] == kCmdMemory;
    CHECK(reads >= 5);   // info, gyro, stick 1 user + factory, stick 2 user + factory
    rig.m.on_link_down(3, "test", rig.now);
    CHECK_EQ(rig.m.sessions(), 0u);
    PadView v[2];
    CHECK_EQ(rig.m.list(v, 2), 0u);
    // it wakes again at once: not tried again within 3 s of the last attempt, then connected
    const std::vector<uint8_t> wake = Rig::advert(kPidPro, kHost, true);
    rig.now += 1000 * kMs;
    rig.m.on_advert(3, wake.data(), wake.size(), -60, rig.now);
    CHECK_EQ(rig.tr.connects.size(), 1u);
    rig.now += 2100 * kMs;
    rig.m.on_advert(3, wake.data(), wake.size(), -60, rig.now);
    CHECK_EQ(rig.tr.connects.size(), 2u);
    rig.m.on_link_up(3, true, rig.now);
    rig.pump();
    CHECK_EQ(rig.m.live_sessions(), 1u);
    // a controller of another host is never connected to, nor one without a host while this host's address is unknown and bonding is wanted
    const uint8_t other[6] = {1, 2, 3, 4, 5, 6};
    const std::vector<uint8_t> foreign = Rig::advert(kPidJoyConL, other, true);
    rig.m.on_advert(9, foreign.data(), foreign.size(), -40, rig.now);
    CHECK_EQ(rig.tr.connects.size(), 2u);
    // a short or foreign advertisement is nothing
    const uint8_t junk[5] = {0x53, 0x05, 1, 2, 3};
    rig.m.on_advert(10, junk, sizeof junk, -40, rig.now);
    CHECK_EQ(rig.tr.connects.size(), 2u);
}

SVC_TEST(sw2, take_without_host) {
    Rig rig(false);   // this host's address is not known
    FakePad pad(kPidJoyConL);
    rig.bring_up(4, &pad, nullptr);
    CHECK_EQ(rig.m.live_sessions(), 1u);
    CHECK(!has_command(pad, kCmdPair, kSubPairHost));
    CHECK(!rig.m.bonded(4));
}

SVC_TEST(sw2, bonding_refused_still_plays) {
    Rig rig;
    FakePad pad(kPidJoyConL);
    pad.refuse_pair = true;
    rig.bring_up(5, &pad, nullptr);
    CHECK_EQ(rig.m.live_sessions(), 0u);
    rig.advance(2100);   // the host-address step times out: it plays without the bond
    CHECK_EQ(rig.m.live_sessions(), 1u);
    CHECK(!rig.m.bonded(5));
    CHECK(!has_command(pad, kCmdPair, kSubPairKey1));
}

SVC_TEST(sw2, timeouts) {
    Rig rig;
    FakePad quiet(kPidPro);
    quiet.answer = false;
    rig.bring_up(6, &quiet, kHost);
    CHECK_EQ(rig.m.sessions(), 1u);
    rig.advance(2100);   // the identity read gets no answer: dropped
    CHECK_EQ(rig.m.sessions(), 0u);
    CHECK_EQ(rig.tr.disconnects.size(), 1u);
    // a link that never comes up is given 10 s
    const std::vector<uint8_t> a = Rig::advert(kPidPro, kHost, true);
    rig.now += 4000 * kMs;
    rig.m.on_advert(8, a.data(), a.size(), -50, rig.now);
    CHECK_EQ(rig.m.sessions(), 1u);
    rig.advance(9900);
    CHECK_EQ(rig.m.sessions(), 1u);
    rig.advance(200);
    CHECK_EQ(rig.m.sessions(), 0u);
    CHECK_EQ(rig.tr.disconnects.size(), 2u);
    // a link up for a device the manager does not know, and replies to nothing, are ignored
    rig.m.on_link_up(99, true, rig.now);
    const uint8_t r[8] = {0x02, 0x01, 0, 0, 0, 0, 0, 0};
    rig.m.on_reply(99, r, sizeof r, rig.now);
    CHECK_EQ(rig.m.sessions(), 0u);
}

SVC_TEST(sw2, joycon_pair_and_gestures) {
    Rig rig;
    FakePad l(kPidJoyConL), r(kPidJoyConR);
    rig.bring_up(1, &l, kHost);
    PadView v[4];
    CHECK_EQ(rig.m.list(v, 4), 1u);
    CHECK(v[0].type == PadType::JoyConLeft);
    const uint32_t gen = v[0].generation, id = v[0].id;
    rig.bring_up(2, &r, kHost);   // the right one joins the left one's pad
    CHECK_EQ(rig.m.list(v, 4), 1u);
    CHECK(v[0].type == PadType::JoyConPair);
    CHECK_EQ(v[0].product, kPidJoyConPair);
    CHECK_EQ(v[0].id, id);
    CHECK(v[0].generation != gen);
    // buttons of both sides; Nintendo's face layout (A east, B south); the left stick from the left one, the right from the right one's second slot
    rig.report(1, kRawUp | kRawL, 2048 + 1200, 2048);
    rig.report(2, kRawA | kRawZR, 2048, 2048, 2048, 2048 + 1200);
    CHECK(rig.m.view(id, &v[0]));
    CHECK(v[0].buttons & (1u << kPbEast));
    CHECK(v[0].buttons & (1u << kPbDpadUp));
    CHECK_EQ(v[0].raw_buttons[0], kRawUp | kRawL);
    CHECK_EQ(v[0].raw_buttons[1], kRawA | kRawZR);
    CHECK(v[0].buttons & (1u << kPbLeftShoulder));
    CHECK(!(v[0].buttons & (1u << kPbSouth)));
    CHECK_EQ(v[0].axes[kPaRightTrigger], 32767);
    CHECK_EQ(v[0].axes[kPaLeftTrigger], -32768);
    CHECK_EQ(v[0].axes[kPaLeftX], 32767);
    CHECK_EQ(v[0].axes[kPaRightY], -32767);   // stick up is SDL's negative y
    CHECK(v[0].motion);
    CHECK(v[0].accel[1] > 9.f);
    // SL + SR held 3 s on the right one: it plays on its own
    rig.report(2, kRawSLR | kRawSRR);
    rig.advance(2900);
    CHECK_EQ(rig.m.list(v, 4), 1u);
    rig.advance(200);
    CHECK_EQ(rig.m.list(v, 4), 2u);
    CHECK(v[0].type == PadType::JoyConLeft && v[1].type == PadType::JoyConRight);
    // alone and sideways: the right one's A is the bottom button, its stick turned a quarter
    rig.report(2, kRawA, 2048, 2048, 2048, 2048 + 1200);
    CHECK(rig.m.view(v[1].id, &v[1]));
    CHECK(v[1].buttons & (1u << kPbSouth));
    CHECK_EQ(v[1].axes[kPaLeftX], 32767);
    rig.report(1, kRawLeft, 2048, 2048 + 1200);
    CHECK(rig.m.view(v[0].id, &v[0]));
    CHECK(v[0].buttons & (1u << kPbSouth));
    CHECK_EQ(v[0].axes[kPaLeftX], -32767);
    // L on the left one and R on the right one, held together 3 s: one pair again, in the older pad
    rig.report(1, kRawL);
    rig.report(2, kRawR);
    rig.advance(3100);
    CHECK_EQ(rig.m.list(v, 4), 1u);
    CHECK(v[0].type == PadType::JoyConPair);
    CHECK_EQ(v[0].id, id);
    // one side goes: the pad stays, as that side alone
    rig.m.on_link_down(1, "test", rig.now);
    CHECK_EQ(rig.m.list(v, 4), 1u);
    CHECK(v[0].type == PadType::JoyConRight);
}

SVC_TEST(sw2, battery_level) {
    Rig rig;
    FakePad l(kPidJoyConL), r(kPidJoyConR);
    rig.bring_up(31, &l, kHost);
    rig.bring_up(32, &r, kHost);
    PadView v[1];
    CHECK_EQ(rig.m.list(v, 1), 1u);
    CHECK_EQ(v[0].battery_level, -1);   // not read yet
    rig.advance(400);
    CHECK(rig.tr.power_reads.empty());
    rig.advance(200);                    // half a second after it came up: each controller's own report is read
    CHECK_EQ(rig.tr.power_reads.size(), 2u);
    const uint8_t left[2] = {0, static_cast<uint8_t>(6 << 2)}, right[2] = {0, static_cast<uint8_t>(8 << 2 | 0x02)};
    rig.m.on_power(31, left, sizeof left, rig.now);
    rig.m.on_power(32, right, sizeof right, rig.now);
    CHECK(rig.m.view(v[0].id, &v[0]));
    CHECK_EQ(v[0].battery_level, 6);     // the lower of the pair
    CHECK(v[0].charging);                // one of them charges
    rig.advance(29000);
    CHECK_EQ(rig.tr.power_reads.size(), 2u);
    rig.advance(1100);                   // every 30 s
    CHECK_EQ(rig.tr.power_reads.size(), 4u);
    const uint8_t bad[2] = {0, 0xFC};    // level 15: not a level, the old one stays
    rig.m.on_power(31, bad, sizeof bad, rig.now);
    CHECK(rig.m.view(v[0].id, &v[0]));
    CHECK_EQ(v[0].battery_level, 6);
}

SVC_TEST(sw2, leds_follow_player) {
    Rig rig;
    FakePad pad(kPidPro);
    rig.bring_up(11, &pad, kHost);
    PadView v[1];
    CHECK_EQ(rig.m.list(v, 1), 1u);
    auto last_led = [&]() -> int {
        for (size_t i = rig.tr.writes.size(); i-- > 0;) {
            const Write& w = rig.tr.writes[i];
            if (w.dev == 11 && w.ch == Channel::Command && w.d.size() >= 9 && w.d[0] == kCmdLeds) return w.d[8];
        }
        return -1;
    };
    CHECK_EQ(last_led(), 0x01);
    rig.m.set_player(v[0].id, 2);
    rig.advance(5);
    CHECK_EQ(last_led(), 0x07);
    // the LEDs are written again every second (the link drops after about ten seconds without a host write)
    const size_t before = rig.tr.count(11, Channel::Command, kCmdLeds);
    rig.advance(3000);
    const size_t after = rig.tr.count(11, Channel::Command, kCmdLeds);
    CHECK(after - before >= 2 && after - before <= 4);
    CHECK(rig.m.view(v[0].id, &v[0]));
    CHECK_EQ(v[0].player, 2);
}

SVC_TEST(sw2, hd_rumble_slots) {
    Rig rig;
    FakePad pad(kPidPro);
    rig.bring_up(12, &pad, kHost);
    PadView v[1];
    rig.m.list(v, 1);
    rig.m.set_player(v[0].id, 0);
    rig.advance(100);
    CHECK_EQ(rig.tr.count(12, Channel::Vibration), 0u);   // nothing to play: nothing written
    // one 5 ms pulse on the left hand, then silence: one sample of one packet carries it, the right grip's block stays silent
    rig.m.hd_vibration(0, 0, 0.5f, 160.f, 0.3f, 320.f, rig.now);
    rig.m.hd_vibration(0, 0, 0.f, 160.f, 0.f, 320.f, rig.now + 5 * kMs);
    rig.advance(200);
    std::vector<Write> vib;
    for (const Write& w : rig.tr.writes)
        if (w.dev == 12 && w.ch == Channel::Vibration) vib.push_back(w);
    CHECK(vib.size() >= 4 && vib.size() <= 5);   // the packet with the pulse and three of silence
    int on_samples = 0;
    for (const Write& w : vib) {
        CHECK_EQ(w.d.size(), kVibReportLen);
        CHECK_EQ(w.d[0], 0x00);
        CHECK_EQ(w.d[1] & 0xF0, 0x70);    // three samples
        CHECK_EQ(w.d[17] & 0xF0, 0x70);   // the Pro Controller 2's second block
        for (int s = 0; s < 3; ++s) {
            const uint8_t* l = w.d.data() + 2 + 5 * s;
            const uint8_t* r = w.d.data() + 18 + 5 * s;
            const bool lon = ((l[1] >> 2) | ((l[2] & 0x0F) << 6)) != 0;   // the low band's amplitude
            on_samples += lon;
            CHECK_EQ(r[1] >> 2 | (r[2] & 0x0F) << 6, 0);
        }
    }
    CHECK_EQ(on_samples, 1);
    CHECK(((vib[0].d[1] + 1) & 0x0F) == (vib[1].d[1] & 0x0F));   // the sequence counts up
    // SDL's plain rumble plays only while the player has no HD values (none for a second)
    rig.advance(1100);
    const size_t before = rig.tr.count(12, Channel::Vibration);
    rig.m.simple_rumble(v[0].id, 65535, 0, rig.now);
    rig.advance(30);
    CHECK(rig.tr.count(12, Channel::Vibration) > before);
}

SVC_TEST(sw2, gamecube_pad) {
    Rig rig;
    FakePad gc(kPidGameCube);
    gc.mem[kAddrTriggerZero] = {30, 32};
    rig.bring_up(13, &gc, kHost);
    CHECK_EQ(rig.m.live_sessions(), 1u);
    bool read_triggers = false;
    for (const auto& c : gc.commands)
        if (c.size() >= 16 && c[0] == kCmdMemory && c[8] == 2 && c[12] == 0x40 && c[13] == 0x31) read_triggers = true;
    CHECK(read_triggers);
    PadView v[1];
    CHECK_EQ(rig.m.list(v, 1), 1u);
    CHECK(v[0].type == PadType::GameCube);
    CHECK_EQ(v[0].product, kPidGameCube);
    rig.report(13, kRawA | kRawZR | kRawR);   // A, Z (ZR) and R's full press
    CHECK(rig.m.view(v[0].id, &v[0]));
    CHECK(v[0].buttons & (1u << kPbEast));   // the GameCube A is the console's A: Nintendo's A place
    CHECK(!(v[0].buttons & (1u << kPbSouth)));
    CHECK(v[0].buttons & (1u << kPbRightShoulder));
    CHECK_EQ(v[0].axes[kPaRightTrigger], 32767);
    // its motor: on / off packets while the game vibrates, then a brake
    rig.m.set_player(v[0].id, 1);
    rig.m.hd_vibration(1, 1, 1.f, 160.f, 0.f, 320.f, rig.now);
    rig.advance(60);
    rig.m.hd_vibration(1, 1, 0.f, 160.f, 0.f, 320.f, rig.now);
    rig.advance(100);
    bool on = false, brake = false;
    for (const Write& w : rig.tr.writes) {
        if (w.dev != 13 || w.ch != Channel::Vibration) continue;
        CHECK_EQ(w.d[1] & 0xF0, 0x50);
        on = on || w.d[2] == 1;
        brake = brake || w.d[2] == 2;
    }
    CHECK(on && brake);
}

SVC_TEST(sw2, mouse_sensor) {
    Rig rig;
    FakePad r(kPidJoyConR);
    rig.bring_up(14, &r, kHost);
    PadView v[1];
    rig.m.list(v, 1);
    int32_t dx = 0, dy = 0;
    bool surface = false;
    CHECK(!rig.m.take_mouse(v[0].id, 1, &dx, &dy, &surface));   // off until the game reads the sensors
    rig.m.set_mouse_wanted(true);
    rig.advance(20);
    bool mouse_mask = false;
    for (const auto& c : r.commands)
        if (c.size() >= 9 && c[0] == kCmdFeature && c[8] == 0x37) mouse_mask = true;
    CHECK(mouse_mask);
    auto mouse_report = [&](int16_t x, int16_t y, uint16_t dist) {
        uint8_t rep[63] = {};
        rep[0x10] = static_cast<uint8_t>(x);
        rep[0x11] = static_cast<uint8_t>(static_cast<uint16_t>(x) >> 8);
        rep[0x12] = static_cast<uint8_t>(y);
        rep[0x13] = static_cast<uint8_t>(static_cast<uint16_t>(y) >> 8);
        rep[0x14] = 0x40;
        rep[0x16] = static_cast<uint8_t>(dist);
        rep[0x17] = static_cast<uint8_t>(dist >> 8);
        rig.m.on_input(14, rep, sizeof rep, rig.now);
    };
    mouse_report(100, 50, 150);
    mouse_report(130, 40, 150);
    mouse_report(32767, 40, 3200);   // lifted: no movement
    mouse_report(-32766, 45, 150);   // back down: no movement across the touch-down (the lifted reading drifted)
    mouse_report(-32760, 47, 150);   // on the surface again (the counter wrapped while lifted): +6, +2
    CHECK(rig.m.take_mouse(v[0].id, 1, &dx, &dy, &surface));
    CHECK_EQ(dx, 30 + 6);
    CHECK_EQ(dy, -10 + 2);
    CHECK(surface);
    // the raw sensor states (hid GetJcmsStates) by player and side, with a sum of their own: the pointer's take does not empty it
    rig.m.set_player(v[0].id, 0);
    CHECK(rig.m.take_sensor(0, 1, &dx, &dy, &surface));
    CHECK_EQ(dx, 36);
    CHECK_EQ(dy, -8);
    CHECK(rig.m.take_sensor(0, 1, &dx, &dy, &surface));
    CHECK(dx == 0 && dy == 0 && surface);
    CHECK(!rig.m.take_sensor(0, 0, &dx, &dy, &surface));   // no left Joy-Con
    CHECK(!rig.m.take_sensor(1, 1, &dx, &dy, &surface));   // not player 2's
    CHECK(rig.m.take_mouse(v[0].id, 1, &dx, &dy, &surface));
    CHECK(dx == 0 && dy == 0);
    CHECK(!rig.m.take_mouse(v[0].id, 0, &dx, &dy, &surface));   // no left Joy-Con in this pad
}
