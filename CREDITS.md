# Credits

PokoMac is based on **[Recompia](https://github.com/Dittobyte/Recompia) by Dittobyte** (GPL-3.0). The recompiler, the runtime, the
launcher and the game work come from it; PokoMac adds the macOS platform layer, build and packaging ([docs/MACOS.md](docs/MACOS.md)).

PokoMac is **built on [AnySwitch](https://github.com/Chaos192/AnySwitch) by Chaos192** (github.com/Chaos192/AnySwitch, PolyForm Noncommercial 1.0.0):
AnySwitch grew out of PokoMac and generalised it to any Switch game; PokoMac takes its work back from it. Every file ported from AnySwitch
says so in its header ("Ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch").

## Fixes from Recompia

PokoMac 1.1.0 takes these fixes from [Recompia](https://github.com/Dittobyte/Recompia), each placed in PokoMac's own files:

- 4f8a43ba (featureJosh): the relay client bounds the room data and datagrams a relay sends.
- 72313d8d (featureJosh): a failed pool import is retried, not given up on for the session (#304).
- 599930bc (featureJosh): NVN deferred readback on by default (#276).
- 4d701fd7 (featureJosh): rumble is sent to the pads at most 30 times a second (#374).
- 1320344a (featureJosh): the GPU choice skips an older driver's copy of the same GPU.
- d1c044dc (featureJosh): loading screens keep the frame limit (unpaced_loading off by default).
- ee8e8033 (featureJosh): NVN asks for 32-wide compute only when the workgroup fits.
- b445b607 (mishap): macOS: the pool shadow copy is clamped to the chunk.
- 32770f47 (featureJosh): the pool copy copies the CPU's bytes around a GPU write in flight.
- a3b1f351 (Fernando Brito): Set up: a power cut never leaves a file a later run trusts.

## Third-party licences

PokoMac's own code is under the PolyForm Noncommercial License 1.0.0 ([LICENSE](LICENSE)); the lifter is
GPL-3.0-or-later ([RELICENSING.md](RELICENSING.md)). The GPL projects named here are references PokoMac learned from; the code it
ships from them is the lifter's (smo-recomp, ssbu-recomp), kept under the GPL.

PokoMac contains or downloads third-party code under its own licences. The licence texts are in `runtime/THIRD_PARTY_NOTICES.md`,
`recomp/THIRD_PARTY.md` and `tools/shaders/licenses/` in this repository, and in the `licenses/` folder of every release. They include
Ryujinx (MIT), Mesa (MIT), pokopia-native (MIT), smo-recomp and ssbu-recomp (GPL-3.0, in the lifter), SuperMarioWonderRecomp (MIT),
libnx, SDL3 (zlib), astc-encoder and bc7enc_rdo (Apache-2.0), libvpx (BSD-3-Clause), ImGui (MIT), Vulkan Memory Allocator (MIT), volk (MIT),
zstd (BSD), miniz (MIT), Lua (MIT), libopus (BSD-3-Clause), AMD FidelityFX and NVIDIA Image Scaling (MIT), Tauri (MIT / Apache-2.0), Zig (MIT), Python (PSF), the .NET runtime
(MIT), the Khronos Vulkan loader (Apache-2.0), KosmicKrisp (MIT) and MoltenVK (Apache-2.0), Kenney input prompts (CC0), the DejaVu fonts,
Inter (SIL OFL 1.1), Cascadia Mono (SIL OFL 1.1, as AnySwitch Mono) and Lucide icons (ISC).
