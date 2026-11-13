// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The machine the runtime runs on, where it changes a default (src/host_device.cpp).
#pragma once

namespace nxrt {

// A gaming handheld played with its own controls: a Steam Deck (LCD or OLED) or a Lenovo Legion Go S, under SteamOS/Proton or
// Windows. Detected from Steam's SteamDeck=1 in the environment or the board (SMBIOS product name Jupiter or Galileo, family
// "Legion Go S ..."), read from the registry's BIOS key (Windows, and Wine fills it from SMBIOS) or Wine's Z:\sys\class\dmi.
// [host] handheld = auto (default) | on | off overrides it. What it changes: [display] render_scale auto renders at the window's
// size instead of at least 1920x1080 (a handheld's GPU), and [prompts] style auto keeps controller prompts when the trackpad,
// touchscreen or on-screen keyboard send mouse or keyboard input.
bool host_is_handheld();

// A Steam Deck (LCD or OLED) only: Steam's SteamDeck=1 or the board Jupiter / Galileo (same sources as above; [host] handheld does not
// change it). For defaults tied to the Deck's fixed-refresh panel, such as [display] fps_snap auto.
bool host_is_steam_deck();

// Running under Wine (Proton, SteamOS): ntdll exports wine_get_version. Read once; any thread.
bool host_is_wine();
// Steam's on-screen keyboard for text the game asks for (the software keyboard applet). Wanted under Wine (Proton) on a Steam Deck
// (host_is_steam_deck), on SteamOS (SteamOS=1) and in Steam's gamepad UI (SteamGamepadUI=1); [applet] steam_keyboard = auto (default)
// | on | off overrides it. Not on Windows by default: a steam:// URL there would start the Steam client if it is not running.
bool host_steam_keyboard_wanted();
// Shows Steam's keyboard (steam://open/keyboard), keeping the text field at window pixels (x, y, w, h) uncovered; Steam types into the
// window like a real keyboard. Close hides it again (steam://close/keyboard). Both return at once: a worker hands the URLs to Steam in
// order (under Proton through the prefix's steam.exe, Proton's helper that forwards steam:// to the native client).
void host_steam_keyboard_open(int x, int y, int w, int h);
void host_steam_keyboard_close();

}  // namespace nxrt
