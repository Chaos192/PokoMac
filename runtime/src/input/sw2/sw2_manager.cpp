// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE: controllers, pads and their timers (nxrt/sw2/manager.h says how they behave).
#include "nxrt/sw2/manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#include "nxrt/log.h"

namespace nxrt::sw2 {
namespace {

constexpr const char* kTag = "sw2";
constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kConnectTimeout = 10000 * kMs;
constexpr uint64_t kHandshakeTimeout = 20000 * kMs;
constexpr uint64_t kCommandTimeout = 2000 * kMs;
constexpr uint64_t kRetryAfter = 3000 * kMs;
constexpr uint64_t kLedEvery = 1000 * kMs;
constexpr uint64_t kGestureHold = 3000 * kMs;
constexpr uint64_t kPowerEvery = 30000 * kMs;   // the battery level is read this often, and half a second after a controller comes up
constexpr uint64_t kPowerFirst = 500 * kMs;
constexpr size_t kMaxSessions = 16;
constexpr int kPlayers = 8;

// Rumble timing: the game sets a value about every 5 ms and holds it until the next, so each value is played once, in the first 5 ms
// sample whose end comes after it was set; a packet carries three samples and goes every 15 ms, its last sample ending kRumbleLag before
// the tick (so every value of its samples is known). Values that pile up wait at most kRumbleBacklog samples. A sample with no new value
// repeats the one before. A buzz ends with three packets of silence (a write without response can be lost).
constexpr uint64_t kSlot = 5 * kMs;
constexpr uint64_t kPacketEvery = 15 * kMs;
constexpr uint64_t kRumbleLag = 5 * kMs;
constexpr uint64_t kRumbleBacklog = 5;
constexpr size_t kRumbleLog = 64;
constexpr uint64_t kHdFresh = 1000 * kMs;   // a player's HD values count as current for this long after the last one

struct Action {
    enum Kind : uint8_t { Connect, Disconnect, EnableInput, Write, ReadPower } kind;
    uint64_t dev;
    Channel ch;
    std::vector<uint8_t> data;
};
using Outbox = std::vector<Action>;

enum class State : uint8_t { Connecting, Handshake, Live };
enum class Step : uint8_t {
    None, ReadInfo, PairHost, PairKey1, PairKey2, PairDone, ReadGyro, ReadStick1User, ReadStick1Factory, ReadStick2User, ReadStick2Factory,
    ReadTriggers, FeatureSet, FeatureEnable, MouseSet, MouseEnable
};

struct RumbleLog {
    uint64_t t[kRumbleLog] = {};
    HdSample m[kRumbleLog][2] = {};
    uint64_t n = 0;
    uint64_t last_ns = 0;
};
struct RumbleCursor {
    uint64_t next = 0, end = 0;
    HdSample now[2] = {};
};

enum Gesture : uint8_t { kGestureNone = 0, kGestureSplit, kGestureJoin };

struct Session {
    uint64_t dev = 0;
    Kind kind = Kind::None;
    AdvertUse use = AdvertUse::Take;
    State state = State::Connecting;
    uint64_t started_ns = 0, link_ns = 0;
    bool has_vib = false;
    // the command in flight
    Step step = Step::None;
    bool pending = false;
    uint8_t pending_cmd = 0;
    uint32_t pending_addr = 0;
    uint8_t pending_len = 0;
    uint64_t sent_ns = 0;
    // what the handshake learned
    DeviceInfo info;
    StickCal cal[2];
    uint8_t trig_zero[2] = {};
    RestBias rest;
    bool bonded = false;
    uint8_t keys[2][16] = {};
    uint8_t features = 0;
    bool mouse_on = false;
    // live
    uint32_t pad = 0;
    Report r;
    bool have_report = false;
    uint32_t reports = 0;
    uint64_t report_ns = 0;
    uint64_t led_ns = 0;
    int led_player = -2;
    uint64_t live_ns = 0, power_ns = 0;   // when it went live, when its power byte was last asked for
    PowerInfo power;
    uint64_t rumble_ns = 0;
    uint8_t vseq = 0;
    int off_left = 0;
    RumbleCursor cur;
    float gc_err = 0.f;
    bool gc_was_on = false;
    // the mouse sensor
    bool mouse_seen = false, mouse_surface = false, mouse_present = false;
    int16_t mx = 0, my = 0;
    int64_t mdx = 0, mdy = 0;   // since the last take_mouse
    int64_t jdx = 0, jdy = 0;   // since the last take_sensor
    // the Joy-Con hold gestures
    uint8_t gesture = kGestureNone;
    uint64_t gesture_since = 0;
    bool gesture_done = false;
};

struct PadRec {
    uint32_t id = 0;
    uint32_t generation = 1;
    uint64_t dev[2] = {};   // Pro / GameCube: [0]; Joy-Cons: [0] left, [1] right
    int player = -1;
    uint16_t simple_lo = 0, simple_hi = 0;
    uint64_t simple_ns = 0;
};

int16_t axis_of(float v) {   // -1..1 to SDL's axis range
    const float s = std::round(v * 32767.f);
    return static_cast<int16_t>(s > 32767.f ? 32767 : s < -32768.f ? -32768 : s);
}
int16_t trigger_axis(bool pressed) { return pressed ? int16_t{32767} : int16_t{-32768}; }

}  // namespace

const char* pad_type_name(PadType t) {
    switch (t) {
        case PadType::Pro: return "Nintendo Switch 2 Pro Controller";
        case PadType::GameCube: return "Nintendo GameCube Controller";
        case PadType::JoyConPair: return "Nintendo Switch 2 Joy-Con (L/R)";
        case PadType::JoyConLeft: return "Nintendo Switch 2 Joy-Con (L)";
        case PadType::JoyConRight: return "Nintendo Switch 2 Joy-Con (R)";
    }
    return "Nintendo Switch 2 controller";
}

uint16_t pad_type_pid(PadType t) {
    switch (t) {
        case PadType::Pro: return kPidPro;
        case PadType::GameCube: return kPidGameCube;
        case PadType::JoyConPair: return kPidJoyConPair;
        case PadType::JoyConLeft: return kPidJoyConL;
        case PadType::JoyConRight: return kPidJoyConR;
    }
    return kPidPro;
}

struct Manager::Impl {
    ManagerOptions opt;
    Transport* tr = nullptr;
    mutable std::mutex mu;
    uint8_t host[6] = {};
    bool host_known = false;
    bool want_mouse = false;
    std::map<uint64_t, Session> sessions;
    std::map<uint64_t, uint64_t> tried;   // dev -> when it was last connected to
    std::map<uint64_t, bool> told_ignore;
    std::vector<PadRec> pads;
    uint32_t next_pad = 1;
    RumbleLog rlog[kPlayers];
    std::mt19937_64 rng{std::random_device{}()};

    // ---- outbox -----------------------------------------------------------------------------------------------------------------
    void run(Outbox& out) {
        Transport* t = tr;
        if (!t) return;
        for (Action& a : out) {
            switch (a.kind) {
                case Action::Connect: t->connect(a.dev); break;
                case Action::Disconnect: t->disconnect(a.dev); break;
                case Action::EnableInput: t->enable_input(a.dev); break;
                case Action::Write: t->write(a.dev, a.ch, a.data.data(), a.data.size()); break;
                case Action::ReadPower: t->read_power(a.dev); break;
            }
        }
    }
    static void write(Outbox& out, uint64_t dev, Channel ch, const uint8_t* d, size_t n) { out.push_back({Action::Write, dev, ch, std::vector<uint8_t>(d, d + n)}); }

    // ---- pads -------------------------------------------------------------------------------------------------------------------
    PadRec* pad_of(uint32_t id) {
        for (PadRec& p : pads)
            if (p.id == id) return &p;
        return nullptr;
    }
    const PadRec* pad_of(uint32_t id) const {
        for (const PadRec& p : pads)
            if (p.id == id) return &p;
        return nullptr;
    }
    Session* sess(uint64_t dev) {
        if (!dev) return nullptr;
        auto it = sessions.find(dev);
        return it == sessions.end() ? nullptr : &it->second;
    }
    const Session* sess(uint64_t dev) const {
        if (!dev) return nullptr;
        auto it = sessions.find(dev);
        return it == sessions.end() ? nullptr : &it->second;
    }
    static int side_of(Kind k) { return k == Kind::JoyConR ? 1 : 0; }
    PadType type_of(const PadRec& p) const {
        const Session* a = sess(p.dev[0]);
        const Session* b = sess(p.dev[1]);
        if (a && a->kind == Kind::Pro) return PadType::Pro;
        if (a && a->kind == Kind::GameCube) return PadType::GameCube;
        if (a && b) return PadType::JoyConPair;
        return b ? PadType::JoyConRight : PadType::JoyConLeft;
    }
    bool paired(const Session& s) const {
        const PadRec* p = pad_of(s.pad);
        return p && is_joycon(s.kind) && p->dev[0] && p->dev[1];
    }

    // A controller that finished its handshake joins a pad: a Joy-Con the pad that holds only the other side, the oldest such.
    void attach(Session& s) {
        if (is_joycon(s.kind)) {
            const int side = side_of(s.kind);
            for (PadRec& p : pads) {
                const Session* a = sess(p.dev[0]);
                if (a && !is_joycon(a->kind)) continue;
                if (!p.dev[side] && p.dev[1 - side]) {
                    p.dev[side] = s.dev;
                    ++p.generation;
                    s.pad = p.id;
                    return;
                }
            }
        }
        PadRec p;
        p.id = next_pad++;
        p.dev[is_joycon(s.kind) ? side_of(s.kind) : 0] = s.dev;
        pads.push_back(p);
        s.pad = p.id;
    }
    void detach(Session& s) {
        PadRec* p = pad_of(s.pad);
        s.pad = 0;
        if (!p) return;
        for (uint64_t& d : p->dev)
            if (d == s.dev) d = 0;
        if (!p->dev[0] && !p->dev[1]) {
            const uint32_t id = p->id;
            pads.erase(std::remove_if(pads.begin(), pads.end(), [id](const PadRec& q) { return q.id == id; }), pads.end());
        } else {
            ++p->generation;
        }
    }

    // ---- commands ---------------------------------------------------------------------------------------------------------------
    void send(Outbox& out, Session& s, Step step, uint8_t cmd, uint8_t sub, const uint8_t* payload, size_t n, uint64_t now) {
        uint8_t f[kCommandHeader + 64];
        const size_t len = encode_command(f, sizeof f, cmd, sub, payload, n);
        if (!len) return;
        s.step = step;
        s.pending = true;
        s.pending_cmd = cmd;
        s.pending_len = 0;
        s.sent_ns = now;
        write(out, s.dev, Channel::Command, f, len);
    }
    void read_memory(Outbox& out, Session& s, Step step, uint8_t len, uint32_t addr, uint64_t now) {
        uint8_t f[kCommandHeader + 8];
        const size_t n = encode_memory_read(f, sizeof f, len, addr);
        s.step = step;
        s.pending = true;
        s.pending_cmd = kCmdMemory;
        s.pending_len = len;
        s.pending_addr = addr;
        s.sent_ns = now;
        write(out, s.dev, Channel::Command, f, n);
    }
    void features(Outbox& out, Session& s, Step step, uint64_t now) {
        uint8_t p[4] = {feature_mask(s.kind, s.mouse_on), 0, 0, 0};
        s.features = p[0];
        send(out, s, step, kCmdFeature, step == Step::FeatureSet || step == Step::MouseSet ? kSubFeatureSet : kSubFeatureEnable, p, sizeof p, now);
    }
    void leds(Outbox& out, Session& s, uint64_t now) {
        const PadRec* p = pad_of(s.pad);
        const int player = p ? p->player : -1;
        uint8_t pl[kLedPayloadLen];
        led_payload(pl, led_pattern(player));
        uint8_t f[kCommandHeader + kLedPayloadLen];
        const size_t n = encode_command(f, sizeof f, kCmdLeds, kSubLedsSet, pl, sizeof pl);
        write(out, s.dev, Channel::Command, f, n);   // not in the reply queue: its answer is not waited for
        s.led_ns = now;
        s.led_player = player;
    }

    void fail(Outbox& out, Session& s, const char* why, uint64_t now) {
        NXLOG_W(kTag, "%s: %s; the link is dropped", kind_name(s.kind), why);
        (void)now;
        const uint64_t dev = s.dev;
        out.push_back({Action::Disconnect, dev, Channel::Command, {}});
        detach(s);
        sessions.erase(dev);
    }

    // The next handshake step after `done` (the reply data, nullptr for no answer or a refused read).
    void advance(Outbox& out, Session& s, const uint8_t* data, size_t n, uint64_t now) {
        const Step done = s.step;
        s.pending = false;
        s.step = Step::None;
        auto next_after_sticks = [&](bool stick2_done) {
            if (!stick2_done && (s.kind == Kind::Pro || s.kind == Kind::GameCube)) {
                read_memory(out, s, Step::ReadStick2User, 0x0B, kAddrStick2User, now);
            } else if (s.kind == Kind::GameCube) {
                read_memory(out, s, Step::ReadTriggers, 0x02, kAddrTriggerZero, now);
            } else {
                features(out, s, Step::FeatureSet, now);
            }
        };
        auto pair_failed = [&](const char* at) {
            NXLOG_W(kTag, "%s: bonding stopped at %s (no answer): it plays now, but needs Sync again next time", kind_name(s.kind), at);
            read_memory(out, s, Step::ReadGyro, 0x10, kAddrGyroFactory, now);
        };
        switch (done) {
            case Step::ReadInfo: {
                DeviceInfo info;
                if (!data || !parse_info(data, n, &info) || info.vid != kVendorId) {
                    fail(out, s, "it did not answer the identity read", now);
                    return;
                }
                const Kind k = kind_of_pid(info.pid);
                if (k != Kind::None && k != s.kind) {
                    NXLOG_I(kTag, "advertised as %s but identifies as %s", kind_name(s.kind), kind_name(k));
                    s.kind = k;
                }
                s.info = info;
                if (s.use == AdvertUse::Pair && opt.pair && host_known) {
                    uint8_t p[kPairHostLen];
                    pair_host_payload(p, host);
                    send(out, s, Step::PairHost, kCmdPair, kSubPairHost, p, sizeof p, now);
                } else {
                    read_memory(out, s, Step::ReadGyro, 0x10, kAddrGyroFactory, now);
                }
                return;
            }
            case Step::PairHost:
            case Step::PairKey1: {
                if (!data) {
                    pair_failed(done == Step::PairHost ? "the host address" : "the first key");
                    return;
                }
                const int k = done == Step::PairHost ? 0 : 1;
                for (uint8_t& b : s.keys[k]) b = static_cast<uint8_t>(rng());
                uint8_t p[kPairKeyLen];
                pair_key_payload(p, s.keys[k]);
                send(out, s, k == 0 ? Step::PairKey1 : Step::PairKey2, kCmdPair, k == 0 ? kSubPairKey1 : kSubPairKey2, p, sizeof p, now);
                return;
            }
            case Step::PairKey2: {
                if (!data) {
                    pair_failed("the second key");
                    return;
                }
                const uint8_t z = 0;
                send(out, s, Step::PairDone, kCmdPair, kSubPairDone, &z, 1, now);
                return;
            }
            case Step::PairDone:
                if (!data) {
                    pair_failed("the last step");
                    return;
                }
                s.bonded = true;
                NXLOG_I(kTag, "%s (%s) bonded to this machine: from now on a press of any of its buttons connects it, no Sync needed", kind_name(s.kind),
                        s.info.serial);
                read_memory(out, s, Step::ReadGyro, 0x10, kAddrGyroFactory, now);
                return;
            case Step::ReadGyro: {
                float off[3];
                if (data && parse_gyro_factory(data, n, off) && gyro_factory_usable(s.kind)) {
                    std::memcpy(s.rest.bias, off, sizeof off);
                    s.rest.have = true;
                }
                read_memory(out, s, Step::ReadStick1User, 0x0B, kAddrStick1User, now);
                return;
            }
            case Step::ReadStick1User:
                if (data && parse_user_stick_cal(data, n, &s.cal[0])) next_after_sticks(false);
                else read_memory(out, s, Step::ReadStick1Factory, 0x0B, kAddrStick1Factory, now);
                return;
            case Step::ReadStick1Factory:
                if (data) parse_stick_cal(data, n, &s.cal[0]);
                next_after_sticks(false);
                return;
            case Step::ReadStick2User:
                if (data && parse_user_stick_cal(data, n, &s.cal[1])) next_after_sticks(true);
                else read_memory(out, s, Step::ReadStick2Factory, 0x0B, kAddrStick2Factory, now);
                return;
            case Step::ReadStick2Factory:
                if (data) parse_stick_cal(data, n, &s.cal[1]);
                next_after_sticks(true);
                return;
            case Step::ReadTriggers:
                if (data && n >= 2) {
                    s.trig_zero[0] = data[0];
                    s.trig_zero[1] = data[1];
                }
                features(out, s, Step::FeatureSet, now);
                return;
            case Step::FeatureSet:
                features(out, s, Step::FeatureEnable, now);
                return;
            case Step::FeatureEnable:
                if (!data) {
                    fail(out, s, "it did not take the features", now);
                    return;
                }
                go_live(out, s, now);
                return;
            case Step::MouseSet:
                features(out, s, Step::MouseEnable, now);
                return;
            case Step::MouseEnable:
                NXLOG_I(kTag, "%s: mouse sensor %s", kind_name(s.kind), data ? "on" : "not turned on (no answer)");
                return;
            case Step::None: return;
        }
    }

    void go_live(Outbox& out, Session& s, uint64_t now) {
        s.state = State::Live;
        s.live_ns = now;
        attach(s);
        const PadRec* p = pad_of(s.pad);
        NXLOG_I(kTag, "%s connected (%s%s, serial %s, sticks %s%s): pad %u, %s", kind_name(s.kind), advert_use_name(s.use), s.bonded ? ", bonded" : "",
                s.info.serial, s.cal[0].valid ? "calibrated" : "default", s.kind == Kind::Pro || s.kind == Kind::GameCube ? (s.cal[1].valid ? "/calibrated" : "/default") : "",
                s.pad, p ? pad_type_name(type_of(*p)) : "?");
        leds(out, s, now);
        out.push_back({Action::EnableInput, s.dev, Channel::Command, {}});
        // the other side of a pair it joined: its LEDs follow the pad's player too
        if (p)
            for (uint64_t d : p->dev)
                if (Session* o = sess(d); o && o != &s) o->led_player = -2;
    }

    // ---- rumble -----------------------------------------------------------------------------------------------------------------
    static void slot(const RumbleLog& l, RumbleCursor& c, unsigned motors) {
        const uint64_t end = c.end += kSlot;
        if (c.next > l.n) c.next = l.n;
        else if (l.n - c.next > kRumbleLog) c.next = l.n - kRumbleLog;
        for (; c.next < l.n; ++c.next) {
            const size_t i = c.next % kRumbleLog;
            if (l.t[i] >= end) return;   // set after this sample ends
            if (c.next + 1 < l.n && l.t[(c.next + 1) % kRumbleLog] + kRumbleBacklog * kSlot < end) continue;   // a newer one is overdue too
            bool changes = false;
            for (int k = 0; k < 2; ++k)
                if ((motors >> k & 1) && !hd_same(l.m[i][k], c.now[k])) changes = true;
            if (!changes) continue;
            c.now[0] = l.m[i][0];
            c.now[1] = l.m[i][1];
            ++c.next;
            return;
        }
    }
    unsigned motors_of(const Session& s) const { return is_joycon(s.kind) && paired(s) ? 1u << side_of(s.kind) : 3u; }
    // What one controller plays of the two hands: a Pro Controller 2 both (a block each), a Joy-Con of a pair its own side, a Joy-Con alone
    // (the whole pad, held sideways) the stronger of the two in each band.
    void pick(const Session& s, const HdSample m[2], HdSample* a, HdSample* b) const {
        if (s.kind == Kind::Pro) {
            *a = m[0];
            *b = m[1];
            return;
        }
        if (is_joycon(s.kind) && paired(s)) {
            *a = *b = m[side_of(s.kind)];
            return;
        }
        HdSample v = m[0];
        if (m[1].lo_amp > v.lo_amp) { v.lo_amp = m[1].lo_amp; v.lo_freq = m[1].lo_freq; }
        if (m[1].hi_amp > v.hi_amp) { v.hi_amp = m[1].hi_amp; v.hi_freq = m[1].hi_freq; }
        *a = *b = v;
    }
    void rumble(Outbox& out, Session& s, uint64_t now) {
        if (!s.has_vib) return;
        PadRec* p = pad_of(s.pad);
        if (!p) return;
        const int pl = p->player;
        HdSample a[3], b[3];
        const bool hd = pl >= 0 && pl < kPlayers && rlog[pl].n && now - rlog[pl].last_ns < kHdFresh;
        if (hd) {
            const uint64_t want = now > kRumbleLag + 3 * kSlot ? now - kRumbleLag - 3 * kSlot : 0;   // where this packet's first sample starts
            if (s.cur.end + 3 * kSlot < want || s.cur.end > want + 3 * kSlot) s.cur.end = want;
            for (int k = 0; k < 3; ++k) {
                slot(rlog[pl], s.cur, motors_of(s));
                pick(s, s.cur.now, &a[k], &b[k]);
            }
        } else {
            s.cur.next = pl >= 0 && pl < kPlayers ? rlog[pl].n : 0;   // HD values that come later start from then
            const bool fresh = now - p->simple_ns < kHdFresh;
            const HdSample v = hd_sample(fresh ? p->simple_lo / 65535.f : 0.f, 160.f, fresh ? p->simple_hi / 65535.f : 0.f, 320.f);
            const HdSample m[2] = {v, v};
            for (int k = 0; k < 3; ++k) pick(s, m, &a[k], &b[k]);
        }
        bool on = false;
        for (int k = 0; k < 3; ++k) on = on || hd_on(a[k]) || hd_on(b[k]);
        if (on) s.off_left = 3;
        else if (s.off_left > 0) --s.off_left;
        else return;
        uint8_t pk[kVibReportLen];
        const size_t cap = std::min<size_t>(sizeof pk, tr ? tr->max_write(s.dev) : sizeof pk);
        size_t n;
        if (s.kind == Kind::GameCube) {
            // the motor is on or off: the strongest of the three samples' amplitudes, as pulse density
            float strength = 0.f;
            for (int k = 0; k < 3; ++k) strength = std::max({strength, a[k].lo_amp / 1023.f, a[k].hi_amp / 1023.f, b[k].lo_amp / 1023.f, b[k].hi_amp / 1023.f});
            GcMotor m = gc_motor_step(strength, &s.gc_err);
            if (!on) m = s.gc_was_on || s.off_left == 2 ? GcMotor::Brake : GcMotor::Off;
            s.gc_was_on = on;
            n = encode_gc_report(pk, cap, m, s.vseq++, true);
        } else {
            n = encode_hd_report(pk, cap, a, s.kind == Kind::Pro ? b : nullptr, 3, s.vseq++, true);
        }
        if (n) write(out, s.dev, Channel::Vibration, pk, n);
    }

    // ---- the Joy-Con hold gestures --------------------------------------------------------------------------------------------------
    void gesture_update(Session& s, uint64_t now) {
        if (!is_joycon(s.kind)) return;
        const bool right = s.kind == Kind::JoyConR;
        const uint32_t rails = right ? (kRawSLR | kRawSRR) : (kRawSLL | kRawSRL);
        const uint32_t b = s.r.buttons;
        uint8_t g = kGestureNone;
        if (paired(s)) g = (b & rails) == rails ? kGestureSplit : kGestureNone;
        else g = (b & (right ? kRawR : kRawL)) && !(b & rails) ? kGestureJoin : kGestureNone;
        if (g != s.gesture) {
            s.gesture = g;
            s.gesture_since = now;
            s.gesture_done = false;
        }
    }
    void gestures(uint64_t now) {
        for (auto& [dev, s] : sessions) {
            if (s.state != State::Live || s.gesture == kGestureNone || s.gesture_done || now - s.gesture_since < kGestureHold) continue;
            if (s.gesture == kGestureSplit && paired(s)) {
                s.gesture_done = true;
                detach(s);
                PadRec p;
                p.id = next_pad++;
                p.dev[side_of(s.kind)] = s.dev;
                pads.push_back(p);
                s.pad = p.id;
                s.led_player = -2;
                NXLOG_I(kTag, "%s plays on its own now: pad %u (SL + SR held)", kind_name(s.kind), s.pad);
                return;   // the pads changed: the next tick looks again
            }
            if (s.gesture == kGestureJoin && !paired(s)) {
                for (auto& [odev, o] : sessions) {
                    if (&o == &s || o.state != State::Live || !is_joycon(o.kind) || o.kind == s.kind || paired(o)) continue;
                    if (o.gesture != kGestureJoin || o.gesture_done || now - o.gesture_since < kGestureHold) continue;
                    s.gesture_done = o.gesture_done = true;
                    // the Joy-Con of the newer pad moves into the older one
                    Session& mover = s.pad > o.pad ? s : o;
                    Session& stay = s.pad > o.pad ? o : s;
                    detach(mover);
                    PadRec* into = pad_of(stay.pad);
                    if (!into) return;
                    into->dev[side_of(mover.kind)] = mover.dev;
                    ++into->generation;
                    mover.pad = into->id;
                    mover.led_player = stay.led_player = -2;
                    NXLOG_I(kTag, "%s and %s play as one pair now: pad %u (L + R held)", kind_name(stay.kind), kind_name(mover.kind), into->id);
                    return;
                }
            }
        }
    }

    // ---- the pad as a gamepad ---------------------------------------------------------------------------------------------------
    void stick(const Session& s, int slot_index, int raw_index, float* x, float* y) const {
        apply_stick_cal(s.cal[slot_index], s.r.stick[raw_index], s.r.stick[raw_index + 1], x, y);
    }
    void motion(const Session& s, Hold h, PadView* v) const {
        if (!opt.motion || !s.have_report) return;
        if (!(s.r.accel[0] | s.r.accel[1] | s.r.accel[2] | s.r.gyro[0] | s.r.gyro[1] | s.r.gyro[2])) return;   // no sensor (or motion off)
        float acc[3], rot[3];
        motion_to_console(s.r.accel, s.r.gyro, motion_map(h), s.rest.have ? s.rest.bias : nullptr, acc, rot);
        console_to_sdl(acc, rot, v->accel, v->gyro);
        v->motion = true;
    }
    void build(const PadRec& p, PadView* v) const {
        *v = PadView{};
        v->id = p.id;
        v->generation = p.generation;
        v->type = type_of(p);
        v->product = pad_type_pid(v->type);
        std::snprintf(v->name, sizeof v->name, "%s", pad_type_name(v->type));
        v->player = p.player;
        const Session* L = nullptr;
        const Session* R = nullptr;
        const Session* one = sess(p.dev[0]);
        if (v->type == PadType::JoyConPair || v->type == PadType::JoyConLeft || v->type == PadType::JoyConRight) {
            L = sess(p.dev[0]);
            R = sess(p.dev[1]);
        }
        uint32_t raw = 0;
        uint16_t batt = 0xFFFF;
        for (int k = 0; k < 2; ++k)
            if (const Session* s = sess(p.dev[k]); s && s->have_report) v->raw_buttons[k] = s->r.buttons;
        for (const Session* s : {sess(p.dev[0]), sess(p.dev[1])}) {
            if (!s || !s->have_report) continue;
            raw |= s->r.buttons;
            v->updated_ns = std::max(v->updated_ns, s->report_ns);
            v->reports += s->reports;
            if (s->r.battery_mv) batt = std::min(batt, s->r.battery_mv);
        }
        v->battery_mv = batt == 0xFFFF ? 0 : batt;
        for (const Session* s : {sess(p.dev[0]), sess(p.dev[1])}) {
            if (!s || s->power.level < 0) continue;
            v->battery_level = v->battery_level < 0 ? s->power.level : std::min(v->battery_level, s->power.level);
            v->charging = v->charging || s->power.charging;
        }
        uint32_t& b = v->buttons;
        auto set = [&](uint32_t mask, PadButton pb) {
            if (raw & mask) b |= 1u << pb;
        };
        float x = 0, y = 0;
        switch (v->type) {
            case PadType::Pro:
            case PadType::JoyConPair:
            case PadType::GameCube: {
                // Nintendo's places, B bottom, A right, Y left, X top: what the console's A B X Y are. The GameCube controller's A B X Y are
                // its buttons of those names (as the console reports them), so they go to the same places (the bridge gives every pad
                // Nintendo's labels), whatever their shape on the controller.
                set(kRawB, kPbSouth);
                set(kRawA, kPbEast);
                set(kRawY, kPbWest);
                set(kRawX, kPbNorth);
                set(kRawMinus, kPbBack);
                set(kRawHome, kPbGuide);
                set(kRawPlus, kPbStart);
                set(kRawLStick, kPbLeftStick);
                set(kRawRStick, kPbRightStick);
                set(kRawL, kPbLeftShoulder);
                set(kRawR, kPbRightShoulder);
                set(kRawUp, kPbDpadUp);
                set(kRawDown, kPbDpadDown);
                set(kRawLeft, kPbDpadLeft);
                set(kRawRight, kPbDpadRight);
                set(kRawCapture, kPbMisc1);
                set(kRawC, kPbMisc2);
                if (v->type == PadType::JoyConPair) {
                    set(kRawGR | kRawSRR, kPbRightPaddle1);
                    set(kRawGL | kRawSLL, kPbLeftPaddle1);
                    set(kRawSLR, kPbRightPaddle2);
                    set(kRawSRL, kPbLeftPaddle2);
                } else {
                    set(kRawGR, kPbRightPaddle1);
                    set(kRawGL, kPbLeftPaddle1);
                }
                v->axes[kPaLeftTrigger] = trigger_axis(raw & kRawZL);
                v->axes[kPaRightTrigger] = trigger_axis(raw & kRawZR);
                const Session* ls = v->type == PadType::JoyConPair ? L : one;
                const Session* rs = v->type == PadType::JoyConPair ? R : one;
                if (ls && ls->have_report) {
                    stick(*ls, 0, 0, &x, &y);
                    v->axes[kPaLeftX] = axis_of(x);
                    v->axes[kPaLeftY] = axis_of(-y);
                }
                if (rs && rs->have_report) {
                    // a right Joy-Con's one stick is in the second raw slot, its calibration in the first
                    stick(*rs, v->type == PadType::JoyConPair ? 0 : 1, 2, &x, &y);
                    v->axes[kPaRightX] = axis_of(x);
                    v->axes[kPaRightY] = axis_of(-y);
                }
                if (v->type == PadType::JoyConPair) {
                    if (R) motion(*R, Hold::Upright, v);
                    else if (L) motion(*L, Hold::Upright, v);
                } else if (one) {
                    motion(*one, Hold::Pro, v);
                }
                break;
            }
            case PadType::JoyConLeft: {   // held sideways, stick on the left: the arrows are the face buttons
                set(kRawLeft, kPbSouth);
                set(kRawDown, kPbEast);
                set(kRawUp, kPbWest);
                set(kRawRight, kPbNorth);
                set(kRawSLL, kPbLeftShoulder);
                set(kRawSRL, kPbRightShoulder);
                set(kRawMinus, kPbStart);
                set(kRawLStick, kPbLeftStick);
                set(kRawCapture, kPbMisc1);
                set(kRawL, kPbLeftPaddle1);
                set(kRawZL, kPbLeftPaddle2);
                if (L && L->have_report) {
                    stick(*L, 0, 0, &x, &y);
                    v->axes[kPaLeftX] = axis_of(-y);   // turned a quarter: the stick's up is the pad's left
                    v->axes[kPaLeftY] = axis_of(-x);
                    motion(*L, Hold::LeftSideways, v);
                }
                v->axes[kPaLeftTrigger] = v->axes[kPaRightTrigger] = -32768;
                break;
            }
            case PadType::JoyConRight: {
                set(kRawA, kPbSouth);
                set(kRawX, kPbEast);
                set(kRawB, kPbWest);
                set(kRawY, kPbNorth);
                set(kRawSLR, kPbLeftShoulder);
                set(kRawSRR, kPbRightShoulder);
                set(kRawPlus, kPbStart);
                set(kRawHome, kPbGuide);
                set(kRawRStick, kPbLeftStick);
                set(kRawC, kPbMisc2);
                set(kRawR, kPbRightPaddle1);
                set(kRawZR, kPbRightPaddle2);
                if (R && R->have_report) {
                    stick(*R, 0, 2, &x, &y);
                    v->axes[kPaLeftX] = axis_of(y);    // turned a quarter the other way: the stick's up is the pad's right
                    v->axes[kPaLeftY] = axis_of(x);
                    motion(*R, Hold::RightSideways, v);
                }
                v->axes[kPaLeftTrigger] = v->axes[kPaRightTrigger] = -32768;
                break;
            }
        }
        for (const Session* s : {L, R}) {
            if (!s || !s->mouse_on) continue;
            const int side = side_of(s->kind);
            v->mouse_present[side] = s->mouse_present;
            v->mouse_surface[side] = s->mouse_surface;
        }
    }
};

Manager::Manager(const ManagerOptions& opt) : d_(std::make_unique<Impl>()) { d_->opt = opt; }
Manager::~Manager() = default;

void Manager::set_transport(Transport* t) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->tr = t;
}

void Manager::set_host_address(const uint8_t le[6]) {
    std::lock_guard<std::mutex> lk(d_->mu);
    std::memcpy(d_->host, le, 6);
    d_->host_known = (le[0] | le[1] | le[2] | le[3] | le[4] | le[5]) != 0;
}

bool Manager::host_address_known() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->host_known;
}

void Manager::on_advert(uint64_t dev, const uint8_t* mfr, size_t n, int rssi, uint64_t now) {
    Outbox out;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        Impl& m = *d_;
        Advert a;
        if (!parse_advert(mfr, n, &a) || m.sessions.count(dev)) return;
        AdvertUse use = advert_use(a, m.host_known ? m.host : nullptr);
        if (use == AdvertUse::Pair && !m.opt.pair) use = AdvertUse::Take;
        if (use == AdvertUse::Ignore) {
            if (!m.told_ignore[dev]) {
                m.told_ignore[dev] = true;
                NXLOG_I(kTag, "a %s belongs to another host (a Switch 2 nearby?): left alone; hold its Sync button to pair it with this machine",
                        kind_name(kind_of_pid(a.pid)));
            }
            return;
        }
        auto t = m.tried.find(dev);
        if (t != m.tried.end() && now - t->second < kRetryAfter) return;
        if (m.sessions.size() >= kMaxSessions) return;
        m.tried[dev] = now;
        Session s;
        s.dev = dev;
        s.kind = kind_of_pid(a.pid);
        s.use = use;
        s.started_ns = now;
        s.mouse_on = m.want_mouse && is_joycon(s.kind);
        m.sessions[dev] = s;
        if (use == AdvertUse::Take && !m.told_ignore[dev]) {
            m.told_ignore[dev] = true;
            if (!m.host_known)
                NXLOG_I(kTag, "this machine's Bluetooth address is not known: the %s is used without bonding and needs Sync again next time", kind_name(s.kind));
        }
        NXLOG_I(kTag, "%s %s (rssi %d): connecting", use == AdvertUse::Wake ? "woke:" : "pairing:", kind_name(s.kind), rssi);
        out.push_back({Action::Connect, dev, Channel::Command, {}});
    }
    d_->run(out);
}

void Manager::on_link_up(uint64_t dev, bool has_vibration, uint64_t now) {
    Outbox out;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        Session* s = d_->sess(dev);
        if (!s || s->state != State::Connecting) return;
        s->state = State::Handshake;
        s->link_ns = now;
        s->has_vib = has_vibration;
        d_->read_memory(out, *s, Step::ReadInfo, 0x40, kAddrInfo, now);
    }
    d_->run(out);
}

void Manager::on_link_down(uint64_t dev, const char* why, uint64_t now) {
    std::lock_guard<std::mutex> lk(d_->mu);
    Session* s = d_->sess(dev);
    if (!s) return;
    NXLOG_I(kTag, "%s %s%s%s", kind_name(s->kind), s->state == State::Live ? "disconnected" : "could not be set up", why && *why ? ": " : "", why ? why : "");
    (void)now;
    d_->detach(*s);
    d_->sessions.erase(dev);
}

void Manager::on_reply(uint64_t dev, const uint8_t* d, size_t n, uint64_t now) {
    Outbox out;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        Session* s = d_->sess(dev);
        if (!s || !s->pending || !reply_is(d, n, s->pending_cmd)) return;
        if (s->pending_cmd == kCmdMemory) {
            // a reply that does not echo this read (a late answer to one that timed out) is not its answer: the timeout handles a refusal
            const uint8_t* data = memory_reply_data(d, n, s->pending_len, s->pending_addr);
            if (!data) return;
            d_->advance(out, *s, data, s->pending_len, now);
        } else {
            d_->advance(out, *s, d + kCommandHeader, n - kCommandHeader, now);
        }
    }
    d_->run(out);
}

void Manager::on_input(uint64_t dev, const uint8_t* d, size_t n, uint64_t now) {
    std::lock_guard<std::mutex> lk(d_->mu);
    Session* s = d_->sess(dev);
    if (!s || s->state != State::Live) return;
    Report r;
    if (!parse_report(d, n, &r)) return;
    s->r = r;
    s->have_report = true;
    ++s->reports;
    s->report_ns = now;
    s->rest.update(r.accel, r.gyro);
    if (s->mouse_on && mouse_present(r)) {
        const bool on = mouse_on_surface(r);
        // lifted, the counters drift with nothing under them: no movement then, nor across the touch-down (from a lifted reading)
        if (s->mouse_seen && on && s->mouse_surface) {
            const int32_t dx = mouse_delta(r.mouse_x, s->mx), dy = mouse_delta(r.mouse_y, s->my);
            s->mdx += dx;
            s->mdy += dy;
            s->jdx += dx;
            s->jdy += dy;
        }
        s->mouse_surface = on;
        s->mouse_present = true;
        s->mouse_seen = true;
        s->mx = r.mouse_x;
        s->my = r.mouse_y;
    }
    d_->gesture_update(*s, now);
}

void Manager::on_power(uint64_t dev, const uint8_t* d, size_t n, uint64_t now) {
    std::lock_guard<std::mutex> lk(d_->mu);
    Session* s = d_->sess(dev);
    PowerInfo p;
    if (!s || s->state != State::Live || !parse_power(d, n, &p)) return;
    if (s->power.level != p.level || s->power.charging != p.charging)
        NXLOG_I(kTag, "%s: battery level %d of 9%s%s (%u mV)", kind_name(s->kind), p.level, p.charging ? ", charging" : "", p.external ? ", on external power" : "",
                s->r.battery_mv);
    (void)now;
    s->power = p;
}

void Manager::tick(uint64_t now) {
    Outbox out;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        Impl& m = *d_;
        std::vector<uint64_t> devs;
        for (auto& [dev, s] : m.sessions) devs.push_back(dev);
        for (uint64_t dev : devs) {
            Session* s = m.sess(dev);
            if (!s) continue;
            if (s->state == State::Connecting && now - s->started_ns > kConnectTimeout) {
                m.fail(out, *s, "connecting timed out", now);
                continue;
            }
            if (s->state == State::Handshake && now - s->link_ns > kHandshakeTimeout) {
                m.fail(out, *s, "the handshake timed out", now);
                continue;
            }
            if (s->pending && now - s->sent_ns > kCommandTimeout) {
                NXLOG_W(kTag, "%s: command 0x%02x got no answer", kind_name(s->kind), s->pending_cmd);
                m.advance(out, *s, nullptr, 0, now);
                if (!m.sess(dev)) continue;
            }
            if (s->state != State::Live) continue;
            const PadRec* p = m.pad_of(s->pad);
            if (now - s->led_ns >= kLedEvery || (p && p->player != s->led_player)) m.leds(out, *s, now);
            if (m.want_mouse && is_joycon(s->kind) && !s->mouse_on && !s->pending) {
                s->mouse_on = true;
                m.features(out, *s, Step::MouseSet, now);
            }
            if (now - s->live_ns >= kPowerFirst && (!s->power_ns || now - s->power_ns >= kPowerEvery)) {
                s->power_ns = now;
                out.push_back({Action::ReadPower, s->dev, Channel::Command, {}});
            }
            if (now - s->rumble_ns >= kPacketEvery) {
                s->rumble_ns = now - s->rumble_ns < 2 * kPacketEvery ? s->rumble_ns + kPacketEvery : now;
                m.rumble(out, *s, now);
            }
        }
        m.gestures(now);
    }
    d_->run(out);
}

size_t Manager::list(PadView* out, size_t max) const {
    std::lock_guard<std::mutex> lk(d_->mu);
    size_t n = 0;
    for (const PadRec& p : d_->pads) {
        if (n >= max) break;
        d_->build(p, &out[n++]);
    }
    return n;
}

bool Manager::view(uint32_t pad_id, PadView* out) const {
    std::lock_guard<std::mutex> lk(d_->mu);
    const PadRec* p = d_->pad_of(pad_id);
    if (!p) return false;
    d_->build(*p, out);
    return true;
}

void Manager::set_player(uint32_t pad_id, int player) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (PadRec* p = d_->pad_of(pad_id)) p->player = player >= 0 && player < kPlayers ? player : -1;   // the LEDs follow at the next tick
}

void Manager::hd_vibration(int player, int hand, float amp_lo, float freq_lo, float amp_hi, float freq_hi, uint64_t now) {
    if (player < 0 || player >= kPlayers || hand < 0 || hand > 1) return;
    std::lock_guard<std::mutex> lk(d_->mu);
    RumbleLog& l = d_->rlog[player];
    HdSample m[2];
    if (l.n) {
        const size_t last = (l.n - 1) % kRumbleLog;
        m[0] = l.m[last][0];
        m[1] = l.m[last][1];
    }
    m[hand] = hd_sample(amp_lo, freq_lo, amp_hi, freq_hi);
    const size_t i = l.n % kRumbleLog;
    l.t[i] = now;
    l.m[i][0] = m[0];
    l.m[i][1] = m[1];
    ++l.n;
    l.last_ns = now;
}

void Manager::simple_rumble(uint32_t pad_id, uint16_t low, uint16_t high, uint64_t now) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (PadRec* p = d_->pad_of(pad_id)) {
        p->simple_lo = low;
        p->simple_hi = high;
        p->simple_ns = now;
    }
}

void Manager::set_mouse_wanted(bool on) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (on && !d_->want_mouse) NXLOG_I(kTag, "the game reads the Joy-Con 2 mouse sensors: Joy-Cons turn theirs on");
    d_->want_mouse = d_->want_mouse || on;   // once on, it stays on while the controllers are connected
}

bool Manager::take_mouse(uint32_t pad_id, int side, int32_t* dx, int32_t* dy, bool* on_surface) {
    std::lock_guard<std::mutex> lk(d_->mu);
    const PadRec* p = d_->pad_of(pad_id);
    if (!p || side < 0 || side > 1) return false;
    Session* s = d_->sess(p->dev[side]);
    if (!s || !is_joycon(s->kind) || !s->mouse_present) return false;
    *dx = static_cast<int32_t>(std::clamp<int64_t>(s->mdx, INT32_MIN, INT32_MAX));
    *dy = static_cast<int32_t>(std::clamp<int64_t>(s->mdy, INT32_MIN, INT32_MAX));
    *on_surface = s->mouse_surface;
    s->mdx = s->mdy = 0;
    return true;
}

bool Manager::take_sensor(int player, int side, int32_t* dx, int32_t* dy, bool* on_surface) {
    if (player < 0 || side < 0 || side > 1) return false;
    std::lock_guard<std::mutex> lk(d_->mu);
    for (const PadRec& p : d_->pads) {
        if (p.player != player) continue;
        Session* s = d_->sess(p.dev[side]);
        if (!s || !is_joycon(s->kind) || !s->mouse_present) continue;
        *dx = static_cast<int32_t>(std::clamp<int64_t>(s->jdx, INT32_MIN, INT32_MAX));
        *dy = static_cast<int32_t>(std::clamp<int64_t>(s->jdy, INT32_MIN, INT32_MAX));
        *on_surface = s->mouse_surface;
        s->jdx = s->jdy = 0;
        return true;
    }
    return false;
}

size_t Manager::sessions() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->sessions.size();
}

size_t Manager::live_sessions() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    size_t n = 0;
    for (const auto& [dev, s] : d_->sessions) n += s.state == State::Live;
    return n;
}

bool Manager::bonded(uint64_t dev) const {
    std::lock_guard<std::mutex> lk(d_->mu);
    const Session* s = d_->sess(dev);
    return s && s->bonded;
}

}  // namespace nxrt::sw2
