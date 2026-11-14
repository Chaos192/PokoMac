<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
# Cheats

Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch): the cheats menu, its engine and this document are AnySwitch's, brought to
PokoMac as they are. The in-game cheat tools are our own C++ in `runtime/src/cheats/`, written from the Atmosphere facts in nxnative's
`docs/reference/cheats.md` (no Atmosphere GPL code). Everything is per build id, kept **outside the repository**, and **defaults off** until
the player turns it on (Settings > Cheats > Enable cheats).

**In PokoMac:** everything AnySwitch's overlay has works, live code patches included. Set up lifts the game with no checks in its code
and with room before every function's entry (see "Cheats cost nothing while no code cheat is on" below), and a patch, an injection or a
breakpoint makes the patched function again on demand (`runtime/src/core/missed.cpp` runs `pkp_build.py --ondemand`, the bundled lifter
and Zig, from `<game folder>/build/missed/manifest.tsv`; the first patch of a function takes a few seconds, later ones come from
`build/missed/cache`), then redirects the function's entry and the callers that inlined it. It reaches them while the game runs, also on
Apple silicon.

## Config contract (pkp.ini, read by the runtime)

The launcher writes these; the runtime reads them. All default to off/empty so a normal run is unaffected.

| Section | Key | Type | Default | Meaning |
|---|---|---|---|---|
| `[cheats]` | `enable` | bool | `false` | Turn the cheat engine and overlay on for this title. |
| `[cheats]` | `dir` | path | `<exe dir>/cheats` | The cheats folder. Holds `<program id>/cheats/<build id>.txt` (the console tree) or a flat `<build id>.txt`, plus `toggles.txt` and the saved cheat tables `<build id>.cheattable.json`. |
| `[cheats]` | `key` | SDL scancode name | `F3` | The key that shows/hides the cheats overlay. **Must differ from `[gfx] overlay_key`** (the F1 dev menu); the runtime warns and keeps F3 if they clash. |
| `[cheats]` | `pad` | pad combo | empty (off) | A controller combo that shows/hides the overlay, e.g. `minus+rstick`. Same combo syntax as `[quit] pad` / `[overlay] pad` (`play_config.cpp`). Ignored if equal to the `[quit]` or `[overlay]` combo (warned). |
| `[cheats]` | `enabled_by_default` | bool | `false` | A cheat that `toggles.txt` does not name is off (every cheat off until the player turns it on; the master code always runs). `true`: dmnt's default, every cheat on unless `toggles.txt` turns it off. |
| `[cheats]` | `pass_input` | bool | `false` | While the overlay is open, also let input reach the game (default: the overlay takes input). |
| `[cheats]` | `pause_game` | bool | `false` | Pause the game while the overlay is open. |
| `[cheats]` | `mini` | bool | `false` | The HUD over the game (see "An overlay first") also lists every active cheat and hotkey, not only what is pinned. |
| `[cheats]` | `theme` | name or path | `sunrise` | The overlay's theme: a built-in one (`sunrise`, `dusk`, `midnight`, `daylight`, `paper`, `frost`) or a theme file's path (see "Themes"). A file that does not read falls back to `sunrise`, logged once. |
| `[cheats]` | `accent` | name or colour | `theme` | `theme` (the theme's own), `auto` (picked from the frame), a preset (`blue`, `green`, `amber`, `violet`, `red`, `teal`, `slate`) or any colour: `#rrggbb`, `#rgb`, `rgb(r, g, b)` or `r, g, b`. |
| `[cheats]` | `style` | `solid` / `glass` | (the theme's) | Glass: the game blurred behind a tinted panel (see "Glass"). |
| `[cheats]` | `opacity` | 30..100 | (the theme's) | The panel's opacity (solid style). |
| `[cheats]` | `blur` | 0..100 | (the theme's) | Glass: how strong the blur is (0: none, a plain half-size copy). |
| `[cheats]` | `tint` | 0..100 | (the theme's) | Glass: the tint's opacity over the blurred game. |
| `[cheats]` | `scale` | 50..200 | `100` | Overlay UI scale (on top of the DPI scale). |
| `[cheats]` | `animations` | bool | `true` | Short fades/slides (150-200 ms); off for no motion. |
| `[cheats]` | `hud_opacity` | 30..100 | `85` | The HUD's opacity. |

The defaults and ranges of the look keys (theme, accent, style, opacity, blur, tint, scale, animations, hud_opacity, the dock) have one
source, `runtime/assets/cheats/look.json`: the runtime compiles it in and the launcher imports it, so the two never disagree (a key out of
its range is clamped).
| `[cheats]` | `test_live` | bool | `false` | Test only: lines appended to the `test` file while the game runs become more steps (a live session beside the test runner's `[test] live` script). |
| `[cheats]` | `test` | path | empty (off) | Test only: a step file the overlay runs at the frame boundaries (scans, freezes, find what writes, code patches, pointer scans, exports, game control), each step logged as `cheats: test N: ...` (`cheats_ui_test.cpp` lists the steps). With the test runner's scripted input and screenshots it is the headless check of the tools. |
| `[fs]` | `romfs_mod` | path | empty (off) | A romfs-shaped folder layered **over** the game's RomFS (its files win). Already implemented (`fs.cpp`, `fs_romfs.cpp`). |
| `[fs]` | `exefs_patches` | path | empty (off) | A folder of IPS / IPS32 code patches named by build id (`<build id>.ips` / `.ips32`), applied to the module before the lift. Build-time (pkp_build.py `--patches`), not a runtime read. |

The overlay's own layout (the panel's place and size in UI units, the page and the Advanced tool last shown, which tools are open in
windows of their own and where) is remembered per title in `<cheats dir>/<build id>.overlay.json` by the runtime, not the launcher.

## Launcher UI (Settings > Cheats)

- **Enable cheats** — `[cheats] enable`, default off ("An in-game overlay to search memory and run cheat files. For single player.").
- **Overlay key** — `[cheats] key`, default `F3`; F2..F12, Insert, Home, End, PageUp / PageDown and Pause, stored as SDL names. F1 (the
  settings menu's key) is not offered.
- **Overlay controller combo** — `[cheats] pad`: none, `minus+rstick`, `minus+lstick`, `plus+rstick`, `l+r+minus`, `zl+zr+minus`
  (never `minus+plus`).
- **Cheats folder** — `[cheats] dir`, default `<game folder>/cheats`.
- **Cheat file** (`cheats_fetch.rs`, from AnySwitch's 5f43b106): PokoMac bundles no cheat file (their authors publish them with no
  licence to redistribute); the launcher fetches the public one for the player's game version on the player's machine, the way Breeze
  or EdiZon do on a console: `tools/build/cheats_fetch.py` (stdlib Python, shared with AnySwitch) asks by program id (`0400A3A01EE4C000`)
  and main's build id, tomvita's MyNXCheats first (`github.com/tomvita/MyNXCheats/releases/download/<TID>/<BID>.txt`), then
  switch-cheats-db (`cheats/<TID>.json`); it takes only a plain Atmosphère cheat file (names and 8-digit hex words, under 64 KiB) and
  writes `<cheats dir>/<build id>.txt` (after label lines naming the source and author), `toggles.txt` with every cheat `false` (the
  player's earlier choices kept) and `<build id>.fetched.json` (source, author, page, licence, sha256). The row shows whose the cheats
  are, **Find cheats online** / **Check for a newer one** and **Open the folder**, and a note credits the author. It also runs when the
  player turns **Enable cheats** on, and at Play with cheats on and no cheat file for this version (once a day at most). A file of the
  player's own (no `.fetched.json`) is never replaced. **Find cheat files online** (launcher.json `online_cheats`, default on;
  `POKOMAC_ONLINE_CHEATS=0` for a command-line run) turns it off. Only the program id and build id go to GitHub, never to Nintendo.
  Tests: `game-build/tests/test_cheats_fetch.py` (no network), `cheats_fetch.rs` unit tests.
- **While open: input reaches the game** / **pause the game** / **Active cheats strip when hidden** — `pass_input`, `pause_game`, `mini`.
- **Overlay look** — the `[cheats]` look keys of pkp.ini: **Theme** (the six built-in themes and the player's own theme files), **Your
  themes** (Import theme... copies a theme and the font it names into `<app config dir>/cheats-themes/` and selects it; Open the folder),
  **Panel** (the theme's / solid / glass), **Panel opacity**, **Glass blur**, **Glass tint** (the theme's, or a number), **Accent** (the
  theme's, from the game picture, a preset, or your own colour with a colour picker), **Size**, **HUD over the game** (its opacity),
  **Animations**. The defaults and ranges come from `runtime/assets/cheats/look.json`, which settings-schema.js imports (the runtime
  compiles the same file in).

## The overlay

A separate layer from the F1 dev menu, drawn into the same ImGui context (`overlay.cpp`, `runtime/src/gfx/play/cheats_ui*.cpp`). It shows
and hides at once with its own key (`[cheats] key`, F3) or controller combination (`[cheats] pad`, for example `minus+rstick`); hiding it
keeps everything running (the engine thread writes freezes 12 times a second, the frame callback runs hotkeys, Lua and the value graphs).
With `[cheats] mini` a strip in the corner lists what is on and its hotkeys while the overlay is hidden. While it is open the game gets no
input (unless `pass_input`), and with `pause_game` (or Game control's checkbox) the game is paused while it is open.

**One panel, four pages** (simple first; L / R or Ctrl+PgUp / PgDn step through them), over the game dimmed by a backdrop so it stays
visible. The page shown last is reopened for that game. The header shows the game: its icon (the title folder's `icon.png` or `icon.jpg`, as the app's library keeps
it; JPEG through stb_image, `runtime/third_party/stb`) and its name, what is active and paused, and a close button.

| Page | What it does |
|---|---|
| Cheats | The game's Atmosphère cheat codes and the enabled mods' (`[cheats] mod_files`), a switch each (turning one on starts the engine); the engine's own switch; import a cheat file, save My table as cheat codes, reload. An empty state says where the file goes. |
| Find a value | The scan as a guided flow. (1) What kind of value: Whole number (4 bytes), Decimal number (float), Not sure (all numeric types), Text, and under More types every other scan type (byte, 2 / 8 bytes, double, UTF-16, an array of bytes with `??` and nibble wildcards). (2) What it is now: I know the number (exact), I don't know it (unknown initial value), It's in a range (between, bigger / smaller, at least / at most). Search, with progress and the count found so far. (3) Change it in the game and say what happened: It went up / down, It changed / didn't change, It's now exactly N, and under More ways increased / decreased by, between, bigger / smaller, same as the first search; the count after every step in large type, the steps so far in plain words, Undo last step, New search (asks first). (4) The matches with their value now and before: select one (A / click) to add it to My table (Undo in the message); X / right-click: find what writes or reads it, a pointer path to it, the memory viewer, the disassembler. Search options: which memory (heap, the game's variables, shared and mirrored memory, stacks, code, the rest, writable only), aligned values, case, pausing the game while searching. |
| My table | What you kept: a switch to freeze each value (or run an AOB script), its name and where it is (or what is wrong with it, always visible), its value (type a new one: written at once, and the freeze moves to it), its hotkey (a key or a pad combination). Groups as headings, a group filter. The row's actions (X, right-click, the dots): freeze mode (keep it exactly / let it go up / let it go down), value type, find what writes / reads it, a pointer path to it, the memory viewer, the structure there, a value graph, a label, the hotkey and what it does (freeze / write / +1 / -1), its group, copy as an Atmosphère cheat code, delete (Undo in the message, also Ctrl+Z / -). Add address, Add script (AOB script entries), Save as cheat codes. |
| Advanced | The other tools, listed by group with what each is for, the chosen one shown beside the list; the pop-out button puts it in a window of its own (several at once, saved per title). |

The Advanced tools:

| Tool | What it does |
|---|---|
| Pointer scanner | Multi-level paths (levels, maximum offset) from any module to an address; the restart rescan keeps the paths that lead to the value's new address; paths go to the list as pointer entries. |
| Snapshots | Copies of guest memory (the filter's regions or a range, up to 512 MiB) compared with each other or with memory now: every changed 4-byte word, with old and new values. |
| Memory viewer | Hex and text, go to an address / `main+offset` / a label, go to pointer, the value inspector (every type and text), writing bytes (code goes through the code patcher), Add to list, What writes it, Structure. |
| Structure | Dissects memory as 8-byte fields: pointer (and where to), text, two floats, integers; opens pointers, adds fields to the list. |
| Memory map | Every block of the guest's address space: address, size, permissions, kind, module. |
| Labels | Names for addresses, shown in the viewer, the disassembler and the list; usable in every address field. |
| Disassembler | The A64 code with module + offset, function starts (`fn`), labels and branch targets; follow a branch, back; per instruction: NOP (live patch), assemble a replacement (any A64 line the assembler takes), revert, breakpoint / trace point, the signature maker, label. |
| Find what accesses | Find what **writes** (or reads and writes) an address: the functions that touched it, with hit counts and the registers at the last access (a hardware watchpoint on the value's bytes in every guest thread; says so when the value changes with no access at that address). |
| Breakpoints | Breakpoints (hold the thread) and trace points (count and record), the registers of a held thread, Continue / Step / Run to return. |
| Code patches | Every live code patch and its state; revert; write the IPS patches for the next build. |
| Threads | The guest threads: id, name, priority and core, state, the function each is in; its code and stack. |
| Game control | Pause / resume, frame advance (N frames), speed x0.1 .. x8, pause the game while the overlay is open. |
| Value graphs | One sample per frame of any list entry (the last 600) and a trigger (any change, above, below, equal) that pauses the game or logs. |
| Lua | Scripts (saved with the table, restarted at the next start if they were running) and a console. |
| Settings | Accent (the launcher's, from the game's picture, or a preset), panel opacity, size. |
| Help | The pages and the controls. |

**Input.** Everything works with a controller alone, a mouse alone and a keyboard alone; the hint bar at the foot of the panel shows the
buttons (as key caps) of the device used last.
- Controller: the D-pad or the left stick moves the focus, A selects, B backs out, X opens a row's actions, Y edits the focused field with
  the **on-screen keyboard** (a hex keypad for addresses and bytes, a number pad for values, a full keyboard for text and scripts; A types,
  B deletes, - switches case, + confirms), L / R switch pages, ZL / ZR page, - undoes what the last message offers, holding - moves between
  windows, + closes the overlay. With the pad in use, activating a text field opens the keyboard by itself.
- Mouse: click, right-click for a row's actions, the wheel scrolls, drag the panel (and pop-out windows) by any empty spot, resize at the
  corner.
- Keyboard: Tab / arrows / Enter / Esc, Ctrl+PgUp / PgDn for the pages, Ctrl+Z to undo, Delete removes a row of My table; typing into
  fields as usual.

**Look.** One theme module (`runtime/src/gfx/play/cheats_theme.cpp`) holds every token, taken from the launcher's `launcher/src/theme.css`
(the default "Sunrise" tone: amber on warm dark, the same surfaces, lines, radii and spacing scale), its typeface Inter (OFL-1.1, compiled
in: `runtime/assets/fonts/`; hex and code use ImGui's monospaced face), vector icons in the launcher's icon style, and the shared widgets
(pill buttons, the launcher's switch, choice cards, chips, empty states, key caps for the hints). The panel is nearly opaque (text keeps
WCAG AA contrast over a white frame); the backdrop around it dims the game a little so it stays visible. Every size scales with the DPI
scale times `[cheats] scale`. `accent = auto` takes the accent from the game frame: the frame under the overlay is read back once when it
opens (`Renderer::capture_next_frame`, the overlay hidden for that one frame) and its most saturated colour, weighted by brightness, sets the
hue; saturation and lightness are clamped so the focus ring and selections stay readable (a grey frame gives slate).

**An overlay first.** The game stays visible and keeps running behind the panel (unless `pause_game`).
- **Dock**: the header's three buttons put the panel on the left or the right (full height, about 40 % of the width, so it covers less of
  the game) or in the centre, where it can be moved and resized. Saved per game. Narrow, the page tabs keep only the chosen one's name.
- **Peek**: hold ZR, hold the overlay key (a press still closes it, on release), or hold the header's eye: the panel fades almost out to
  show the game, and comes back when it is let go. A tap of ZR still pages down.
- **HUD**: pin a value (My table, the row's actions: Pin to the HUD) or a cheat code (the Cheats page, its pin): it shows in a small
  translucent strip over the game while the panel is closed, with its live value (frozen values marked), at its own opacity
  (`hud_opacity`, the Settings tool). Drag it to move it while the panel is open (saved per game). Nothing pinned, no HUD, and the overlay
  draws nothing while it is closed (`[cheats] mini` lists every active cheat in it too, as the old strip did).

**Themes.** A theme is a JSON file (UTF-8, at most 64 KB) that sets the whole look. Every key is optional: what a file leaves out is the
default theme's. The built-in themes (`runtime/assets/cheats/themes/*.json`, compiled in: the launcher's five tones and Frost, a glass
one) are the examples to copy.

```json
{
  "format": 1, "name": "Forest", "author": "a player", "description": "...",
  "style": "solid", "opacity": 94, "blur": 60, "tint": 55, "radius": 8,
  "accent": "#9be15d",
  "colors": {
    "panel": "#0f1a14", "dialog": "#16241c", "surface": "#dff5e6", "card": "#ffffff0b", "well": "#00000044",
    "text": "#eef7f0", "text2": "#eef7f0b8", "text3": "#eef7f080", "line": "#dff5e617", "line_strong": "#dff5e633",
    "on_accent": "#0c1a06", "ok": "#8fd3ff", "warn": "#f4c066", "danger": "#ff8a80", "danger_solid": "#d84a55",
    "scrim": "#02080580"
  },
  "font": "MyFont.ttf", "font_size": 16
}
```

- `colors`: `panel` the panel, `dialog` popups and the keyboard, `surface` the tint of cards, controls and hover (used at low
  opacities, as the launcher's tones do), `well` text fields, `text` (`text2` / `text3` secondary and tertiary; by default the text at 72 %
  and 50 %), `line` / `line_strong` the hairlines, `on_accent` the text on the accent, `ok` / `warn` / `danger` (`danger_solid` the delete
  button), `scrim` the backdrop over the game. Colours: `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`.
- `style` `solid` or `glass`; `opacity` 30..100 (the panel, solid); `blur` and `tint` 0..100 (glass); `radius` the panel's corner radius in
  pixels at scale 1, 0..40 (the other radii follow in the launcher's proportions); `font` a `.ttf` / `.otf` next to the theme (it replaces
  Inter; it is loaded when the game starts), `font_size` 12..22.
- Checked when it is read: valid JSON, known keys only, values in range, colours that parse, the font there, and text that reads: the
  text on the panel needs a contrast of at least 4.5:1 (WCAG AA) and `on_accent` on the accent 3:1. On any error the default theme is used
  and the reason is logged once (`[cheats] theme: <file>: <why>`), and the Settings tool shows it.
- The player's own settings (`[cheats] accent`, `style`, `opacity`, `blur`, `tint`) win over the theme's.
- Sharing: a theme is its `.json` (and its font, if any); the launcher's theme picker lists the built-in ones and the files in its themes
  folder, and imports a file into it.

**Glass.** With `style = glass` the panel shows the game blurred behind its tint: each frame the panel is open and glass, the overlay
blits the panel's rectangle of the frame (with a margin, out to 32-pixel steps) at half size into an image and runs the dual Kawase blur
on it (`runtime/src/gfx/extras/backdrop_blur.cpp`, compute shader `kBackdropBlurComp` from `runtime/scripts/gen_blur_shaders.py`): up to
four passes down and four back up, every tap clamped into the rectangle, so only those pixels are touched; ImGui draws the result rounded
behind the tint. The blur is made at the first glass frame; nothing of it runs while the overlay is closed or solid. It needs a frame the
overlay can read (TRANSFER_SRC; every desktop swapchain here) and SDR output: with HDR output the panel draws solid. Measured on an RTX 5090
(`nxrt_cheats_ui_test --gpu`, the mean of 20, Frost's strength, a centred panel of 64 % x 84 %): 0.008 ms at 720p, 0.016 ms at 1080p,
0.021 ms at 1440p, 0.035 ms at 4K. The same test compares the GPU's result with the CPU reference of the same arithmetic
(`tests/cheats_ui/blur_ref.cpp`): within 1 to 2 levels (of 255) everywhere, the GPU's bilinear sub-texel precision. On an M5 Max with
KosmicKrisp (the round-1 ICD, d1b586e7) it matches the same way (worst 2, mean at most 0.31) and costs 0.240 ms at 720p, 0.266 ms at 1080p,
0.328 ms at 1440p and 0.466 ms at 4K: a floor of about 0.22 ms that hardly grows with the size, the per-pass cost of the driver (one blit
and up to eight dispatches with barriers), not the pixels. The CPU screenshot test passes on the Mac against the PC-made references.

**Screenshot test.** `nxrt_cheats_ui_test` (`runtime/tests/cheats_ui`, a target of the runtime build) draws every main screen with no window
and no GPU (a CPU renderer of ImGui's draw data, `soft_raster.cpp`) over a synthetic game frame, at UI scale 1 (1280x720) and 2
(2560x1440), on a real guest heap with known values (its scans are real scans), and compares them with `tests/cheats_ui/ref` (stored at
640x360): the pages, the keyboard, a light theme, a player's theme file, a custom accent, the docked panel, Peek, the HUD and glass (its
backdrop from the CPU reference of the blur). It also checks the theme reader (valid and broken files, colours, the look spec) and the
pad (L / R pages, - undo). `--update` writes the references, `--out <dir>` keeps the full-size pictures, `--gpu` (inside the GPU slot
only) runs the glass blur on the GPU against the reference and times it.

## How the tools work (all native: never an interpreter)

- **Memory.** Guest addresses are host addresses (`include/nxrt/memory.h`), so a console cheat's `main+X` lands where it would on the console.
  Every access is fault safe (`gmem.cpp`): an unmapped page fails the access instead of crashing; a read-only page (main's code and rodata)
  is made writable for a write and put back, as Horizon's debugger writes do. The guarded copy is a function of its own: under clang-cl
  `/EHa` an inlined `__try` took the caller's address from a register the fault had changed (see Evidence), and the protected write
  refuses any address outside the guest's window.
- **Find what writes / reads** (`accesswatch.cpp`): a hardware data watchpoint on the value's bytes in every guest thread (Windows x64: DR0 /
  DR7, set from a helper thread with each thread suspended; macOS on Apple silicon: watchpoint 0 of `ARM_DEBUG_STATE64` through
  `thread_set_state`, which a process may do to its own threads), so only an access that touches those bytes traps; the handler maps the
  lifted host PC to its guest function (the same host-PC-to-guest map the crash handler uses), counts it and keeps the guest registers. On
  x64 the trap comes after the access, so every access is counted; on Apple silicon it comes before, so the handler switches the
  watchpoint off on that thread to let it through and the engine's 1/12 s pass switches it on again (and arms threads started since).
  Where no watchpoint can be set, the page that holds the address is made read-only (writes) or no-access (reads) instead and re-armed each
  pass; that misses the value whenever another field of the page is written first (Hollow Knight's frame counter: 72 accesses on its page in
  6 s, none at the value, run hk5).
  nxnative's debugging notes warn that hardware watchpoints on words touched by `ldxr` / `stxr` may stay silent
  (`docs/agents/05-debugging.md`): a value the game changes only with exclusive stores can show no writer.
- **Code patches** (`codepatch.cpp`). Changing an instruction's bytes changes nothing by itself: the game runs compiled host code. A patch
  writes the bytes into guest memory, lifts the function that holds them again from the patched bytes into an on-demand unit (the build's
  own lifter and toolchain, `missed::make_code_unit`, cached by the bytes), registers it with the dispatcher (indirect calls run it at
  once), and at the next frame boundary, with every other guest thread parked, writes a jump over the compiled function's entry so the
  direct calls run it too (on macOS the patched code page is made elsewhere and mapped over the program's in one step: see "Live code
  patches on Apple silicon"). Where the compiled function is shorter than the jump, only indirect calls run the new code; callers the
  compiler inlined the function into keep the old code unless the game is built for live patching (below). The overlay shows which, and
  **Write the IPS patches** gives the next build the patch everywhere (`<cheats dir>/exefs_patches/<build id>.ips`, see "Code patches
  (IPS / IPS32)").
- **Cheats cost nothing while no code cheat is on** (a cheat should only affect the game while it is on;
  dmnt on the console costs nothing either). The default build has no live-patch checks (the code of each function is that of
  `--no-live-patch`, compared instruction by instruction on Pokopia's sdk, subsdk0 and rtld) and **room before every function's entry**:
  `pkp_build.py` `ENTRY_ROOM`, `-fpatchable-function-entry` with all of it before the entry (4 nops on arm64, 14 one-byte nops on
  x86-64), never run. Data cheats need nothing else: the VM writes guest memory as on the console. A code cheat redirects only what it
  patches (`entryjump.cpp`): the function lifted again from the patched bytes as a unit (as before), then a far jump written into the
  room of its compiled entry and one branch over the entry's first instruction (LLVM never branches to a function's first instruction:
  its entry block has no predecessors), so every call, from any caller, runs the unit; turned off, the branch goes first, then the room
  (its nops fall through to the entry). On macOS the code page is replaced by a changed copy in one `mach_vm_remap` (no thread runs half
  a change; the process stays signed-valid); on Windows the game's other threads are suspended, none inside the bytes that change, as
  debuggers and Detours change live code. The redirect goes through a stub that records the unit's first run: a patched function the
  game has not called in the 10 s since is logged once and shown as **not run yet** (a function the game is inside, in a loop it never
  leaves, keeps the old code until it returns: the case the loop-head checks below covered). **Inlined copies**: the guest callers of the
  patched function (its `bl` / `b` sites, scanned in the module's code) whose compiled code has no call or jump to its compiled entry
  inlined it (`inliners.cpp`; arm64 `bl` / `b`, through a linker branch island; x86-64 `call` / `jmp` rel32): each is lifted again from
  its unchanged code and redirected too, recursively, at most 64; turned off, they go back with it. `nxrt_entryjump_test` (macOS and
  Windows: 4 threads calling throughout redirects and their removal, every result the old or the new one), `nxrt_inliners_test`.
- **Live patching through checks** (`--live-patch`, the default from f721f36 on 2026-11-10 to 11-13; a build with it plays as it is and
  the next Build drops it: +9.7 % instructions in Pokopia with every cheat off, 396,890 checks and 75,308 resume sites). Every lifted function starts with `NX_OV` (recomp
  `ctx.h`): a load of `Ctx.ov` and a branch. While no live patch exists `Ctx.ov` is null and that is all it costs; when one does, the
  runtime points every guest thread's `Ctx.ov` at a table of per-module bitmaps (one bit per 4-byte word of code) and sets the patched
  function's bit, and its entry sends the call to `nx_hook_entry`, where the function recompiled from the patched bytes runs. So a live
  patch reaches every call, direct ones and the copies the compiler inlined into callers included, on every host, with no write to the
  program's code (which Apple silicon refuses). The same check stands at every **loop head** of the function (a target of a backward
  branch, `NX_OVL`, same bit): a loop that is already running when the patch lands (a thread's loop that never returns) continues at its
  next iteration in the recompiled function, entered at that guest address (`nx_hook_resume`: the unit's `u_<entry>(c, pc)`; a patched
  function's unit has its loop heads as entries, `ondemand.lift_code`), with the guest registers as they are in `Ctx`; the old frame then
  returns. That is what a code patch does on the console: dmnt writes main's code through the kernel's debug write
  (`WriteDebugMemory` takes debuggable read-only pages, nxnative `docs/reference/cheats.md`), and the next instruction fetch runs it, in a
  running loop too; nxnative, which runs the guest's own instructions, gets the same by writing main's text through a second, writable
  mapping and invalidating the instruction cache (`dm/host/hle_cheats.c` `main_write_pages`). Such a module declares ABI 5
  (`NXRECOMP_ABI_LIVE`); one lifted without `--live-patch` stays ABI 4, never reads `Ctx.ov`, and still plays. The cost: see "Evidence".
- **ASM cheats** (Atmosphère cheats that write code: `vmcode.cpp`, the engine's write router). A console's dmnt writes main's code through
  the kernel's debug write and the next instruction fetch runs it; most code cheats put a branch over one instruction into a code cave in
  the padding after the module's last instruction (Pokopia 2.0.1's 4 ASM cheats, 59 of Z-A 2.0.2's 60). The VM's writes are split by
  where they land: data (anything but a lifted function's code or a cave) is written at once, exactly as before; a word of a lifted
  function is held until the pass ends and then, when it changed, becomes a one-word live code patch (`cp::apply_words`: each function it
  touches lifted again once, swapped in at the next frame boundary, through the entry and loop-head checks of a `--live-patch` build);
  cave bytes (from the module's text end to the end of its last 4 KiB code page, as Inject's cave) are written first and made a code
  region the dispatcher lifts from its bytes (`missed::code_region_add`; rewritten bytes drop the units made from the old ones). Each
  held word remembers the cheats whose last write is what it holds (a "Restore Code" cheat writing the original before another cheat's
  patch owns nothing). Turning a cheat off puts back the words no other enabled cheat holds: code at once (`cp::revert_words`), its cave
  once every function reverted with it has swapped (`cp::settled`), so no thread can still branch into a cave being cleared; a cheat
  that writes the original bytes back reverts the same way, and a reload of the file puts everything back. A console never reverts (the
  code stays until the game restarts): here a cheat switched off puts the game back. These patches are the cheat's, never written to
  an IPS (`by_cheat`). With data cheats only, a write costs one range test more and the pass ends with nothing to do: the engine's
  `code` counters stay 0 (`nxrt_vmcode_test`, `nxrt_cheat_engine_test`; the scripted test's `codewords` step logs them and the words held).
- **No frame waits for the compiler** when a code patch goes on (`reach.cpp`, `missed::prepare`): before the swap, the patch's worker
  thread also makes ready every address the patched words reach that the dispatcher may not have (a branch out of the function, into a
  code cave and on through the cave's calls and its branch back, the instruction after the hook), from the cache or compiled there; the
  game runs the old code meanwhile and the cheats list says **applying…** until the swap. Before, the first guest thread to enter a
  cheat's cave waited for its compile (pokomac-port's PC run: a 12.7 s frame; Pokopia's Leafage on the Mac: 0.75 s for two cave units).
  `nxrt_reach_test` (Pokopia's real Leafage, Water Gun and crop caves).
- **Never half on** (`reach::make_ready`, `Engine::turn_off_failed`): a patch is swapped in only when its function and everything it
  reaches compiled. When one of them does not (the on-demand helper fails, the patcher refuses the word), nothing is swapped in, the
  cheat goes off at the next pass with its code and cave put back, the cheats list says **could not turn on** (the reason on hover) and
  the log says why once; toggles.txt keeps the player's choice, so the next start tries again. Before, PokoMac's PC run went live with
  the Water Gun cheat's cave not compiled and its first use raised a guest trap that ended the game. `nxrt_reach_test`,
  `nxrt_vmcode_test`, `nxrt_cheat_engine_test` (a forced compile failure).
- **Breakpoints** use the same unit with an inline site (`nx_site_inline`, hooks.h) before the instruction, and before every other
  instruction of the function when a breakpoint there stops, so a held thread can step. A held thread waits in the site's hook; on the
  presenting thread the pause service keeps the overlay running meanwhile.
- **Pause / frame advance** (`control.cpp`): at the frame boundary (play.cpp's frame hook, on the presenting thread) every other guest thread
  is asked to park as `svcSetThreadActivity` pauses one; the presenting thread waits in the hook, where play.cpp's pause service pumps the
  window and the pads, runs the overlay and shows the last frame again. Threads the game paused itself are left alone. The frame callbacks
  keep running while it holds (every 8 ms; `ctl::held()` is true there), so table hotkeys, Lua hotkeys and a scripted test still answer
  during a pause, and code swaps asked for meanwhile happen at once (the others are already parked); value graphs and Lua `onframe`
  functions wait for real frames.
- **Speed** (`kernel::set_time_scale`): the console clock (svcGetSystemTick, CNTVCT_EL0), the guest's sleeps and timed waits, the vsync event
  and the frame limiter run at the chosen speed; the clock stays continuous across changes. The audio renderer keeps real time, so a game that
  paces itself on audio buffers does not change speed.
- **Disassembler and assembler** (`disasm.cpp`): every integer, load/store (including exclusives, LSE atomics and SIMD&FP registers),
  branch, system and scalar FP class, printed as LLVM prints it (its aliases: mov, cmp, lsl, ubfx, cset, mul ...); the assembler takes the
  same syntax. Advanced SIMD vector instructions print as `.inst 0x........` (never a wrong mnemonic). Checked against llvm-objdump (below).

## Live code patches on Apple silicon

The question (2026-11-10): a live code patch must reach every caller on the Mac as on Windows, with no rebuild. What the platform
allows, from Apple's documents and measured on an M5 Max (macOS 27.2) with test programs signed as pkp is (ad hoc, no hardened runtime:
`codesign -dv` of pkp shows `flags=0x2(adhoc)`; `make-release-mac.sh` signs the app with `codesign --force --sign -`, not notarized):

- Apple, "Porting just-in-time compilers to Apple silicon": "Apple silicon enables memory protection for all apps, regardless of whether
  they adopt the Hardened Runtime ... a thread cannot write to a memory region and execute instructions in that region at the same time";
  `MAP_JIT` memory is switched per thread with `pthread_jit_write_protect_np`; the `com.apple.security.cs.allow-jit` entitlement "is
  required only when an app adopts the Hardened Runtime capability", which "prohibits the execution of code in a memory page, unless that
  code is accompanied by a valid code signature". `com.apple.security.cs.disable-executable-page-protection` "disable[s] all code signing
  protections" ("an extreme entitlement"); `allow-unsigned-executable-memory` is the narrower one for unsigned code.
- Measured (three small test programs, kept out of the repository: a function on its own 16 KiB code page returning 1, patched to return 2): `mprotect` of the
  code page to read+write(+execute) fails with EACCES (the page's maximum protection is r-x); `mach_vm_protect(VM_PROT_COPY | RW)`, the
  write, then back to r-x runs the patched code and the process stays valid (csops flags unchanged; a later `dlopen` of an ad-hoc dylib
  works), but the page has no execute permission between the two calls; a `MAP_JIT` page cannot be mapped over the code page
  (`mach_vm_remap`: KERN_PROTECTION_FAILURE) nor created there (`MAP_FIXED | MAP_JIT`: EINVAL); **an anonymous page holding a copy of
  the code page with the patch, made r-x and mapped over the code page with `mach_vm_remap(VM_FLAGS_OVERWRITE)`, runs the patch, with
  four threads calling the function throughout the swap (209,758,443 calls, every one returned the old or the new result)**. The same
  rewrite under the hardened runtime (`codesign -o runtime`) is killed (SIGKILL).
- nxnative (which runs Switch code natively on Apple silicon) patches no code at run time: its cheats write data only, through a second,
  writable mapping of main (`dm/host/hle_cheats.c` `alias_setup`, `mach_vm_remap` + `mach_vm_protect`); its one `MAP_JIT` region is the
  code memory of the console's `nn::jit` plugins (`hle_jit.c`), and it notes that on the iPhone "code pages are the app's signed pages,
  which no mapping can make writable".

So the patcher's jump over a compiled function's entry works on the Mac too (`codepatch.cpp` `write_code`, the last mechanism above: the
code there never lacks execute permission), and a default build reaches direct callers live, as on Windows. What no rewrite of a function's
entry can reach, on any host, are the copies of the function the compiler inlined into its callers (Wonder's main, wb2: 211,364 functions;
168 with four or more `bl` callers in the guest code have no `bl` / `b` left to them in the host code, every caller inlined them) and
functions whose compiled code is shorter than the 16-byte jump (12,885 of the 251,805 lifted functions in a default build of Wonder). The entry check every build carries since f721f36 (`NX_OV`) reaches those too.
Never an interpreter: in every case the code that runs is the function lifted again from the patched bytes and compiled.

## AOB scripts

A table entry can be a Cheat Engine-style script (`aobscript.cpp`): its `[ENABLE]` part finds the code by an AOB pattern and patches it,
its `[DISABLE]` part puts it back (a `[DISABLE]` that is empty or writes nothing restores what `[ENABLE]` wrote). Because the code is found by its bytes, with the
moving fields wildcarded, the script keeps working after an update moves the code; when the pattern is gone, enabling fails with "the
pattern is not in main (the script was made for another build of the game?)" and the entry stays off.

```
[ENABLE]
aobscanmodule(coindec, main, 08 05 00 51 68 ?? 00 B9)   // sub w8, w8, #1 ; str w8, [x19, #..]
coindec:
  nop
registersymbol(coindec)
[DISABLE]
coindec:
  db 08 05 00 51
unregistersymbol(coindec)
```

Statements: `aobscan(name, pattern)`, `aobscanmodule(name, module, pattern)`, `aobscanregion(name, start, end, pattern)`, `define(name,
expr)`, `label(name)`, `registersymbol` / `unregistersymbol`, `assert(expr, bytes)`, `<expr>:` (the write address), `db` / `dw` / `dd` /
`dq`, `readmem(expr, n)` and any A64 instruction. Expressions are names, module names, hex numbers (`#` for decimal) joined by `+` / `-`.
Writes into code go through the live code patcher. The disassembler's signature maker writes such a script for "NOP this instruction".

**The signature maker** reads the bytes from the address on, wildcards the bytes the disassembler marks as moving between builds (branch
distances, adrp pages, immediates and load/store offsets) and grows the pattern until it matches once in the module.

## Lua scripts

Lua 5.4.7 (MIT, PUC-Rio; `cmake/lua_deps.cmake`, compiled as C++ so script errors unwind properly), `runtime/src/cheats/luavm.cpp`. Scripts run
on the presenting thread at the frame boundary. The sandbox: only base, coroutine, math, string, table and utf8 are built (no io, os,
package, debug); `dofile`, `loadfile` and `string.dump` are removed and `load` takes text only; files only through `nx.readfile` /
`nx.writefile` with a plain file name inside the title's cheats folder. A call that runs more than 20 million instructions stops its script
("took too long"); all scripts together get 64 MiB.

| Function | |
|---|---|
| `nx.read8/16/32/64(a)`, `nx.readf(a)`, `nx.readd(a)`, `nx.readbytes(a, n)`, `nx.readstring(a [, max])` | read guest memory (nil when unreadable) |
| `nx.write8/16/32/64(a, v)`, `nx.writef(a, v)`, `nx.writed(a, v)`, `nx.writebytes(a, s)` | write (true / false) |
| `nx.module(name)` | base, size |
| `nx.ptr(base, off1, ...)` | follow a pointer path |
| `nx.aobscan(pattern [, module])`, `nx.aobscanall(pattern [, module])` | the first / every match |
| `nx.freeze(a, type, value)`, `nx.unfreeze(id)` | freeze through the engine (`u8` `u16` `u32` `u64` `f32` `f64`) |
| `nx.onframe(fn)`, `nx.hotkey(name, fn)`, `nx.held(name)` | per-frame function, hotkeys ("F5", "L+R+A") |
| `nx.pause(on)`, `nx.advance()`, `nx.speed([x])`, `nx.frame()`, `nx.tick()`, `nx.time()` | game control; the console clock (speed hack included) and real time in seconds |
| `nx.readfile(name)`, `nx.writefile(name, s)` | the cheats folder only |
| `print(...)`, `nx.log(...)` | the console |

## Evidence

Measured on 2026-11-10 (Windows: an RTX 5090 PC, under other load; macOS: an M5 Max), Super Mario Bros. Wonder.

- **A64 decoder and assembler against LLVM.** Every word of a 1-in-21 sample of Wonder's main `.text` (414,511 instructions) and a 1-in-3
  sample of its sdk (514,561) was disassembled by llvm-objdump 19 and by `disasm.cpp`: on every word the decoder models (98 % of main; the
  rest are Advanced SIMD vector instructions and `udf`, printed as `.inst`) the mnemonic and every operand value equal LLVM's (LLVM prints
  some 64-bit `mov` immediates signed), and `assemble(decode(word))` gives the word back for all of them. `tests/cheats/disasm_test` keeps
  272 instructions clang 19 assembled, with LLVM's text.
- **Signatures survive an update.** For 200 sampled instructions of Wonder 1.2.0's main (build id ff773e90972d544e) the signature maker's
  pattern (its wildcards on branch distances, pages, immediates and offsets; at most 64 bytes) was unique in that build for 101; 95 of those
  are found in Wonder 1.0.0's main (cd6e42aee7934f4d, a different layout: 33.6 against 34.8 MB of code), 90 exactly once. So nine in ten
  AOB scripts made on one version still find their code on the other. The one used live below (the title screen's frame counter store,
  `str w10, [x8, #0x1e8]` at 1.2.0 main+0x598994) finds 1.0.0 main+0x597d54, `str w10, [x8, #0x668]`: the 29 instructions around it are the
  same but for the counter's page and offset (and where the code sits).
- **clang-cl's `__try` and memcpy.** With the build's `/EHsc`, a `__try` around a memcpy from a reserved page does not catch the fault (the
  process ends 0xC0000005); with `/EHa` it does. The first live scan crashed so (memcpy under `gmem::read`); gmem.cpp is now built `/EHa`.
- **clang-cl `/EHa` and an inlined `__try`.** The first live NOP failed with "write to 0x1a4640000 refused: not committed", not the code
  page's address: inlined into `gmem::write`, the guarded copy's `__except` path read the address and the source from rcx / rdx
  (gmem.cpp.obj, write+0xb6), which hold whatever the fault left. Out of line, the caller keeps its values on its own stack (checked in the
  new object), and the next NOP was written. (The CRT's `memcpy_repmovs` pushes rsi / rdi with unwind data: not a cause.)

**Live** (headless, or in a hidden window where a pad is needed; the test runner's scripted keys, pad and screenshots; `[cheats] test`
steps at the frame boundaries, each logged), Wonder 1.2.0: Windows on the title screen, macOS on the World 1 map. The step files and
screenshots are not kept in the repository (they show the game).

| Tool | What the run showed |
|---|---|
| Scanner | Unknown initial value, 4 bytes, then three "increased": Windows 1748 MiB scanned, 1814 -> 452 -> 351 results; macOS 2080 MiB, 64,087 -> 4,223 -> 1,578. Both keep the title's frame counter, main+0x37c81e8. |
| Freeze | Held across frames (Windows 189 engine writes, the value stayed 394; macOS 2943 over 3 s); a frozen pointer entry came back frozen after a restart. |
| Find what writes | Windows and macOS name the same writer, main+0x598920; the "likely instruction" is the store whose base register plus offset is the address, `str w10, [x8, #0x1e8]` at main+0x598994. A frozen entry's own writes are listed as host. |
| Trace point | The function lifted from the code at run time (295 instructions, 0 not translated) and compiled once (198 s; the very first use of the on-demand cache also built zig's MinGW C library, 37 min on the loaded PC; macOS 12 s), then from the cache: live in 0.0 s; 79 hits in 37 s, with x0, sp and the thread (main, guest-127). macOS: 1,704 hits (the game calls this function through a pointer, so the dispatcher's new unit takes every call). |
| Live NOP | The store's page made writable and put back ("code patch at main+0x598994: 4 bytes"), the function recompiled from the patched bytes in 110 s (macOS 1.1 s), live. macOS: the counter stopped (5349 and 5349 three seconds apart): "live for indirect calls", which is how the game calls it. Revert: the original bytes back, the original unit from the cache in 0.0 s. |
| IPS | "IPS patch for main written: cheats/exefs_patches/ff773e90972d544e.ips (17 bytes)": `PATCH`, offset 0x598a94 (0x598994 + the 0x100 header), 4 bytes `1F 20 03 D5`, `EOF`. Rebuilt with it (`pkp_build.py --patches`, the Windows package's tools: "Applied 1 code patch records to main", 245 of 246 objects from the cache, 764 s on the loaded PC), the game runs the NOP as compiled code: on the title screen the counter stayed 0 for 12 s and Find what writes caught nothing, with no live patch. |
| Breakpoint (stop) | Held guest-126 at main+0x598994 (the thread the frames wait for: they stop while it is held, so the held thread keeps the overlay and the steps going); step -> held at main+0x598998; continue -> the next caller held in turn; removing the breakpoint let that thread go (the frames came back, the counter went 1275 -> 1309). |
| Atmosphère import | A hand-written file (`04000000 037C81E8 00001E61`, the counter = 7777) imported into the title's cheat file: "imported 1 cheats", running at once (the counter jumped to 7777 and counted on from there once switched off); switched on again it held 7777 (64 engine writes in 5 s). |
| AOB script | The script made from the signature (`aobscanmodule(ctrstore, main, 0A ?? ?? B9 ?? ?? ?? 52 ?? ?? ?? 72 ?? ?? ?? 52)`, `nop`) patched main+0x598994: "on (1 writes)", its unit the NOP's, from the cache. Saved on, it came back on at the next start (0.05 s after the table loaded); the counter then stood still for 11 s and Find what writes caught no access at all (0 faults in 14 s). That run also showed that a `[DISABLE]` holding only `unregistersymbol` left the NOP in place, so the next `[ENABLE]` no longer found its bytes: such a `[DISABLE]` now restores what `[ENABLE]` wrote (aobscript_test). |
| Pointer scanner | 3 levels: 168 paths from 3,938,092 pointers (macOS on the map: 208 from 5,736,277, the same first path); the path main+0x3725fb8 -> +0xf78, added to the list, led to main+0x37c81e8 again after two restarts. |
| Atmosphère export | 3 cheats: two static writes (opcode 0) and the pointer path (opcodes 5, 4, A). At the next start the engine loaded the file and ran them (3,449 writes, the counter held at the exported 3534); switched off, the toggles went to toggles.txt and held at the following start. |
| Hotkeys | F6, bound to the pointer entry, pressed by the test script with the overlay hidden: "hotkey F6: counter by pointer"; the mini strip went from `[on]  counter by pointer  (F6)` to `[off]`; the binding came back after a restart. |
| Controller | The `minus+rstick` combination on an SDL virtual Xbox pad opened and closed the overlay ("cheats overlay shown" / "hidden"); the D-pad moved the focus through the address list's rows, A on a row's box froze it ("1 frozen", the hold glyph), A on a row's type opened its combo and picked another type, the status bar showed the pad's prompts ((A) select, (Y) edit, (L)(R) window, (+) close), + closed it. SDL updates pads only with a window, so these runs use a hidden window (`NXRT_GFX_HIDDEN=1`), not `--headless`. |
| Pause / frame advance | "game paused at frame 3256 (54 threads parked)"; macOS: the counter stayed 3104 for 2 s, advance 1 -> 3105, advance 1 -> 3106, resume -> 3205. The steps kept running while paused. |
| Speed | A Lua script compares `nx.tick()` with `nx.time()`: x1 19,200,007 ticks a second, x2 38,400,016, x0.5 9,600,003 (Windows); macOS 19,200,004 / 38,400,006 / 9,600,001. |
| Snapshots | The window's own snapshot: 512 MiB at frame 755, compared with memory 3 s later: 1,939 words changed, the frame counter among them (755 -> 782 ^); macOS 256 MiB, 7,635 words. |
| Viewers, labels, graphs | A label `frame_counter` on main+0x37c81e8, used as the address of the memory viewer and the structure view; the disassembler at main+0x598994 shows `str w10, [x8, #0x1e8]` and the function around it; a value graph of the counter (685 .. 742, rising). |
| Threads, memory map | 56 guest threads with priority, core, state and the function each is in; 6,730 blocks, 1,856.6 MiB (macOS screenshot). |
| RomFS mod | "2 of the 19081 files come from the mod folder ... 2 of them replace a game file" on Windows, macOS and native Linux (WSL2, lavapipe); the user select screen shows the mod's text, "CHEATS MOD WORKS" in place of "Delete Save Data" (Windows and Linux screenshots). |

**Live memory editing on macOS** (run mv, the World 1 map, 2026-11-10 evening; wb1 is a default build, ABI 4; wb2 the same game built
with `--live-patch`, ABI 5; both with the 17-byte IPS compiled in):

- **Value writes and freezes in every kind of guest memory** (test steps `poke`, `set`, `freeze`, `restore`), on both builds: main's data
  (main+0x37d1fc0), the heap (heap+0x5b7fffc0), the alias region, a thread's stack (0x7e00000040), shared memory (read-only to the game), a
  GPU memory pool of 128 MiB (flags 0x189, heap+0xfc74fc0) and main's code (read-only to the game; written through the protected-page path):
  each written to 305419896 and read back, frozen at 1234567 and still 1234567 two seconds later, then the original value written back.
- **Live code patches reach direct calls on a default build.** Every patch on wb1 reported "live" (the jump over the compiled entry, written
  by swapping the code page: see "Live code patches on Apple silicon"), where before only the dispatcher's calls ran it. A trace point on
  main+0x205db0, which the game reaches only by `bl` (3,414 in main's code; the host code calls f_205db0 3,445 times, never through a
  pointer), counted 823 hits in 10 s and 1,125 in 15 s. The slow counter's store (main+0x25f73c, `str x10, [x12, #0x608]`): NOP live, the
  counter 135 and 135 four seconds apart; reverted, 139, 142. The same store as an AOB script made by the signature maker (16 bytes, unique
  in main): on, 148 and 148; off, 152, 155. wb2 the same, each patch "live for every call".
- **A value whose writer the game calls by BL** (runs dw2 / dw3: increasing values found by the scanner, each one's writer by the
  hardware watchpoints, the BL in main to the writer's function counted, then that writer's store NOPed). wb1 (ABI 4, the jump): the value
  heap+0x32ca9b7c is written by main+0x9a3d90 (`str w9, [x10]` at main+0x9a3e24), which main calls by one BL (main+0x295a8); NOPed live
  ("recompiled in 1.0 s"), it read 2680, 2680, 2680 over 6 s (2548 and 2651 before); reverted, 3104, 3197. A default build of HEAD (wb3: entry and loop-head checks, 246 of 246
  units compiled and linked in 425 s on the Mac; run dw4): the value heap+0x32ca1304, written by the same main+0x9a3d90 (its start ran 4,980
  times in 15 s), read 4158, 4340; NOPed ("live for every call", 0.5 s), 4373, 4373, 4373 over 6 s; reverted, 5188, 5369.
- **A loop that is already running** (run lp: trace points at the loop around each of the main thread's outer call sites and at those
  functions' starts). main+0x1dd5e60 is the main thread's frame loop: its start never ran in either run. On wb3 its loop head
  (main+0x1dd5e70) entered the recompiled unit 735 times in 15 s and 1,035 in 20 s (about one per frame) with 0 hits at the start: the
  running frame continued in the unit at its next iteration (`NX_OVL`). On wb1 (no loop-head check) the same head had 0 hits in both
  counts. Windows the same (run wlp: Wonder built on the PC with HEAD tools, 246 units in 692 s; the title screen): the frame loop's head
  7,046 then 7,347 hits, its start 0.
  (dw2's pick on wb2, main+0x2ffe80, whose value went on counting after its NOP, is no such loop: its start ran 788 times in dw4's
  15 s. Run dx on wb3 settled it: after the NOP of main+0x3002b8, Find what writes on the value caught 70 writes in 6 s, all from the
  SDK's `nn::os::detail::InternalCriticalSectionImplByHorizon::Enter` (sdk+0x137e24): the word has a second writer, which the one-second
  watch before the NOP had not shown, and the patch itself worked.)
- **Code caves** (macOS (run hkm, Hollow Knight's title, a default build): `mov w8, #77` before the frame counter's store (main+0xa6a5c8, `str w8, [x0, #0x30]`): the unit live in 0.5 s, the counter read 77 and 77 three seconds apart; reverted, 319, 500); on Windows (run w26, the title screen): `mov w10, #4321` before the title counter's store
  (main+0x598994): the counter read 4321 and 4321 three seconds apart; reverted, 4563, 4742.
- **Inlined copies**: the check every function entry carries is compiled into the copies the compiler inlines, so they are reached as
  the function is. Not shown live: the seven functions run pf traced (those main's code calls by BL but its compiled callers no longer
  call: 0x9e85d0 inlined into 1,414 callers, 0xca5200 into 1,088, ...) did not run on the World 1 map in either build (0 hits).
- **The cost of the checks** (run pf, Wonder's World 1 map, the whole GPU held so both builds saw the same idle machine, one after the
  other): CPU time of the whole process per frame over three 15 s windows, wb1 (no checks) 14.09, 14.13 and 13.88 ms at 59.6-60 fps,
  wb3 (entry and loop-head checks: 210,308 + 241,255 of them in main) 12.85, 14.74 and 14.71 ms (the last window at 50 fps). The
  difference is inside the window-to-window spread (about 1 ms): not measurable this way. Frame rate comes last; the checks
  are the default either way. The same run's code cave held the slow counter at 4321 on both builds.
- **IPS on macOS:** in both builds the title's frame counter (main+0x37c81e8, whose store the IPS turns into a NOP) read 0 and 0 four
  seconds apart on the map and Find what writes caught no access in 6 s; in a build without the IPS the same counter counts on the map
  (the Freeze row above: 2,943 writes in 3 s).
- **Auto accent** from three game frames: Wonder's title screen (Windows) #8f97a9 -> accent #7997db, Wonder's World 1 map (macOS) #7d7f5b
  -> #d6db79, Hollow Knight's title (Windows) #334e8e -> #4f78db.

**Every scan type and next-scan mode, live** (Windows, Wonder's title screen, the game paused; runs w22, w23, w24): a heap word only the
test writes (heap+0x5b7fffc0), for each of byte, 2, 4 and 8 bytes, float and double: an unknown first scan over 1,762.5 MiB, then decreased
by 10, increased by 20, increased, decreased, unchanged, changed, between, bigger, smaller, same as the first scan, exact: the word was the
one result of every step. Float exact 1000.5: 3 results (the word and two other 1000.5 floats); all numeric types exact 5000123: 1 (the
word); UTF-16 "WQZX" written into the word: 1, ignore-case "wqzx": 1; UTF-8 ignore-case "NN::VFX2": 5 (main holds that text 5 times).
Fixed on the way (c5f6d07, eddc21a; scanner_test): a byte "smaller than 200" took 200 as the signed byte -56; floats used a 1e-3 relative
tolerance ("exact 1000.5" kept 4,043 results, "changed" missed 1012.5 -> 1013.5), and then a size-scaled one made every unchanged large
float "decreased by 10". w21's empty next scans were the test's own: a frozen entry left in that title's table put the counter back to
4990 a second after the test unfroze its own (w22: 5000, then 4990); w21's "NN::OS" found nothing because neither main nor sdk holds it.

**Lua:** with F7 pressed by the test script, `nx.hotkey("F7", ...)` ran ("hotkey F7 pressed at frame 4225", twice; w22).

**Find what writes with hardware watchpoints** (9d52794): Windows, Wonder's title counter (w25): "a hardware watchpoint in 55 threads",
299 writes in 5 s, all from main+0x598920; Hollow Knight's frame counter (hk6): 36 threads, 303 writes in 5 s from main+0xa6a5c0, where the
page protection had caught none in three runs (hk3 to hk5). macOS (hkm): "a hardware watchpoint in 36 threads", 58 writes in 5 s (one per thread per engine pass: the trap comes before the store, so the handler switches it off until the next pass), all from main+0xa6a5c0.

**Hollow Knight** (Unity / IL2CPP; Windows runs hk1 to hk6 with the perf agent's build, macOS run hkm with a build made on the Mac):
- Scan: the game paused, an unknown first scan (1,025 MiB), frame advance 1, "increased by 1", 1, "increased by 1", 5, "increased by 5":
  327 values that count the frames, alias+0x44dba3e0 the first (3391 -> 3398).
- Freeze at 7: read 8 and 10 over 5 s (the engine's 36 writes against the game's own each frame); unfrozen, it counts on (132 two seconds
  later).
- Pointer: 3 levels, 1,012 paths from 9,686,503 pointers; main+0x669b548 -> +0x90 led to the counter again after each restart (hk4, hk5,
  hk6) , and on the Mac build too (hkm: the alias region lies elsewhere on macOS, so the plain address entry made on Windows reads "??" there while the pointer entry finds the counter).
- AOB script: made from the writer's store (main+0xa6a5c8, `08 ?? ?? B9 08 ?? ?? B9 ?? ?? ?? 34 FF ?? ?? D1`, 16 bytes, unique in main) on
  Windows; saved with the table and copied to the Mac with a build of Hollow Knight made there (hkm): at the start the script found its code by the Windows signature and switched itself on ("on (1 writes)", the function recompiled in 0.5 s, live by the jump), and the counter stood at 34 and 34 four seconds apart; switched off, it counted (275, 457). (The Windows build of Hollow Knight has no on-demand compiler, so there the patch waits for the next build.)
- The first "increased" value hk1 picked (alias+0x3ff01180, about 180,000 a second) is updated from the host side: Find what writes saw no
  access at all while it changed, and freezing it stopped the game's frames. The overlay now says so (d235318).

**Controller and the on-screen keyboard** (an SDL virtual Xbox pad, a hidden window): Windows (p3, Wonder's title screen): the focus put on
the Address list's first button as the D-pad leaves it (test step `focus`), D-pad right twice reached the group filter, Y opened the
on-screen keyboard on it ("Keyboard: group filter"), A typed, + confirmed: the filter read "qt" and the table showed no entry of such a
group (a 300 ms D-pad hold repeated once in the keyboard: "t", not "w"); the combination closed the overlay. macOS (p4, Wonder's title screen): the same presses (the D-pad held 120 ms) typed "qw", + confirmed, the filter read "qw"; no permission prompt (a virtual pad, a hidden window).

## Save states: not feasible natively (investigated)

A save state must capture and later restore everything the running game is. Here that is:

- **Guest memory**: the heap (gigabytes: a Wonder QA run met the memory guard at 8 GB, FINDINGS.md), the alias region, the modules' data and bss, every thread's
  guest stack and TLS. Copyable (Snapshots does it for up to 512 MiB), but large.
- **Thread state is not only the guest registers.** Lifted code calls lifted code as host C++ calls, so a guest thread's call chain lives on
  its host thread's stack (64 MiB each, `[threads] host_stack_mb`: lifted code nests deeply) as host return addresses and spilled host
  registers, interleaved with runtime frames (an SVC waiting in the kernel, an HLE import, a service request in progress). A guest
  context (`Ctx`) only names the function a thread is in (`Ctx.fn`), not where in the host code it is. Restoring a thread means restoring
  its host stack and host registers: a process checkpoint (as CRIU does on Linux), not something a Windows or macOS process can do to
  itself.
- **Kernel and service objects**: handles, events, mutex owners, waiters, timers, every service session's state (fs files open at an
  offset, audio renderer voices, hid shared memory, nvn/vi layers), all host C++ objects pointing at each other and at host resources.
- **GPU state**: textures, buffers, pipelines and in-flight work live in Vulkan / Metal objects on the host GPU; the game's NVN memory pools
  are mirrored into them by the runtime's caches.

Restoring guest memory alone while threads, objects and the GPU keep their own state leaves the game inconsistent (a pointer in memory to
an object that no longer exists, a thread mid-function whose locals were restored to another frame). A faithful save state would need the
whole process checkpointed, which needs the operating system's help that neither Windows nor macOS offers, or a runtime in which no guest
state lives on host stacks, which is an interpreter (out of the question). **Save states are therefore not built.** The game's own saves,
the cheat table (values, freezes, pointer paths) and Snapshots (memory diffs) cover what a cheat user needs from them.

## Mods (game files and code patches)

Mods have their own page and document: [MODS.md](MODS.md). In short, the Mods page (mods.py) installs a mod in any common layout;
game file mods are layered over the RomFS through `[fs] romfs_mod` (a folder, or the mod manager's layer list), and code patches
(IPS / IPS32 / .pchtxt, by build id, Atmosphère's offsets) make the effective ExeFS that the next build (**Build with mods**) lifts and
compiles natively, so a patch becomes compiled code. A patch made with **Code > Write the IPS patches** goes into a mod's `exefs/`.

## Cheat tables (our own format)

Saved per title and build id next to the title, outside the repository: `<cheats dir>/<build id>.cheattable.json` (the table) and
`<build id>.overlay.json` (which windows are open, and where). The table holds `entries` — `{description, type (0..5: u8 u16 u32 u64 f32
f64), value, freeze, freeze_mode (0 hold, 1 allow increases, 2 allow decreases), hotkey_name ("F5", "L+R+A"), hotkey_action (0 toggle,
1 write the value, 2 add 1, 3 subtract 1), group}` with either `address`, a `pointer` `{module, mem_type, base_offset, offsets[]}`, a
`signature` with `sig_offset` (AOB-anchored: the address is found again by its bytes) or a `script` (an AOB script; `freeze` = on) —
plus `labels` `[{address, name}]` and `lua` `[{name, run, source}]`. Frozen entries, scripts that were on and Lua scripts that were running
come back at the next start. The overlay saves on hide and every few seconds after a change.

**Atmosphère.** The engine runs the title's own cheat file as a console does (`<cheats dir>/<program id>/cheats/<build id>.txt` or a flat
`<build id>.txt`, `toggles.txt` beside it, the full cheat VM, 12 passes a second; with no `toggles.txt` every cheat starts on, as on a
console, so a file that should start off ships with one that says `false` for each). Code cheats become live code patches ("ASM
cheats" above). Cheat files: Import appends another file's cheats to the
title's file; Export writes every value entry in main or the heap (opcode 0: a static write) and every pointer path rooted in main
(opcodes 5, 4, A: load through the path, store) to it, so the same cheats run on a console, Ryujinx or a yuzu fork.
