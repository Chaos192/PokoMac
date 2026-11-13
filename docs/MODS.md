<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch). -->
# Mods

PokoMac installs the mods people make for Pokopia, and helps you make your own. Mods work with the normal PokoMac build of the game.
Nothing is emulated or interpreted:
- **Game file mods** (textures, models, text, sound, levels) are layered over the game's files when it starts.
- **Code mods** (IPS / IPS32 / IPSwitch patches, or a replaced code module of the same game version) are compiled into the game natively
  the next time it is built. Only the changed code is compiled again.

The mod manager is AnySwitch's (by Chaos192, [github.com/Chaos192/AnySwitch](https://github.com/Chaos192/AnySwitch)), brought to PokoMac
for its one game. Cheats are covered in [CHEATS.md](CHEATS.md). A mod can carry cheat files too: they are loaded with the game's own.

## Installing a mod

Open **Mods** in the sidebar:

- **Install from a file** takes a mod as it was downloaded: a `.zip` or `.7z`. A `.rar`, or a 7z packed with BCJ2, needs a 7-Zip or
  bsdtar on the computer (macOS has bsdtar).
- **Install from a folder** takes an unpacked mod.
- Every mod has an on/off switch, and **Up / Down** to set the order. A mod higher in the list wins over the ones below it where both
  change the same file or the same code. The list says which mods share a file and which one wins.
- **Uninstall** removes the mod's files from the mods folder (`mods/` in the game folder).
- When code mods change, the page and Home show **Build with mods**. That build (Set up) compiles only the changed code, and your
  saves and shaders are kept. Game file mods need no build: they apply the next time the game starts.

Mods change only while the game is closed and Set up is not running. PokoMac reads the common layouts. One archive may hold several
mods: each one is installed as its own mod.

| Layout | Where the files are |
|---|---|
| Atmosphère (console SD card) | `atmosphere/contents/<title id>/romfs/`, `exefs/`, `cheats/`; code patches in `atmosphere/exefs_patches/<patch name>/<build id>.ips` |
| yuzu, Ryujinx and their forks | `<title id>/<mod name>/romfs/`, `romfs_ext/`, `exefs/`, `cheats/` (or `<mod name>/romfs/` with no title id); Ryujinx's `exefs_patches/<set>/`, `romfs.bin` and `exefs.nsp` |
| A patch set | `<build id>.ips`, `.ips32` or `.pchtxt` files, alone or in one folder |
| A bare game file tree | The game's own folders, alone or in one folder |

Other details:
- `romfslite` is read as `romfs`.
- A `<file>.stub` deletes a game file: in `romfs_ext/` (yuzu), and in `romfs/` when the game has a file of that name without `.stub`.
- A `romfs_ext/<file>.ips` patches one game file.

**What is refused, and why:**
- **A mod for another game.** The title id in its folders, or in its `mod.json`, is not Pokopia's.
- **A mod for another version of the game.** None of its code patches or cheats names this game's build id, and it changes nothing
  else. The message names both builds. Code is made for one exact build: a patch for another version writes into the wrong
  instructions. A mod that ships patches for several versions installs, and only the matching patches are used.
- **What PokoMac cannot use**, each with its reason:
  - a code module the mod adds (a plugin loader such as skyline's `subsdk9`) or removes (`<module>.stub` in `exefs/`): PokoMac builds
    the game's own four modules (`main`, `sdk`, `subsdk0`, `rtld`);
  - a replaced module of another build, and a `main.npdm`;
  - NRO plugins and NRO patches: Pokopia loads no NRO;
  - a `frame_rate.ini` (AnySwitch's 60 fps mods): PokoMac has no frame-rate engine. Beside code patches or game files it is named as
    not used, and the rest of the mod installs.

## Making a mod

### Start from your own copy of the game

Use **Mods > Make a mod > Game files: Export**, or `mods.py export <game folder> --out <folder> [--romfs] [--romfs-path <part>]`. It
writes `<out>/<title id>/`:
- `info.json`: the title id and each code module's build id. `patch_name` is the name its patch file takes.
- `exefs/<module>`: the code modules, uncompressed. **A file offset in these files is exactly the offset an IPS/IPS32 patch uses.**
  Load them into a disassembler (Ghidra, IDA, Binary Ninja: the NSO loaders read them) to find what to change.
- `romfs/` (with **Game files**): the game's files, or only those under `--romfs-path`. They are linked to the game's own files where
  the drive allows it, so they take no extra space. **Do not edit them in place: copy the ones you change into your mod.**

Everything comes from your own dump. Never share the game's files, only your changes.

**New mod** (`mods.py template`) makes a folder to fill in:

```
My mod/
  mod.json                          name, title id, version, author
  romfs/                            game files to replace or add
  exefs/<build id>.pchtxt.example   a code patch skeleton for main
  cheats/<build id>.txt.example     a cheat skeleton
```

### A game file mod

Put each file at the same path the game's RomFS has it: `romfs/<folder>/<file>` replaces the game's `<folder>/<file>`. Folders merge,
and a file only your mod has is added. To remove a game file, put an empty `romfs_ext/<its path>.stub`. Test it by installing the
folder (Install from a folder). The change shows at the next start.

### A code patch

Patches are named by the module's build id (the first 16 hex digits are enough):

- **IPS** (`<build id>.ips`: `PATCH`, records of a 3-byte offset, 2-byte size and bytes, `EOF`), or **IPS32** (`.ips32`: `IPS32`,
  4-byte offsets, `EEOF`). The offset is 0x100 (the module header) plus the offset in the mapped module (text at 0), as Atmosphère
  reads it. That is the file offset in the exported `exefs/<module>`.
- **IPSwitch** (`.pchtxt`), easier to write by hand:

  ```
  @nsobid-<build id of the module>
  @flag offset_shift 0x100        // the offsets below are mapped offsets (text at 0)
  @enabled
  00123456 1F2003D5               // a NOP (the bytes as stored, little-endian)
  00ABC000 "New text\0"           // a string
  @disabled                       // a patch that is off
  00222222 00008052
  @stop
  ```

  `@little-endian` (the default) or `@big-endian` sets how hex values are read. `//` or `#` starts a comment, and lines that are not
  records (credits) are ignored.

Patches can change code and data (.rodata, .data). The patched module is what the game runs and what it reads back, so a patch to a
constant works too.

Put the patch in `exefs/` of your mod (or in a patch-set folder). A replaced module of the same build goes there too, under its own
name (`main`, `sdk`, `subsdk0`, `rtld`). Press **Build with mods** after installing: the patched modules are compiled natively, and
only the units that changed are compiled again.

A patch made while playing, with the cheats overlay's **Code > Write the IPS patches** ([CHEATS.md](CHEATS.md)), is written to
`<cheats folder>/exefs_patches/<build id>.ips`: copy it into your mod's `exefs/`.

### A cheat

`cheats/<build id of main>.txt` in the Atmosphère cheat format (see [CHEATS.md](CHEATS.md)). The mod's cheats are listed with the game's
own in the cheats overlay, and each one is turned on or off there.

### Packaging

**Share > Pack** (`mods.py pack <folder>`) zips the folder as `<name>/romfs/...`, `<name>/exefs/...`, `<name>/mod.json`, leaving the
`.example` files out. yuzu, Ryujinx, AnySwitch and PokoMac all install this layout. For a console, copy `romfs/`, `exefs/` and `cheats/`
to `atmosphere/contents/<title id>/` and put the `.ips` files in `atmosphere/exefs_patches/<name>/`. Atmosphère does not read `.pchtxt`:
convert it to IPS first.

## How it works (for developers)

- **Store.** Mods live in `<game folder>/mods/`: `mods.json` (the order and on/off), one normalized folder per mod (`romfs/`,
  `romfs_ext/`, `exefs/`, `patches/`, `cheats/`, `mod.json`), and `.active/`, which `mods.py resolve` makes again after every change:
  - `romfs_layers.txt`: the RomFS layer list. `delete <path>` and `layer <folder>` lines, lowest priority first, relative to the list.
    The runtime reads it through `[fs] romfs_mod` (`runtime/src/services/fs_romfs.cpp`, `apply_layer_list`). A folder in that key
    still works as one layer (Settings > Game > Mods), and a folder set there before mods were installed stays as the lowest layer.
  - `romfs_gen/<mod>/`: game files a mod patches (`romfs_ext/*.ips`), each layered just above its mod.
  - `exefs-<stamp>/`: the effective ExeFS of the enabled code mods. Unchanged modules are hardlinks to the game's where the drive
    allows it, else copies. Patched ones are written with each segment at 0x100 + its memory offset (`apply_ips.patch_nso_many`).
    Replaced modules come from the mod.
  - `code.json` (`stamp`, `exefs`): what the next build compiles. An empty stamp means no code mod is on.
  - `cheats.txt`: the enabled mods' cheat files for this build. The cheat engine reads it through `[cheats] mod_files`
    (`runtime/src/cheats/engine.cpp`).
  - `conflicts.json`: what two enabled mods both change, and which one wins.
- **Build.** `pkp_build.py` verifies the dump as always, then lifts `code.json`'s ExeFS in place of the dump's (`mods_code`), and the
  object cache compiles only the changed units. The stamp in `build/done.json` (`mods`) is the code mods' stamp, so `pkp_build.py
  --check` and the launcher (`mods.rs`, Home's **Build with mods**) see when they changed. Code made again on demand is lifted from
  the same bytes, so data patches hold. Once the build is done, `mods.py built` writes `[fs] exefs_mod` (`pkp_main.cpp`), so the
  runtime maps exactly the modules that were compiled.
- **No mods.** A game folder with no mod installed has no `mods/.active/code.json`, and builds and plays exactly as before: the build
  stamp is the same as without mod support.
- **The command line:** `python -I tools/build/mods.py scan|install|list|enable|disable|move|uninstall|resolve|built|export|template|pack ...`
  (JSON out; `--store` and `--no-ini` for a test store that leaves the game's settings alone).
- **Tests:**
  - `game-build/tests/test_mods.py`: every layout, zip and 7z, stubs, order and conflicts, the refusals (PokoMac's included), the
    effective ExeFS and what pkp_build takes from it, pkp.ini, export, template and pack.
  - `game-build/tests/test_apply_ips.py`.
  - `runtime/tests/romfs_layers/romfs_layers_test.cpp` (`nxrt_romfs_layers_test`): the layer list, a single mod folder, the base layer.
  - `launcher/scripts/test-mods-model.mjs`, and cargo test `mods::`.

## Limits

- **Switch 2 modules compressed with ZBIC** are patched from plain copies. Pokopia's modules are LZ4, which the mod manager reads itself.
  A replacement module in a mod must be plain or LZ4.
- Plugin loaders (skyline and similar), NRO plugins, NRO patches and 60 fps `frame_rate.ini` blocks are AnySwitch features PokoMac does
  not have; such mods are refused with the reason (above).

Sources: Ryujinx's mod loader (MIT; `src/Ryujinx.HLE/HOS/ModLoader.cs` ApplyNsoPatches and `Loaders/Mods/MemPatch.cs`: NSO patches 0x100
from the header) for the patch offsets; the layouts and formats are documented from Lichii (yuzu family, GPL:
`src/core/file_sys/patch_manager.cpp`, `ips_layer.cpp`, `fsmitm_romfsbuild.cpp`; facts only, no code taken) and from Atmosphère's and
Ryujinx's documented mod folders.
