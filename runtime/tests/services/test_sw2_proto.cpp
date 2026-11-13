// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE: the protocol (nxrt/sw2/proto.h) on published and hand-built bytes. The advertisements are the
// ones ndeadly/switch2_controller_research (bluetooth_interface.md) recorded from a Pro Controller 2 and a Switch 2; the command frames are
// the console's (09 91 01 07 player LEDs, 0C 91 01 02 / 04 with the console's feature masks); the rumble codes follow the console's encoder
// as nxnative 8c436e33 measured it (code 333 = 110 Hz, 96 an octave).
#include <cmath>
#include <cstring>

#include "nxrt/sw2/proto.h"
#include "svc_test.h"

using namespace nxrt::sw2;

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// A report 0x05 of 63 bytes with the given fields.
void make_report(uint8_t r[63], uint32_t counter, uint32_t buttons, uint16_t lx, uint16_t ly, uint16_t rx, uint16_t ry) {
    std::memset(r, 0, 63);
    for (int i = 0; i < 4; ++i) {
        r[i] = static_cast<uint8_t>(counter >> (8 * i));
        r[4 + i] = static_cast<uint8_t>(buttons >> (8 * i));
    }
    const uint32_t a = lx | (static_cast<uint32_t>(ly) << 12), b = rx | (static_cast<uint32_t>(ry) << 12);
    for (int i = 0; i < 3; ++i) {
        r[0x0A + i] = static_cast<uint8_t>(a >> (8 * i));
        r[0x0D + i] = static_cast<uint8_t>(b >> (8 * i));
    }
}

}  // namespace

SVC_TEST(sw2, advert) {
    // the Pro Controller 2's standard advertisement (Sync held: no host)
    const uint8_t standard[] = {0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    Advert a;
    CHECK(parse_advert(standard, sizeof standard, &a));
    CHECK_EQ(a.vid, 0x057E);
    CHECK_EQ(a.pid, kPidPro);
    CHECK(a.pairing);
    CHECK(!a.wake);
    uint8_t host[6];
    CHECK(parse_address("02:A1:B2:C3:D4:E5", host));
    CHECK(advert_use(a, host) == AdvertUse::Pair);
    CHECK(advert_use(a, nullptr) == AdvertUse::Take);
    // reconnection toward host 02:A1:B2:C3:D4:E5 (written backwards at 0x0C), and the wake advertisement (0x81 at 0x0B)
    uint8_t rec[sizeof standard];
    std::memcpy(rec, standard, sizeof rec);
    const uint8_t back[6] = {0xE5, 0xD4, 0xC3, 0xB2, 0xA1, 0x02};
    std::memcpy(rec + 0x0C, back, 6);
    CHECK(parse_advert(rec, sizeof rec, &a));
    CHECK(!a.pairing);
    CHECK(!a.wake);
    CHECK(std::memcmp(a.host, host, 6) == 0);
    CHECK(advert_use(a, host) == AdvertUse::Wake);
    CHECK(advert_use(a, nullptr) == AdvertUse::Ignore);
    uint8_t other[6] = {1, 2, 3, 4, 5, 6};
    CHECK(advert_use(a, other) == AdvertUse::Ignore);   // a controller of another host (a Switch 2 nearby) is never touched
    rec[0x0B] = 0x81;
    CHECK(parse_advert(rec, sizeof rec, &a));
    CHECK(a.wake);
    // the kinds by product id; a console's own advertisement and short data are not controllers
    rec[7] = 0x67;
    CHECK(parse_advert(rec, sizeof rec, &a) && kind_of_pid(a.pid) == Kind::JoyConL);
    rec[7] = 0x66;
    CHECK(parse_advert(rec, sizeof rec, &a) && kind_of_pid(a.pid) == Kind::JoyConR);
    rec[7] = 0x73;
    CHECK(parse_advert(rec, sizeof rec, &a) && kind_of_pid(a.pid) == Kind::GameCube);
    rec[7] = 0x60;
    CHECK(!parse_advert(rec, sizeof rec, &a));
    const uint8_t console[] = {0x53, 0x05, 0x03, 0x00, 0x01, 0x00, 0x80, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0x01};
    CHECK(!parse_advert(console, sizeof console, &a));
    CHECK(!parse_advert(standard, 17, &a));
    uint8_t wrong_company[sizeof standard];
    std::memcpy(wrong_company, standard, sizeof standard);
    wrong_company[0] = 0x4C;
    CHECK(!parse_advert(wrong_company, sizeof wrong_company, &a));
}

SVC_TEST(sw2, address) {
    uint8_t le[6], le2[6];
    CHECK(parse_address("02-a1-b2-c3-d4-e5", le));
    address_from_u64(0x02A1B2C3D4E5ull, le2);
    CHECK(std::memcmp(le, le2, 6) == 0);
    CHECK_EQ(le[0], 0xE5);
    CHECK_EQ(le[5], 0x02);
    CHECK(!parse_address("02:A1:B2:C3:D4", le));
    CHECK(!parse_address("02:A1:B2:C3:D4:E5:00", le));
    CHECK(!parse_address("zz:A1:B2:C3:D4:E5", le));
}

SVC_TEST(sw2, commands) {
    uint8_t f[64];
    uint8_t pl[kLedPayloadLen];
    led_payload(pl, led_pattern(0));
    size_t n = encode_command(f, sizeof f, kCmdLeds, kSubLedsSet, pl, sizeof pl);
    const uint8_t led1[] = {0x09, 0x91, 0x01, 0x07, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    CHECK_EQ(n, sizeof led1);
    CHECK(std::memcmp(f, led1, n) == 0);
    const uint8_t patterns[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};
    for (int p = 0; p < 8; ++p) CHECK_EQ(led_pattern(p), patterns[p]);
    CHECK_EQ(led_pattern(-1), 0x01);
    CHECK_EQ(led_pattern(9), 0x01);
    // the console's feature masks
    CHECK_EQ(feature_mask(Kind::Pro, false), 0x2F);
    CHECK_EQ(feature_mask(Kind::JoyConR, true), 0x37);
    CHECK_EQ(feature_mask(Kind::JoyConL, false), 0x27);
    CHECK_EQ(feature_mask(Kind::GameCube, false), 0x27);
    uint8_t fm[4] = {feature_mask(Kind::JoyConL, true), 0, 0, 0};
    n = encode_command(f, sizeof f, kCmdFeature, kSubFeatureSet, fm, sizeof fm);
    const uint8_t feat[] = {0x0C, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00};
    CHECK(n == sizeof feat && std::memcmp(f, feat, n) == 0);
    // memory read of the info block, and its reply
    n = encode_memory_read(f, sizeof f, 0x40, kAddrInfo);
    const uint8_t rd[] = {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00};
    CHECK(n == sizeof rd && std::memcmp(f, rd, n) == 0);
    uint8_t reply[16 + 0x40] = {0x02, 0x01, 0x01, 0x04, 0x00, 0x48, 0x00, 0x00, 0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00};
    const char serial[] = "HEW12345678901";
    std::memcpy(reply + 16 + 2, serial, 14);
    reply[16 + 18] = 0x7E;
    reply[16 + 19] = 0x05;
    reply[16 + 20] = 0x66;
    reply[16 + 21] = 0x20;
    const uint8_t* data = memory_reply_data(reply, sizeof reply, 0x40, kAddrInfo);
    CHECK(data == reply + 16);
    DeviceInfo info;
    CHECK(parse_info(data, 0x40, &info));
    CHECK_EQ(info.vid, 0x057E);
    CHECK_EQ(info.pid, kPidJoyConR);
    CHECK(std::strcmp(info.serial, serial) == 0);
    CHECK(memory_reply_data(reply, sizeof reply, 0x40, kAddrInfo + 0x40) == nullptr);   // another read's answer
    CHECK(memory_reply_data(reply, sizeof reply, 0x10, kAddrInfo) == nullptr);
    CHECK(memory_reply_data(reply, 40, 0x40, kAddrInfo) == nullptr);                    // short
    CHECK(reply_is(reply, sizeof reply, kCmdMemory));
    reply[1] = 0x03;
    CHECK(!reply_is(reply, sizeof reply, kCmdMemory));
    // bonding payloads
    uint8_t host[6] = {0xE5, 0xD4, 0xC3, 0xB2, 0xA1, 0x02}, hp[kPairHostLen], key[16], kp[kPairKeyLen];
    pair_host_payload(hp, host);
    CHECK(hp[0] == 0x00 && hp[1] == 0x02 && std::memcmp(hp + 2, host, 6) == 0 && std::memcmp(hp + 8, host, 6) == 0);
    for (int i = 0; i < 16; ++i) key[i] = static_cast<uint8_t>(i * 7);
    pair_key_payload(kp, key);
    CHECK(kp[0] == 0 && std::memcmp(kp + 1, key, 16) == 0);
    CHECK_EQ(encode_command(f, 10, kCmdPair, kSubPairHost, hp, sizeof hp), 0u);   // does not fit
}

SVC_TEST(sw2, report) {
    uint8_t r[63];
    make_report(r, 0x01020304, kRawA | kRawZR | kRawHome | kRawUp | kRawGL, 0x123, 0x456, 0x789, 0xABC);
    const int16_t acc[3] = {100, -200, 4096}, gyr[3] = {-5, 6, 1000};
    for (int i = 0; i < 3; ++i) {
        r[0x30 + 2 * i] = static_cast<uint8_t>(acc[i]);
        r[0x31 + 2 * i] = static_cast<uint8_t>(static_cast<uint16_t>(acc[i]) >> 8);
        r[0x36 + 2 * i] = static_cast<uint8_t>(gyr[i]);
        r[0x37 + 2 * i] = static_cast<uint8_t>(static_cast<uint16_t>(gyr[i]) >> 8);
    }
    r[0x1F] = 0x10;
    r[0x20] = 0x0E;   // 3600 mV
    r[0x3C] = 30;
    r[0x3D] = 200;
    r[0x10] = 0xFE;
    r[0x11] = 0xFF;   // mouse x -2
    r[0x16] = 150;    // on a surface
    Report rep;
    CHECK(parse_report(r, sizeof r, &rep));
    CHECK_EQ(rep.counter, 0x01020304u);
    CHECK_EQ(rep.buttons, kRawA | kRawZR | kRawHome | kRawUp | kRawGL);
    CHECK_EQ(rep.stick[0], 0x123);
    CHECK_EQ(rep.stick[1], 0x456);
    CHECK_EQ(rep.stick[2], 0x789);
    CHECK_EQ(rep.stick[3], 0xABC);
    CHECK_EQ(rep.accel[2], 4096);
    CHECK_EQ(rep.gyro[0], -5);
    CHECK_EQ(rep.gyro[2], 1000);
    CHECK_EQ(rep.battery_mv, 3600);
    CHECK_EQ(rep.trigger[0], 30);
    CHECK_EQ(rep.trigger[1], 200);
    CHECK_EQ(rep.mouse_x, -2);
    CHECK(mouse_present(rep) && mouse_on_surface(rep));
    CHECK_EQ(mouse_delta(static_cast<int16_t>(-32767), static_cast<int16_t>(32767)), 2);   // the counters wrap
    CHECK_EQ(mouse_delta(5, 10), -5);
    CHECK(!parse_report(r, 0x3B, &rep));
    CHECK(!parse_report(r, 0x51, &rep));
}

SVC_TEST(sw2, stick_calibration) {
    // centre (2048, 2000), up (1300, 1250), down (1200, 1100): three packed 12-bit pairs
    auto pack = [](uint8_t* p, uint16_t a, uint16_t b) {
        const uint32_t v = a | (static_cast<uint32_t>(b) << 12);
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
    };
    uint8_t blk[11] = {0xB2, 0xA1};
    pack(blk + 2, 2048, 2000);
    pack(blk + 5, 1300, 1250);
    pack(blk + 8, 1200, 1100);
    StickCal c;
    CHECK(parse_user_stick_cal(blk, sizeof blk, &c));
    CHECK(c.valid);
    CHECK(near(c.cx, 2048) && near(c.cy, 2000) && near(c.up_x, 1300) && near(c.down_y, 1100));
    float x, y;
    apply_stick_cal(c, 2048 + 1300, 2000, &x, &y);
    CHECK(near(x, 1.f) && near(y, 0.f));
    apply_stick_cal(c, 2048 - 600, 2000 - 1100, &x, &y);
    CHECK(near(x, -0.5f) && near(y, -1.f));
    apply_stick_cal(c, 4095, 0, &x, &y);   // past the travel: clamped
    CHECK(near(x, 1.f) && near(y, -1.f));
    StickCal f;
    CHECK(parse_stick_cal(blk + 2, 9, &f));
    blk[0] = 0xFF;
    CHECK(!parse_user_stick_cal(blk, sizeof blk, &c));   // no user calibration: the factory's is read
    const uint8_t blank[9] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!parse_stick_cal(blank, 9, &f));
    StickCal def;
    apply_stick_cal(def, 2048, 2048, &x, &y);
    CHECK(near(x, 0.f) && near(y, 0.f));
    CHECK(near(trigger_value(232, 30), 1.f));
    CHECK(near(trigger_value(30, 30), 0.f));
    CHECK(near(trigger_value(10, 30), 0.f));
    CHECK(near(trigger_value(131, 30), 0.5f));
}

SVC_TEST(sw2, motion) {
    // a Joy-Con of a pair lying still, face up: the accelerometer reads +1 G on its z; the console reports (0, 0, -1), SDL (0, +9.8, 0)
    const int16_t acc[3] = {0, 0, 4096}, gyr[3] = {0, 0, 1000};
    float a[3], g[3], sa[3], sg[3];
    motion_to_console(acc, gyr, motion_map(Hold::Upright), nullptr, a, g);
    CHECK(near(a[0], 0) && near(a[1], 0) && near(a[2], -1.f, 2e-3f));
    const float rad = 1000.f * 2000.f / 32767.f * 3.14159265f / 180.f;
    CHECK(near(g[2], rad, 1e-4f));
    console_to_sdl(a, g, sa, sg);
    CHECK(near(sa[1], 9.80665f, 0.02f) && near(sa[0], 0) && near(sa[2], 0));
    // SDL -> console as the play layer maps it (pad_map.cpp map_sensors) gives the console vector back
    const float back_a[3] = {-sa[0] / 9.80665f, sa[2] / 9.80665f, -sa[1] / 9.80665f}, back_g[3] = {sg[0], -sg[2], sg[1]};
    const float va[3] = {0.3f, -0.6f, 0.75f}, vg[3] = {1.f, -2.f, 3.f};
    float wa[3], wg[3];
    console_to_sdl(va, vg, wa, wg);
    const float ra[3] = {-wa[0] / 9.80665f, wa[2] / 9.80665f, -wa[1] / 9.80665f}, rg[3] = {wg[0], -wg[2], wg[1]};
    for (int i = 0; i < 3; ++i) {
        CHECK(near(back_a[i], a[i]) && near(back_g[i], g[i]));
        CHECK(near(ra[i], va[i]) && near(rg[i], vg[i]));
    }
    // a bias in counts comes off the gyro; sideways maps turn the axes a quarter
    const float bias[3] = {0, 0, 1000};
    motion_to_console(acc, gyr, motion_map(Hold::Upright), bias, a, g);
    CHECK(near(g[2], 0.f));
    const int16_t acc_x[3] = {4096, 0, 0}, zero[3] = {0, 0, 0};
    motion_to_console(acc_x, zero, motion_map(Hold::RightSideways), nullptr, a, g);
    CHECK(near(a[1], -1.f, 2e-3f) && near(a[0], 0));
    motion_to_console(acc_x, zero, motion_map(Hold::LeftSideways), nullptr, a, g);
    CHECK(near(a[1], 1.f, 2e-3f));
    // the rest offset: about a second of still samples, and a move starts it again
    RestBias rb;
    const int16_t still_g[3] = {10, -20, 5}, still_a[3] = {0, 0, 4096};
    for (int i = 0; i < 32; ++i) rb.update(still_a, still_g);
    CHECK(!rb.have);
    rb.update(still_a, still_g);
    CHECK(rb.have && near(rb.bias[0], 10) && near(rb.bias[1], -20) && near(rb.bias[2], 5));
    RestBias moving;
    const int16_t turn[3] = {500, 0, 0};
    for (int i = 0; i < 40; ++i) moving.update(still_a, i % 2 ? turn : still_g);
    CHECK(!moving.have);
    // the factory offset: a temperature, then x, y, z in rad/s at 0.070 dps a count
    float blkf[4] = {25.f, 0.001f, -0.002f, 0.0005f}, off[3];
    uint8_t raw[16];
    std::memcpy(raw, blkf, 16);
    CHECK(parse_gyro_factory(raw, 16, off));
    CHECK(near(off[0], 0.001f * 57.29578f / 0.070f));
    blkf[2] = 0.2f;   // 11 degrees per second: not an offset
    std::memcpy(raw, blkf, 16);
    CHECK(!parse_gyro_factory(raw, 16, off));
    blkf[2] = std::nanf("");
    std::memcpy(raw, blkf, 16);
    CHECK(!parse_gyro_factory(raw, 16, off));
    CHECK(gyro_factory_usable(Kind::JoyConR) && !gyro_factory_usable(Kind::JoyConL) && !gyro_factory_usable(Kind::Pro));
}

SVC_TEST(sw2, rumble_codes) {
    CHECK_EQ(freq_code(110.f), 333);
    CHECK_EQ(freq_code(220.f), 333 + 96);
    CHECK_EQ(freq_code(160.f), 385);   // 159.0 and 160.2 Hz around it: the nearer
    CHECK_EQ(freq_code(320.f), 481);
    CHECK_EQ(freq_code(0.f), 0);
    CHECK_EQ(freq_code(std::nanf("")), 0);
    CHECK_EQ(freq_code(1e6f), 0x3FF);
    CHECK(near(freq_hz(333), 110.f, 1e-3f));
    for (float hz = 20.f; hz < 3000.f; hz *= 1.37f) CHECK(std::fabs(freq_hz(freq_code(hz)) - hz) <= hz * 0.0037f);   // within half a step
    CHECK_EQ(amp_code(1.f), 1023);
    CHECK_EQ(amp_code(2.f), 1023);
    CHECK_EQ(amp_code(0.f), 0);
    CHECK_EQ(amp_code(-1.f), 0);
    CHECK_EQ(amp_code(0.25f), 256);
    float lo = 0.8f, hi = 0.6f;
    rumble_cap(&lo, &hi);
    CHECK(near(lo, 0.56f) && near(hi, 0.42f));
    lo = 0.5f;
    hi = 0.2f;
    rumble_cap(&lo, &hi);
    CHECK(near(lo, 0.49f) && near(hi, 0.196f));
    HdSample s;
    s.lo_freq = 385;
    s.lo_amp = 1023;
    s.hi_freq = 481;
    s.hi_amp = 0;
    uint8_t p[5];
    pack_hd(s, p);
    const uint8_t want[5] = {0x81, 0xFD, 0x1F, 0x1E, 0x00};
    CHECK(std::memcmp(p, want, 5) == 0);
    s.hi_amp = 0x3FF;
    pack_hd(s, p);
    CHECK_EQ(p[4], 0xFF);
    CHECK_EQ(p[3], 0xDE);
    const HdSample v = hd_sample(1.f, 160.f, 1.f, 320.f);   // both full: capped to 0.49 each
    CHECK_EQ(v.lo_freq, 385);
    CHECK_EQ(v.hi_freq, 481);
    CHECK_EQ(v.lo_amp, amp_code(0.49f));
    CHECK(hd_on(v) && !hd_on(HdSample{}));
    // the report: a zero, then a block per actuator with its sample count and sequence
    HdSample three[3] = {v, HdSample{}, v};
    uint8_t out[64];
    size_t n = encode_hd_report(out, sizeof out, three, nullptr, 3, 5, false);
    CHECK_EQ(n, 17u);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[1], 0x75);
    uint8_t first[5];
    pack_hd(v, first);
    CHECK(std::memcmp(out + 2, first, 5) == 0);
    CHECK(out[7] == 0 && out[11] == 0 && std::memcmp(out + 12, first, 5) == 0);
    n = encode_hd_report(out, sizeof out, three, three, 1, 0x1F, true);
    CHECK_EQ(n, kVibReportLen);
    CHECK_EQ(out[1], 0x5F);
    CHECK_EQ(out[17], 0x5F);
    CHECK_EQ(out[41], 0x00);
    CHECK_EQ(encode_hd_report(out, 10, three, nullptr, 3, 0, false), 0u);
    CHECK_EQ(encode_hd_report(out, sizeof out, three, nullptr, 4, 0, false), 0u);
    // the GameCube motor: on / off / brake and pulse density
    n = encode_gc_report(out, sizeof out, GcMotor::On, 3, false);
    const uint8_t gc[5] = {0x00, 0x53, 0x01, 0x00, 0x00};
    CHECK(n == 5 && std::memcmp(out, gc, 5) == 0);
    CHECK_EQ(encode_gc_report(out, sizeof out, GcMotor::Brake, 0, true), kVibReportLen);
    CHECK_EQ(out[2], 2);
    float err = 0.f;
    int on = 0;
    for (int i = 0; i < 100; ++i) on += gc_motor_step(0.3f, &err) == GcMotor::On;
    CHECK(on >= 29 && on <= 31);
    CHECK(gc_motor_step(0.f, &err) == GcMotor::Off && err == 0.f);
    for (int i = 0; i < 5; ++i) CHECK(gc_motor_step(1.f, &err) == GcMotor::On);
}

SVC_TEST(sw2, power_byte) {
    // a controller's own report: counter, then the power byte (bit 0 external power, bit 1 charging, bits 2-5 level 0..9)
    uint8_t r[0x3F] = {0x12, static_cast<uint8_t>(7 << 2 | 0x02)};
    PowerInfo p;
    CHECK(parse_power(r, sizeof r, &p));
    CHECK_EQ(p.level, 7);
    CHECK(p.charging && !p.external);
    r[1] = static_cast<uint8_t>(9 << 2 | 0x01);
    CHECK(parse_power(r, sizeof r, &p) && p.level == 9 && p.external && !p.charging);
    r[1] = static_cast<uint8_t>(12 << 2);   // past 9: not a level
    CHECK(!parse_power(r, sizeof r, &p));
    CHECK_EQ(p.level, -1);
    CHECK(!parse_power(r, 1, &p));
    CHECK(is_report_uuid("8261cba1-9435-420c-84d6-f0c75a2c8e4d") && is_report_uuid(kUuidReportJoyConL) && !is_report_uuid(kUuidInput));
}

SVC_TEST(sw2, uuids) {
    CHECK(uuid_equal(kUuidInput, "ab7de9be-89fe-49ad-828f-118f09df7fd2"));
    CHECK(!uuid_equal(kUuidInput, kUuidCommand));
    CHECK(is_vibration_uuid("fa19b0fb-cd1f-46a7-84a1-bbb09e00c149"));
    CHECK(is_vibration_uuid(kUuidVibGameCube));
    CHECK(!is_vibration_uuid(kUuidReply));
    CHECK(uuid_equal(vibration_uuid(Kind::Pro), kUuidVibPro));
    CHECK(vibration_uuid(Kind::None) == nullptr);
    CHECK(kind_of_pid(pid_of_kind(Kind::GameCube)) == Kind::GameCube);
}
