// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "game_info.h"
#include "keymap_win.h"
#include "pad_capture.h"

namespace mprl {

namespace {

enum Kind { kCheck = 1, kCombo, kSlider, kNumber };

constexpr wchar_t kPageClass[] = L"MprLauncherPage";
constexpr wchar_t kCaptureClass[] = L"MprLauncherCapture";
constexpr int kMargin = 16;       // logical px
constexpr int kLabelW = 200;      // label column of a one-column form
constexpr int kRowGap = 6;

}  // namespace

// ---- Page ------------------------------------------------------------------------------------------------------------

Page::~Page() = default;

void Page::load(const Settings& s) {
    loading_ = true;
    for (Binder& b : binders_) {
        if (b.load) b.load(s);
        if (b.kind == kSlider && b.value_label && b.format) SetWindowTextW(b.value_label, b.format().c_str());
    }
    loading_ = false;
}

void Page::store(Settings* s) {
    for (Binder& b : binders_)
        if (b.store) b.store(*s);
}

// ---- PageWindow: the child window a page lives in ----------------------------------------------------------------------

// The wndproc needs the private members of Page, so it is a static member of the friend class.
class PageWindowProc {
public:
    static Page::Binder* find_binder(std::vector<Page::Binder>& v, int id) {
        for (auto& b : v)
            if (b.id == id) return &b;
        return nullptr;
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Page* page = reinterpret_cast<Page*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!page) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_COMMAND: {
                const int id = LOWORD(wp), code = HIWORD(wp);
                if (page->command(id, code, reinterpret_cast<HWND>(lp))) return 0;
                Page::Binder* b = find_binder(page->binders_, id);
                if (!b || page->loading_) return 0;
                const bool fire = (b->kind == kCheck && code == BN_CLICKED) || (b->kind == kCombo && code == CBN_SELCHANGE) ||
                                  (b->kind == kNumber && code == EN_CHANGE);
                if (fire) {
                    if (b->after) b->after();
                    page->host_->changed();
                }
                return 0;
            }
            case WM_HSCROLL: {
                HWND ctl = reinterpret_cast<HWND>(lp);
                for (auto& b : page->binders_) {
                    if (b.ctl != ctl || b.kind != kSlider) continue;
                    if (b.value_label && b.format) SetWindowTextW(b.value_label, b.format().c_str());
                    if (!page->loading_) {
                        if (b.after) b.after();
                        page->host_->changed();
                    }
                }
                return 0;
            }
            case WM_NOTIFY: {
                bool handled = false;
                const LRESULT r = page->notify(reinterpret_cast<NMHDR*>(lp), &handled);
                return handled ? r : 0;
            }
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wp);
                HWND c = reinterpret_cast<HWND>(lp);
                COLORREF col = GetSysColor(COLOR_WINDOWTEXT);
                if (std::find(page->hints_.begin(), page->hints_.end(), c) != page->hints_.end()) col = kGray;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, col);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            case WM_ERASEBKGND: {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, GetSysColorBrush(COLOR_WINDOW));
                return 1;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

bool PageWindow::register_class(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &PageWindowProc::proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    wc.lpszClassName = kPageClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND PageWindow::create(Page& page, HWND parent, Host& host, int id_base, const RECT& area) {
    const Env& env = host.env();
    page.host_ = &host;
    page.id_base_ = id_base;
    page.next_id_ = 0;
    page.binders_.clear();
    page.hints_.clear();
    page.bolds_.clear();
    page.hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, area.left, area.top,
                                 area.right - area.left, area.bottom - area.top, parent, nullptr, env.inst, nullptr);
    if (!page.hwnd_) return nullptr;
    SetWindowLongPtrW(page.hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&page));
    Form form(page, area.right - area.left);
    page.build(form);
    return page.hwnd_;
}

// ---- Form ----------------------------------------------------------------------------------------------------------------

Form::Form(Page& page, int page_width) : page_(page), env_(page.host_->env()), width_(page_width), y_(env_.S(12)) {}

int Form::label_w() const {
    if (cols_ == 1) return env_.S(kLabelW);
    if (compact_) return cell_w() * 24 / 100;
    return cell_w() * (cols_ == 2 ? 40 : 44) / 100;
}

int Form::cell_w() const {
    if (cols_ == 1) return width_ - 2 * env_.S(kMargin);
    return (width_ - 2 * env_.S(kMargin) - (cols_ - 1) * env_.S(16)) / cols_;
}

int Form::cell_x() const { return env_.S(kMargin) + col_ * (cell_w() + env_.S(16)); }

int Form::control_x() const { return cell_x() + label_w(); }

int Form::control_w() const { return cell_w() - label_w(); }

HWND Form::make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int* id, DWORD ex_style) {
    const int ctl_id = page_.id_base_ + page_.next_id_++;
    HWND h_ = CreateWindowExW(ex_style, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, page_.hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctl_id)), env_.inst, nullptr);
    if (h_) SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(env_.font), TRUE);
    if (id) *id = ctl_id;
    return h_;
}

HWND Form::create(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int* id, DWORD ex_style) {
    return make(cls, text, style, x, y, w, h, id, ex_style);
}

void Form::label(const wchar_t* text, int x, int y, int w) {
    if (!text || !*text) return;
    make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y + env_.S(4), w - env_.S(8), env_.S(18), nullptr);
}

void Form::next_cell(int row_h) {
    row_h_ = std::max(row_h_, row_h);
    if (cols_ == 1) {
        y_ += row_h_ + env_.S(kRowGap);
        row_h_ = 0;
        return;
    }
    if (++col_ >= cols_) {
        col_ = 0;
        y_ += row_h_ + env_.S(kRowGap);
        row_h_ = 0;
    }
}

void Form::columns(int n) {
    if (col_ != 0) {   // finish a half-filled row
        col_ = 0;
        y_ += row_h_ + env_.S(kRowGap);
        row_h_ = 0;
    }
    cols_ = std::max(1, std::min(n, 3));
}

void Form::gap(int px) { y_ += env_.S(px); }

void Form::heading(const wchar_t* text) {
    columns(1);
    if (y_ > env_.S(16)) y_ += env_.S(10);
    HWND h = make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, env_.S(kMargin), y_, width_ - 2 * env_.S(kMargin), env_.S(20), nullptr);
    if (h) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(env_.bold), TRUE);
        page_.bolds_.push_back(h);
    }
    y_ += env_.S(26);
}

HWND Form::hint(const wchar_t* text, int lines, bool full_width) {
    columns(1);
    const int h = env_.S(16) * lines;
    const int x = full_width ? env_.S(kMargin) : control_x();
    const int w_ = full_width ? width_ - 2 * env_.S(kMargin) : control_w();
    HWND w = make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y_ - env_.S(2), w_, h, nullptr);
    if (w) page_.hints_.push_back(w);
    y_ += h + env_.S(4);
    return w;
}

void Form::checkbox(const wchar_t* text, GetBool get, SetBool set, After after) {
    int id = 0;
    const int x = cols_ == 1 ? control_x() : cell_x();
    const int w = cols_ == 1 ? control_w() : cell_w();
    HWND h = make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y_ + env_.S(2), w, env_.S(22), &id);
    Page::Binder b;
    b.ctl = h;
    b.id = id;
    b.kind = kCheck;
    b.load = [h, get](const Settings& s) { SendMessageW(h, BM_SETCHECK, get(s) ? BST_CHECKED : BST_UNCHECKED, 0); };
    b.store = [h, set](Settings& s) { set(s, SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED); };
    b.after = std::move(after);
    page_.binders_.push_back(std::move(b));
    next_cell(env_.S(26));
}

HWND Form::combo(const wchar_t* text, const std::vector<std::wstring>& items, GetInt get, SetInt set, After after) {
    int id = 0;
    label(text, cell_x(), y_, label_w());
    HWND h = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, control_x(), y_, control_w(), env_.S(260), &id);
    for (const std::wstring& it : items) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it.c_str()));
    Page::Binder b;
    b.ctl = h;
    b.id = id;
    b.kind = kCombo;
    b.load = [h, get](const Settings& s) { SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(get(s)), 0); };
    b.store = [h, set](Settings& s) {
        const int i = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
        if (i >= 0) set(s, i);
    };
    b.after = std::move(after);
    page_.binders_.push_back(std::move(b));
    next_cell(env_.S(26));
    return h;
}

HWND Form::slider(const wchar_t* text, int lo, int hi, GetInt get, SetInt set, std::function<std::wstring(int)> format, After after) {
    int id = 0;
    label(text, cell_x(), y_, label_w());
    const int value_w = (compact_ && cols_ > 1) ? env_.S(44) : env_.S(100);
    HWND h = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, control_x(), y_, control_w() - value_w, env_.S(26), &id);
    SendMessageW(h, TBM_SETRANGEMIN, FALSE, lo);
    SendMessageW(h, TBM_SETRANGEMAX, TRUE, hi);
    SendMessageW(h, TBM_SETPAGESIZE, 0, std::max(1, (hi - lo) / 10));
    HWND v = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, control_x() + control_w() - value_w + env_.S(8), y_ + env_.S(4), value_w - env_.S(8), env_.S(18), nullptr);
    Page::Binder b;
    b.ctl = h;
    b.value_label = v;
    b.id = id;
    b.kind = kSlider;
    b.load = [h, get, lo, hi](const Settings& s) { SendMessageW(h, TBM_SETPOS, TRUE, std::clamp(get(s), lo, hi)); };
    b.store = [h, set](Settings& s) { set(s, static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0))); };
    b.format = [h, format]() { return format(static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0))); };
    b.after = std::move(after);
    page_.binders_.push_back(std::move(b));
    next_cell(env_.S(28));
    return h;
}

HWND Form::number(const wchar_t* text, int lo, int hi, GetInt get, SetInt set, After after, int width) {
    int id = 0;
    label(text, cell_x(), y_, label_w());
    HWND h = make(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, control_x(), y_, env_.S(width), env_.S(24), &id, WS_EX_CLIENTEDGE);
    SendMessageW(h, EM_LIMITTEXT, 6, 0);
    Page::Binder b;
    b.ctl = h;
    b.id = id;
    b.kind = kNumber;
    b.load = [h, get](const Settings& s) { SetWindowTextW(h, std::to_wstring(get(s)).c_str()); };
    b.store = [h, set, lo, hi](Settings& s) {
        wchar_t buf[16] = {};
        GetWindowTextW(h, buf, 15);
        set(s, std::clamp(_wtoi(buf), lo, hi));
    };
    b.after = std::move(after);
    page_.binders_.push_back(std::move(b));
    next_cell(env_.S(28));
    return h;
}

int Form::button(const wchar_t* text, int width, bool at_left) {
    int id = 0;
    const int x = (cols_ == 1 && !at_left) ? control_x() : cell_x();
    make(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y_, env_.S(width), env_.S(26), &id);
    next_cell(env_.S(30));
    return id;
}

HWND Form::text_row(const wchar_t* text, const wchar_t* value) {
    label(text, cell_x(), y_, label_w());
    HWND h = make(L"STATIC", value, SS_LEFT | SS_NOPREFIX, control_x(), y_ + env_.S(4), control_w(), env_.S(18), nullptr);
    next_cell(env_.S(24));
    return h;
}

RECT Form::custom_row(const wchar_t* text, int height_px) {
    label(text, cell_x(), y_, label_w());
    RECT r{control_x(), y_, control_x() + control_w(), y_ + env_.S(height_px)};
    next_cell(env_.S(height_px) + env_.S(2));
    return r;
}

// ---- helpers -----------------------------------------------------------------------------------------------------------

bool xinput_pad_connected() {
    static HMODULE lib = LoadLibraryW(L"xinput1_4.dll");
    if (!lib) return false;
    using Fn = DWORD(WINAPI*)(DWORD, void*);
    static Fn get = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputGetState")));
    if (!get) return false;
    unsigned char state[32] = {};   // XINPUT_STATE is 16 bytes; the buffer is generous
    for (DWORD i = 0; i < 4; ++i)
        if (get(i, state) == ERROR_SUCCESS) return true;
    return false;
}

int message(HWND owner, const std::wstring& text, UINT type) {
    return MessageBoxW(owner, text.c_str(), product_name().c_str(), type);
}

std::wstring input_label(const nxrt::play::InputId& id) {
    using nxrt::play::InKind;
    switch (id.kind) {
        case InKind::Mouse: {
            static const wchar_t* const names[] = {L"", L"Left mouse button", L"Middle mouse button", L"Right mouse button", L"Mouse button 4", L"Mouse button 5"};
            return id.code >= 1 && id.code <= 5 ? names[id.code] : L"Mouse button";
        }
        case InKind::Wheel: return id.code > 0 ? L"Mouse wheel up" : L"Mouse wheel down";
        case InKind::Key: {
            const std::string n = nxrt::play::input_name(id);
            if (n.empty()) return L"?";
            const int len = MultiByteToWideChar(CP_UTF8, 0, n.c_str(), static_cast<int>(n.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(std::max(len, 0)), L'\0');
            if (len > 0) MultiByteToWideChar(CP_UTF8, 0, n.c_str(), static_cast<int>(n.size()), w.data(), len);
            return w;
        }
        default: return L"";
    }
}

// ---- capture window ----------------------------------------------------------------------------------------------------

namespace {

struct CaptureState {
    bool done = false;
    bool got = false;
    nxrt::play::InputId id;
};

LRESULT CALLBACK capture_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CaptureState* st = reinterpret_cast<CaptureState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
    using nxrt::play::InKind;
    auto take = [&](InKind kind, int code) {
        st->id = {kind, code};
        st->got = true;
        st->done = true;
    };
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (wp == VK_ESCAPE) {
                st->done = true;
                return 0;
            }
            if (lp & (1 << 30)) return 0;   // auto-repeat
            const int sc = sdl_scancode_from_key_message(wp, lp);
            if (sc != 0) take(InKind::Key, sc);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;   // Alt or F10 must not open the system menu
            break;
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            const int b = sdl_mouse_button_from_message(msg, wp);
            if (b) take(InKind::Mouse, b);
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        }
        case WM_MOUSEWHEEL: {
            const int n = wheel_notch_from_message(wp);
            if (n) take(InKind::Wheel, n);
            return 0;
        }
        case WM_CLOSE:
            st->done = true;
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool capture_input(HWND owner, const Env& env, const std::wstring& what, nxrt::play::InputId* out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &capture_proc;
        wc.hInstance = env.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
        wc.lpszClassName = kCaptureClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    CaptureState st;
    const int w = env.S(400), h = env.S(150);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kCaptureClass, L"Press an input", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner,
                               nullptr, env.inst, nullptr);
    if (!dlg) return false;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    // The client area is a little smaller than the window; the text fills it.
    RECT crc{};
    GetClientRect(dlg, &crc);
    const std::wstring text = L"Press the key or mouse button for\r\n\r\n" + what + L"\r\n\r\nClick inside this window to use a mouse button. Escape cancels.";
    HWND label = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, env.S(12), env.S(10), crc.right - env.S(24), crc.bottom - env.S(20),
                                 dlg, nullptr, env.inst, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(env.font), TRUE);
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(dlg);
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    if (st.got && out) *out = st.id;
    return st.got;
}

// ---- Find a button (Controller tab) -----------------------------------------------------------------------------------------

namespace {

constexpr wchar_t kPadCaptureClass[] = L"MprLauncherPadCapture";
constexpr UINT_PTR kPadPollTimer = 1;
constexpr UINT kPadPollMs = 30;
constexpr wchar_t kPadPressText[] = L"Press the controller button you want to change. Escape cancels.";
constexpr wchar_t kPadNoPadText[] = L"No controller found. Connect one and press a button, or press Escape.";

struct PadCaptureState {
    PadCapture* pad = nullptr;
    HWND text = nullptr;
    bool shown_no_pad = false;   // which of the two texts the window shows
    bool done = false;
    int source = -1;             // the row pressed, or -1 when cancelled
};

LRESULT CALLBACK pad_capture_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PadCaptureState* st = reinterpret_cast<PadCaptureState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_TIMER: {
            if (wp != kPadPollTimer) break;
            const int row = st->pad->poll();
            if (row >= 0) {
                st->source = row;
                st->done = true;
                return 0;
            }
            const bool none = !st->pad->has_pad();
            if (none != st->shown_no_pad) {
                st->shown_no_pad = none;
                SetWindowTextW(st->text, none ? kPadNoPadText : kPadPressText);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE) st->done = true;
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;   // Alt or F10 must not open the system menu
            break;
        case WM_CLOSE:
            st->done = true;
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool capture_pad_button(HWND owner, const Env& env, int* out_source) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &pad_capture_proc;
        wc.hInstance = env.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
        wc.lpszClassName = kPadCaptureClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    PadCapture pad;
    pad.open();   // if this fails the window just reports no controller; Escape still cancels
    PadCaptureState st;
    st.pad = &pad;
    st.shown_no_pad = !pad.has_pad();
    const int w = env.S(400), h = env.S(150);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kPadCaptureClass, L"Find a button", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h,
                               owner, nullptr, env.inst, nullptr);
    if (!dlg) return false;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    // The client area is a little smaller than the window; the text fills it.
    RECT crc{};
    GetClientRect(dlg, &crc);
    st.text = CreateWindowExW(0, L"STATIC", st.shown_no_pad ? kPadNoPadText : kPadPressText, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, env.S(12),
                              env.S(10), crc.right - env.S(24), crc.bottom - env.S(20), dlg, nullptr, env.inst, nullptr);
    SendMessageW(st.text, WM_SETFONT, reinterpret_cast<WPARAM>(env.font), TRUE);
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(dlg);
    SetTimer(dlg, kPadPollTimer, kPadPollMs, nullptr);
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    KillTimer(dlg, kPadPollTimer);
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    pad.close();
    if (st.source < 0) return false;
    if (out_source) *out_source = st.source;
    return true;
}

}  // namespace mprl
