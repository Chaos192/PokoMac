// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Nintendo Switch 2 controllers over Bluetooth LE (Joy-Con 2 left and right, Pro Controller 2, GameCube controller for Nintendo Switch 2):
// the protocol, as pure functions. No platform headers: tests/services/test_sw2_proto.cpp checks it on any machine; manager.h is the
// connect / pair / reconnect state machine built on it, and the CoreBluetooth (platform/mac/core/sw2_corebluetooth.mm) and WinRT
// (sw2_winrt.cpp) transports carry its bytes.
//
// Facts and where they come from (written here from those notes; no code is copied from any of them):
//  - GATT service, characteristics, report 0x05 layout, advertisement layout, the console's feature masks and the GameCube controller's
//    vibration characteristic and trigger bytes: ndeadly/switch2_controller_research (bluetooth_interface.md, hid_reports.md).
//  - Command framing, memory reads, the 0x15 bonding with a host's own keys, player LEDs, HD rumble coding (logarithmic frequency codes,
//    checked there against the console's own encoder), the motion axes and scales measured on Joy-Con 2s, the mouse sensor's fields:
//    seedds/nxnative (MIT) a342d640, 8c436e33, 413e8808 (dm/host/switch2_proto.h, docs/reference/services.md).
//  - Wired layouts of the same controllers (stick calibration blocks, user calibration magic, GameCube trigger zero at 0x13140, the
//    GameCube motor's on / off / brake bytes): SDL 3.4.18 src/joystick/hidapi/SDL_hidapi_switch2.c (zlib).
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::sw2 {

constexpr uint16_t kCompanyId = 0x0553;   // Nintendo's Bluetooth SIG company id, the first two bytes of the manufacturer data
constexpr uint16_t kVendorId = 0x057E;
constexpr uint16_t kPidJoyConR = 0x2066, kPidJoyConL = 0x2067, kPidJoyConPair = 0x2068, kPidPro = 0x2069, kPidGameCube = 0x2073;

// One Bluetooth device.
enum class Kind : uint8_t { None = 0, JoyConL, JoyConR, Pro, GameCube };
Kind kind_of_pid(uint16_t pid);
uint16_t pid_of_kind(Kind k);
const char* kind_name(Kind k);   // "Joy-Con 2 (L)", ...
inline bool is_joycon(Kind k) { return k == Kind::JoyConL || k == Kind::JoyConR; }

// ---- GATT --------------------------------------------------------------------------------------------------------------------------
// Every write is Write Without Response. Report 0x05 (63 bytes) arrives on kUuidInput for every controller; command replies on kUuidReply.
constexpr const char* kUuidService = "AB7DE9BE-89FE-49AD-828F-118F09DF7FD0";
constexpr const char* kUuidInput = "AB7DE9BE-89FE-49AD-828F-118F09DF7FD2";
constexpr const char* kUuidCommand = "649D4AC9-8EB7-4E6C-AF44-1EA54FE5F005";
constexpr const char* kUuidReply = "C765A961-D9D8-4D36-A20A-5315B111836A";
// The vibration output of each controller (output reports 0x01 Joy-Con, 0x02 Pro, 0x03 GameCube); a device has one of them.
constexpr const char* kUuidVibJoyConL = "289326CB-A471-485D-A8F4-240C14F18241";
constexpr const char* kUuidVibJoyConR = "FA19B0FB-CD1F-46A7-84A1-BBB09E00C149";
constexpr const char* kUuidVibPro = "CC483F51-9258-427D-A939-630C31F72B05";
constexpr const char* kUuidVibGameCube = "3F8FB670-AB25-45BF-B540-38C72834D064";
const char* vibration_uuid(Kind k);
// Each controller's own input report (0x07 Joy-Con (L), 0x08 Joy-Con (R), 0x09 Pro, 0x0A GameCube; READ and NOTIFY): read now and then for
// its power byte (parse_power); the input itself comes from report 0x05 on kUuidInput.
constexpr const char* kUuidReportJoyConL = "CC1BBBB5-7354-4D32-A716-A81CB241A32A";
constexpr const char* kUuidReportJoyConR = "D5A9E01E-2FFC-4CCA-B20C-8B67142BF442";
constexpr const char* kUuidReportPro = "7492866C-EC3E-4619-8258-32755FFCC0F8";
constexpr const char* kUuidReportGameCube = "8261CBA1-9435-420C-84D6-F0C75A2C8E4D";
bool is_report_uuid(const char* uuid);
// Whether a characteristic's UUID (any case, with dashes) is one of the four vibration outputs.
bool is_vibration_uuid(const char* uuid);
bool uuid_equal(const char* a, const char* b);   // case-insensitive, both with dashes

// ---- advertisements ----------------------------------------------------------------------------------------------------------------
// Manufacturer data as the OS gives it, the company id first (a transport whose stack strips it puts it back): company id at 0, vendor
// id at 5, product id at 7, 0x81 at 0x0B in a wake advertisement, at 0x0C the host the controller belongs to (six bytes, little endian:
// the address read backwards), all zero while Sync is held (pairing mode).
struct Advert {
    uint16_t vid = 0, pid = 0;
    bool wake = false;      // 0x0B = 0x81: a button press woke it toward its host
    bool pairing = false;   // no host: Sync is held
    uint8_t host[6] = {};
};
constexpr size_t kAdvertMin = 0x12;
bool parse_advert(const uint8_t* mfr, size_t n, Advert* out);   // true for a Switch 2 controller of a known kind
// What the host does with an advertisement. host: this machine's own Bluetooth address, little endian; nullptr when it cannot be known.
//  Pair:   Sync is held and the host is known: bond it to this machine (0x15 commands), then use it.
//  Take:   Sync is held and the host is not known: use it without bonding (it needs Sync again next time).
//  Wake:   it belongs to this machine: connect.
//  Ignore: it belongs to another host (a Switch 2 nearby): never connect, never write to it.
enum class AdvertUse : uint8_t { Ignore, Wake, Pair, Take };
AdvertUse advert_use(const Advert& a, const uint8_t* host);
const char* advert_use_name(AdvertUse u);
// "aa:bb:cc:dd:ee:ff" (or '-' separated) as written on screen to the little-endian bytes the advertisement carries.
bool parse_address(const char* text, uint8_t le[6]);
void address_from_u64(uint64_t a, uint8_t le[6]);   // a Windows BluetoothAddress (the printed order, most significant byte first)

// ---- commands ----------------------------------------------------------------------------------------------------------------------
// Frame: cmd, 0x91, 0x01 (Bluetooth), sub, 0, payload length, 0, 0, payload. A reply starts with cmd, then 0x01 (done) or 0x02 (status).
enum : uint8_t { kCmdMemory = 0x02, kCmdLeds = 0x09, kCmdFeature = 0x0C, kCmdPair = 0x15 };
enum : uint8_t { kSubMemoryRead = 0x04, kSubLedsSet = 0x07, kSubFeatureSet = 0x02, kSubFeatureEnable = 0x04,
                 kSubPairHost = 0x01, kSubPairKey1 = 0x04, kSubPairKey2 = 0x02, kSubPairDone = 0x03 };
constexpr size_t kCommandHeader = 8;
size_t encode_command(uint8_t* out, size_t cap, uint8_t cmd, uint8_t sub, const uint8_t* payload, size_t n);   // 0 when it does not fit
bool reply_is(const uint8_t* d, size_t n, uint8_t cmd);

// Memory read: payload length, 0x7E, 0, 0, address (little endian). The reply is its 8-byte header, the payload echoed, then the data.
constexpr size_t kMemoryReplyData = 16;
size_t encode_memory_read(uint8_t* out, size_t cap, uint8_t len, uint32_t addr);
// The data of a memory-read reply when it answers this read (length and address echoed), else nullptr.
const uint8_t* memory_reply_data(const uint8_t* d, size_t n, uint8_t len, uint32_t addr);

enum : uint32_t {
    kAddrInfo = 0x00013000,          // 0x40: serial number at 2, vendor id at 18, product id at 20
    kAddrGyroFactory = 0x00013040,   // 0x10: four floats, a temperature and the gyro offset about x, y, z
    kAddrStick1Factory = 0x000130A8, // 0x0B: the first stick (a Joy-Con's only stick)
    kAddrStick2Factory = 0x000130E8, // 0x0B: the second stick (Pro Controller 2, GameCube C stick)
    kAddrTriggerZero = 0x00013140,   // 0x02: the GameCube controller's analog triggers at rest, left and right
    kAddrStick1User = 0x001FC040,    // 0x0B: 0xB2 0xA1 when the user calibrated the stick, then its calibration
    kAddrStick2User = 0x001FC080,
};

struct DeviceInfo {
    uint16_t vid = 0, pid = 0;
    char serial[17] = {};
};
bool parse_info(const uint8_t* d, size_t n, DeviceInfo* out);

// Bonding (cmd 0x15), in order: the host's address twice (kSubPairHost: 0x00, 0x02, address, address), a 16-byte key after a 0x00
// (kSubPairKey1), a second one (kSubPairKey2), then kSubPairDone with 0x00. The keys are the host's choice (fresh random keys bond); the
// link itself is not encrypted afterwards. From then on a button press makes the controller advertise toward this host (AdvertUse::Wake).
constexpr size_t kPairHostLen = 14, kPairKeyLen = 17;
void pair_host_payload(uint8_t out[kPairHostLen], const uint8_t host_le[6]);
void pair_key_payload(uint8_t out[kPairKeyLen], const uint8_t key[16]);

// Player LEDs: the console's patterns for players 1..8 (bit 0 the first light). The payload is the pattern and three zeros.
uint8_t led_pattern(int player);   // 0..7; any other value gives player 1's
constexpr size_t kLedPayloadLen = 4;
void led_payload(uint8_t out[kLedPayloadLen], uint8_t pattern);

// Features (cmd 0x0C: set, then enable, with the same mask): buttons 0x01, sticks 0x02, motion 0x04, the Joy-Con 2's mouse sensor 0x10,
// battery current 0x20 (0x08 the Pro Controller 2's own). The masks the console sends: Pro 0x2F, Joy-Con 0x37 (with the mouse), GameCube 0x27.
// The mouse sensor is turned on only for a game that reads it (want_mouse), as the console does.
constexpr uint8_t kFeatureMouse = 0x10;
uint8_t feature_mask(Kind k, bool want_mouse);

// ---- input report 0x05 ---------------------------------------------------------------------------------------------------------------
// Raw button bits (32 bits, little endian at offset 4).
enum : uint32_t {
    kRawY = 1u << 0, kRawX = 1u << 1, kRawB = 1u << 2, kRawA = 1u << 3, kRawSRR = 1u << 4, kRawSLR = 1u << 5, kRawR = 1u << 6, kRawZR = 1u << 7,
    kRawMinus = 1u << 8, kRawPlus = 1u << 9, kRawRStick = 1u << 10, kRawLStick = 1u << 11, kRawHome = 1u << 12, kRawCapture = 1u << 13, kRawC = 1u << 14,
    kRawDown = 1u << 16, kRawUp = 1u << 17, kRawRight = 1u << 18, kRawLeft = 1u << 19, kRawSRL = 1u << 20, kRawSLL = 1u << 21, kRawL = 1u << 22, kRawZL = 1u << 23,
    kRawGR = 1u << 24, kRawGL = 1u << 25,
};
// The GameCube controller reports its buttons in the same bits: A B X Y, R (the trigger's full press) as R, Z as ZR, L as L, its ZL as ZL,
// Start as Plus, Home, Capture, C.

struct Report {
    uint32_t counter = 0;        // 0x00
    uint32_t buttons = 0;        // 0x04
    uint16_t stick[4] = {};      // 0x0A, 0x0D: left x, y, right x, y as packed 12-bit pairs (uncalibrated, y up)
    int16_t mouse_x = 0, mouse_y = 0;   // 0x10, 0x12: the Joy-Con 2 mouse sensor's position counters (they wrap)
    uint16_t mouse_q = 0, mouse_dist = 0;   // 0x14, 0x16: about 1,600 and 150 on a surface, 4,600 and 3,200 lifted
    uint16_t battery_mv = 0;     // 0x1F
    uint8_t charge = 0;          // 0x21
    int16_t accel[3] = {}, gyro[3] = {};   // 0x30, 0x36: raw counts
    uint8_t trigger[2] = {};     // 0x3C, 0x3D: the GameCube controller's analog L and R (uncalibrated)
};
constexpr size_t kReportMin = 0x3C, kReportMax = 0x50;   // the same characteristic carries longer packets of other kinds: the length tells
bool parse_report(const uint8_t* d, size_t n, Report* out);

// The power byte at 0x01 of a controller's own report (0x07..0x0A): bit 0 external power, bit 1 charging, bits 2-5 the controller's own
// battery level 0..9 (ndeadly hid_reports.md, "Power Info"). False for a short report or a level past 9.
struct PowerInfo {
    bool external = false, charging = false;
    int level = -1;
};
bool parse_power(const uint8_t* d, size_t n, PowerInfo* out);

// The mouse sensor: the movement between two counter readings (16-bit wrap), on a surface, present at all (all four fields 0 with it off).
inline int32_t mouse_delta(int16_t now, int16_t before) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint16_t>(now) - static_cast<uint16_t>(before))); }
constexpr uint16_t kMouseSurfaceMax = 1000;
inline bool mouse_on_surface(const Report& r) { return r.mouse_dist < kMouseSurfaceMax; }
inline bool mouse_present(const Report& r) { return r.mouse_x || r.mouse_y || r.mouse_q || r.mouse_dist; }

// ---- stick and trigger calibration ---------------------------------------------------------------------------------------------------
// Nine bytes: the centre, the travel above it and the travel below it, each a packed 12-bit (x, y) pair.
struct StickCal {
    float cx = 2048, cy = 2048;
    float up_x = 1200, up_y = 1200, down_x = 1200, down_y = 1200;
    bool valid = false;   // false: the defaults above
};
bool parse_stick_cal(const uint8_t* d, size_t n, StickCal* out);        // false for a blank (FF FF FF) or implausible block
bool parse_user_stick_cal(const uint8_t* d, size_t n, StickCal* out);   // the 0xB2 0xA1 magic, then the nine bytes
// One stick in -1..1 (x right, y up), clamped.
void apply_stick_cal(const StickCal& c, uint16_t raw_x, uint16_t raw_y, float* x, float* y);
// The GameCube controller's analog trigger: 0 at rest (zero, from kAddrTriggerZero) to 1 at 232, the wired driver's full press.
float trigger_value(uint8_t raw, uint8_t zero);

// ---- motion ------------------------------------------------------------------------------------------------------------------------
// Scales: +-8 G and +-2000 degrees per second over the signed 16-bit range (measured on Joy-Con 2s: 1/4096 G and 0.0607 dps a count).
constexpr float kAccelG = 8.0f / 32767.0f;
constexpr float kGyroDps = 2000.0f / 32767.0f;
// Channel maps from raw counts to the console's frame for how the controller is held: six signed channel numbers (1..6 = raw accel x y z,
// gyro x y z; negative flips): the output acceleration x y z, then the rotation x y z. The console's frame: x right, y away from the
// player, z up out of the face; acceleration reads (0, 0, -1) lying still face up. A Joy-Con of a pair is held upright; one alone sideways
// (the left one turned a quarter to the right, the right one to the left). The Pro Controller 2 and the GameCube controller take the
// Switch Pro Controller's map (not measured on them).
enum class Hold : uint8_t { Upright, LeftSideways, RightSideways, Pro };
const int8_t* motion_map(Hold h);
// Raw counts to the console's frame: accel in G, gyro in radians per second. bias: the gyro's rest offset in counts (nullptr: none).
void motion_to_console(const int16_t accel[3], const int16_t gyro[3], const int8_t map[6], const float* bias, float acc_g[3], float gyro_rad[3]);
// The console's frame to SDL's sensor frame (SDL_sensor.h for a gamepad: +x right, +y out of the face, so lying still it reads +9.8 m/s^2
// on y, +z toward the player; m/s^2 and rad/s), the inverse of the play layer's map_sensors (pad_map.h):
// SDL accel = (-ax, -az, ay) * 9.80665, SDL gyro = (gx, gz, -gy).
void console_to_sdl(const float acc_g[3], const float gyro_rad[3], float accel_ms2[3], float gyro_sdl[3]);

// The gyro's rest offset: about a second of samples in which neither sensor moves is a controller lying or held still; its mean is the
// offset (blended with the one before). An offset past 4 degrees per second is a slow turn, not rest, and is not taken.
struct RestBias {
    int n = 0;
    bool have = false;
    int32_t sum[3] = {};
    int16_t gmin[3] = {}, gmax[3] = {}, amin[3] = {}, amax[3] = {};
    float bias[3] = {};
    void update(const int16_t accel[3], const int16_t gyro[3]);
};
// The factory gyro offset (kAddrGyroFactory): little-endian floats, the temperature then the offset about x, y, z in radians per second, at
// 0.070 degrees per second per count. Measured: a right Joy-Con 2's matches its rest offset; a left one's does not, and the other models'
// are not measured, so only the right Joy-Con's is used (gyro_factory_usable). Returns false for a short block, a value that is not finite
// or an offset past 5 degrees per second.
bool parse_gyro_factory(const uint8_t* d, size_t n, float offset_counts[3]);
inline bool gyro_factory_usable(Kind k) { return k == Kind::JoyConR; }

// ---- rumble --------------------------------------------------------------------------------------------------------------------------
// HD rumble (Joy-Con 2, Pro Controller 2): a sample is two bands, low then high, each a 10-bit frequency code and a 10-bit amplitude,
// 40 bits little endian. The frequency code is logarithmic: 96 steps an octave, code 333 at 110 Hz (9.935 Hz to 16034 Hz); the amplitude
// is linear, 1023 at full. Both round as the console's encoder rounds: truncate, then take the nearer of that code and the next.
struct HdSample {
    uint16_t lo_freq = 0, lo_amp = 0, hi_freq = 0, hi_amp = 0;   // codes
};
uint16_t freq_code(float hz);
float freq_hz(uint16_t code);
uint16_t amp_code(float amp);
// The console's limit on one sample: both bands scaled by 0.98, or so that together they come to 0.98 when they add up to more than 1.
void rumble_cap(float* lo, float* hi);
// A game's VibrationValue (amplitudes 0..1, frequencies in Hz) as a sample, the limit applied.
HdSample hd_sample(float amp_lo, float freq_lo, float amp_hi, float freq_hi);
inline bool hd_on(const HdSample& s) { return s.lo_amp || s.hi_amp; }
inline bool hd_same(const HdSample& a, const HdSample& b) { return a.lo_freq == b.lo_freq && a.lo_amp == b.lo_amp && a.hi_freq == b.hi_freq && a.hi_amp == b.hi_amp; }
void pack_hd(const HdSample& s, uint8_t out[5]);
// The vibration report: 0x00, then a 16-byte block per actuator (a Joy-Con 2 has one, the Pro Controller 2 two: left grip, right grip):
// 0x40 | the number of samples << 4 | a 4-bit sequence, then the samples (5 bytes each, up to three), played in turn 5 ms apart. The console
// writes 42 bytes; the rest is zero. Returns the length written (pad: up to 42, else only the blocks).
constexpr size_t kVibReportLen = 42;
size_t encode_hd_report(uint8_t* out, size_t cap, const HdSample* first, const HdSample* second, int count, uint8_t seq, bool pad);
// The GameCube controller's motor turns only on or off: 0x00, then 0x50 | sequence and the motor's state (1 on, 0 off, 2 brake).
enum class GcMotor : uint8_t { Off = 0, On = 1, Brake = 2 };
size_t encode_gc_report(uint8_t* out, size_t cap, GcMotor m, uint8_t seq, bool pad);
// Strength 0..1 as on / off over time (pulse density, the wired driver's way): err carries the remainder from one step to the next.
GcMotor gc_motor_step(float strength, float* err);

}  // namespace nxrt::sw2
