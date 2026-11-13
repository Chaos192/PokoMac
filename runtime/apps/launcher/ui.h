// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Launcher UI toolkit: plain Win32 with Common Controls v6, the same toolkit and look as the setup tool (system message font,
// white window colour, grey hint text, 16 px margins). A launcher page is a child window of the tab control. A page builds its
// controls with a Form (rows of label + control) and the Form remembers how each control reads from and writes to a Settings
// copy, so a page rarely needs more than a few lines per setting.
#pragma once
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <string>
#include <vector>

#include "play/play_types.h"

namespace mprl {

using nxrt::play::Settings;

constexpr COLORREF kGray = RGB(0x5F, 0x5F, 0x5F);
constexpr COLORREF kGreen = RGB(0x0F, 0x6B, 0x2F);
constexpr COLORREF kRed = RGB(0xB3, 0x1B, 0x1B);
constexpr COLORREF kAmber = RGB(0x8A, 0x5A, 0x00);

// Fonts and scale of the window the pages live in.
struct Env {
    HINSTANCE inst = nullptr;
    int dpi = 96;
    HFONT font = nullptr;   // regular
    HFONT bold = nullptr;   // semi-bold
    int S(int px) const { return MulDiv(px, dpi, 96); }
};

// What a page may ask of the window that holds it.
class Host {
public:
    virtual ~Host() = default;
    virtual void changed() = 0;                          // the player changed a control: unsaved changes
    virtual HWND window() const = 0;                     // the launcher window (owner of dialogs)
    virtual const Env& env() const = 0;
    virtual bool controller_connected() const = 0;       // an XInput pad answers
    virtual bool shader_prepare_offered() const = 0;     // the game folder has a shader list (programs.bin)
    virtual bool shader_prepare_busy() const = 0;        // setup or shader preparation is running
    virtual void prepare_shaders_now() = 0;              // run the full shader preparation now (Extras button)
};

class Form;

class Page {
public:
    virtual ~Page();
    virtual const wchar_t* title() const = 0;
    virtual void build(Form& f) = 0;                     // create the controls of the page
    virtual void load(const Settings& s);                // controls <- settings (default: every control the Form registered)
    virtual void store(Settings* s);                     // controls -> settings
    // Messages of the page's controls, before the Form's own handling. Return true when handled.
    virtual bool command(int id, int code, HWND ctl) { return false; }
    virtual LRESULT notify(NMHDR* hdr, bool* handled) { *handled = false; return 0; }
    virtual void rebuilt() {}                            // after build() ran again (DPI change)
    virtual void state_changed() {}                      // the game folder or a running job changed: update the controls that depend on it

    HWND hwnd() const { return hwnd_; }
    Host* host() const { return host_; }

private:
    friend class Form;
    friend class PageWindow;
    friend class PageWindowProc;
    struct Binder {
        HWND ctl = nullptr;
        HWND value_label = nullptr;                      // slider: the text that shows the value
        int kind = 0;                                    // see ui.cpp
        int id = 0;
        std::function<void(const Settings&)> load;
        std::function<void(Settings&)> store;
        std::function<std::wstring()> format;            // slider: text of the current position
        std::function<void()> after;                     // called after the player changed it (before changed())
    };
    HWND hwnd_ = nullptr;
    Host* host_ = nullptr;
    std::vector<Binder> binders_;
    std::vector<HWND> hints_;                            // grey static texts
    std::vector<HWND> bolds_;                            // headings
    int next_id_ = 0;
    int id_base_ = 0;
    bool loading_ = false;                               // load() is running: control changes are not the player's
};

// Row builder. All sizes are logical pixels (96 DPI) and scaled by the Env.
class Form {
public:
    Form(Page& page, int page_width);

    void heading(const wchar_t* text);                   // bold title with some space above
    HWND hint(const wchar_t* text, int lines = 1, bool full_width = false);   // grey text under the previous row (control column, or the page width)
    void gap(int px = 8);

    // One column (default) or two: rows then fill left to right. Switching back to 1 starts a new row.
    void columns(int n);
    // Narrow labels and slider values in multi-column rows (for pages that put several sliders side by side); off by default.
    void compact(bool on) { compact_ = on; }

    using GetBool = std::function<bool(const Settings&)>;
    using SetBool = std::function<void(Settings&, bool)>;
    using GetInt = std::function<int(const Settings&)>;
    using SetInt = std::function<void(Settings&, int)>;
    using After = std::function<void()>;

    void checkbox(const wchar_t* text, GetBool get, SetBool set, After after = nullptr);
    // A drop-down list. get() returns the index of the item to show (-1: none).
    HWND combo(const wchar_t* label, const std::vector<std::wstring>& items, GetInt get, SetInt set, After after = nullptr);
    // A trackbar from lo to hi (integers); format() turns the position into the text shown next to it.
    HWND slider(const wchar_t* label, int lo, int hi, GetInt get, SetInt set, std::function<std::wstring(int)> format, After after = nullptr);
    // A single-line number box.
    HWND number(const wchar_t* label, int lo, int hi, GetInt get, SetInt set, After after = nullptr, int width = 80);
    // A push button in the control column (or the full width of a column) that calls command() with its id. Returns the id.
    int button(const wchar_t* label, int width = 150, bool at_left = false);
    // A static text row: label on the left, text on the right. The returned window can be changed with SetWindowTextW.
    HWND text_row(const wchar_t* label, const wchar_t* text);
    // Free space for a custom control: label (may be null) and a rectangle (control column x, current y, width, height) in pixels.
    RECT custom_row(const wchar_t* label, int height_px);
    // Creates any child window of the page with the page's font and the next free id (set *id when not null).
    HWND create(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int* id = nullptr, DWORD ex_style = 0);

    int y() const { return y_; }                         // pixels
    int width() const { return width_; }                 // pixels
    int control_x() const;                               // pixels
    int control_w() const;

private:
    HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int* id, DWORD ex_style = 0);
    void label(const wchar_t* text, int x, int y, int w);
    void next_cell(int row_h);                           // moves the cursor after a control of this height
    int cell_x() const;
    int cell_w() const;
    int label_w() const;

    Page& page_;
    const Env& env_;
    int width_;       // page width in pixels
    int y_;           // pixels
    int cols_ = 1;
    bool compact_ = false;
    int col_ = 0;
    int row_h_ = 0;   // tallest control of the current row (pixels)
};

// Looks at the system for a pad (XInput); cheap, no state.
bool xinput_pad_connected();

// A modal "press a key or click a mouse button" window. Returns true with *out set, false when the player cancelled (Escape or
// closed it). Keys become keyboard inputs, the mouse buttons Mouse1..Mouse5, the wheel WheelUp/WheelDown. `what` names the thing
// being bound ("Jump").
bool capture_input(HWND owner, const Env& env, const std::wstring& what, nxrt::play::InputId* out);

// A modal "press the controller button you want to change" window for the Controller tab. Polls the game controllers until one button
// (or a trigger pulled past half) is pressed, then returns true with *out_source set to that row (pad_capture.h). Returns false when
// the player cancelled (Escape or closed it). Without a controller it says so and keeps waiting for one.
bool capture_pad_button(HWND owner, const Env& env, int* out_source);

// Message-box helpers with the launcher's title.
int message(HWND owner, const std::wstring& text, UINT type = MB_OK | MB_ICONINFORMATION);
std::wstring input_label(const nxrt::play::InputId& id);   // the player-facing name of an input ("Left Ctrl", "Left mouse button", "Wheel up")

// Registers the window class pages live in, and makes a page window (child of `parent`), builds and loads it. Pages are owned by the caller.
class PageWindow {
public:
    static bool register_class(HINSTANCE inst);
    static HWND create(Page& page, HWND parent, Host& host, int id_base, const RECT& area);
};

}  // namespace mprl
