// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia: repair of the shared player UUID (pkp_save_uuid.cpp, [pkp] uuid_repair).
// Every save is given a player UUID at new game (main 0x5aef60, nn::util::GenerateUuid at 0x5af08c), and Link Play's host refuses a
// visitor with its own UUID (0x5463c0, error 9005). Until GetInfo RandomEntropy came from the OS (src/kernel/svc_info.cpp) it was one
// fixed value, so every save made with this runtime got the same UUID, b9ae85ed-20e7-4f6b-89a1-9f30fcf9f0f9, and no two players could
// visit each other. At start, before the game reads its save, a save with that UUID gets a fresh random one in every place it is stored:
// the owner stamps in CMNDAT (G00 and its backup B00; zlib from 0x130, end offset at +0x10) and the plain copy in S00/SYDAT. STGDAT has
// none. A copy of the account folder is kept first, in <save dir>/uuid-backup/<time>/.
#pragma once

namespace nxrt::games::pkp {

// Runs once per process; does nothing when [pkp] uuid_repair = false, without a save, or when the save has another UUID. Logged.
void save_uuid_repair();

}  // namespace nxrt::games::pkp
