# The in-game settings menu (F1), its pins and the shortcuts

Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch): the menu's redesign, its pins and this document are AnySwitch's.
PokoMac differences: no Frame generation page (PokoMac has no frame generation), and the launcher writes the shortcuts and the default
pins into the game's pkp.ini (one game, no "for every game").

The runtime draws two menus over the game, each with its own toggle: the **settings menu** (F1, `runtime/src/gfx/play/play_ui*.cpp`) and the
**cheats menu** (F3, docs/CHEATS.md "The overlay"). They are separate menus that share one look (the cheats overlay's theme module,
`play/cheats_theme.*`: the same built-in and custom themes, glass, accent and size from `[cheats] theme / style / accent / opacity / blur /
tint / scale`) and one pin engine (`play/pin_board.*`). Each menu manages only its own pins.

## The settings menu

One panel (centred, or docked left / right from its header), over the game dimmed by the theme's backdrop (glass: the game blurred behind
it, the same backdrop blur as the cheats menu, `overlay.cpp`). The header shows the game's icon and name, the frame rate, the dock buttons,
Quit game and close. Pages down the left (only icons when the panel is narrow):

| Page | What it holds |
|---|---|
| Display & FPS | Frame rate (vsync, frame limit with any typed rate, Steam Deck snap, VRR cap), the frame rate counter, window, resolution, low latency, what runs now |
| Upscaler | The upscaler, quality, sharpness, scene resolution, the picture size now |
| Graphics | HDR output, picture, colour, GPU and shaders |
| Audio | Volume, output device |
| Controls | Button prompts; Mouse, Keys and buttons (bindings, quit key), Controller (layout, sticks, pad buttons, quit chord), Switch layout, Status |
| Performance HUD | The pins (below) |
| Game | The game's own tweaks (only when it has some) |
| About | The settings file (Apply and save, Revert, Factory defaults), build, folders, the overlay's diagnostics |

Every row is a label on the left and its control on the right (`play_ui_widgets.h`: cards, rows, the launcher's switch, segmented choices);
the footer shows what the focused row does, the controls of the device used last (key caps), and the toggles in force (`Menu F1 · Cheats
menu F3 · Pins F7`) or a shortcut conflict. Controller: D-pad / left stick move, A select, B back, L / R pages, ZL / ZR scroll, + close.
Keyboard: Tab / arrows, Enter, Esc, Ctrl+PgUp / PgDn pages. Mouse: click, wheel. Changes apply and save as they are made.

## Pins

A pin is a small panel of readouts that stays over the game while its menu is closed. A **group** is one pin with several readouts; every
pin is a group, with its own name, readouts in order, place, size, opacity, layout (vertical, horizontal, compact) and its own toggle.

- The settings menu's readouts (`play/perf_pins.*`): frame rate, frame time with a graph, 1% low, GPU time, the render thread's CPU time,
the upscaler and scene size (when on), shaders compiled in play (0 after a
  bake), memory, video memory, resolution; and settings as readouts (vsync, frame limit, HDR, low latency, volume).
- Performance HUD page: quick pins (each readout on its own, in the Performance group), then every group: name, shown, layout, size,
  opacity, its key (pressed like a binding), its optional controller chord, its readouts (move up / down, remove, add), back to the corner,
  delete. New pin group. While the menu is open the pins can be dragged.
- Saved per game in the settings file (`controls.ini [hud]`: `shown`, one `group = ` line per group, written only once the player has
  arranged pins). Without it a game starts with `[hud] default` (the app's "Pinned by default", for every game or this one).
- The cheats menu's pins (its HUD: pinned values and cheat codes) are drawn by the same engine, saved in its layout file, with their own
  key (`[cheats] hud_key`).

**Display only.** Pins are ImGui windows with `NoInputs | NoNav | NoFocusOnAppearing | NoBringToFrontOnFocus` while their menu is closed:
clicks, the wheel, keys and the pad go to the game exactly as without them; they never pause the game. The toggles' keys are the overlay's
whether the pins are shown or hidden, so the game's input is the same either way (`nxrt_menu_ui_test`: input equality).

**Cost.** The overlay runs its ImGui frame only while something is shown. With three groups (eleven readouts and a graph): 0.011-0.014 ms
CPU per frame on the PC (theme, the pins' windows, ImGui), about 1,500 vertices to draw; the numbers are read four times a second, the
frame time every frame.

## Shortcuts

| Toggle | Key (config) | Controller (config) | Default |
|---|---|---|---|
| Settings menu | `[menu] key` (older: `[gfx] overlay_key`) | `[menu] pad` (else the settings file's `[overlay] pad`) | F1, none |
| Cheats menu | `[cheats] key` | `[cheats] pad` | F3, none |
| Cheats pins | `[cheats] hud_key` | - | none |
| Every pin of the settings menu | `[hud] key` | `[hud] pad` | F7, none |
| One pin | its `key=` in its group | its `pad=` | F8, F9, F10 for new groups |

Keys are SDL names with optional `Ctrl+`, `Shift+`, `Alt+` (`play/hotkeys.h`). A toggle's press, repeats and release never reach the game;
the same key with other modifiers does, so with the menu on Ctrl+F1 a plain F1 is the game's.

**A pin's controller chord** is opt-in (none by default). It must hold a button that presses nothing in the game (a rear grip or a
trackpad click that `[padmap]` leaves unmapped), and that button must go down first: then the chord fires once and, while that button is
held, the chord's other buttons are held back from the game (`PadToggle`). Pressing a game button first and then the rear grip does
nothing, so the game never sees a stray press or loses one it already had, and normal play never fires it by accident. A chord without a
free button is not used (the menu greys those out).

**The launcher.** Settings > **In-game shortcuts** sets the settings menu's key and chord, the pins' hide-all key and chord, the cheats
pins' key and the pins a game starts with ("Pinned by default"), in the game's pkp.ini (`[menu] key`, `[menu] pad`, `[hud] key`, `[hud] pad`,
`[cheats] hud_key`, `[hud] default`); the cheats menu's key and chord are on its Cheats page. The page lists clashes: two toggles on one
key or chord, a chord that holds another or the quit chord, a key the game's keyboard bindings use. The runtime checks the same
(`hotkeys.h toggle_conflicts`), logs them and shows the first in the menu's footer.

## Tests

- `nxrt_menu_ui_test` (`runtime/tests/menu_ui`, a target of the runtime build; CPU only): every page, glass, a light theme, a custom accent,
  the dock and the pins with a group pin at 1x and 2x against `runtime/tests/menu_ui/ref`; the toggles (a remapped menu key opens the menu
  and F1 then reaches the game), a pin's own key hides only that pin, the hide-all key, the pad chords, input equality with the pins shown
  and hidden, the pins' windows' flags, `[hud]` round trip, `[hud] default`, conflicts, pad paging, and the pins' CPU cost (< 0.1 ms).
  `--update` writes the references, `--out <dir>` keeps the full-size pictures.
- Launcher: `node launcher/scripts/test-shortcuts.mjs` (rows, the pkp.ini keys, the pins' text form, clashes, the cheats look).
