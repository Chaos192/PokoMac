# PokoMac: how the macOS port works

PokoMac recompiles Pokémon Pokopia on your Mac, from your own dump, into a native arm64 program, `pkp`, that runs on the same runtime as
the Windows build of the project it is based on, with graphics on Vulkan over Metal. No emulator, no Wine, no Rosetta. Players: see the
[README](../README.md).

## Differences from Windows

- **Graphics** run on Vulkan over Metal. The default driver is **KosmicKrisp** (LunarG's conformant Vulkan 1.4 driver for Apple silicon);
  **MoltenVK** is included as well: set `[gfx] vulkan_driver = moltenvk` in `pkp.ini` to use it.
- **DLSS and FSR 3** are Windows/PC-GPU components and are not available. FSR 1 and NVIDIA Image Scaling work.
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
