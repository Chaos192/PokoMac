// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The launcher: a small window that opens before the game. It edits controls.ini in the game folder (display, picture, mouse,
// keyboard, controller, button prompts, frame rate counter), shows whether the game files are in place, starts the setup tool
// for the first-time setup, and starts mpr.exe.
//   mpr_launcher.exe [--game <folder>] [--page <n>] [--auto-play] [--auto-prepare] [--no-prepare] [--game-args "<text>"] [--setup-args "<text>"]
// --game uses that game folder for this run (the folder saved in mpr_launcher.ini is left alone). --page opens that tab.
// --auto-play saves nothing, opens no window and starts the game at once (for scripted runs). --game-args adds text to
// the command line of mpr.exe; --setup-args does the same for the setup tool (scripted runs).
// --auto-prepare runs the shader preparation without asking: at startup when a window is open, and before the game when
// --auto-play is given (then it waits for it). --no-prepare never asks and never starts the preparation on its own; the
// Extras tab button still works.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game_info.h"
#include "pages.h"
#include "ui.h"

namespace mprl {
namespace {

using nxrt::play::default_settings;
using nxrt::play::load_settings;
using nxrt::play::save_settings;

constexpr wchar_t kClassName[] = L"MprLauncherWindow";
#ifndef MPR_VERSION
#define MPR_VERSION "dev"
#endif
constexpr int kLogicalW = 680;
constexpr int kLogicalH = 650;
constexpr UINT_PTR kTimerSetup = 1;
constexpr UINT kMsgPrepareUpdate = WM_APP + 1;     // PrepareRun: the progress or the result changed (posted from its worker thread)
constexpr UINT kMsgTestDpi = WM_APP + 40;            // test aid (launcher_e2e.py): wParam = a DPI; runs the WM_DPICHANGED path with a rectangle scaled to it
constexpr UINT kMsgPrepareFinished = WM_APP + 2;   // PrepareRun: the child has exited and its output is read

enum ControlId : int {
    IDC_TITLE = 2000, IDC_STATUS, IDC_FOLDER_LABEL, IDC_FOLDER, IDC_CHANGE, IDC_SETUP, IDC_TABS, IDC_RESET, IDC_NOTE, IDC_SAVE, IDC_PLAY,
    IDC_PREP_TEXT, IDC_PREP_BAR, IDC_PREP_CANCEL, IDC_VERSION,
};

// "v0.1.0" for a release build, "dev build" for a local one.
std::wstring version_text() {
    const char* v = MPR_VERSION;
    std::wstring w(v, v + std::strlen(v));
    return (v[0] >= '0' && v[0] <= '9') ? L"v" + w : L"dev build";
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Who started a shader preparation: decides what is said when it ends.
enum class PrepOrigin { automatic, play, extras };

std::wstring with_commas(unsigned long long v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

// Scripted runs (--auto-play): runs the shader preparation and waits for it. False, with *err, when it did not finish.
bool prepare_blocking(const std::wstring& dir, std::wstring* err) {
    PrepareRun run;
    if (!run.start(dir, true, nullptr, 0, 0, err)) return false;
    run.finish();   // waits until the child has exited and its output has ended
    const PrepareLine r = run.result();
    if (r.kind == PrepareKind::ready || r.kind == PrepareKind::done) return true;
    *err = r.kind == PrepareKind::error ? widen(r.text) : std::wstring(L"Shader preparation did not finish.");
    return false;
}

struct CmdLine {
    std::wstring game;
    std::wstring game_args;
    std::wstring setup_args;
    int page = 0;
    bool auto_play = false;
    bool auto_prepare = false;
    bool no_prepare = false;
};

CmdLine parse_cmdline() {
    CmdLine c;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return c;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--game" && more) c.game = argv[++i];
        else if (a == L"--game-args" && more) c.game_args = argv[++i];
        else if (a == L"--setup-args" && more) c.setup_args = argv[++i];
        else if (a == L"--page" && more) c.page = _wtoi(argv[++i]);
        else if (a == L"--auto-play") c.auto_play = true;
        else if (a == L"--auto-prepare") c.auto_prepare = true;
        else if (a == L"--no-prepare") c.no_prepare = true;
    }
    LocalFree(argv);
    return c;
}

class App : public Host {
public:
    explicit App(HINSTANCE inst, const CmdLine& cmd) : cmd_(cmd) { env_.inst = inst; }
    ~App() override { free_fonts(); }

    int run();

    // Host
    void changed() override {
        if (!dirty_) {
            dirty_ = true;
            refresh_buttons();
        }
    }
    HWND window() const override { return hwnd_; }
    const Env& env() const override { return env_; }
    bool controller_connected() const override { return xinput_pad_connected(); }
    bool shader_prepare_offered() const override { return shader_prepare_available(game_dir_); }
    bool shader_prepare_busy() const override { return setup_running_ || prep_.active(); }
    void prepare_shaders_now() override;

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp);
    void on_create();
    void on_command(int id, int code);
    void on_timer(UINT_PTR id);

    void make_fonts();
    void free_fonts();
    void apply_fonts();
    void build_pages();
    void destroy_pages();
    void layout();
    void change_dpi(int dpi, const RECT& suggested);
    void show_page(int i);

    void load_game_dir(const std::wstring& dir);
    bool store_and_save(bool quiet);
    void refresh_status();
    void refresh_buttons();
    void choose_folder();
    void run_setup();
    void play();
    void launch_game();
    void reset_defaults();
    bool confirm_close();
    void start_prepare(bool force, bool then_play, PrepOrigin origin);
    void on_prepare_update();
    void on_prepare_finished();
    void cancel_prepare();

    CmdLine cmd_;
    Env env_;
    HWND hwnd_ = nullptr;
    HWND version_ = nullptr, title_ = nullptr, status_ = nullptr, folder_label_ = nullptr, folder_ = nullptr, change_ = nullptr, setup_btn_ = nullptr;
    HWND tabs_ = nullptr, reset_ = nullptr, note_ = nullptr, save_ = nullptr, play_ = nullptr;
    HWND prep_text_ = nullptr, prep_bar_ = nullptr, prep_cancel_ = nullptr;   // shader preparation row, shown while it runs
    HFONT title_font_ = nullptr;
    std::vector<std::unique_ptr<Page>> pages_;
    int current_page_ = 0;

    std::wstring game_dir_;
    bool game_dir_pinned_ = false;     // --game: do not write the folder to mpr_launcher.ini
    GameStatus game_;
    Settings cur_;                     // the settings the pages edit (whole file, including what no page shows)
    bool settings_broken_ = false;     // controls.ini exists but could not be read
    std::wstring settings_error_;
    bool dirty_ = false;
    bool setup_running_ = false;
    HANDLE setup_proc_ = nullptr;
    bool setup_first_time_ = false;    // the folder was not ready when the running setup started
    PrepareRun prep_;
    bool prep_showing_ = false;        // the progress row is up (layout reserves its space)
    bool prep_then_play_ = false;      // start the game when this run ends well
    bool prep_declined_ = false;       // "No" to the Play question: not asked again in this session
    PrepOrigin prep_origin_ = PrepOrigin::automatic;
    COLORREF status_color_ = kGray;
};

void App::make_fonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, env_.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    env_.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    env_.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 14 / 10;
    title_font_ = CreateFontIndirectW(&lf);
}

void App::free_fonts() {
    if (env_.font) DeleteObject(env_.font);
    if (env_.bold) DeleteObject(env_.bold);
    if (title_font_) DeleteObject(title_font_);
    env_.font = env_.bold = title_font_ = nullptr;
}

void App::apply_fonts() {
    for (HWND h : {version_, folder_label_, folder_, change_, setup_btn_, tabs_, reset_, note_, save_, status_, prep_text_, prep_cancel_})
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(env_.font), TRUE);
    SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    SendMessageW(play_, WM_SETFONT, reinterpret_cast<WPARAM>(env_.bold), TRUE);
}

void App::on_create() {
    env_.dpi = static_cast<int>(GetDpiForWindow(hwnd_));
    make_fonts();
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), env_.inst, nullptr);
    };
    title_ = mk(L"STATIC", product_name().c_str(), SS_LEFT | SS_NOPREFIX, IDC_TITLE);
    version_ = mk(L"STATIC", version_text().c_str(), SS_RIGHT | SS_NOPREFIX, IDC_VERSION);
    status_ = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_STATUS);
    folder_label_ = mk(L"STATIC", L"Game folder", SS_LEFT | SS_NOPREFIX, IDC_FOLDER_LABEL);
    folder_ = mk(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, IDC_FOLDER, WS_EX_CLIENTEDGE);
    change_ = mk(L"BUTTON", L"Change...", BS_PUSHBUTTON | WS_TABSTOP, IDC_CHANGE);
    setup_btn_ = mk(L"BUTTON", L"Set up game files...", BS_PUSHBUTTON | WS_TABSTOP, IDC_SETUP);
    tabs_ = mk(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, IDC_TABS);
    reset_ = mk(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESET);
    note_ = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_NOTE);
    save_ = mk(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, IDC_SAVE);
    play_ = mk(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, IDC_PLAY);
    prep_text_ = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_PREP_TEXT);
    prep_bar_ = mk(L"msctls_progress32", L"", 0, IDC_PREP_BAR);
    prep_cancel_ = mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDC_PREP_CANCEL);
    for (HWND h : {prep_text_, prep_bar_, prep_cancel_}) ShowWindow(h, SW_HIDE);   // shown while shaders are prepared
    apply_fonts();

    pages_.push_back(make_display_page());
    pages_.push_back(make_extras_page());
    pages_.push_back(make_controls_page());
    pages_.push_back(make_keys_page());
    pages_.push_back(make_pad_page());
    pages_.push_back(make_tweaks_page());
    for (size_t i = 0; i < pages_.size(); ++i) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(pages_[i]->title());
        TabCtrl_InsertItem(tabs_, static_cast<int>(i), &it);
    }
}

void App::layout() {
    if (!tabs_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int S_16 = env_.S(16);
    const int w = rc.right - rc.left;
    auto put = [](HWND h, int x, int y, int cx, int cy) { MoveWindow(h, x, y, cx, cy, TRUE); };
    const int vw = env_.S(150);
    put(title_, S_16, env_.S(10), w - 2 * S_16 - vw, env_.S(30));
    put(version_, w - S_16 - vw, env_.S(18), vw, env_.S(20));
    put(status_, S_16, env_.S(44), w - 2 * S_16, env_.S(20));
    const int row = env_.S(72);
    put(folder_label_, S_16, row + env_.S(4), env_.S(84), env_.S(20));
    const int bw_change = env_.S(90), bw_setup = env_.S(160), gap = env_.S(8);
    const int fx = S_16 + env_.S(88);
    const int right = w - S_16;
    put(setup_btn_, right - bw_setup, row, bw_setup, env_.S(26));
    put(change_, right - bw_setup - gap - bw_change, row, bw_change, env_.S(26));
    put(folder_, fx, row, right - bw_setup - bw_change - 2 * gap - fx, env_.S(26));
    const int tab_top = row + env_.S(40);
    const int bottom_h = env_.S(56) + (prep_showing_ ? env_.S(32) : 0);
    put(tabs_, S_16, tab_top, w - 2 * S_16, rc.bottom - tab_top - bottom_h);
    const int by = rc.bottom - env_.S(42);
    put(reset_, S_16, by, env_.S(140), env_.S(28));
    put(play_, right - env_.S(100), by, env_.S(100), env_.S(28));
    put(save_, right - env_.S(100) - gap - env_.S(90), by, env_.S(90), env_.S(28));
    put(note_, S_16 + env_.S(150), by + env_.S(5), right - env_.S(200) - gap - env_.S(90) - (S_16 + env_.S(150)), env_.S(20));
    // shader preparation row, between the tabs and the buttons: status text, progress bar, Cancel
    const int py = rc.bottom - env_.S(80);
    const int text_w = env_.S(300);
    put(prep_text_, S_16, py + env_.S(4), text_w, env_.S(20));
    const int bar_x = S_16 + text_w + gap;
    put(prep_bar_, bar_x, py + env_.S(4), right - env_.S(100) - gap - bar_x, env_.S(18));
    put(prep_cancel_, right - env_.S(100), py, env_.S(100), env_.S(26));
    for (HWND h : {prep_text_, prep_bar_, prep_cancel_}) ShowWindow(h, prep_showing_ ? SW_SHOW : SW_HIDE);

    // pages sit in the display area of the tab control
    RECT tr{};
    GetWindowRect(tabs_, &tr);
    TabCtrl_AdjustRect(tabs_, FALSE, &tr);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&tr), 2);
    for (auto& p : pages_)
        if (p->hwnd()) MoveWindow(p->hwnd(), tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top, TRUE);
}

void App::change_dpi(int dpi, const RECT& r) {
    for (auto& p : pages_)
        if (p->hwnd()) p->store(&cur_);
    destroy_pages();
    free_fonts();
    env_.dpi = dpi;
    make_fonts();
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    apply_fonts();
    layout();
    build_pages();
    layout();
}

void App::build_pages() {
    RECT tr{};
    GetWindowRect(tabs_, &tr);
    TabCtrl_AdjustRect(tabs_, FALSE, &tr);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&tr), 2);
    for (size_t i = 0; i < pages_.size(); ++i) {
        PageWindow::create(*pages_[i], hwnd_, *this, 10000 + static_cast<int>(i) * 1000, tr);
        pages_[i]->load(cur_);
    }
    show_page(current_page_);
}

void App::destroy_pages() {
    for (auto& p : pages_)
        if (p->hwnd()) DestroyWindow(p->hwnd());
}

void App::show_page(int i) {
    current_page_ = std::clamp(i, 0, static_cast<int>(pages_.size()) - 1);
    TabCtrl_SetCurSel(tabs_, current_page_);
    for (size_t k = 0; k < pages_.size(); ++k) {
        HWND h = pages_[k]->hwnd();
        if (!h) continue;
        if (static_cast<int>(k) == current_page_) {
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);   // above the tab control
        } else {
            ShowWindow(h, SW_HIDE);
        }
    }
}

void App::load_game_dir(const std::wstring& dir) {
    game_dir_ = dir;
    SetWindowTextW(folder_, game_dir_.c_str());
    std::string err;
    Settings s;
    const std::string path = narrow(controls_path(game_dir_));
    const bool ok = load_settings(path, &s, &err);
    cur_ = s;
    settings_broken_ = !ok && !err.empty();
    settings_error_ = widen(err);
    dirty_ = false;
    for (auto& p : pages_)
        if (p->hwnd()) p->load(cur_);
    refresh_status();
    refresh_buttons();
}

bool App::store_and_save(bool quiet) {
    for (auto& p : pages_)
        if (p->hwnd()) p->store(&cur_);
    std::error_code ec;
    const std::filesystem::path path(controls_path(game_dir_));
    if (settings_broken_ && std::filesystem::exists(path, ec)) {   // keep the unreadable file once, then replace it
        std::filesystem::path bak = path;
        bak += L".bak";
        std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
    }
    std::string err;
    if (!save_settings(narrow(path.wstring()), cur_, &err)) {
        if (!quiet) message(hwnd_, L"The settings could not be saved:\n\n" + widen(err), MB_OK | MB_ICONWARNING);
        return false;
    }
    settings_broken_ = false;
    dirty_ = false;
    refresh_status();
    refresh_buttons();
    return true;
}

void App::refresh_status() {
    game_ = check_game_dir(game_dir_);
    std::wstring text = game_.summary();
    status_color_ = game_.ready() && game_.setup_finished ? kGreen : (game_.ready() ? kAmber : kRed);
    if (settings_broken_) {
        text = L"controls.ini could not be read, so the defaults are shown. Saving replaces it (a copy is kept as controls.ini.bak).";
        status_color_ = kAmber;
    }
    SetWindowTextW(status_, text.c_str());
    InvalidateRect(status_, nullptr, TRUE);
    SetWindowTextW(setup_btn_, game_.ready() ? L"Run setup again..." : L"Set up game files...");
}

void App::refresh_buttons() {
    const bool busy = setup_running_ || prep_.active();
    EnableWindow(save_, dirty_ && !busy);
    EnableWindow(play_, game_.ready() && !busy);
    EnableWindow(setup_btn_, !busy);
    EnableWindow(change_, !busy);
    EnableWindow(reset_, !setup_running_);
    EnableWindow(prep_cancel_, !prep_.cancel_requested());
    SetWindowTextW(note_, setup_running_ ? L"Setup is running in its own window."
                                         : (prep_.active() ? L"Shader preparation is running." : (dirty_ ? L"Unsaved changes" : L"")));
    for (auto& p : pages_)
        if (p->hwnd()) p->state_changed();
}

void App::choose_folder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose the game folder");
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                if (dirty_) store_and_save(true);   // the changes belong to the folder they were made in
                load_game_dir(path);
                if (!game_dir_pinned_) {
                    LauncherConfig cfg;
                    cfg.game_dir = game_dir_;
                    std::wstring err;
                    save_launcher_config(launcher_config_path(), cfg, &err);
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void App::run_setup() {
    if (setup_running_ || prep_.active()) return;
    if (dirty_) store_and_save(true);
    setup_first_time_ = !game_.ready();
    std::wstring err;
    HANDLE proc = nullptr;
    if (!start_setup(game_dir_, &proc, &err, cmd_.setup_args)) {
        message(hwnd_, err.empty() ? L"The setup tool could not be started." : err, MB_OK | MB_ICONWARNING);
        return;
    }
    setup_proc_ = proc;
    setup_running_ = true;
    SetWindowTextW(status_, L"Setup is running. Finish it in the setup window; this window updates when it closes.");
    status_color_ = kGray;
    InvalidateRect(status_, nullptr, TRUE);
    refresh_buttons();
    SetTimer(hwnd_, kTimerSetup, 500, nullptr);
}

void App::on_timer(UINT_PTR id) {
    if (id != kTimerSetup || !setup_proc_) return;
    if (WaitForSingleObject(setup_proc_, 0) != WAIT_OBJECT_0) return;
    KillTimer(hwnd_, kTimerSetup);
    CloseHandle(setup_proc_);
    setup_proc_ = nullptr;
    setup_running_ = false;
    refresh_status();
    refresh_buttons();
    SetForegroundWindow(hwnd_);
    // first-time setup that finished: prepare the shaders now, so the first play session has no stutter
    if (setup_first_time_ && !cmd_.no_prepare && game_.ready() && game_.setup_finished && shader_prepare_available(game_dir_) &&
        !shader_prepare_done(game_dir_))
        start_prepare(false, false, PrepOrigin::automatic);
}

void App::play() {
    if (!game_.ready()) {
        if (message(hwnd_, L"The game files are not set up yet. Run setup now?", MB_YESNO | MB_ICONQUESTION) == IDYES) run_setup();
        return;
    }
    if (prep_.active()) return;
    if (!cmd_.no_prepare && !prep_declined_ && shader_prepare_available(game_dir_) && !shader_prepare_done(game_dir_)) {
        if (cmd_.auto_prepare) {
            start_prepare(false, true, PrepOrigin::play);
            return;
        }
        const int r = message(hwnd_,
                              L"Prepare shaders now? This takes a few minutes and only has to be done once (again after a graphics driver "
                              L"update). Without it the game stutters while it compiles shaders during the first hours of play.",
                              MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return;
        if (r == IDYES) {
            start_prepare(false, true, PrepOrigin::play);
            return;
        }
        prep_declined_ = true;   // No: start the game without it, and do not ask again in this session
    }
    launch_game();
}

void App::launch_game() {
    if (!store_and_save(false)) return;
    std::wstring err;
    if (!start_game(game_dir_, &err, cmd_.game_args)) {
        message(hwnd_, err.empty() ? L"The game could not be started." : err, MB_OK | MB_ICONWARNING);
        return;
    }
    DestroyWindow(hwnd_);
}

void App::prepare_shaders_now() {
    if (prep_.active() || setup_running_) return;
    if (!shader_prepare_available(game_dir_)) {
        message(hwnd_, L"This game folder has no shader list (shader_cache\\programs.bin), so there is nothing to prepare. Run setup first.",
                MB_OK | MB_ICONINFORMATION);
        return;
    }
    start_prepare(true, false, PrepOrigin::extras);
}

// Starts mpr.exe --prepare-shaders (with --if-needed unless force) and shows the progress row. then_play: start the game when it
// ends well. The result is handled in on_prepare_finished().
void App::start_prepare(bool force, bool then_play, PrepOrigin origin) {
    if (prep_.active() || setup_running_) return;
    std::wstring err;
    if (!prep_.start(game_dir_, !force, hwnd_, kMsgPrepareUpdate, kMsgPrepareFinished, &err)) {
        message(hwnd_, err.empty() ? L"Shader preparation could not be started." : err, MB_OK | MB_ICONWARNING);
        return;
    }
    prep_then_play_ = then_play;
    prep_origin_ = origin;
    prep_showing_ = true;
    SendMessageW(prep_bar_, PBM_SETRANGE32, 0, 1);
    SendMessageW(prep_bar_, PBM_SETPOS, 0, 0);
    SetWindowTextW(prep_text_, L"Preparing shaders...");
    layout();
    refresh_buttons();
}

// The worker has read new progress or a result. Runs on the UI thread; the worker only posted the message.
void App::on_prepare_update() {
    prep_.ack_update();
    const PrepareLine p = prep_.progress();
    if (p.kind != PrepareKind::progress || !prep_showing_) return;
    const int total = static_cast<int>(std::max<unsigned long long>(p.total, 1));
    const int pos = static_cast<int>(std::min(p.done, p.total));
    SendMessageW(prep_bar_, PBM_SETRANGE32, 0, total);
    SendMessageW(prep_bar_, PBM_SETPOS, static_cast<WPARAM>(pos), 0);
    SetWindowTextW(prep_text_, (L"Preparing shaders: " + with_commas(p.done) + L" of " + with_commas(p.total)).c_str());
}

// The child has exited and its output is read: join the worker, then say what happened.
void App::on_prepare_finished() {
    prep_.finish();
    const bool cancelled = prep_.cancel_requested();
    const PrepareLine res = prep_.result();
    const bool then_play = prep_then_play_;
    const PrepOrigin origin = prep_origin_;
    prep_then_play_ = false;
    prep_showing_ = false;
    refresh_status();
    layout();
    refresh_buttons();
    SetForegroundWindow(hwnd_);
    if (cancelled) return;   // the player stopped it; the run starts over the next time

    const bool finished = res.kind == PrepareKind::ready || res.kind == PrepareKind::done;
    const bool shaders_ready = res.kind == PrepareKind::ready || (res.kind == PrepareKind::done && res.failed == 0);
    if (!finished) {
        const std::wstring why = res.kind == PrepareKind::error ? widen(res.text) : std::wstring(L"The run ended without a result.");
        message(hwnd_, L"Shader preparation did not finish.\n\n" + why, MB_OK | MB_ICONWARNING);
        return;
    }
    if (then_play) {
        launch_game();   // done with some programs left out still starts the game: those compile when needed
        return;
    }
    if (origin == PrepOrigin::extras) {
        if (shaders_ready) {
            message(hwnd_, L"Shaders are ready.", MB_OK | MB_ICONINFORMATION);
        } else if (res.failed > 0) {
            message(hwnd_,
                    L"Shader preparation finished, but " + with_commas(static_cast<unsigned long long>(res.failed)) +
                        L" programs could not be prepared. The game compiles those when it needs them.",
                    MB_OK | MB_ICONINFORMATION);
        } else {
            message(hwnd_, L"Shader preparation finished.", MB_OK | MB_ICONINFORMATION);
        }
    }
}

void App::cancel_prepare() {
    if (!prep_.active()) return;
    prep_.cancel();   // TerminateProcess of the preparation child only
    SetWindowTextW(prep_text_, L"Cancelling...");
    refresh_buttons();
}

void App::reset_defaults() {
    if (message(hwnd_, L"Reset every setting, key binding and controller button to the defaults? Nothing is saved until you press Save or Play.",
                MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    cur_ = default_settings();
    for (auto& p : pages_)
        if (p->hwnd()) p->load(cur_);
    dirty_ = true;
    refresh_buttons();
}

bool App::confirm_close() {
    if (setup_running_) {
        message(hwnd_, L"Setup is still running. Close its window first.", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (prep_.active()) {
        message(hwnd_, L"Shader preparation is still running. Cancel it first, or wait until it finishes.", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (!dirty_) return true;
    const int r = message(hwnd_, L"Save your changes before closing?", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return store_and_save(false);
    return true;
}

void App::on_command(int id, int code) {
    if (code != BN_CLICKED) return;
    switch (id) {
        case IDC_CHANGE: choose_folder(); break;
        case IDC_SETUP: run_setup(); break;
        case IDC_SAVE: store_and_save(false); break;
        case IDC_PLAY: play(); break;
        case IDC_RESET: reset_defaults(); break;
        case IDC_PREP_CANCEL: cancel_prepare(); break;
    }
}

LRESULT App::on_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: on_create(); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_DPICHANGED:
            change_dpi(HIWORD(wp), *reinterpret_cast<const RECT*>(lp));
            return 0;
        case kMsgTestDpi: {
            // Windows does not deliver a WM_DPICHANGED that another process sends, so the test drives the same code through this message.
            RECT r{};
            GetWindowRect(hwnd_, &r);
            const int w = MulDiv(r.right - r.left, static_cast<int>(wp), env_.dpi), h = MulDiv(r.bottom - r.top, static_cast<int>(wp), env_.dpi);
            r.right = r.left + w;
            r.bottom = r.top + h;
            change_dpi(static_cast<int>(wp), r);
            return 0;
        }
        case WM_COMMAND: on_command(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_NOTIFY: {
            NMHDR* h = reinterpret_cast<NMHDR*>(lp);
            if (h->hwndFrom == tabs_ && h->code == TCN_SELCHANGE) show_page(TabCtrl_GetCurSel(tabs_));
            return 0;
        }
        case WM_TIMER: on_timer(wp); return 0;
        case kMsgPrepareUpdate: on_prepare_update(); return 0;
        case kMsgPrepareFinished: on_prepare_finished(); return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND c = reinterpret_cast<HWND>(lp);
            COLORREF col = GetSysColor(COLOR_WINDOWTEXT);
            if (c == status_) col = status_color_;
            else if (c == note_ || c == version_) col = kGray;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, col);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE:
            if (confirm_close()) DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK App::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->on_message(msg, wp, lp);
}

int App::run() {
    // game folder: --game, else the saved one, else the folder of the launcher
    LauncherConfig cfg;
    load_launcher_config(launcher_config_path(), &cfg);
    std::wstring dir = cfg.game_dir.empty() ? default_game_dir() : cfg.game_dir;
    if (!cmd_.game.empty()) {
        dir = cmd_.game;
        game_dir_pinned_ = true;
    }

    if (cmd_.auto_play) {   // scripted start: no window, settings untouched
        std::wstring err;
        if (cmd_.auto_prepare && !cmd_.no_prepare && shader_prepare_available(dir) && !shader_prepare_done(dir) && !prepare_blocking(dir, &err)) {
            OutputDebugStringW(err.c_str());
            return 1;
        }
        if (!start_game(dir, &err, cmd_.game_args)) {
            OutputDebugStringW(err.c_str());
            return 1;
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    PageWindow::register_class(env_.inst);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &App::wndproc;
    wc.hInstance = env_.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(env_.inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const std::wstring caption = product_name() + L"  " + version_text();
    CreateWindowExW(0, kClassName, caption.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, env_.inst, this);
    if (!hwnd_) return 1;
    // The DPI at WM_CREATE can still be the placeholder; it settles once the window is placed. Rebuild the fonts if it differs.
    const int dpi_now = static_cast<int>(GetDpiForWindow(hwnd_));
    if (dpi_now != env_.dpi) {
        free_fonts();
        env_.dpi = dpi_now;
        make_fonts();
        apply_fonts();
    }
    RECT rc{0, 0, env_.S(kLogicalW), env_.S(kLogicalH)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, static_cast<UINT>(env_.dpi));
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(hwnd_, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    current_page_ = cmd_.page;
    game_dir_ = dir;
    // settings first, then the pages are built from them
    {
        std::string err;
        Settings s;
        const bool ok = load_settings(narrow(controls_path(game_dir_)), &s, &err);
        cur_ = s;
        settings_broken_ = !ok && !err.empty();
        settings_error_ = widen(err);
    }
    SetWindowTextW(folder_, game_dir_.c_str());
    build_pages();
    layout();
    refresh_status();
    refresh_buttons();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    if (cmd_.auto_prepare && !cmd_.no_prepare && shader_prepare_available(game_dir_) && !shader_prepare_done(game_dir_))
        start_prepare(false, false, PrepOrigin::automatic);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace
}  // namespace mprl

#ifdef NXRT_LAUNCHER_GAME_REGISTER
void NXRT_LAUNCHER_GAME_REGISTER();     // the data part of the game module the launcher is built for (cmake/launcher.cmake)
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
#ifdef NXRT_LAUNCHER_GAME_REGISTER
    NXRT_LAUNCHER_GAME_REGISTER();
#endif
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    mprl::App app(inst, mprl::parse_cmdline());
    const int rc = app.run();
    CoUninitialize();
    return rc;
}
