// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Switch 2 controllers over Bluetooth LE: the protocol (proto.h says what and from where).
#include "nxrt/sw2/proto.h"

#include <cctype>
#include <cmath>
#include <cstring>

namespace nxrt::sw2 {
namespace {

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
int16_t s16(const uint8_t* p) { return static_cast<int16_t>(u16(p)); }
// A packed pair of 12-bit values in three bytes: the first in the low 12 bits.
void u12x2(const uint8_t* p, uint16_t* a, uint16_t* b) {
    const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    *a = static_cast<uint16_t>(v & 0xFFF);
    *b = static_cast<uint16_t>(v >> 12);
}
void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace

Kind kind_of_pid(uint16_t pid) {
    switch (pid) {
        case kPidJoyConL: return Kind::JoyConL;
        case kPidJoyConR: return Kind::JoyConR;
        case kPidPro: return Kind::Pro;
        case kPidGameCube: return Kind::GameCube;
        default: return Kind::None;
    }
}

uint16_t pid_of_kind(Kind k) {
    switch (k) {
        case Kind::JoyConL: return kPidJoyConL;
        case Kind::JoyConR: return kPidJoyConR;
        case Kind::Pro: return kPidPro;
        case Kind::GameCube: return kPidGameCube;
        default: return 0;
    }
}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::JoyConL: return "Joy-Con 2 (L)";
        case Kind::JoyConR: return "Joy-Con 2 (R)";
        case Kind::Pro: return "Pro Controller 2";
        case Kind::GameCube: return "GameCube controller (Switch 2)";
        default: return "unknown controller";
    }
}

const char* vibration_uuid(Kind k) {
    switch (k) {
        case Kind::JoyConL: return kUuidVibJoyConL;
        case Kind::JoyConR: return kUuidVibJoyConR;
        case Kind::Pro: return kUuidVibPro;
        case Kind::GameCube: return kUuidVibGameCube;
        default: return nullptr;
    }
}

bool uuid_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

bool is_report_uuid(const char* uuid) {
    return uuid_equal(uuid, kUuidReportJoyConL) || uuid_equal(uuid, kUuidReportJoyConR) || uuid_equal(uuid, kUuidReportPro) ||
           uuid_equal(uuid, kUuidReportGameCube);
}

bool parse_power(const uint8_t* d, size_t n, PowerInfo* out) {
    if (out) *out = PowerInfo{};
    if (!d || !out || n < 2) return false;
    const int level = (d[1] >> 2) & 0x0F;
    if (level > 9) return false;
    out->external = (d[1] & 0x01) != 0;
    out->charging = (d[1] & 0x02) != 0;
    out->level = level;
    return true;
}

bool is_vibration_uuid(const char* uuid) {
    return uuid_equal(uuid, kUuidVibJoyConL) || uuid_equal(uuid, kUuidVibJoyConR) || uuid_equal(uuid, kUuidVibPro) || uuid_equal(uuid, kUuidVibGameCube);
}

// ---- advertisements ----------------------------------------------------------------------------------------------------------------

bool parse_advert(const uint8_t* mfr, size_t n, Advert* out) {
    if (out) *out = Advert{};
    if (!mfr || !out || n < kAdvertMin || u16(mfr) != kCompanyId) return false;
    out->vid = u16(mfr + 5);
    out->pid = u16(mfr + 7);
    out->wake = mfr[0x0B] == 0x81;
    out->pairing = true;
    for (int i = 0; i < 6; ++i) {
        out->host[i] = mfr[0x0C + i];
        if (out->host[i]) out->pairing = false;
    }
    return out->vid == kVendorId && kind_of_pid(out->pid) != Kind::None;
}

AdvertUse advert_use(const Advert& a, const uint8_t* host) {
    if (a.pairing) return host ? AdvertUse::Pair : AdvertUse::Take;
    return host && std::memcmp(a.host, host, 6) == 0 ? AdvertUse::Wake : AdvertUse::Ignore;
}

const char* advert_use_name(AdvertUse u) {
    switch (u) {
        case AdvertUse::Wake: return "wake";
        case AdvertUse::Pair: return "pair";
        case AdvertUse::Take: return "take";
        default: return "ignore";
    }
}

bool parse_address(const char* text, uint8_t le[6]) {
    if (!text) return false;
    uint8_t be[6];
    const char* p = text;
    for (int i = 0; i < 6; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k, ++p) {
            const int c = std::tolower(static_cast<unsigned char>(*p));
            if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
            else return false;
        }
        be[i] = static_cast<uint8_t>(v);
        if (i < 5) {
            if (*p != ':' && *p != '-') return false;
            ++p;
        }
    }
    if (*p) return false;
    for (int i = 0; i < 6; ++i) le[i] = be[5 - i];
    return true;
}

void address_from_u64(uint64_t a, uint8_t le[6]) {
    for (int i = 0; i < 6; ++i) le[i] = static_cast<uint8_t>(a >> (8 * i));
}

// ---- commands ----------------------------------------------------------------------------------------------------------------------

size_t encode_command(uint8_t* out, size_t cap, uint8_t cmd, uint8_t sub, const uint8_t* payload, size_t n) {
    if (!out || n > 0xFF || cap < kCommandHeader + n || (n && !payload)) return 0;
    out[0] = cmd;
    out[1] = 0x91;
    out[2] = 0x01;
    out[3] = sub;
    out[4] = 0;
    out[5] = static_cast<uint8_t>(n);
    out[6] = out[7] = 0;
    if (n) std::memcpy(out + kCommandHeader, payload, n);
    return kCommandHeader + n;
}

bool reply_is(const uint8_t* d, size_t n, uint8_t cmd) { return d && n >= kCommandHeader && d[0] == cmd && (d[1] == 1 || d[1] == 2); }

size_t encode_memory_read(uint8_t* out, size_t cap, uint8_t len, uint32_t addr) {
    uint8_t p[8] = {len, 0x7E, 0, 0};
    put_u32(p + 4, addr);
    return encode_command(out, cap, kCmdMemory, kSubMemoryRead, p, sizeof p);
}

const uint8_t* memory_reply_data(const uint8_t* d, size_t n, uint8_t len, uint32_t addr) {
    if (!reply_is(d, n, kCmdMemory) || n < kMemoryReplyData + len) return nullptr;
    if (d[8] != len || u32(d + 12) != addr) return nullptr;
    return d + kMemoryReplyData;
}

bool parse_info(const uint8_t* d, size_t n, DeviceInfo* out) {
    if (out) *out = DeviceInfo{};
    if (!d || !out || n < 22) return false;
    out->vid = u16(d + 18);
    out->pid = u16(d + 20);
    for (int i = 0; i < 16; ++i) {
        const uint8_t c = d[2 + i];
        if (c < 0x20 || c > 0x7E) break;
        out->serial[i] = static_cast<char>(c);
    }
    return true;
}

void pair_host_payload(uint8_t out[kPairHostLen], const uint8_t host_le[6]) {
    out[0] = 0x00;
    out[1] = 0x02;
    std::memcpy(out + 2, host_le, 6);
    std::memcpy(out + 8, host_le, 6);
}

void pair_key_payload(uint8_t out[kPairKeyLen], const uint8_t key[16]) {
    out[0] = 0x00;
    std::memcpy(out + 1, key, 16);
}

uint8_t led_pattern(int player) {
    static constexpr uint8_t kPatterns[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};
    return kPatterns[player >= 0 && player < 8 ? player : 0];
}

void led_payload(uint8_t out[kLedPayloadLen], uint8_t pattern) {
    out[0] = pattern;
    out[1] = out[2] = out[3] = 0;
}

uint8_t feature_mask(Kind k, bool want_mouse) {
    switch (k) {
        case Kind::Pro: return 0x2F;
        case Kind::JoyConL:
        case Kind::JoyConR: return static_cast<uint8_t>(0x27 | (want_mouse ? kFeatureMouse : 0));
        default: return 0x27;
    }
}

// ---- input report ------------------------------------------------------------------------------------------------------------------

bool parse_report(const uint8_t* d, size_t n, Report* out) {
    if (out) *out = Report{};
    if (!d || !out || n < kReportMin || n > kReportMax) return false;
    out->counter = u32(d);
    out->buttons = u32(d + 4);
    u12x2(d + 0x0A, &out->stick[0], &out->stick[1]);
    u12x2(d + 0x0D, &out->stick[2], &out->stick[3]);
    out->mouse_x = s16(d + 0x10);
    out->mouse_y = s16(d + 0x12);
    out->mouse_q = u16(d + 0x14);
    out->mouse_dist = u16(d + 0x16);
    out->battery_mv = u16(d + 0x1F);
    out->charge = d[0x21];
    for (int i = 0; i < 3; ++i) {
        out->accel[i] = s16(d + 0x30 + 2 * i);
        out->gyro[i] = s16(d + 0x36 + 2 * i);
    }
    if (n >= 0x3E) {
        out->trigger[0] = d[0x3C];
        out->trigger[1] = d[0x3D];
    }
    return true;
}

// ---- calibration -------------------------------------------------------------------------------------------------------------------

bool parse_stick_cal(const uint8_t* d, size_t n, StickCal* out) {
    if (!d || !out || n < 9) return false;
    if (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF) return false;   // never written
    uint16_t a, b;
    StickCal c;
    u12x2(d, &a, &b);
    c.cx = a;
    c.cy = b;
    u12x2(d + 3, &a, &b);
    c.up_x = a;
    c.up_y = b;
    u12x2(d + 6, &a, &b);
    c.down_x = a;
    c.down_y = b;
    // a centre near the middle of the range and some travel each way; anything else is not a calibration
    const bool centre_ok = c.cx > 1000 && c.cx < 3100 && c.cy > 1000 && c.cy < 3100;
    const bool travel_ok = c.up_x > 100 && c.up_y > 100 && c.down_x > 100 && c.down_y > 100;
    if (!centre_ok || !travel_ok) return false;
    c.valid = true;
    *out = c;
    return true;
}

bool parse_user_stick_cal(const uint8_t* d, size_t n, StickCal* out) {
    if (!d || n < 11 || d[0] != 0xB2 || d[1] != 0xA1) return false;
    return parse_stick_cal(d + 2, n - 2, out);
}

namespace {
float cal_axis(float v, float centre, float up, float down) {
    const float s = v - centre;
    float r = s >= 0 ? (up > 0 ? s / up : 0.f) : (down > 0 ? s / down : 0.f);
    if (r > 1.f) r = 1.f;
    if (r < -1.f) r = -1.f;
    return r;
}
}  // namespace

void apply_stick_cal(const StickCal& c, uint16_t raw_x, uint16_t raw_y, float* x, float* y) {
    *x = cal_axis(static_cast<float>(raw_x), c.cx, c.up_x, c.down_x);
    *y = cal_axis(static_cast<float>(raw_y), c.cy, c.up_y, c.down_y);
}

float trigger_value(uint8_t raw, uint8_t zero) {
    constexpr float kFull = 232.f;
    if (zero >= kFull) zero = 0;
    float v = (static_cast<float>(raw) - zero) / (kFull - zero);
    return v < 0.f ? 0.f : v > 1.f ? 1.f : v;
}

// ---- motion ------------------------------------------------------------------------------------------------------------------------

const int8_t* motion_map(Hold h) {
    // measured on a right Joy-Con 2 held upright: raw x right, y forward, z up, the gyro right-handed about the same axes; the accelerometer
    // reads the specific force (+z face up), the console reports gravity's direction (-z), so the pair's map negates it
    static constexpr int8_t kUpright[6] = {-1, -2, -3, 4, 5, 6};
    static constexpr int8_t kLeftSideways[6] = {-2, 1, -3, 5, -4, 6};
    static constexpr int8_t kRightSideways[6] = {2, -1, -3, -5, 4, 6};
    switch (h) {
        case Hold::Upright: return kUpright;
        case Hold::LeftSideways: return kLeftSideways;
        case Hold::RightSideways:
        case Hold::Pro:
        default: return kRightSideways;
    }
}

void motion_to_console(const int16_t accel[3], const int16_t gyro[3], const int8_t map[6], const float* bias, float acc_g[3], float gyro_rad[3]) {
    constexpr float kRad = kGyroDps * 3.14159265358979323846f / 180.f;
    for (int i = 0; i < 6; ++i) {
        int ch = map[i] < 0 ? -map[i] : map[i];
        float sign = map[i] < 0 ? -1.f : 1.f;
        if (ch < 1 || ch > 6) {
            ch = 1;
            sign = 0.f;
        }
        const float raw = ch <= 3 ? static_cast<float>(accel[ch - 1]) : static_cast<float>(gyro[ch - 4]) - (bias ? bias[ch - 4] : 0.f);
        if (i < 3) acc_g[i] = sign * raw * kAccelG;
        else gyro_rad[i - 3] = sign * raw * kRad;
    }
}

void console_to_sdl(const float acc_g[3], const float gyro_rad[3], float accel_ms2[3], float gyro_sdl[3]) {
    constexpr float kG = 9.80665f;
    accel_ms2[0] = -acc_g[0] * kG;
    accel_ms2[1] = -acc_g[2] * kG;
    accel_ms2[2] = acc_g[1] * kG;
    gyro_sdl[0] = gyro_rad[0];
    gyro_sdl[1] = gyro_rad[2];
    gyro_sdl[2] = -gyro_rad[1];
}

void RestBias::update(const int16_t accel[3], const int16_t gyro[3]) {
    constexpr int kSamples = 33;   // about a second of reports
    if (n == 0) {
        for (int i = 0; i < 3; ++i) {
            sum[i] = 0;
            gmin[i] = gmax[i] = gyro[i];
            amin[i] = amax[i] = accel[i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        sum[i] += gyro[i];
        if (gyro[i] < gmin[i]) gmin[i] = gyro[i];
        if (gyro[i] > gmax[i]) gmax[i] = gyro[i];
        if (accel[i] < amin[i]) amin[i] = accel[i];
        if (accel[i] > amax[i]) amax[i] = accel[i];
        if (gmax[i] - gmin[i] > 100 || amax[i] - amin[i] > 300) {   // it moved: start the second again
            n = 0;
            return;
        }
    }
    if (++n < kSamples) return;
    n = 0;
    float m[3];
    for (int i = 0; i < 3; ++i) {
        m[i] = static_cast<float>(sum[i]) / kSamples;
        if (std::fabs(m[i]) * kGyroDps > 4.f) return;   // a slow turn, not rest
    }
    for (int i = 0; i < 3; ++i) bias[i] = have ? 0.5f * bias[i] + 0.5f * m[i] : m[i];
    have = true;
}

bool parse_gyro_factory(const uint8_t* d, size_t n, float offset_counts[3]) {
    if (!d || n < 16) return false;
    float v[4];
    std::memcpy(v, d, sizeof v);
    for (float f : v)
        if (!std::isfinite(f)) return false;
    for (int i = 0; i < 3; ++i) {
        const float dps = v[1 + i] * 57.29577951308232f;
        if (std::fabs(dps) > 5.f) return false;
        offset_counts[i] = dps / 0.070f;
    }
    return true;
}

// ---- rumble ------------------------------------------------------------------------------------------------------------------------

float freq_hz(uint16_t code) { return 110.f * std::exp2((static_cast<float>(code) - 333.f) / 96.f); }

uint16_t freq_code(float hz) {
    if (!(hz >= 9.935f)) hz = 9.935f;   // NaN too
    if (hz > 16034.f) hz = 16034.f;
    // 96 log2(hz / 110) + 333, in single precision as the console's encoder computes it
    int c = static_cast<int>(96.f * std::log2(hz) - 318.0105285644531f);
    if (c >= 0x3FF) return 0x3FF;
    if (c < 0) c = 0;
    return static_cast<uint16_t>(std::fabs(freq_hz(static_cast<uint16_t>(c + 1)) - hz) < std::fabs(freq_hz(static_cast<uint16_t>(c)) - hz) ? c + 1 : c);
}

uint16_t amp_code(float a) {
    if (!(a > 0.f)) a = 0.f;
    if (a > 1.f) a = 1.f;
    const int c = static_cast<int>(a * 1023.f);
    if (c >= 0x3FF) return 0x3FF;
    return static_cast<uint16_t>(std::fabs(static_cast<float>(c + 1) / 1023.f - a) < std::fabs(static_cast<float>(c) / 1023.f - a) ? c + 1 : c);
}

void rumble_cap(float* lo, float* hi) {
    const float sum = *lo + *hi;
    const float k = sum > 1.f ? 0.98f / sum : 0.98f;
    *lo *= k;
    *hi *= k;
}

HdSample hd_sample(float amp_lo, float freq_lo, float amp_hi, float freq_hi) {
    if (!(amp_lo > 0.f)) amp_lo = 0.f;
    if (!(amp_hi > 0.f)) amp_hi = 0.f;
    if (amp_lo > 1.f) amp_lo = 1.f;
    if (amp_hi > 1.f) amp_hi = 1.f;
    rumble_cap(&amp_lo, &amp_hi);
    HdSample s;
    s.lo_freq = freq_code(freq_lo);
    s.lo_amp = amp_code(amp_lo);
    s.hi_freq = freq_code(freq_hi);
    s.hi_amp = amp_code(amp_hi);
    return s;
}

void pack_hd(const HdSample& s, uint8_t out[5]) {
    const uint64_t v = static_cast<uint64_t>(s.lo_freq & 0x3FF) | (static_cast<uint64_t>(s.lo_amp & 0x3FF) << 10) |
                       (static_cast<uint64_t>(s.hi_freq & 0x3FF) << 20) | (static_cast<uint64_t>(s.hi_amp & 0x3FF) << 30);
    for (int i = 0; i < 5; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
}

size_t encode_hd_report(uint8_t* out, size_t cap, const HdSample* first, const HdSample* second, int count, uint8_t seq, bool pad) {
    if (!out || !first || count < 1 || count > 3) return 0;
    const int blocks = second ? 2 : 1;
    const size_t need = 1 + 16 * static_cast<size_t>(blocks);
    const size_t len = pad ? (cap < kVibReportLen ? cap : kVibReportLen) : need;
    if (len < need || cap < len) return 0;
    std::memset(out, 0, len);
    for (int b = 0; b < blocks; ++b) {
        uint8_t* blk = out + 1 + 16 * b;
        blk[0] = static_cast<uint8_t>(0x40 | (count << 4) | (seq & 0x0F));
        const HdSample* s = b ? second : first;
        for (int i = 0; i < count; ++i) pack_hd(s[i], blk + 1 + 5 * i);
    }
    return len;
}

size_t encode_gc_report(uint8_t* out, size_t cap, GcMotor m, uint8_t seq, bool pad) {
    const size_t need = 5;
    const size_t len = pad ? (cap < kVibReportLen ? cap : kVibReportLen) : need;
    if (!out || len < need || cap < len) return 0;
    std::memset(out, 0, len);
    out[1] = static_cast<uint8_t>(0x50 | (seq & 0x0F));
    out[2] = static_cast<uint8_t>(m);
    return len;
}

GcMotor gc_motor_step(float strength, float* err) {
    if (!(strength > 0.f)) {
        *err = 0.f;
        return GcMotor::Off;
    }
    if (strength > 1.f) strength = 1.f;
    *err += strength;
    if (*err >= 1.f) {
        *err -= 1.f;
        return GcMotor::On;
    }
    return GcMotor::Off;
}

}  // namespace nxrt::sw2
