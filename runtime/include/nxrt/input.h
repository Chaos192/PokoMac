// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Input event queue of the platform layer (S4b). The SDL3 platform code (platform.h) turns window, keyboard, mouse and
// gamepad events into the small POD `Event` below and pushes them here; later streams (game input mapping, PC features) drain
// the queue. No game mapping lives here. Thread safe: one producer (the thread that pumps the platform) and any consumers.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::input {

enum class EventType : uint8_t {
    None = 0,
    Quit,
    KeyDown, KeyUp,               // key: SDL scancode and keycode; flags bit0 = auto-repeat
    Text,                         // text: UTF-8, NUL terminated
    MouseMotion,                  // motion: dx, dy = raw relative counts since the last event (valid in relative mode), x, y = pointer position in window pixels; flags bit0 = relative mode was on
    MouseButtonDown, MouseButtonUp,   // button: 1 left, 2 middle, 3 right, 4 x1, 5 x2
    MouseWheel,                   // wheel: dx, dy in notches (positive y = away from the user)
    GamepadAdded, GamepadRemoved, // gamepad: id (SDL instance id)
    GamepadButtonDown, GamepadButtonUp,   // gpbutton: id, button (SDL_GamepadButton)
    GamepadAxis,                  // gpaxis: id, axis (SDL_GamepadAxis), value -32768..32767
    WindowResized,                // window: new size in pixels
    WindowFocusGained, WindowFocusLost,
    WindowMinimized, WindowRestored,
    WindowFullscreenChanged,      // window: size in pixels; flags bit0 = now fullscreen
};

// kFlagTouch (mouse motion and buttons): SDL made this mouse event from a finger on a touch screen (its mouse id is SDL_TOUCH_MOUSEID).
enum EventFlags : uint8_t { kFlagRepeat = 1, kFlagRelative = 1, kFlagFullscreen = 1, kFlagTouch = 2 };

struct Event {
    EventType type = EventType::None;
    uint8_t flags = 0;
    uint16_t mods = 0;            // SDL_Keymod bits at the time of the event (keys, buttons)
    uint64_t timestamp_ns = 0;    // SDL_GetTicksNS() of the event (nanoseconds since SDL init); 0 for synthetic events
    union {
        struct { int32_t scancode; int32_t keycode; } key;
        struct { float dx, dy, x, y; } motion;
        struct { uint8_t button; uint8_t clicks; float x, y; } button;
        struct { float dx, dy; } wheel;
        struct { char utf8[16]; } text;
        struct { uint32_t id; } gamepad;
        struct { uint32_t id; uint8_t button; } gpbutton;
        struct { uint32_t id; uint8_t axis; int16_t value; } gpaxis;
        struct { int32_t w, h; } window;
    } u = {};
};

// ---- the queue --------------------------------------------------------------------------------------------------------
// Bounded ring (kQueueCapacity events). When full the oldest event is dropped and queue_dropped() counts it.
constexpr size_t kQueueCapacity = 4096;
void queue_push(const Event& e);                 // also usable for synthetic events (tests, scripted runs)
size_t queue_drain(Event* out, size_t max);      // removes up to max events, oldest first; returns how many
bool queue_pop(Event* out);                      // removes the oldest event
size_t queue_size();
uint64_t queue_dropped();                        // since the last queue_clear
uint64_t queue_pushed();                         // total pushed since the last queue_clear
void queue_clear();

// ---- raw mouse accumulator --------------------------------------------------------------------------------------------
// Relative motion (SDL relative mouse mode, raw and unaccelerated) is also summed here so that aim code can read it as late as
// possible, once per rendered frame, without draining the queue: mouse deltas go straight into the aim code.
struct MouseDelta { double dx = 0, dy = 0; uint64_t events = 0; };
void mouse_delta_add(double dx, double dy);      // called by the platform for every relative motion event
MouseDelta mouse_delta_take();                   // returns the sum since the last take and resets it
MouseDelta mouse_delta_peek();

// ---- gamepads ---------------------------------------------------------------------------------------------------------
// The platform opens a gamepad when it is plugged in (GamepadAdded) and closes it when it is unplugged (GamepadRemoved).
// Controller family, as SDL reports it for the open pad: the button-prompt feature picks its icons by this.
enum class PadKind : uint8_t { Other = 0, Xbox, PlayStation, Switch, Steam };

struct GamepadInfo {
    uint32_t id = 0;                // SDL instance id, as in the events
    char name[64] = {};
    bool rumble = false;
    bool gyro = false;
    PadKind kind = PadKind::Other;
};
size_t gamepad_count();
size_t gamepad_list(GamepadInfo* out, size_t max);   // returns the number written
PadKind gamepad_kind_of(uint32_t id);                // Other when the id is not open
void gamepad_set_info(const GamepadInfo& info);      // platform use: add or replace an entry
void gamepad_remove_info(uint32_t id);               // platform use

}  // namespace nxrt::input
