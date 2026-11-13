// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The machine the runtime runs on (include/nxrt/host_device.h). One source for every system: on macOS and Linux the Win32 calls go
// through the posix shim (runtime/platform/posix), where the registry and Wine's Z: drive cannot be read and no Wine ntdll exists, so
// only the environment and the config decide there.
// - The board: the SMBIOS product name and family from the registry's BIOS key (Windows; Wine's wineboot fills it from SMBIOS), else
//   from Wine's view of Linux's /sys/class/dmi/id. Valve's handhelds are boards Jupiter (Steam Deck LCD) and Galileo (OLED); Lenovo's
//   Legion Go S has family "Legion Go S ..."; ASUS's ROG Ally names itself "ROG Ally ..." in product or family.
// - Steam: SteamDeck=1 (Steam on a Deck), SteamGamepadUI=1 (Big Picture / gamepad UI), SteamOS=1.
// - Wine: its ntdll exports wine_get_version (Wine's documented way to tell it apart).
// - Steam's on-screen keyboard: steam://open/keyboard?XPosition=..&YPosition=..&Width=..&Height=..&Mode=0 and steam://close/keyboard,
//   the URLs SDL's X11 screen keyboard uses (zlib); handed to Steam's steam.exe (or Proton's helper of that name), else Wine's
//   winebrowser, else the shell.
#include "nxrt/host_device.h"

#include <windows.h>
#include <shellapi.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt {
namespace {

constexpr const char* kTag = "host";

// Leading and trailing whitespace off, ASCII lowercase.
std::string normalised(std::string s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };
    size_t b = 0, e = s.size();
    while (b < e && space(s[b])) ++b;
    while (e > b && space(s[e - 1])) --e;
    s = s.substr(b, e - b);
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// An SMBIOS string: the registry's BIOS key value `value` (REG_SZ, up to 127 characters), else the first line of Wine's
// Z:\sys\class\dmi\id\<dmi_file>, normalised; "" when neither is there.
std::string smbios(const char* value, const char* dmi_file) {
    char buf[128];
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return normalised(buf);
    const std::string path = std::string("Z:\\sys\\class\\dmi\\id\\") + dmi_file;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::string line;
        for (int c = std::fgetc(f); c != EOF && c != '\n'; c = std::fgetc(f)) line.push_back(static_cast<char>(c));
        std::fclose(f);
        return normalised(line);
    }
    return "";
}

std::string product() { return smbios("SystemProductName", "product_name"); }
std::string family() { return smbios("SystemFamily", "product_family"); }

// An environment variable of at most cap - 1 characters, normalised; "" when unset, empty or longer.
std::string env(const char* name, DWORD cap) {
    char buf[64];
    const DWORD n = GetEnvironmentVariableA(name, buf, cap);
    if (n == 0 || n >= cap) return "";
    return normalised(std::string(buf, n));
}

enum class Switch { kAuto, kOn, kOff };

Switch config_switch(const char* section, const char* key) {
    const std::string v = normalised(config_get_str(section, key, "auto"));
    if (v == "1" || v == "on" || v == "true" || v == "yes") return Switch::kOn;
    if (v == "0" || v == "off" || v == "false" || v == "no") return Switch::kOff;
    return Switch::kAuto;
}

// ---- the keyboard URLs, handed to Steam one at a time by one worker (FIFO; never stopped, never destroyed)

struct UrlQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> urls;
    bool worker = false;
};

UrlQueue& url_queue() {
    static UrlQueue* q = new UrlQueue;   // outlives static destruction: the worker may still run then
    return *q;
}

// Starts `path` "url" without a window and waits up to 5 s for it. False when the file is not there or cannot be started.
bool run_with(const char* path, const std::string& url) {
    std::wstring wpath, wurl;
    for (const char* p = path; *p; ++p) wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    for (const char c : url) wurl.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    const DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    std::wstring cmd = L"\"" + wpath + L"\" \"" + wurl + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wpath.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void hand_to_steam(const std::string& url) {
    // Steam's own client, then Proton's steam.exe helper in the prefix (it forwards steam:// to the native client)
    for (const char* path : {"C:\\Program Files (x86)\\Steam\\steam.exe", "C:\\windows\\system32\\steam.exe"}) {
        if (run_with(path, url)) {
            NXLOG_I(kTag, "Steam keyboard: %s (through %s)", url.c_str(), path);
            return;
        }
    }
    if (host_is_wine() && run_with("C:\\windows\\system32\\winebrowser.exe", url)) {
        NXLOG_I(kTag, "Steam keyboard: %s (through winebrowser)", url.c_str());
        return;
    }
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32) NXLOG_I(kTag, "Steam keyboard: %s (ShellExecute)", url.c_str());
    else NXLOG_W(kTag, "Steam keyboard: %s could not be handed to Steam (no steam.exe, ShellExecute %lld)", url.c_str(), static_cast<long long>(r));
}

void queue_url(std::string url) {
    UrlQueue& q = url_queue();
    std::lock_guard<std::mutex> g(q.m);
    q.urls.push_back(std::move(url));
    if (!q.worker) {
        q.worker = true;
        std::thread([&q] {
            for (;;) {
                std::string next;
                {
                    std::unique_lock<std::mutex> lk(q.m);
                    q.cv.wait(lk, [&q] { return !q.urls.empty(); });
                    next = std::move(q.urls.front());
                    q.urls.pop_front();
                }
                hand_to_steam(next);
            }
        }).detach();
    }
    q.cv.notify_one();
}

}  // namespace

bool host_is_handheld() {
    static const bool handheld = [] {
        bool yes = false;
        std::string why;
        const Switch s = config_switch("host", "handheld");
        if (s == Switch::kOn) {
            yes = true;
            why = "[host] handheld = on";
        } else if (s == Switch::kOff) {
            why = "[host] handheld = off";
        } else if (env("SteamDeck", 16) == "1") {
            yes = true;
            why = "Steam's SteamDeck=1";
        } else {
            const std::string p = product(), f = family();
            yes = p == "jupiter" || p == "galileo" || starts(f, "legion go s") || starts(p, "rog ally") || starts(f, "rog ally");
            why = yes ? "board " + p + " / " + f : "not a known handheld (board '" + p + "' / '" + f + "')";
        }
        NXLOG_I(kTag, "handheld: %s (%s)", yes ? "yes" : "no", why.c_str());
        return yes;
    }();
    return handheld;
}

bool host_is_steam_deck() {
    static const bool deck = [] {
        const std::string p = product();
        const bool yes = env("SteamDeck", 16) == "1" || p == "jupiter" || p == "galileo";
        NXLOG_I(kTag, "steam deck: %s (board '%s')", yes ? "yes" : "no", p.c_str());
        return yes;
    }();
    return deck;
}

bool host_is_wine() {
    static const bool wine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return wine;
}

bool host_steam_keyboard_wanted() {
    static const bool wanted = [] {
        bool yes = false;
        std::string why;
        const Switch s = config_switch("applet", "steam_keyboard");
        if (s == Switch::kOn) {
            yes = true;
            why = "[applet] steam_keyboard = on";
        } else if (s == Switch::kOff) {
            why = "[applet] steam_keyboard = off";
        } else if (!host_is_wine()) {
            why = "Windows (a steam:// URL would start Steam if it is not running)";
        } else if (host_is_steam_deck()) {
            yes = true;
            why = "Steam Deck";
        } else if (env("SteamGamepadUI", 64) == "1") {
            yes = true;
            why = "Steam's gamepad UI (SteamGamepadUI=1)";
        } else if (env("SteamOS", 64) == "1") {
            yes = true;
            why = "SteamOS (SteamOS=1)";
        } else {
            why = "Wine, but not a Steam Deck, SteamOS or Steam's gamepad UI";
        }
        NXLOG_I(kTag, "Steam keyboard for text the game asks for: %s (%s)", yes ? "yes" : "no", why.c_str());
        return yes;
    }();
    return wanted;
}

void host_steam_keyboard_open(int x, int y, int w, int h) {
    const auto clamp0 = [](int v) { return v < 0 ? 0 : v; };
    char url[160];
    std::snprintf(url, sizeof url, "steam://open/keyboard?XPosition=%d&YPosition=%d&Width=%d&Height=%d&Mode=0", clamp0(x), clamp0(y),
                  clamp0(w), clamp0(h));
    queue_url(url);
}

void host_steam_keyboard_close() { queue_url("steam://close/keyboard"); }

}  // namespace nxrt
