// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// ldn:u on a Ryujinx LDN master server ([net] protocol = ryuldn; the protocol client is net/ryuldn.h): the IUserLocalCommunicationService
// command table of services/ldn.cpp's backends, with rooms, scans and the game's datagrams carried by the master. Config:
//   [net] ryuldn_server      host[:port] of the master (port 30456 by default). Empty (the default): local wireless off (Scan finds
//                            nothing, CreateNetwork answers DeviceNotAvailable, Connect ConnectNotFound).
//   [net] ryuldn_passphrase  "" (public rooms) or Ryujinx's private-room passphrase "Ryujinx-" + 8 lowercase hex digits; anything else:
//                            local wireless off (logged), never the public rooms by mistake.
#pragma once
#include <memory>

#include "nxrt/ipc.h"

namespace nxrt::services {

class LdnRyuBackend {
public:
    virtual ~LdnRyuBackend() = default;
    // Leaves the room, closes the connection to the master and joins its threads (the service's on_close).
    virtual void shutdown() = 0;
};

// Adds the RyuLDN backend's ldn:u commands to s (an nn::ldn::detail::IUserLocalCommunicationService). s keeps the returned object as long
// as it lives (the commands hold it weakly) and calls shutdown() from its on_close.
std::shared_ptr<LdnRyuBackend> ldn_ryu_backend(ipc::Service& s);

}  // namespace nxrt::services
