// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode pad mapping: SDL gamepad state to the Switch pad state. Pure functions, see pad_map.h for the rules.
#include "play/pad_map.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play {
namespace {

// SDL_GamepadButton bit numbers (platform.h GamepadRaw::buttons).
enum SdlButton : int {
    kSdlSouth = 0, kSdlEast, kSdlWest, kSdlNorth, kSdlBack, kSdlGuide, kSdlStart, kSdlLeftStick, kSdlRightStick,
    kSdlLeftShoulder, kSdlRightShoulder, kSdlDpadUp, kSdlDpadDown, kSdlDpadLeft, kSdlDpadRight, kSdlMisc1,
    kSdlRightPaddle1, kSdlLeftPaddle1, kSdlRightPaddle2, kSdlLeftPaddle2, kSdlTouchpad, kSdlMisc2,
};
// SDL_GamepadAxis order (platform.h GamepadRaw::axes).
enum SdlAxis : int { kAxisLeftX = 0, kAxisLeftY, kAxisRightX, kAxisRightY, kAxisLeftTrigger, kAxisRightTrigger };

// Face buttons, indexed by the physical position (SOUTH, EAST, WEST, NORTH).
// By printed label: label 0 (A), 1 (B), 2 (X), 3 (Y) is the Switch button of the same name.
constexpr hid::NpadButtonBit kLabelBtn[4] = {hid::kBtnA, hid::kBtnB, hid::kBtnX, hid::kBtnY};
// By position: the bottom button is Switch B, right is A, left is Y, top is X.
constexpr hid::NpadButtonBit kPosBtn[4] = {hid::kBtnB, hid::kBtnA, hid::kBtnY, hid::kBtnX};

struct ButtonMap {
    int sdl;
    PadSrc src;
    hid::NpadButtonBit btn;
};
constexpr ButtonMap kOtherButtons[] = {
    {kSdlLeftShoulder, PadSrc::LeftShoulder, hid::kBtnL},   {kSdlRightShoulder, PadSrc::RightShoulder, hid::kBtnR},
    {kSdlStart, PadSrc::Start, hid::kBtnPlus},              {kSdlBack, PadSrc::Back, hid::kBtnMinus},
    {kSdlLeftStick, PadSrc::LeftStick, hid::kBtnStickL},    {kSdlRightStick, PadSrc::RightStick, hid::kBtnStickR},
    {kSdlDpadUp, PadSrc::DpadUp, hid::kBtnUp},              {kSdlDpadDown, PadSrc::DpadDown, hid::kBtnDown},
    {kSdlDpadLeft, PadSrc::DpadLeft, hid::kBtnLeft},        {kSdlDpadRight, PadSrc::DpadRight, hid::kBtnRight},
};

// Buttons with no built-in meaning: they press nothing until [padmap] gives them a Switch button (pad_default_buttons returns 0).
// SDL names the Steam Controller's inputs as: R4 RIGHT_PADDLE1, L4 LEFT_PADDLE1, R5 RIGHT_PADDLE2, L5 LEFT_PADDLE2, left trackpad click TOUCHPAD,
// right trackpad click MISC2 (SDL 3.4 SDL_gamepad.c, the mapping it gives the Steam Controller).
struct ExtraButton {
    int sdl;
    PadSrc src;
};
constexpr ExtraButton kExtraButtons[] = {
    {kSdlRightPaddle1, PadSrc::RightPaddle1}, {kSdlLeftPaddle1, PadSrc::LeftPaddle1}, {kSdlRightPaddle2, PadSrc::RightPaddle2},
    {kSdlLeftPaddle2, PadSrc::LeftPaddle2},   {kSdlTouchpad, PadSrc::Touchpad},       {kSdlMisc2, PadSrc::TouchpadRight},
};

// SDL stick axis (-32768..32767) to -1..1.
double norm_axis(int16_t v) { return std::clamp(v / 32768.0, -1.0, 1.0); }

// -1..1 to the hid range, rounded and clamped.
int32_t to_stick(double v) {
    const long r = std::lround(v * 32767.0);
    return static_cast<int32_t>(std::clamp(r, -32767L, 32767L));
}

// Radial dead zone and outer zone. x, y are -1..1 (hid y up = +). dz is already clamped to 0..0.9, max_zone to 0.5..1.
// The travel between dz and max_zone is rescaled to 0..1 and anything past max_zone is full deflection, so a stick that only reaches 0.96 of its
// range can still give the game a full push. max_zone 1 is the plain dead zone rescale.
hid::StickValue stick(double x, double y, double dz, double max_zone) {
    const double len = std::sqrt(x * x + y * y);
    if (len > 1.0) {  // square corners: pull back onto the unit circle so the result never exceeds full deflection
        x /= len;
        y /= len;
    }
    const double r = std::min(len, 1.0);
    if (r <= dz) return {};
    const double hi = std::min(1.0, std::max(max_zone, dz + 0.05));   // always past the dead zone
    const double s = std::min(1.0, (r - dz) / (hi - dz)) / r;
    return {to_stick(x * s), to_stick(y * s)};
}

// Rumble amplitude 0..1 times the scale, clamped, to a motor value 0..65535.
uint16_t motor(float amp, double scale) {
    double v = std::clamp(static_cast<double>(amp), 0.0, 1.0) * scale;
    if (!(v > 0.0)) return 0;
    v = std::min(v, 1.0);
    return static_cast<uint16_t>(std::lround(v * 65535.0));
}

}  // namespace

void map_pad(const platform::GamepadRaw& raw, const PadSettings& cfg, hid::NpadInput* out) {
    uint64_t bits = 0;
    // A pressed source adds its remap, or its built-in buttons when it has none (kPadDefault).
    auto press = [&](PadSrc src, uint64_t builtin) {
        const uint64_t remap = cfg.remap[static_cast<int>(src)];
        bits |= remap == kPadDefault ? builtin : remap;
    };

    for (int pos = 0; pos < 4; ++pos) {
        if (!(raw.buttons & (1u << pos))) continue;
        const uint8_t label = raw.face_label[pos];
        // An out-of-range label has no meaning; fall back to the position.
        const bool by_label = cfg.label_abxy && label < 4;
        press(static_cast<PadSrc>(pos), hid::btn(by_label ? kLabelBtn[label] : kPosBtn[pos]));
    }
    for (const ButtonMap& m : kOtherButtons) {
        if (raw.buttons & (1u << m.sdl)) press(m.src, hid::btn(m.btn));
    }
    for (const ExtraButton& e : kExtraButtons) {
        if (raw.buttons & (1u << e.sdl)) press(e.src, 0);   // nothing unless remapped
    }

    if (raw.axes[kAxisLeftTrigger] / 32767.0 >= cfg.trigger_threshold) press(PadSrc::LeftTrigger, hid::btn(hid::kBtnZL));
    if (raw.axes[kAxisRightTrigger] / 32767.0 >= cfg.trigger_threshold) press(PadSrc::RightTrigger, hid::btn(hid::kBtnZR));

    const double dz = std::clamp(cfg.stick_deadzone, 0.0, 0.9), rdz = std::clamp(cfg.right_deadzone, 0.0, 0.9);
    out->buttons = bits;
    const double lmax = std::clamp(cfg.stick_max, 0.5, 1.0);
    out->left = stick(norm_axis(raw.axes[kAxisLeftX]), -norm_axis(raw.axes[kAxisLeftY]), dz, lmax);
    out->right = stick(norm_axis(raw.axes[kAxisRightX]), -norm_axis(raw.axes[kAxisRightY]), rdz, std::clamp(cfg.right_stick_max, 0.5, 1.0));
}

uint32_t pad_src_held(const platform::GamepadRaw& raw, const PadSettings& cfg) {
    uint32_t held = 0;
    for (int pos = 0; pos < 4; ++pos) {   // SOUTH EAST WEST NORTH share their numbers with PadSrc
        if (raw.buttons & (1u << pos)) held |= pad_src_bit(static_cast<PadSrc>(pos));
    }
    for (const ButtonMap& m : kOtherButtons) {
        if (raw.buttons & (1u << m.sdl)) held |= pad_src_bit(m.src);
    }
    for (const ExtraButton& e : kExtraButtons) {
        if (raw.buttons & (1u << e.sdl)) held |= pad_src_bit(e.src);
    }
    if (raw.axes[kAxisLeftTrigger] / 32767.0 >= cfg.trigger_threshold) held |= pad_src_bit(PadSrc::LeftTrigger);
    if (raw.axes[kAxisRightTrigger] / 32767.0 >= cfg.trigger_threshold) held |= pad_src_bit(PadSrc::RightTrigger);
    return held;
}

hid::StickValue adjust_look_stick(hid::StickValue right, const PadSettings& cfg) {
    const double k = std::clamp(cfg.look_sens, 0.25, 3.0);
    double x = right.x / 32767.0 * k, y = right.y / 32767.0 * k * (cfg.invert_y ? -1.0 : 1.0);
    const double len = std::sqrt(x * x + y * y);
    if (len > 1.0) {
        x /= len;
        y /= len;
    }
    return {to_stick(x), to_stick(y)};
}

uint64_t pad_default_buttons(PadSrc src, bool label_abxy) {
    switch (src) {
        case PadSrc::South:
        case PadSrc::East:
        case PadSrc::West:
        case PadSrc::North: {
            const int pos = static_cast<int>(src);
            return hid::btn(label_abxy ? kLabelBtn[pos] : kPosBtn[pos]);
        }
        case PadSrc::LeftTrigger: return hid::btn(hid::kBtnZL);
        case PadSrc::RightTrigger: return hid::btn(hid::kBtnZR);
        default: break;
    }
    for (const ButtonMap& m : kOtherButtons) {
        if (m.src == src) return hid::btn(m.btn);
    }
    return 0;
}

void map_sensors(const float accel_sdl[3], const float gyro_sdl[3], const PadSettings& cfg, hid::SixAxisSample* out) {
    constexpr double kGravity = 9.80665;
    constexpr double kTwoPi = 6.283185307179586;
    constexpr double kSign[3] = {-1.0, 1.0, -1.0};   // SDL x, y, z to hid

    if (!cfg.gyro) {
        *out = hid::SixAxisSample{};   // the rest sample: accel (0, 0, -1), gyro 0, at_rest
        return;
    }
    for (int i = 0; i < 3; ++i) {
        out->accel[i] = static_cast<float>(kSign[i] * accel_sdl[i] / kGravity);
    }
    bool at_rest = true;
    for (int i = 0; i < 3; ++i) {
        out->gyro[i] = static_cast<float>(kSign[i] * gyro_sdl[i] / kTwoPi * cfg.gyro_scale);
        if (std::fabs(out->gyro[i]) >= 0.01f) at_rest = false;
    }
    out->at_rest = at_rest;
}

void map_rumble(float amp_low, float amp_high, const PadSettings& cfg, uint16_t* low, uint16_t* high) {
    if (!cfg.rumble) {
        *low = 0;
        *high = 0;
        return;
    }
    *low = motor(amp_low, cfg.rumble_scale);
    *high = motor(amp_high, cfg.rumble_scale);
}

}  // namespace nxrt::play
