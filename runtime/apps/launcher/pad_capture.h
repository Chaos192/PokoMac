// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// "Find a button" on the Controller tab: which row of the tab a physical controller button belongs to, and a poller that waits for the
// first press on a connected game controller. No window code here; the modal window is in ui.cpp (capture_pad_button).
#pragma once
#include <cstdint>
#include <vector>

struct SDL_Gamepad;

namespace mprl {

// The row index (nxrt::play::PadSrc order, the order of the Controller tab) of an SDL_GamepadButton value, or -1 for a button that has no
// row (guide, misc, paddles, touchpad). Same mapping as nxrt::play's map_pad (src/gfx/play/pad_map.cpp).
int pad_source_for_gamepad_button(int sdl_gamepad_button);

// The row index of the left (true) or right trigger.
int pad_source_for_trigger(bool left);

// Waits for a press on any connected game controller.
class PadCapture {
public:
    PadCapture() = default;
    ~PadCapture();
    PadCapture(const PadCapture&) = delete;
    PadCapture& operator=(const PadCapture&) = delete;

    // Starts SDL's gamepad subsystem and opens the controllers already connected. False when SDL cannot start it.
    bool open();
    // Pumps the SDL event queue (opens controllers that are plugged in, closes the ones that are unplugged) and returns the row index of
    // the first mapped button pressed, or of a trigger pulled past half its travel. -1 when there is none yet.
    int poll();
    bool has_pad() const { return !pads_.empty(); }   // at least one game controller is open
    void close();                                      // closes the controllers and stops the subsystem; open() may be called again

private:
    void add_pad(uint32_t id);
    void remove_pad(uint32_t id);

    bool inited_ = false;
    std::vector<SDL_Gamepad*> pads_;
};

}  // namespace mprl
