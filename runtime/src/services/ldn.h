// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The ldn HLE's process lobby (services/ldn.cpp). By default it is built on first use from the config: [net] server (auto = the default:
// host on a relay in this process, find hosts on the LAN; empty or off = local wireless off; host:port = that relay), [net] password,
// [account] nickname. Tests put their own lobby in place.
#pragma once
#include <memory>

#include "../net/lobby.h"

struct coop_node;   // coop.h

namespace nxrt::services {

// Replaces the lobby the ldn service uses from its next Initialize on (null = local wireless off). network_reset() drops it and goes back
// to the config.
void ldn_set_lobby(std::shared_ptr<vnet::Lobby> lobby);

// [net] protocol = native: the process's co-op node (third_party/pokopia-coop) once an ldn:u service object made it, else null. The bsd
// service routes the game's datagram sockets to it (coop_vnet_*).
coop_node* ldn_native_node();

}  // namespace nxrt::services
