# PokoMac: how the macOS port works

PokoMac recompiles Pokémon Pokopia on your Mac, from your own dump, into a native arm64 program, `pkp`, that runs on the same runtime as
the Windows build of the project it is based on, with graphics on Vulkan over Metal. No emulator, no Wine, no Rosetta. Players: see the
[README](../README.md).

## Differences from Windows

- **Graphics** run on Vulkan over Metal. The default driver is **KosmicKrisp** (LunarG's conformant Vulkan 1.4 driver for Apple silicon);
  **MoltenVK** is included as well: set `[gfx] vulkan_driver = moltenvk` in `pkp.ini` to use it.
- **Upscaling:** **MetalFX**, Apple's temporal upscaler, is the default (below). DLSS and FSR 3 are PC components and are not
  available; FSR 1 and Nearest work as on Windows.
- **HDR**: on an XDR or HDR display the game outputs HDR10 through macOS's EDR; the peak brightness comes from the display.
- **Controllers** work through SDL (DualSense, DualShock 4, Xbox, Switch Pro and more), keyboard and mouse as on Windows.
- **Link Play** works with Windows, Linux and pokopia-native players. macOS asks once whether the app may use the local network: allow it.

## Building from source on a Mac

You need Xcode (or its command line tools), CMake 3.25+, Ninja, Python 3.13 with `capstone` and `lz4`, Rust, Node.js, the .NET 10 SDK and
the Vulkan SDK for macOS (for development: the release carries its own Vulkan loader and drivers).

```sh
# 1. ASTC -> BC7 transcoder (NEON build)
tools/textures/scripts/build_mac.sh
# 2. libvpx for the movies (static, into ~/Library/Caches/nxrt/libvpx)
runtime/scripts/build_libvpx_mac.sh
# 3. the runtime: libpkp_rt.dylib
cmake -S game-build -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DPKP_RT_DLL=ON \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4
cmake --build build/mac --target pkp_rt
# 4. the shader translator (self-contained osx-arm64)
tools/shaders/scripts/setup_ryujinx.sh <ryujinx checkout>
dotnet publish -c Release -r osx-arm64 --self-contained true tools/shaders/harness/SpirvHarness.csproj -o build/mac-harness
# 5. the launcher app and the release package
(cd launcher && npm install && npm run tauri build -- --bundles app)
game-build/scripts/make-release-mac.sh --version <version>
```

## How the port works

The original Windows runtime is kept as it is; the macOS build adds:

- **`runtime/platform/posix` ("nxwin")**: the part of the Win32 API the runtime uses, implemented over POSIX and Mach and found as
  `<windows.h>`, `<winsock2.h>` and friends on the include path of the macOS build only. Locks and condition variables over
  `os_sync_wait_on_address`; threads, events, waitable timers, named mutexes and processes as reference-counted kernel objects; files with
  UTF-8 paths; `VirtualAlloc` and sections over `mach_vm` and Mach named memory entries (every view shares the same pages); vectored
  exception handlers and `__try` over POSIX signals; Winsock over BSD sockets. Most of the runtime's sources compile unchanged.
- **`runtime/platform/mac`**: the macOS pieces where Windows uses a system component: Core Audio output (SDL3), the EDR display query,
  symbols through `dladdr`, the Vulkan loader search, AVFoundation for `movie::` (Switch 1 titles), and the load-time claim of the guest
  address window. Pokopia's own movies (WebM, VP9) go through `nn::codec` on libvpx, linked statically.
- **The guest address space.** Guest pointers are host pointers. macOS keeps the shared cache and a kernel hole below `0x70_0000_0000`, so
  the guest window is `[0x71_0000_0000, 0x80_0000_0000)`: the module images at their lifted addresses, the alias region at `0x72` and the
  remote region at `0x7D` (the other regions keep their Windows addresses). For executables built with a newer SDK, the kernel slides a
  23 GiB data range into that window at random; `pkp` therefore carries SDK version 12.0 in its Mach-O header, which keeps the classic
  layout, and the runtime holds the whole window from the moment it loads.
- **16 KiB pages.** Apple silicon protects memory in 16 KiB pages, the guest in 4 KiB pages. The write tracker of the NVN back end works
  in host pages behind its 4 KiB interface; protection changes never take access away from a guest page that shares a host page.
- **The game program** is linked on your Mac by `pkp_build.py` with the downloaded Zig (`-target aarch64-macos`), against
  `libpkp_rt.dylib`, then signed ad hoc.
- **Memory ordering.** The lifter already emits C11 atomics for the guest's barriers and acquire/release instructions, so the generated code
  is correct on an ARM host, which is weakly ordered like the guest.

## Graphics on Metal

Both drivers turn every Vulkan pipeline into MSL source and hand it to Apple's shader compiler, which takes about a second for a pipeline
it has not seen and, for a few of Pokopia's, much longer. The game skips a draw whose pipeline is still being built, so the port makes sure
the pipelines are built before they are needed:

- **Set up compiles the opening ahead.** The release carries `pipeline_seed.nxpk`: the pipeline keys (shader hashes and render state, no
  shader code) of the title, a new game, the character creator and the first area, recorded on this port. Set up runs
  `pkp --prepare-pipelines`, which adds them to the player's own key file and builds each once on a headless device (about 50 s with
  KosmicKrisp and 20 s with MoltenVK on an M5 Max). macOS keeps the compiled code in its own Metal cache, so at Play the game's prewarm
  finds them in well under a second. A state file per driver (`pipelines-<driver>.state`) records what was built; every Play runs the
  same command with `--if-needed`, which only builds what is new (an update with a larger seed, a driver update, or keys that waited for a
  shader the game makes while it runs).
- **Shaders the game makes while it runs** are translated during play, every few seconds (`launcher/src-tauri/src/shaders.rs`
  `translate_live`), and the running game picks them up; the Windows build only does this between sessions.
- **More builds at once.** Metal's compiler service runs as many compilations at once as the GPU allows (`shouldMaximizeConcurrentCompilation`,
  `runtime/platform/mac/gfx/metal_mac.mm`; MoltenVK's own setting too), the pipeline prewarm uses up to 8 threads and the in-play builds
  (`[nvn] async_whole_threads`) 8, so one slow pipeline never holds up the others. MoltenVK compiles under the lock of the
  `VkPipelineCache` a pipeline is made with, so with MoltenVK pipelines are made without one (`[nvn] pipeline_cache_moltenvk`).
- A build of 3 s or more is logged (`slow graphics pipeline` / `slow compute pipeline`).

Apple's GPUs render in tiles: ending a render pass stores the tiles to memory and the next pass on the same targets loads them again.

- **Barriers** the game records between draws on the same targets no longer end the pass when no draw of it wrote memory other than its
  attachments (`[nvn] lazy_barriers`); the end of the pass carries the barrier. Pokopia records about 150 per frame. A draw that writes
  neither depth nor stencil stays in a pass whose depth is read-only (`[nvn] sticky_depth_ro`). The log line `pass ends, last 20 s` says
  why passes ended.
- **Unified memory**: the device-local copies of memory pools the Windows build makes (`[nvn] pool_mirror`) are off, and the F1 option for
  them is hidden.
- **The render thread** runs as a real-time thread on the performance cores (`[gfx] render_thread_policy = realtime`, `interactive` for
  QoS only, `default` to leave it alone).

**MetalFX.** `[upscale] mode = metalfx` (the default on macOS) renders the 3D scene at the `[upscale] quality` preset's size (67% for
`quality`, never below 34%: MetalFX enlarges at most 3x per side) and has Apple's temporal scaler rebuild it at full size before the game's
tone map; `metalfx_aa` runs it at full size for anti-aliasing only. The runtime jitters the game's camera and takes its depth and motion
from the G-buffer, as for the PC's temporal upscalers (`runtime/src/gfx/extras/temporal.cpp`). MetalFX runs on its own Metal queue
(`runtime/platform/mac/gfx/temporal_metalfx.mm`), so the frame's Vulkan recording is split around it (`nvn_gpu.cpp`
`split_recording`): the first part signals a timeline semaphore, MetalFX waits for it and signals the next value, and the rest of the
frame waits for that. With MoltenVK the semaphore's `MTLSharedEvent` does both on the GPU; KosmicKrisp has no event export, so a helper
thread waits for the first part and releases MetalFX's already committed command buffer, whose completion lets the rest go. The images
MetalFX reads and writes are the Vulkan images themselves (MoltenVK's exported textures; on KosmicKrisp a texture over the image's
exported heap). If MetalFX cannot start (an old GPU, a driver without Metal interop), the log says why once and FSR 1 takes over.
