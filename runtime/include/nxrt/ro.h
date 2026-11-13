// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// ldr:ro and ro:1 (nn::ro): NRO code modules a game loads while it runs (Smash's fighters, Bezel's minigames in Mario Party, the Source
// engine's renderer in Portal, FFX's effects). src/services/ro.cpp says how it works here; in short:
//   * the game's SDK (lifted, run as on the console) registers the NRR that lists the modules' SHA-256 (RegisterModuleInfo), reads an
//     NRO into its heap and asks the service to map it (MapManualLoadModuleMemory); then it zeroes the module's bss, relocates it through
//     rtld's module lists and calls its init, all in guest code;
//   * the service checks the image as Horizon's ro does (the NRR's header and hash list, the NRO's header, sizes, hash and module id) and
//     maps a copy of it at an address the title keeps for that module in every run (nro.tsv), so its lifted code stays valid;
//   * the module's code runs natively: the whole NRO is lifted and compiled on demand once (missed::load_module_unit), then found in the
//     title's cache by the image's hash in every later run.
// Configuration ([ro] section):
//   table = <file>                   the title's NRO table (default: nro.tsv next to the exe, where missed.txt is)
//   online_sdks = refuse | load      an NRO that exports an online SDK (Epic Online Services: EOS_Platform_Create) is refused (default),
//                                    as nxnative does: the game then plays offline, as it did before NROs loaded
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/ipc.h"
#include "nxrt/memory.h"

namespace nxrt::ro {

// Where NROs are mapped: [kNroBase, kNroBase + kNroSize), free on every platform (between the remote region and the HLE thunks; Android
// 256 GiB lower) and below 2^39, so an NRO's data can be an IPC buffer. A Switch 1 game cannot hold more than its 4 GB of memory in NROs at
// once. Each module keeps its address from run to run (nro.tsv), so its lifted code stays valid in the cache.
constexpr uint64_t kNroBase = 0x7300000000ull - mem::kGuestShift, kNroSize = 0x100000000ull;
constexpr uint32_t kNroFirstIndex = 64;   // NxModule.index of the NROs: 64 + their row in nro.tsv (build modules 0..12, jit plugins 48..63)

// IRoInterface: ldr:ro (the game's NRRs, kind User) or ro:1 (the jit plugin's NRRs, kind JitPlugin, for RegisterProcessModuleInfo).
std::shared_ptr<ipc::Service> make_service(bool jit_plugin = false);

// An NRO mapped now.
struct Loaded {
    uint64_t base = 0, nro_size = 0, bss_size = 0;
    std::string name;                 // the module's name (its path in .rodata, else "nro_<module id>")
    const NxModule* nx = nullptr;     // its lifted code
};
std::vector<Loaded> loaded();

std::string table_path();             // the title's NRO table ([ro] table)
void reset_for_tests();               // unmaps every NRO and forgets the NRRs, the sessions and the table read

}  // namespace nxrt::ro

namespace nxrt::services {
void register_ro();    // ldr:ro and ro:1 (services::register_misc)
}
