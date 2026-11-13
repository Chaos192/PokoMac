// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pad_capture.h"

#include <SDL3/SDL.h>

#include <algorithm>

#include "play/play_types.h"

namespace mprl {
namespace {

using nxrt::play::PadSrc;

// SDL reports a trigger from 0 (released) to 32767 (fully pulled). Past half is above this.
constexpr int kTriggerHalf = 32767 / 2;

int src(PadSrc s) { return static_cast<int>(s); }

}  // namespace

int pad_source_for_gamepad_button(int b) {
    // Face buttons by position (the bottom button is South); the rest as pad_map.cpp's kOtherButtons and the face loop read them.
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return src(PadSrc::South);
        case SDL_GAMEPAD_BUTTON_EAST: return src(PadSrc::East);
        case SDL_GAMEPAD_BUTTON_WEST: return src(PadSrc::West);
        case SDL_GAMEPAD_BUTTON_NORTH: return src(PadSrc::North);
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return src(PadSrc::LeftShoulder);
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return src(PadSrc::RightShoulder);
        case SDL_GAMEPAD_BUTTON_START: return src(PadSrc::Start);
        case SDL_GAMEPAD_BUTTON_BACK: return src(PadSrc::Back);
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return src(PadSrc::LeftStick);
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return src(PadSrc::RightStick);
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return src(PadSrc::DpadUp);
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return src(PadSrc::DpadDown);
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return src(PadSrc::DpadLeft);
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return src(PadSrc::DpadRight);
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return src(PadSrc::RightPaddle1);
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return src(PadSrc::LeftPaddle1);
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return src(PadSrc::RightPaddle2);
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return src(PadSrc::LeftPaddle2);
        case SDL_GAMEPAD_BUTTON_TOUCHPAD: return src(PadSrc::Touchpad);
        case SDL_GAMEPAD_BUTTON_MISC2: return src(PadSrc::TouchpadRight);
        default: return -1;
    }
}

int pad_source_for_trigger(bool left) { return src(left ? PadSrc::LeftTrigger : PadSrc::RightTrigger); }

PadCapture::~PadCapture() { close(); }

bool PadCapture::open() {
    if (inited_) return true;
    // A pad must keep reporting while the launcher window is in the background (the pad is used while the player looks at another window).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    inited_ = true;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) add_pad(ids[i]);
        SDL_free(ids);
    }
    return true;
}

int PadCapture::poll() {
    if (!inited_) return -1;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
                add_pad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                remove_pad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                const int row = pad_source_for_gamepad_button(ev.gbutton.button);
                if (row >= 0) return row;
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                    if (ev.gaxis.value > kTriggerHalf) return pad_source_for_trigger(ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
                }
                break;
            default:
                break;
        }
    }
    return -1;
}

void PadCapture::close() {
    for (SDL_Gamepad* p : pads_) SDL_CloseGamepad(p);
    pads_.clear();
    if (inited_) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        inited_ = false;
    }
}

void PadCapture::add_pad(uint32_t id) {
    if (SDL_GetGamepadFromID(id)) return;   // already open (a duplicate notice)
    SDL_Gamepad* p = SDL_OpenGamepad(id);
    if (p) pads_.push_back(p);
}

void PadCapture::remove_pad(uint32_t id) {
    auto it = std::find_if(pads_.begin(), pads_.end(), [id](SDL_Gamepad* p) { return SDL_GetGamepadID(p) == id; });
    if (it == pads_.end()) return;
    SDL_CloseGamepad(*it);
    pads_.erase(it);
}

}  // namespace mprl
