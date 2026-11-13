// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// pm:dmnt is not served in PokoMac. AnySwitch's pm.cpp hands a skyline code mod a handle to the game's own process (it needs
// AnySwitch's kernel process objects), and PokoMac's mod manager takes no added code modules (docs/MODS.md), so no skyline build runs
// here; PokoMac never served pm:dmnt before either. misc.cpp (AnySwitch's, by Chaos192) registers it by this name.
#include "services_internal.h"

namespace nxrt::services {

void register_pm() {}

}  // namespace nxrt::services
