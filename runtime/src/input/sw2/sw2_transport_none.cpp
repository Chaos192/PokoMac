// SPDX-License-Identifier: GPL-3.0-or-later
// PokoMac takes AnySwitch's Switch 2 controller manager (sw2_manager.cpp, sw2_service.cpp, by Chaos192) without its Bluetooth LE
// transports (CoreBluetooth on macOS, WinRT on Windows): [input] switch2_ble stays without a transport here, as on AnySwitch's other
// platforms (sw2_service.cpp logs that the platform has none). Wired and paired controllers reach the game through the play layer as
// before.
#include <memory>

#include "nxrt/sw2/service.h"

namespace nxrt::sw2 {

#if defined(_WIN32) || defined(__APPLE__)
std::unique_ptr<Transport> make_platform_transport(Manager&) { return nullptr; }
#endif

}  // namespace nxrt::sw2
