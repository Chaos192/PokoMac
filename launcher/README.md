# PokoMac launcher

Tauri 2 launcher for the Pokopia PC port (`pkp.exe`). It ships no game files and never changes yours. It reads the game
files you extracted from your own copy, writes `pkp.ini` next to `pkp.exe` and builds the shader cache from your romfs.

## First run

1. **Game files**: pick the dump folder (`<dump>\base\{exefs,romfs}` and/or `<dump>\updates\<version>\{exefs,romfs}`).
   The newest complete update is used, otherwise the base. "Use these game files" writes `[fs] exefs` and `[fs] romfs`.
2. **Set up**: writes the `pkp.ini` paths (`[fs] exefs`, `[fs] romfs`, `[fs] save_dir`, `[nvn] shader_cache =
   shader_cache/spirv`) and builds `shader_cache\spirv\` next to `pkp.exe`:
   - Extract: `tools\shaders\extract_corpus.py` finds every shader in the romfs and in `exefs\main` and writes the
     distinct blocks to `shader_cache\corpus\` (a few pack files, about 5 GB, deleted when setup finishes).
   - Translate: `tools\spirv_harness\spirv_harness.exe --batch` in chunks of 1000 into `shader_cache\spirv\`
     (`<hash>.spv` + `results.tsv`, about 26 GB for the 99,012 blocks of 2.0.1).
   - Pack (on by default, "Packed shader files" on the Set up page): each translated chunk is moved into
     `shader_cache\spirv.archive\NNNNNN.pack` (zstd segments the runtime reads, `src-tauri/src/shader_archive.rs`) as soon as it
     is committed, so the loose files never pile up. A cache that already has loose files keeps them until the player packs it.
   A progress bar shows each step. Pause stops the tools and Continue setup picks up where they stopped: a block listed
   in `results.tsv` is never translated twice, and extraction is skipped if it already ran for the same romfs. The tools
   run at below-normal priority.
   State: `shader_cache\setup.json`.
   Before it starts, Set up checks the free disk space on the game drive (it refuses to start without about 14 GB with packed shader files or 31 GB with loose ones, less
   when resumed), Vulkan 1.3 (vulkan-1.dll and a GPU), RAM, and warns about 13th/14th generation Intel desktop CPUs
   whose microcode is older than 0x12B (a BIOS update fixes their crashes under long loads). Warnings can be continued
   past. "Use fewer cores" runs setup with a quarter of the threads (2 to 8) at idle priority.
   **Shader files** (Settings > Game, and a question at launcher start while loose files are left): "Free up space" moves the loose
   `.spv` files into the archive without translating anything (a segment is flushed and renamed before its loose files are deleted;
   it can be stopped and continued). "Use loose files" writes them back out. Neither runs while the game, setup or an update runs.
   Everything setup does is appended to `setup.log` next to `pkp.exe` (UTC timestamps, the commands, the tools' error
   output; rotated to `setup.log.prev` at 2 MB). The Set up card has an "Open setup log" button; attach the file to
   bug reports about a failed setup.
3. **Play** turns on once setup has finished. The launcher starts pkp.exe with no paths on the command line. A `pkp.ini`
   next to it makes its folder the working directory, and every path comes from the ini (relative paths start there).

`release\Play.bat` starts the game the same way without the launcher (after setup).
`PokoMac --play [--game <folder>]` does what Play does without the window (it also prepares the run-time shaders
before the start and translates the new ones after it) and waits for the game: for a Steam, Sunshine or desktop shortcut.
The game folder is the launcher's own folder unless `--game` names another.

## Release model (b): pkp.exe built on the player's PC

When `tools\build\pkp_build.py` is present (the package from the main project's release script), the release holds no
`pkp.exe`. Set up first builds it from the chosen dump (`src-tauri/src/gamebuild.rs`): verify the ExeFS by hash, lift,
compile with the bundled Zig, link against `pkp_rt.dll`; then the shader steps below run. Play stays off until both are
done. Pause works in every step and Continue setup resumes. A built pkp.exe counts only while `build\done.json` matches
the tools of the release in use (`pkp_build.py --check`: lifter, `main.sites`, compile flags, SDK files, `pkp_rt.lib`), so
an update extracted over an install that changes any of them turns Play off with "This update changes the game code"
until Set up rebuilds pkp.exe (the shader cache is kept). Headless check of the whole first run:
`cargo run --release --example first_run -- <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]`.

## Release folder

The launcher looks for its tools in `tools\` next to its own exe. A release folder must contain:

```
pkp.exe                     the game (game-build, -Release)
PokoMac.exe         this launcher (npm run tauri build)
Play.bat                    release\Play.bat (optional)
tools\
  spirv_harness\            self-contained publish of nx tools/shaders/harness:
                              dotnet publish tools\shaders\harness\SpirvHarness.csproj -c Release -r win-x64
                                --self-contained true -o <release>\tools\spirv_harness     (~84 MB)
                            (stage the Ryujinx sources first: tools\shaders\scripts\setup_ryujinx.ps1)
  shaders\extract_corpus.py nx tools/shaders/ampere/extract_corpus.py
  python\python.exe         optional: Python 3 (for example the embeddable package) with the lz4 package installed.
                            Without it the first python.exe on PATH that can `import lz4.block` is used.
```

If a file is missing, setup stops and names it. `pkp.exe` can live in another folder (Settings, game folder), but
`tools\` always sits next to the launcher.

## Linux (SteamOS, the Steam Deck, Bazzite, other distributions)

The launcher also builds as a native Linux program (`npm run tauri build -- --no-bundle` on Linux, or an AppImage with
`--bundles appimage`). Tauri draws it with WebKitGTK there, so it needs no WebView2, which Proton lacks (the Windows launcher
does not start under Proton). Releases ship it as `PokoMac-<version>-linux-x64.tar.gz`: the Windows package with
`PokoMac.AppImage` in place of `PokoMac.exe` (built by the `linux` job of `.github/workflows/release.yml`). A
build of your own goes in the release folder next to `tools/`. The game stays the Windows
`pkp.exe`; what differs on Linux is in `src-tauri/src/host.rs`:

- **Build tools:** the first Set up downloads Python 3.13 (python-build-standalone) with the lz4 and capstone wheels, and
  Zig 0.17.0 (the version the release bundles for Windows), into `tools/linux/`, each pinned by size and SHA-256.
  `pkp_build.py` compiles with that Zig and reads free memory from `/proc/meminfo` there.
- **Proton:** the shader translator and `pkp.exe` (the hidden first run and Play) run through Steam's Proton Experimental,
  else the newest Proton N.N Steam installed, inside the Steam Linux Runtime its `toolmanifest.vdf` asks for, as Steam starts a
  game, with the prefix in `~/.local/share/pokomac/proton`. `PKP_PROTON=<folder>` picks another Proton build. Without
  Steam: `umu-run`, then `wine`. Paths handed to them are `Z:\...`. Linux translates shaders in chunks of 5000 (every run also
  starts Proton); Pause kills the tool's process group, Proton's wine processes included; the hidden first run is stopped if it
  is still going 4 minutes after its 60 s.
  Every Proton run gets GPU driver shader caches of the game's own that are never trimmed
  (`~/.local/share/pokomac/shadercache/{nvidia,mesa}`), as Steam sets them up for the games it starts.
- **The checks before Set up** read statvfs, `/proc` and `libvulkan.so.1`, and Set up refuses to start without Proton (it says
  so before the build, not after it).
- **Desktop:** Link Play lists addresses from getifaddrs and `/proc/net/route` (Tailscale and ZeroTier as overlays; bridges,
  veth and VPN interfaces skipped), and the firewall hint names firewalld; save import looks in `~/.local/share/eden` and
  `~/.config/Ryujinx`; files and links open with `xdg-open`; on NVIDIA the launcher sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` (a
  blank window otherwise).
- **AppImage:** `tools/` is looked up next to the `.AppImage` file (`$APPIMAGE`), and the programs the launcher starts get the
  environment without the variables the AppImage runtime points into its mount.
- **Building** needs Rust, Node.js and the webkit2gtk-4.1, gtk3 and libsoup3 development packages. Built on Ubuntu 22.04 the
  binary and the AppImage need only glibc 2.35.

Tested on Bazzite 45 (Ryzen 9 9900X, 16 GB, RTX 4070 Ti, Proton Experimental in the Steam Linux Runtime 4), end to end with a
2.0.1 dump: Set up built pkp.exe in 12 minutes, translated the 99,012 shaders in 17 minutes (also paused and continued in the
middle), the hidden first run collected 212 shaders, and Play ran the game; from the native binary and from the AppImage.

## macOS (Apple silicon, macOS 14.4 or newer)

On macOS everything is native: the runtime is `libpkp_rt.dylib`, Set up links the game as the arm64 program `pkp`, and the shader
translator is an osx-arm64 build, so nothing runs through Wine or Proton (`src-tauri/src/host.rs`). The launcher is
`PokoMac.app` (`npm run tauri build -- --bundles app`; `src-tauri/tauri.macos.conf.json` adds the bundle settings: ad hoc
signature, LSMinimumSystemVersion 14.4, `icons/icon.icns`). The app must stay as it was signed, so nothing is ever written into it:

- **In the app** (`Contents/Resources`, read only): `tools/` as in the Windows package (`build/` with pkp_build.py, the lifter and
  `sdk/libpkp_rt.dylib`; `spirv_harness/spirv_harness`; `shaders/extract_corpus.py`) and `runtime/`, the files the game needs
  beside it: `libpkp_rt.dylib`, the Vulkan loader `libvulkan.1.dylib`, the drivers `libvulkan_kosmickrisp.dylib` (default) and
  `libMoltenVK.dylib` with their manifests in `vulkan/icd.d/` (library_path `../../<dylib>`), `fonts/` and `assets/`.
- **Game folder**: `~/Library/Application Support/PokoMac` unless Settings names another. Before every run (Play, `--play`, the
  hidden first run) the launcher copies `runtime/` into it, file by file when missing or different, each written beside its target
  and renamed over it, so an updated app refreshes it. The runtime picks the Vulkan driver itself (VK_DRIVER_FILES is left to the
  player's environment).
- **Build tools**: the first Set up downloads Python 3.13 (python-build-standalone, aarch64-apple-darwin) with the macOS arm64 lz4
  and capstone wheels, and Zig 0.17.0 (aarch64-macos), into `~/Library/Application Support/PokoMac/tools/macos`, each pinned by size
  and SHA-256 as on Linux; pkp_build.py gets that Zig as `PKP_ZIG`. A launcher run outside a bundle (cargo run) keeps them in
  `tools/macos` next to its own `tools/`.
- **The checks before Set up** read the free space (statvfs), the RAM, the processor and the macOS version (sysctl): macOS 14.4 or
  newer and Apple silicon are required; Vulkan is the app's own runtime, so its files are checked instead of a driver.
- **Desktop**: save import looks in `~/Library/Application Support/Ryujinx` and `~/Library/Application Support/eden` (or
  `~/.local/share/eden`); Steam's shortcuts in `~/Library/Application Support/Steam`; files and links open with `open`; Link Play
  reads the default route from `route` and the hardware ports from `networksetup`; DLSS is not offered (no NVIDIA GPU). A crash ends
  `pkp` with a signal, which the message after the game names.
- **Updates**: the package is `PokoMac-<version>-macos-arm64.zip`; the in-app update replaces `PokoMac.app` as a
  whole (the old one becomes `PokoMac.app.old` until the next start) and starts the new one with `open`.
- **Release**: `game-build/scripts/make-release-mac.sh` assembles the package from the built app, `build/mac/libpkp_rt.dylib`, the
  osx-arm64 translator (`tools/shaders/scripts/setup_ryujinx.sh`, then `dotnet publish -c Release -r osx-arm64 --self-contained true
  tools/shaders/harness/SpirvHarness.csproj`) and the Vulkan SDK, signs the app again ad hoc and zips it. Players open an ad hoc
  signed app with right-click, Open the first time.

## Development

- `cargo check` in `src-tauri`; `npm run tauri build` for the exe.
- `cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]` runs the setup's
  shader steps without the window (same code, `src-tauri/src/shaders.rs`); `limit` translates only that many blocks.
