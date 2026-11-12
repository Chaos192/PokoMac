// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay on the Vulkan path (S4b): the settings / developer menu of the PC port. Hidden by default; a key
// (F1, [gfx] overlay_key = any SDL scancode name) toggles it. ImGui is fed from the nxrt input queue (input.h), not from an
// ImGui platform backend, so it works the same with a window and headless. While it is visible and wants the mouse or the
// keyboard, handle_event() returns true and the game must not see the event. Showing the overlay releases the relative mouse
// mode and hiding it restores it.
// Changes made in the menu that touch the swapchain or the window (present mode, fullscreen) are applied by apply_pending(),
// which the frame loop calls before Renderer::begin_frame().
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"
#include "nxrt/input.h"

namespace nxrt::gfx {

// Background shader work (S24). The threads that compile pipeline parts publish how many jobs are waiting or running; while there are some
// the overlay shows a small note ("Preparing shaders ..."). Any thread. [nvn] compile_note = false keeps the note off.
void set_shader_backlog(int pending);
int shader_backlog();

class Overlay {
public:
    struct Context {
        Device* device = nullptr;
        Renderer* renderer = nullptr;
        FrameLimiter* limiter = nullptr;     // optional: shows and sets the frame cap
        bool has_window = true;              // false: no window controls (headless)
    };
    static std::unique_ptr<Overlay> create(const Context& ctx, std::string* error = nullptr);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Feed every input event here (before the game sees it). Returns true when the overlay consumed it.
    bool handle_event(const input::Event& e);

    void set_visible(bool v);
    // The cheats overlay (phase 6): a separate layer from the F1 dev menu, with its own key ([cheats] key, default F3) and controller combo.
    // It shares this ImGui context (input routing, the renderer hook, the DPI scale) but its own visibility, so it never clashes with F1.
    void toggle_cheats();
    void set_cheats_visible(bool v);
    bool cheats_visible() const;
    int cheats_scancode() const;             // SDL scancode of the cheats toggle key
    void set_cheats_scancode(int sc);
    bool cheats_blocks_game() const;         // cheats visible and not "pass input through": the game gets no keyboard/mouse
    // Touch screens: a "Menu" button in the bottom left corner of the frame (a handheld has no F1 key). A finger on it opens the menu; while the
    // menu is open it reads "Close" and closes it. The menu also gets finger-sized controls while this is on. Any thread. Off by default.
    void set_touch_button(bool on);
    bool visible() const;
    void toggle();
    int toggle_scancode() const;             // SDL scancode of the toggle key
    void set_toggle_scancode(int sc);
    // The on-screen keyboard (the software keyboard applet's question, services_api.h swkbdui): drawn over the game whenever a question is
    // open, menu or not (the menu is not drawn meanwhile). While it is up it takes the keyboard, the mouse and the pad (handle_event returns
    // true for them). Physical keys type into it (needs SDL text input: platform::set_text_input), Enter answers, Escape cancels; the mouse
    // clicks its keys; the pad walks the key grid (d-pad or left stick, with repeat): A types the key, B deletes, Y space, X shift, L/R
    // move the caret, + answers, - cancels. Nothing here depends on the menu being visible.
    bool keyboard_open() const;
    bool blocks_game() const;                // the menu is visible, the on-screen keyboard or the quit prompt is open: the game gets no input
    // The "Close the game?" prompt (play mode's quit shortcut and the F1 page's Quit button): drawn over the game, menu or not (the menu and
    // the on-screen keyboard are not drawn meanwhile). It takes the keyboard, the mouse and the pad: Enter / Space or pad A quits, Escape or
    // pad B cancels, the mouse clicks its buttons. open_quit_prompt: any thread. Quitting only records the answer: the owner of the process
    // polls take_quit_confirmed() on its main thread and ends the process there (the prompt stays up, holding the game back, until it does).
    void open_quit_prompt();
    bool quit_prompt_open() const;
    bool take_quit_confirmed();              // true once after Quit was chosen
    // The prompt's Settings choice (pad X, or its button): the prompt closes and the owner opens the F1 page on its main thread, so a
    // handheld reaches the menu with the quit combination and no keyboard. True once after it was chosen.
    bool take_settings_wanted();
    void set_keyboard_abxy(bool by_label);   // how the face buttons are named in the keyboard's help line (play settings label_abxy)
    // The merged pad of this poll (Switch buttons after the player's mapping, left stick -32767..32767, up = +), before it is held back
    // from the game. Returns the buttons the game must not see: all while the keyboard or the quit prompt is open, then those still held from
    // the press that closed it until they are released. Call it on every pad poll. The quit prompt reads its A / B presses here.
    uint64_t set_keyboard_pad(uint64_t buttons, int lx, int ly);
    bool wants_input() const;                // visible and ImGui wants the mouse or keyboard
    bool wants_mouse() const;                // visible and the pointer is over the menu or a widget is being dragged
    bool wants_keyboard() const;             // visible and a text field or navigation has the keyboard
    // (both reflect the last drawn frame: ImGui decides hover while it builds the frame)

    // Extra panels for later streams (settings, dev menu): fn runs inside an ImGui window titled `title` while visible.
    void add_panel(const std::string& title, std::function<void()> fn);

    // Applies menu choices that cannot be made in the middle of a frame. Call before Renderer::begin_frame().
    void apply_pending();
    // The same in two halves for a multi-threaded caller: the present mode must change on the thread that presents (before its begin_frame), the
    // window (fullscreen) on the thread that owns the SDL window.
    void apply_present_pending();
    void apply_window_pending();

    // Corner frame-rate counter, off by default. Safe to call from any thread; it takes effect on the next presented frame.
    struct CounterConfig {
        bool show = false;
        int corner = 0;                  // 0 top left, 1 top right, 2 bottom left, 3 bottom right
        int detail = 1;                  // 0 frame rate; 1 also the frame time; 2 also the 1 % low and the worst frame
        bool in_screenshots = false;     // leave it in screenshots too (by default it is not drawn into them)
    };
    void set_counter(const CounterConfig& c);

    // A drawn arrow at a window pixel (test aid, S28: a screenshot of the game's own frame does not contain the OS cursor). Any thread.
    void set_pointer(bool show, float x, float y, bool in_screenshots);

    // The Renderer overlay hook: builds and draws the menu on top of the frame (no-op while hidden, unless the counter is on).
    void draw(const FrameInfo& f);
    void install_on_renderer();              // renderer->set_overlay_hook(draw)

    uint64_t frames_drawn() const;

private:
    Overlay() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace nxrt::gfx
